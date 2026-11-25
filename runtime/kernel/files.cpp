// File system imports. Guest paths (game:\, d:\, \Device\Cdrom0\, cache:\ ...) map to host
// directories; lookups are case-insensitive like the Xbox file systems.
#include "objects.h"
#include "../platform.h"
#include "import_support.h"
#include "pack_overlay.h"
#include "../log.h"
#include "../debug/thread_times.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#ifndef _WIN32
#include <fnmatch.h> // Windows: runtime/platform.h
#endif
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <set>
#include <vector>

using namespace mem;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
    constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
    constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
    constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
    constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
    constexpr uint32_t STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
    constexpr uint32_t STATUS_INVALID_INFO_CLASS = 0xC0000003;

    constexpr uint32_t FILE_ATTRIBUTE_READONLY = 0x01, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80;

    // overlay: a directory whose files replace the root's at the same path (the mods folder, #134)
    struct Mount { std::string prefix; fs::path root; bool writable; fs::path overlay = {}; };

    std::mutex g_fsLock;
    std::vector<Mount> g_mounts;
    std::map<std::string, std::string> g_symlinks; // lowercase name -> target (guest path)

    struct File : kernel::Object
    {
        int fd = -1;
        fs::path hostPath;
        std::string guestPath;
        bool directory = false;
        uint64_t position = 0;
        std::vector<fs::path> dirEntries; // for NtQueryDirectoryFile
        size_t dirIndex = 0;
        bool dirListed = false;
        // The game's archive with entries replaced from the mods folder (#169): reads and sizes
        // come from it instead of fd. packIndex: this is its .pkh, otherwise its .pkd.
        std::shared_ptr<const kernel::pack::Overlay> pack;
        bool packIndex = false;
        File() : kernel::Object(Type::Other) {}
        ~File() override { if (fd >= 0) close(fd); }
    };

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    std::string ansiString(uint32_t pStr)
    {
        if (!pStr) return {};
        uint16_t len = read16(pStr);
        uint32_t buf = read32(pStr + 4);
        return std::string((const char*)host(buf), len);
    }

    // OBJECT_ATTRIBUTES { u32 RootDirectory; ANSI_STRING* ObjectName; u32 Attributes; }
    std::string pathFromAttributes(uint32_t attr)
    {
        std::string name = ansiString(read32(attr + 4));
        uint32_t root = read32(attr);
        if (root && root != 0xFFFFFFFD)
        {
            auto* dir = static_cast<File*>(kernel::objectFromHandle(root));
            if (dir && dir->type == kernel::Object::Type::Other)
                name = dir->guestPath + "\\" + name;
        }
        return name;
    }

    // Resolve one component case-insensitively; returns the existing entry or the literal name.
    fs::path matchComponent(const fs::path& dir, const std::string& comp)
    {
        fs::path exact = dir / comp;
        if (fs::exists(exact))
            return exact;
        std::string want = lower(comp);
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (lower(e.path().filename().string()) == want)
                return e.path();
        return exact;
    }

    std::set<std::string> g_loggedOverlay; // replaced files already in the log (under g_fsLock)
    fs::path g_discOverlay;                 // the mods folder, if there is one (under g_fsLock)
    // Archives (.pkh + .pkd) whose entries the mods folder replaces (#169), by the host path of the
    // .pkh; null for one it replaces nothing in. Built at the first open. Under g_fsLock.
    std::map<std::string, std::shared_ptr<const kernel::pack::Overlay>> g_packs;

    // Guest path -> host path. Returns false if no mount matches. Called with g_fsLock held.
    bool resolve(std::string guest, fs::path& out, bool& writable)
    {
        std::replace(guest.begin(), guest.end(), '/', '\\');
        for (int depth = 0; depth < 8; depth++)
        {
            // Expand symbolic links such as "game:" -> "\Device\Cdrom0"
            std::string lg = lower(guest);
            bool replaced = false;
            for (auto& [name, target] : g_symlinks)
            {
                if (lg.rfind(name, 0) == 0 && (lg.size() == name.size() || lg[name.size()] == '\\'))
                {
                    guest = target + guest.substr(name.size());
                    replaced = true;
                    break;
                }
            }
            if (!replaced) break;
        }
        std::string lg = lower(guest);
        for (auto& m : g_mounts)
        {
            if (lg.rfind(m.prefix, 0) != 0)
                continue;
            std::string rest = guest.substr(m.prefix.size());
            auto walk = [&](fs::path p) {
                size_t i = 0;
                while (i < rest.size())
                {
                    size_t j = rest.find('\\', i);
                    if (j == std::string::npos) j = rest.size();
                    std::string comp = rest.substr(i, j - i);
                    if (!comp.empty() && comp != ".")
                        p = comp == ".." ? p.parent_path() : matchComponent(p, comp);
                    i = j + 1;
                }
                return p;
            };
            out = walk(m.root);
            writable = m.writable;
            // A file in the overlay at the same path (matched without case, like the disc) is used
            // instead. Only files: directories, and so directory listings, stay the root's.
            if (!m.overlay.empty())
            {
                std::error_code ec;
                fs::path o = walk(m.overlay);
                if (fs::is_regular_file(o, ec))
                {
                    // "adds": a file the disc doesn't have, such as a new play the game looks for
                    // here once its archive doesn't have it either (#169)
                    if (g_loggedOverlay.insert(o.string()).second)
                        info("[mods] %s %s %s", o.string().c_str(), fs::exists(out, ec) ? "replaces" : "adds", guest.c_str());
                    out = o;
                }
            }
            return true;
        }
        return false;
    }

    // The overlay for the archive at these host paths, or null: built at the first use. Under g_fsLock.
    std::shared_ptr<const kernel::pack::Overlay> packAt(const fs::path& pkh, const fs::path& pkd)
    {
        if (g_discOverlay.empty())
            return nullptr;
        auto [it, fresh] = g_packs.try_emplace(pkh.string());
        if (fresh)
        {
            std::vector<std::string> log;
            it->second = kernel::pack::Overlay::build(pkh, pkd, g_discOverlay, log);
            for (auto& line : log)
                info("%s", line.c_str());
        }
        return it->second;
    }

    // The overlay for a read-only .pkh or .pkd, or null. `index`: guest is the .pkh. Under g_fsLock.
    std::shared_ptr<const kernel::pack::Overlay> packFor(const std::string& guest, bool& index)
    {
        if (g_discOverlay.empty() || guest.size() < 4)
            return nullptr;
        std::string ext = lower(guest.substr(guest.size() - 4));
        if (ext != ".pkh" && ext != ".pkd")
            return nullptr;
        index = ext == ".pkh";
        std::string stem = guest.substr(0, guest.size() - 4);
        fs::path pkh, pkd;
        bool writableIndex = false, writableData = false;
        if (!resolve(stem + ".pkh", pkh, writableIndex) || !resolve(stem + ".pkd", pkd, writableData) || writableIndex || writableData)
            return nullptr;
        return packAt(pkh, pkd);
    }

    // NtReadFile from an archive with replaced entries: into guest memory through a host buffer, as
    // mem::readFile does, and the log names each replacement the first time the game reads it
    long readPack(const File& f, uint32_t guest, uint32_t len, uint64_t offset)
    {
        static thread_local std::vector<uint8_t> buffer;
        buffer.resize(len);
        long n = f.pack->read(f.packIndex, buffer.data(), len, offset);
        if (n > 0)
        {
            std::memcpy(host(guest), buffer.data(), size_t(n)); // faults on watched pages, as a game write would
            if (!f.packIndex)
                for (auto& entry : f.pack->firstReads(offset, uint32_t(n)))
                    info("[mods] the game read %s from the mods folder", entry.c_str());
        }
        return n;
    }

    uint64_t fileTime(const struct stat& st)
    {
#ifdef _WIN32
        return 116444736000000000ull + uint64_t(st.st_mtime) * 10000000ull; // whole seconds only there
#else
        return 116444736000000000ull + uint64_t(st.st_mtim.tv_sec) * 10000000ull + st.st_mtim.tv_nsec / 100;
#endif
    }

    uint32_t attributesOf(const struct stat& st, bool writable)
    {
        uint32_t a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
        if (!writable) a |= FILE_ATTRIBUTE_READONLY;
        return a ? a : FILE_ATTRIBUTE_NORMAL;
    }

    // FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
    void writeNetworkOpenInfo(uint32_t out, const struct stat& st, bool writable)
    {
        uint64_t t = fileTime(st);
        write64(out + 0x00, t); // CreationTime
        write64(out + 0x08, t); // LastAccessTime
        write64(out + 0x10, t); // LastWriteTime
        write64(out + 0x18, t); // ChangeTime
        write64(out + 0x20, (st.st_size + 0xFFF) & ~0xFFFull); // AllocationSize
        write64(out + 0x28, st.st_size);                        // EndOfFile
        write32(out + 0x30, attributesOf(st, writable));
        write32(out + 0x34, 0);
    }

    void setIoStatus(uint32_t iosb, uint32_t status, uint32_t info)
    {
        if (!iosb) return;
        write32(iosb, status);
        write32(iosb + 4, info);
    }

    void signalCompletion(uint32_t event)
    {
        if (!event) return;
        if (auto* o = kernel::objectFromHandle(event); o && o->type == kernel::Object::Type::Event)
            kernel::setEvent(static_cast<kernel::Event*>(o));
    }

    File* fileFromHandle(uint32_t h)
    {
        auto* o = kernel::objectFromHandle(h);
        return o && o->type == kernel::Object::Type::Other ? static_cast<File*>(o) : nullptr;
    }

    // Shared by NtCreateFile / NtOpenFile
    uint32_t openFile(uint32_t pHandle, uint32_t access, uint32_t attr, uint32_t iosb, uint32_t disposition)
    {
        std::string guest = pathFromAttributes(attr);
        fs::path hostPath;
        bool writable = false, packIndex = false;
        std::shared_ptr<const kernel::pack::Overlay> pack;
        {
            std::lock_guard guard(g_fsLock);
            if (!resolve(guest, hostPath, writable))
            {
                warn("open \"%s\": no device mapping", guest.c_str());
                setIoStatus(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }
            pack = packFor(guest, packIndex);
        }

        constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5;
        constexpr uint32_t GENERIC_WRITE = 0x40000000, FILE_WRITE_DATA = 0x2, FILE_APPEND_DATA = 0x4;
        bool wantsWrite = access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA);
        bool exists = fs::exists(hostPath);

        if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
        {
            info("open \"%s\": not found", guest.c_str());
            setIoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if ((wantsWrite || !exists) && !writable)
        {
            info("open \"%s\": read-only device", guest.c_str());
            setIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }

        auto* f = new File();
        f->hostPath = hostPath;
        f->guestPath = guest;
        f->directory = exists && fs::is_directory(hostPath);
        uint32_t result = 1; // FILE_OPENED
        if (!f->directory)
        {
            int flags = wantsWrite ? O_RDWR : O_RDONLY;
            if (!exists) { flags |= O_CREAT; result = 2; } // FILE_CREATED
            if (exists && (disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF)) { flags |= O_TRUNC; result = 3; }
            if (!exists) fs::create_directories(hostPath.parent_path());
            f->fd = ::open(hostPath.string().c_str(), flags, 0644);
            if (f->fd < 0)
            {
                warn("open \"%s\" -> %s failed: %s", guest.c_str(), hostPath.string().c_str(), strerror(errno));
                delete f;
                setIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
                return STATUS_ACCESS_DENIED;
            }
        }
        (void)FILE_CREATE; (void)FILE_OPEN_IF;
        if (!f->directory && !wantsWrite)
        {
            f->pack = std::move(pack);
            f->packIndex = packIndex;
        }
        write32(pHandle, kernel::registerObject(f));
        setIoStatus(iosb, STATUS_SUCCESS, result);
        info("open \"%s\"%s", guest.c_str(), f->directory ? " (directory)" : "");
        return STATUS_SUCCESS;
    }
}

namespace kernel
{
    std::string g_savesRoot;
    const std::string& savesRoot() { return g_savesRoot; }

    void addMount(const std::string& devicePrefix, const std::string& hostRoot, bool writable)
    {
        std::lock_guard guard(g_fsLock);
        std::string prefix = lower(devicePrefix);
        g_mounts.erase(std::remove_if(g_mounts.begin(), g_mounts.end(), [&](auto& m) { return m.prefix == prefix; }), g_mounts.end());
        g_mounts.push_back({ prefix, hostRoot, writable });
        std::sort(g_mounts.begin(), g_mounts.end(), [](auto& a, auto& b) { return a.prefix.size() > b.prefix.size(); });
    }

    void setDiscOverlay(const std::string& dir)
    {
        std::lock_guard guard(g_fsLock);
        for (auto& m : g_mounts)
            if (m.prefix == "\\device\\cdrom0")
            {
                m.overlay = dir;
                g_discOverlay = dir;
                info("[mods] files in %s replace the disc's at the same path, and the archive's entries", dir.c_str());
            }
    }

    bool readArchiveEntry(const std::string& archive, const std::string& entry, std::string& out)
    {
        std::lock_guard guard(g_fsLock);
        fs::path pkh, pkd;
        bool writable = false;
        if (!resolve(archive + ".pkh", pkh, writable) || !resolve(archive + ".pkd", pkd, writable))
            return false;
        return pack::readEntry(pkh, pkd, packAt(pkh, pkd).get(), entry, out);
    }

    bool readHostArchiveEntry(const std::string& archive, const std::string& entry, std::string& out)
    {
        std::lock_guard guard(g_fsLock);
        const fs::path pkh = archive + ".pkh", pkd = archive + ".pkd";
        return pack::readEntry(pkh, pkd, packAt(pkh, pkd).get(), entry, out);
    }

    void removeMount(const std::string& devicePrefix)
    {
        std::lock_guard guard(g_fsLock);
        std::string prefix = lower(devicePrefix);
        g_mounts.erase(std::remove_if(g_mounts.begin(), g_mounts.end(), [&](auto& m) { return m.prefix == prefix; }), g_mounts.end());
    }

    void addSymbolicLink(const std::string& name, const std::string& target)
    {
        std::lock_guard guard(g_fsLock);
        g_symlinks[lower(name)] = target;
    }

    void removeSymbolicLink(const std::string& name)
    {
        std::lock_guard guard(g_fsLock);
        g_symlinks.erase(lower(name));
    }

    void initFileSystem(const std::string& discRoot, const std::string& cacheRoot, const std::string& savesRoot,
                        const std::string& updateRoot)
    {
        fs::create_directories(cacheRoot);
        fs::create_directories(savesRoot);
        g_savesRoot = savesRoot;
        // Longest prefixes first
        g_mounts = {
            { "\\device\\cdrom0", discRoot, false },
            { "\\device\\harddisk0\\cache0", cacheRoot, true },
            { "\\device\\harddisk0\\cache1", cacheRoot, true },
        };
        if (!updateRoot.empty())
            g_mounts.push_back({ "\\device\\update", updateRoot, false });
        std::sort(g_mounts.begin(), g_mounts.end(), [](auto& a, auto& b) { return a.prefix.size() > b.prefix.size(); });
        g_symlinks = {
            { "game:", "\\Device\\Cdrom0" },
            { "d:", "\\Device\\Cdrom0" },
            { "\\??\\game:", "\\Device\\Cdrom0" },
            { "\\??\\d:", "\\Device\\Cdrom0" },
            { "cache:", "\\Device\\Harddisk0\\Cache0" },
            { "\\??\\cache:", "\\Device\\Harddisk0\\Cache0" },
        };
        if (!updateRoot.empty())
        {
            g_symlinks["update:"] = "\\Device\\Update";
            g_symlinks["\\??\\update:"] = "\\Device\\Update";
            info("file system: update:\\ -> %s", updateRoot.c_str());
        }
        info("file system: game:\\ -> %s, cache:\\ -> %s", discRoot.c_str(), cacheRoot.c_str());
    }
}

// NTSTATUS NtCreateFile(HANDLE* Handle, u32 DesiredAccess, OBJECT_ATTRIBUTES* Attr, IO_STATUS_BLOCK* Iosb,
//                       i64* AllocationSize, u32 FileAttributes, u32 ShareAccess, u32 CreateDisposition, ...)
GUEST_IMPORT(NtCreateFile)
{
    debug::threadtimes::Timer timer(debug::threadtimes::kFile, uint32_t(ctx.lr));
    ctx.r3.u64 = openFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32);
}

// NTSTATUS NtOpenFile(HANDLE* Handle, u32 DesiredAccess, OBJECT_ATTRIBUTES* Attr, IO_STATUS_BLOCK* Iosb, u32 ShareAccess, u32 OpenOptions)
GUEST_IMPORT(NtOpenFile)
{
    debug::threadtimes::Timer timer(debug::threadtimes::kFile, uint32_t(ctx.lr));
    ctx.r3.u64 = openFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1 /* FILE_OPEN */);
}

// NTSTATUS NtReadFile(HANDLE File, HANDLE Event, void* Apc, void* ApcContext, IO_STATUS_BLOCK* Iosb,
//                     void* Buffer, u32 Length, i64* ByteOffset)
GUEST_IMPORT(NtReadFile)
{
    uint32_t event = ctx.r4.u32, iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, pOffset = ctx.r10.u32;
    File* f = fileFromHandle(ctx.r3.u32);
    if (!f || f->fd < 0) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t off = pOffset ? read64(pOffset) : f->position;
    debug::threadtimes::Timer timer(debug::threadtimes::kFile, uint32_t(ctx.lr));
    ssize_t n = f->pack ? readPack(*f, buf, len, off)
                        : mem::readFile(f->fd, buf, len, off); // host buffer for watched physical pages
    if (n < 0) n = 0;
    f->position = off + n;
    uint32_t status = (n == 0 && len > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    setIoStatus(iosb, status, uint32_t(n));
    signalCompletion(event);
    ctx.r3.u64 = status;
}

// NTSTATUS NtReadFileScatter(HANDLE File, HANDLE Event, void* Apc, void* ApcContext, IO_STATUS_BLOCK* Iosb,
//                            FILE_SEGMENT_ELEMENT* Segments, u32 Length, i64* ByteOffset)
GUEST_IMPORT(NtReadFileScatter)
{
    uint32_t event = ctx.r4.u32, iosb = ctx.r7.u32, segs = ctx.r8.u32, len = ctx.r9.u32, pOffset = ctx.r10.u32;
    File* f = fileFromHandle(ctx.r3.u32);
    if (!f || f->fd < 0) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t off = pOffset ? read64(pOffset) : f->position;
    debug::threadtimes::Timer timer(debug::threadtimes::kFile, uint32_t(ctx.lr));
    uint32_t total = 0;
    for (uint32_t i = 0; total < len; i++)
    {
        uint32_t page = read32(segs + i * 8 + 4); // 64-bit element, low word is the address
        uint32_t chunk = std::min<uint32_t>(0x1000, len - total);
        ssize_t n = f->pack ? readPack(*f, page, chunk, off + total) : mem::readFile(f->fd, page, chunk, off + total);
        if (n <= 0) break;
        total += n;
        if (uint32_t(n) < chunk) break;
    }
    f->position = off + total;
    uint32_t status = (total == 0 && len > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    setIoStatus(iosb, status, total);
    signalCompletion(event);
    ctx.r3.u64 = status;
}

// NTSTATUS NtWriteFile(HANDLE File, HANDLE Event, void* Apc, void* ApcContext, IO_STATUS_BLOCK* Iosb,
//                      const void* Buffer, u32 Length, i64* ByteOffset)
GUEST_IMPORT(NtWriteFile)
{
    uint32_t event = ctx.r4.u32, iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, pOffset = ctx.r10.u32;
    File* f = fileFromHandle(ctx.r3.u32);
    if (!f || f->fd < 0) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t off = pOffset ? read64(pOffset) : f->position;
    ssize_t n = pwrite(f->fd, host(buf), len, off);
    if (n < 0) { setIoStatus(iosb, STATUS_ACCESS_DENIED, 0); ctx.r3.u64 = STATUS_ACCESS_DENIED; return; }
    f->position = off + n;
    setIoStatus(iosb, STATUS_SUCCESS, uint32_t(n));
    signalCompletion(event);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtFlushBuffersFile) { ctx.r3.u64 = STATUS_SUCCESS; }

// NTSTATUS NtQueryInformationFile(HANDLE File, IO_STATUS_BLOCK* Iosb, void* Info, u32 Length, u32 Class)
GUEST_IMPORT(NtQueryInformationFile)
{
    uint32_t iosb = ctx.r4.u32, out = ctx.r5.u32, len = ctx.r6.u32, cls = ctx.r7.u32;
    File* f = fileFromHandle(ctx.r3.u32);
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    struct stat st{};
    if (f->fd >= 0) fstat(f->fd, &st); else stat(f->hostPath.string().c_str(), &st);
    if (f->pack) st.st_size = off_t(f->pack->size(f->packIndex));
    uint32_t written = 0;
    switch (cls)
    {
    case 4: // FileBasicInformation
        for (int i = 0; i < 4; i++) write64(out + i * 8, fileTime(st));
        write32(out + 0x20, attributesOf(st, false));
        written = 0x28;
        break;
    case 5: // FileStandardInformation
        write64(out + 0x00, (st.st_size + 0xFFF) & ~0xFFFull);
        write64(out + 0x08, st.st_size);
        write32(out + 0x10, 1);
        write8(out + 0x14, 0);
        write8(out + 0x15, f->directory);
        written = 0x18;
        break;
    case 14: // FilePositionInformation
        write64(out, f->position);
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        writeNetworkOpenInfo(out, st, false);
        written = 0x38;
        break;
    default:
        warn("NtQueryInformationFile: class %u not implemented", cls);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS;
        return;
    }
    if (len < written) { ctx.r3.u64 = STATUS_INFO_LENGTH_MISMATCH; return; }
    setIoStatus(iosb, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtSetInformationFile(HANDLE File, IO_STATUS_BLOCK* Iosb, void* Info, u32 Length, u32 Class)
GUEST_IMPORT(NtSetInformationFile)
{
    uint32_t iosb = ctx.r4.u32, in = ctx.r5.u32, cls = ctx.r7.u32;
    File* f = fileFromHandle(ctx.r3.u32);
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    switch (cls)
    {
    case 14: f->position = read64(in); break;                                   // FilePositionInformation
    case 20: if (f->fd >= 0 && ftruncate(f->fd, read64(in)) != 0) {} break;     // FileEndOfFileInformation
    case 19: break;                                                             // FileAllocationInformation
    default:
        warn("NtSetInformationFile: class %u not implemented", cls);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS;
        return;
    }
    setIoStatus(iosb, STATUS_SUCCESS, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* Attr, FILE_NETWORK_OPEN_INFORMATION* Info)
GUEST_IMPORT(NtQueryFullAttributesFile)
{
    std::string guest = pathFromAttributes(ctx.r3.u32);
    fs::path p;
    bool writable = false;
    struct stat st{};
    bool ok;
    {
        std::lock_guard guard(g_fsLock);
        ok = resolve(guest, p, writable);
    }
    if (!ok || stat(p.string().c_str(), &st) != 0)
    {
        info("query attributes \"%s\": not found", guest.c_str());
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    {
        std::lock_guard guard(g_fsLock);
        bool index = false;
        if (auto pack = packFor(guest, index))
            st.st_size = off_t(pack->size(index));
    }
    writeNetworkOpenInfo(ctx.r4.u32, st, writable);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE File, IO_STATUS_BLOCK* Iosb, void* Info, u32 Length, u32 Class)
GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    uint32_t iosb = ctx.r4.u32, out = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t written = 0;
    switch (cls)
    {
    case 1: // FileFsVolumeInformation { i64 CreationTime; u32 SerialNumber; u32 LabelLength; u8 SupportsObjects; wchar Label[] }
        write64(out, 0); write32(out + 8, 0x12345678); write32(out + 12, 0); write8(out + 16, 0);
        written = 0x18;
        break;
    case 3: // FileFsSizeInformation
        write64(out + 0, 0x100000); // total units
        write64(out + 8, 0x80000);  // available units
        write32(out + 16, 8);       // sectors per unit
        write32(out + 20, 0x200);   // bytes per sector
        written = 0x18;
        break;
    case 5: // FileFsAttributeInformation { u32 Attributes; i32 MaxComponentLength; u32 NameLength; wchar Name[] }
        write32(out + 0, 0x6); write32(out + 4, 255); write32(out + 8, 8);
        write16(out + 12, 'F'); write16(out + 14, 'A'); write16(out + 16, 'T'); write16(out + 18, 'X');
        written = 0x14;
        break;
    default:
        warn("NtQueryVolumeInformationFile: class %u not implemented", cls);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS;
        return;
    }
    setIoStatus(iosb, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE File, HANDLE Event, void* Apc, void* ApcContext, IO_STATUS_BLOCK* Iosb,
//                               FILE_DIRECTORY_INFORMATION* Info, u32 Length, ANSI_STRING* FileMask, BOOL RestartScan)
GUEST_IMPORT(NtQueryDirectoryFile)
{
    uint32_t iosb = ctx.r7.u32, out = ctx.r8.u32, len = ctx.r9.u32, pMask = ctx.r10.u32;
    File* f = fileFromHandle(ctx.r3.u32);
    if (!f || !f->directory) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::string mask = lower(ansiString(pMask));
    if (!f->dirListed)
    {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(f->hostPath, ec))
        {
            std::string n = lower(e.path().filename().string());
            bool match = mask.empty() || mask == "*" || mask == "*.*" || fnmatch(mask.c_str(), n.c_str(), 0) == 0;
            if (match) f->dirEntries.push_back(e.path());
        }
        f->dirListed = true;
    }
    if (f->dirIndex >= f->dirEntries.size())
    {
        setIoStatus(iosb, STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = STATUS_NO_MORE_FILES;
        return;
    }
    // One entry per call: FILE_DIRECTORY_INFORMATION (0x40 header + name)
    const fs::path& p = f->dirEntries[f->dirIndex++];
    std::string name = p.filename().string();
    if (len < 0x40 + name.size()) { ctx.r3.u64 = STATUS_INFO_LENGTH_MISMATCH; return; }
    struct stat st{};
    stat(p.string().c_str(), &st);
    write32(out + 0x00, 0); // NextEntryOffset
    write32(out + 0x04, uint32_t(f->dirIndex - 1));
    for (int i = 0; i < 4; i++) write64(out + 0x08 + i * 8, fileTime(st));
    write64(out + 0x28, st.st_size);
    write64(out + 0x30, (st.st_size + 0xFFF) & ~0xFFFull);
    write32(out + 0x38, attributesOf(st, false));
    write32(out + 0x3C, uint32_t(name.size()));
    memcpy(host(out + 0x40), name.data(), name.size());
    setIoStatus(iosb, STATUS_SUCCESS, uint32_t(0x40 + name.size()));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS ObCreateSymbolicLink(ANSI_STRING* Name, ANSI_STRING* Target)
GUEST_IMPORT(ObCreateSymbolicLink)
{
    std::string name = lower(ansiString(ctx.r3.u32)), target = ansiString(ctx.r4.u32);
    info("symbolic link %s -> %s", name.c_str(), target.c_str());
    std::lock_guard guard(g_fsLock);
    g_symlinks[name] = target;
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(ObDeleteSymbolicLink)
{
    std::string name = lower(ansiString(ctx.r3.u32));
    info("delete symbolic link %s", name.c_str());
    ctx.r3.u64 = STATUS_SUCCESS; // keep our defaults so game:\ keeps working
}

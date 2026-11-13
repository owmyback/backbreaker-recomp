// XAM content: save devices and content packages (saves), backed by host folders (roadmap phase 8,
// issue #7). Signatures, structure layouts and behavior follow Xenia's xam_content.cc,
// xam_content_device.cc, xam_enum.cc and content_manager.h (BSD; docs/licenses/xenia-LICENSE.txt).
//
// A content package (e.g. a save) lives in <savesRoot>/<content type, 8 hex digits>/<file name>/.
// Its display name is kept next to it in <file name>.name (UTF-8), its thumbnail in <file name>.png.
// XamContentCreateEx mounts a package as a drive: "<root>:" -> \Device\Content\<n> -> the folder.
#include "kernel.h"
#include "objects.h"
#include "import_support.h"
#include "../log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace mem;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
    constexpr uint32_t ERROR_NO_MORE_FILES = 0x12;
    constexpr uint32_t ERROR_INVALID_PARAMETER = 0x57;
    constexpr uint32_t ERROR_ALREADY_EXISTS = 0xB7;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    constexpr uint32_t E_INVALIDARG = 0x80070057;

    constexpr uint32_t kHddDeviceId = 1, kOddDeviceId = 2; // Xenia's dummy devices
    constexpr uint32_t kSavedGame = 1;                     // XContentType::kSavedGame

    // XCONTENT_DATA (0x134 bytes): u32 DeviceId; u32 ContentType; u16 DisplayName[128] (UTF-16BE);
    //                              char FileName[42]; u8 Padding[2]
    constexpr uint32_t kContentDataSize = 0x134;
    struct ContentData
    {
        uint32_t deviceId = kHddDeviceId, contentType = 0;
        std::u16string displayName;
        std::string fileName;

        static ContentData read(uint32_t p)
        {
            ContentData c;
            c.deviceId = read32(p);
            c.contentType = read32(p + 4);
            for (uint32_t i = 0; i < 128; i++)
            {
                uint16_t ch = read16(p + 8 + i * 2);
                if (!ch) break;
                c.displayName.push_back(char16_t(ch));
            }
            const char* name = (const char*)host(p + 0x108);
            c.fileName.assign(name, strnlen(name, 42));
            return c;
        }

        void write(uint8_t* out) const // guest byte order
        {
            memset(out, 0, kContentDataSize);
            auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) out[o + i] = uint8_t(v >> (24 - 8 * i)); };
            put32(0, deviceId);
            put32(4, contentType);
            for (size_t i = 0; i < displayName.size() && i < 127; i++)
            {
                out[8 + i * 2] = uint8_t(displayName[i] >> 8);
                out[8 + i * 2 + 1] = uint8_t(displayName[i]);
            }
            memcpy(out + 0x108, fileName.data(), std::min<size_t>(fileName.size(), 42));
        }
    };

    // Host folder of a package; empty if the file name can't be used as a folder name
    fs::path packagePath(const ContentData& c)
    {
        if (c.fileName.empty() || c.fileName == "." || c.fileName == ".." || c.fileName.find_first_of("/\\") != std::string::npos)
            return {};
        char type[16];
        snprintf(type, sizeof(type), "%08X", c.contentType);
        return fs::path(kernel::savesRoot()) / type / c.fileName;
    }

    bool exists(const ContentData& c)
    {
        fs::path p = packagePath(c);
        std::error_code ec;
        return !p.empty() && fs::is_directory(p, ec);
    }

    std::string toUtf8(const std::u16string& s)
    {
        std::string out;
        for (char16_t ch : s)
        {
            if (ch < 0x80) out += char(ch);
            else if (ch < 0x800) { out += char(0xC0 | (ch >> 6)); out += char(0x80 | (ch & 0x3F)); }
            else { out += char(0xE0 | (ch >> 12)); out += char(0x80 | ((ch >> 6) & 0x3F)); out += char(0x80 | (ch & 0x3F)); }
        }
        return out;
    }

    std::u16string fromUtf8(const std::string& s)
    {
        std::u16string out;
        for (size_t i = 0; i < s.size();)
        {
            unsigned char c = s[i];
            if (c < 0x80) { out += char16_t(c); i += 1; }
            else if ((c >> 5) == 6 && i + 1 < s.size()) { out += char16_t(((c & 0x1F) << 6) | (s[i + 1] & 0x3F)); i += 2; }
            else if (i + 2 < s.size()) { out += char16_t(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F)); i += 3; }
            else break;
        }
        return out;
    }

    // Packages of one type on the hard drive, in name order
    std::vector<ContentData> listContent(uint32_t contentType)
    {
        std::vector<ContentData> list;
        char type[16];
        snprintf(type, sizeof(type), "%08X", contentType);
        fs::path dir = fs::path(kernel::savesRoot()) / type;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec))
        {
            if (!e.is_directory()) continue;
            ContentData c;
            c.contentType = contentType;
            c.fileName = e.path().filename().string();
            if (c.fileName.size() > 42) continue;
            std::ifstream name(e.path().string() + ".name");
            std::string utf8((std::istreambuf_iterator<char>(name)), std::istreambuf_iterator<char>());
            c.displayName = fromUtf8(utf8);
            list.push_back(c);
        }
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.fileName < b.fileName; });
        return list;
    }

    // Open packages: root name (lowercase, e.g. "save") -> device path
    std::mutex g_lock;
    std::map<std::string, std::string> g_open;
    uint32_t g_nextDevice = 1;

    struct ContentEnumerator : kernel::Object
    {
        std::vector<std::array<uint8_t, kContentDataSize>> items;
        size_t next = 0;
        uint32_t perCall = 1;
        ContentEnumerator() : kernel::Object(Type::Other) {}
    };

    std::string guestString(uint32_t p)
    {
        if (!p) return {};
        const char* s = (const char*)host(p);
        return std::string(s, strnlen(s, 256));
    }
}

// u32 XamContentCreateEnumerator(u32 UserIndex, u32 DeviceId, u32 ContentType, u32 ContentFlags,
//                                u32 ItemsPerEnumerate, u32* BufferSize, HANDLE* Handle)
GUEST_IMPORT(XamContentCreateEnumerator)
{
    uint32_t device = ctx.r4.u32, type = ctx.r5.u32, perCall = ctx.r7.u32, pSize = ctx.r8.u32, pHandle = ctx.r9.u32;
    if ((device && device != kHddDeviceId && device != kOddDeviceId) || !pHandle)
    {
        if (pSize) write32(pSize, 0);
        ctx.r3.u64 = E_INVALIDARG;
        return;
    }
    if (pSize) write32(pSize, kContentDataSize * perCall);
    auto* e = new ContentEnumerator;
    e->perCall = perCall ? perCall : 1;
    if (!device || device == kHddDeviceId)
        for (const ContentData& c : listContent(type))
        {
            e->items.emplace_back();
            c.write(e->items.back().data());
        }
    write32(pHandle, kernel::registerObject(e));
    info("[xam] content enumerator: type %08X on device %u, %zu item(s)", type, device, e->items.size());
    ctx.r3.u64 = ERROR_SUCCESS;
}

// u32 XamEnumerate(HANDLE Enumerator, u32 Flags, void* Buffer, u32 BufferLength, u32* ItemsReturned,
//                  XOVERLAPPED* Overlapped)
GUEST_IMPORT(XamEnumerate)
{
    uint32_t handle = ctx.r3.u32, buf = ctx.r5.u32, length = ctx.r6.u32, pReturned = ctx.r7.u32, overlapped = ctx.r8.u32;
    auto* e = dynamic_cast<ContentEnumerator*>(kernel::objectFromHandle(handle));
    if (!e)
    {
        warn("[xam] XamEnumerate on handle %08X: not a content enumerator", handle);
        ctx.r3.u64 = 6; // ERROR_INVALID_HANDLE
        return;
    }
    uint32_t result, count = 0;
    if (!buf)
        result = ERROR_INVALID_PARAMETER;
    else
    {
        count = uint32_t(std::min<size_t>({ e->items.size() - e->next, e->perCall, length / kContentDataSize }));
        for (uint32_t i = 0; i < count; i++) memcpy(host(buf + i * kContentDataSize), e->items[e->next + i].data(), kContentDataSize);
        e->next += count;
        result = count ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
    }
    if (overlapped)
        ctx.r3.u64 = kernel::completeOverlapped(overlapped, result, count);
    else
    {
        if (pReturned) write32(pReturned, result == ERROR_SUCCESS ? count : 0);
        ctx.r3.u64 = result;
    }
}

// u32 XamContentCreateEx(u32 UserIndex, char* RootName, XCONTENT_DATA* ContentData, u32 Flags,
//                        u32* Disposition, u32* LicenseMask, u32 CacheSize, u64 ContentSize, XOVERLAPPED*)
// Flags & 0xF: 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing.
// Disposition: 1 created, 2 opened (also the overlapped's Length).
GUEST_IMPORT(XamContentCreateEx)
{
    std::string root = guestString(ctx.r4.u32);
    ContentData c = ContentData::read(ctx.r5.u32);
    uint32_t flags = ctx.r6.u32, pDisposition = ctx.r7.u32, pLicense = ctx.r8.u32, overlapped = stackArg(ctx, 8);
    fs::path path = packagePath(c);
    std::error_code ec;
    uint32_t result = ERROR_SUCCESS, disposition = 0; // 0 unknown, 1 create, 2 open
    bool there = exists(c);
    switch (flags & 0xF)
    {
    case 1: if (there) result = ERROR_ALREADY_EXISTS; else disposition = 1; break;
    case 2: if (there) fs::remove_all(path, ec); disposition = 1; break;
    case 3: if (!there) result = ERROR_PATH_NOT_FOUND; else disposition = 2; break;
    case 4: disposition = there ? 2 : 1; break;
    case 5: if (!there) result = ERROR_PATH_NOT_FOUND; else { fs::remove_all(path, ec); disposition = 1; } break;
    default: result = ERROR_INVALID_PARAMETER;
    }
    if (path.empty() || root.empty()) result = ERROR_INVALID_PARAMETER, disposition = 0;
    if (disposition == 1)
    {
        fs::create_directories(path, ec);
        std::ofstream(path.string() + ".name") << toUtf8(c.displayName);
    }
    if (disposition)
    {
        std::lock_guard guard(g_lock);
        std::string key = root; // root names are case-insensitive
        for (auto& ch : key) ch = char(tolower((unsigned char)ch));
        if (g_open.count(key))
        {
            kernel::removeMount(g_open[key]);
            g_open.erase(key);
        }
        std::string device = "\\Device\\Content\\" + std::to_string(g_nextDevice++);
        kernel::addMount(device, path.string(), true);
        kernel::addSymbolicLink(root + ":", device);
        kernel::addSymbolicLink("\\??\\" + root + ":", device);
        g_open[key] = device;
        info("[xam] content %s: %s \"%s\" as %s: (%s)", disposition == 1 ? "created" : "opened", c.fileName.c_str(),
             toUtf8(c.displayName).c_str(), root.c_str(), path.c_str());
    }
    else
        info("[xam] content create/open of \"%s\" (flags %X) failed: %u", c.fileName.c_str(), flags, result);
    if (pDisposition) write32(pDisposition, disposition);
    if (pLicense && result == ERROR_SUCCESS) write32(pLicense, 0);
    ctx.r3.u64 = kernel::completeOverlapped(overlapped, result, disposition);
}

// u32 XamContentClose(char* RootName, XOVERLAPPED* Overlapped)
GUEST_IMPORT(XamContentClose)
{
    std::string root = guestString(ctx.r3.u32), key = root;
    for (auto& ch : key) ch = char(tolower((unsigned char)ch));
    uint32_t result = ERROR_SUCCESS;
    {
        std::lock_guard guard(g_lock);
        auto it = g_open.find(key);
        if (it == g_open.end())
            result = ERROR_PATH_NOT_FOUND;
        else
        {
            kernel::removeSymbolicLink(root + ":");
            kernel::removeSymbolicLink("\\??\\" + root + ":");
            kernel::removeMount(it->second);
            g_open.erase(it);
            info("[xam] content %s: closed", root.c_str());
        }
    }
    ctx.r3.u64 = kernel::completeOverlapped(ctx.r4.u32, result);
}

// u32 XamContentGetCreator(u32 UserIndex, XCONTENT_DATA* ContentData, BOOL* IsCreator, u64* CreatorXuid,
//                          XOVERLAPPED* Overlapped): the user creates all saves
GUEST_IMPORT(XamContentGetCreator)
{
    ContentData c = ContentData::read(ctx.r4.u32);
    uint32_t pIsCreator = ctx.r5.u32, pXuid = ctx.r6.u32, result = ERROR_SUCCESS;
    if (!exists(c))
        result = ERROR_PATH_NOT_FOUND;
    else
    {
        bool save = c.contentType == kSavedGame;
        if (pIsCreator) write32(pIsCreator, save ? 1 : 0);
        if (pXuid) write64(pXuid, save ? 0xB13EBABEBABEBABEull : 0); // the signed-in profile (see xam.cpp)
    }
    ctx.r3.u64 = kernel::completeOverlapped(ctx.r7.u32, result);
}

// u32 XamContentSetThumbnail(u32 UserIndex, XCONTENT_DATA* ContentData, void* Png, u32 Size, XOVERLAPPED*)
GUEST_IMPORT(XamContentSetThumbnail)
{
    ContentData c = ContentData::read(ctx.r4.u32);
    uint32_t buf = ctx.r5.u32, size = ctx.r6.u32, result = ERROR_SUCCESS;
    fs::path path = packagePath(c);
    if (!exists(c))
        result = ERROR_PATH_NOT_FOUND;
    else
        std::ofstream(path.string() + ".png", std::ios::binary).write((const char*)host(buf), size);
    ctx.r3.u64 = kernel::completeOverlapped(ctx.r7.u32, result);
}

// u32 XamContentGetDeviceData(u32 DeviceId, X_CONTENT_DEVICE_DATA* Data)
// X_CONTENT_DEVICE_DATA (0x50): u32 DeviceId; u32 DeviceType (1 HDD, 4 ODD); u64 TotalBytes;
//                               u64 FreeBytes; u16 Name[28]
GUEST_IMPORT(XamContentGetDeviceData)
{
    uint32_t device = ctx.r3.u32, p = ctx.r4.u32;
    if (device != kHddDeviceId && device != kOddDeviceId) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    bool hdd = device == kHddDeviceId;
    memset(host(p), 0, 0x50);
    write32(p + 0x00, device);
    write32(p + 0x04, hdd ? 1 : 4);
    write64(p + 0x08, (hdd ? 20ull : 7ull) << 30); // Xenia's dummy sizes: 20 GB hard drive, 7 GB disc
    write64(p + 0x10, hdd ? 3ull << 30 : 0);
    const char* name = hdd ? "Dummy HDD" : "Dummy ODD";
    for (uint32_t i = 0; name[i] && i < 27; i++) write16(p + 0x18 + i * 2, uint16_t(name[i]));
    ctx.r3.u64 = ERROR_SUCCESS;
}

// u32 XamContentGetDeviceState(u32 DeviceId, XOVERLAPPED* Overlapped)
GUEST_IMPORT(XamContentGetDeviceState)
{
    uint32_t device = ctx.r3.u32;
    bool known = device == kHddDeviceId || device == kOddDeviceId;
    ctx.r3.u64 = kernel::completeOverlapped(ctx.r4.u32, known ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED);
}

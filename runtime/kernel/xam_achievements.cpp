// Achievements (roadmap phase 11, issue #97): the title's achievement list from its SPA resource,
// unlocks the game reports through the XGI app, kept per machine under game/achievements/, and
// the enumerator the game reads them back with. One local profile (user 0) earns them all.
//
// How an unlock reaches XAM: the game imports no XamUserWriteAchievements. XUserWriteAchievements
// in the XDK sends message 0x000B0008 to the XGI app (0xFB) through XMsgStartIORequest, with
// { u32 count; X_USER_ACHIEVEMENT* list } and X_USER_ACHIEVEMENT = { u32 user_index; u32 id }.
// Layouts and behavior follow Xenia (xenia-canary src/xenia/kernel/xam/xam_msg.cc,
// apps/xgi_app.cc, xam_user.cc XamUserCreateAchievementEnumerator, xenumerator.cc
// XAchievementEnumerator::WriteItems, achievement_manager.h X_ACHIEVEMENT_DETAILS; BSD 3-clause,
// docs/licenses/xenia-LICENSE.txt).
//
// The player's list (the overlay and --achievements) leaves out the online achievements and
// rescales the others to 1000 G (achievement_list.h); the game gets the title's own list.
#include "kernel.h"
#include "import_support.h"
#include "achievement_list.h"
#include "spa.h"
#include "xam_enum.h"
#include "../log.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

using namespace mem;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_NO_MORE_FILES = 0x12;
    constexpr uint32_t ERROR_INVALID_PARAMETER = 0x57;
    constexpr uint32_t XEX_HEADER_RESOURCE_INFO = 0x000002FF;
    constexpr uint32_t XEX_HEADER_EXECUTION_INFO = 0x00040006;
    constexpr uint32_t kAppXgi = 0xFB;
    constexpr uint32_t kMsgSetContext = 0x000B0006;       // XUserSetContext (rich presence)
    constexpr uint32_t kMsgSetProperty = 0x000B0007;      // XUserSetPropertyEx (stats, presence)
    constexpr uint32_t kMsgWriteAchievements = 0x000B0008; // XUserWriteAchievements
    constexpr uint32_t kMsgReadStats = 0x000B0021;        // XUserReadStats (leaderboards)
    constexpr uint32_t X_ONLINE_E_LOGON_NOT_LOGGED_ON = 0x80151802;
    // X_ACHIEVEMENT_DETAILS: u32 id, label*, description*, unachieved*, image id, gamerscore;
    // FILETIME unlock time (2 x u32); u32 flags. Then strings when flags & 7 asked for them.
    constexpr uint32_t kDetailsSize = 36;
    constexpr uint32_t kStringBufferSize = 464;

    std::mutex g_lock;
    bool g_loaded = false;
    uint32_t g_titleId = 0;
    kernel::spa::Info g_spa;
    std::vector<kernel::spa::Achievement> g_shown; // the player's list (achievement_list.h)
    std::string g_dir;                       // game/achievements
    std::map<uint16_t, uint64_t> g_unlocked; // achievement id -> unlock time (unix seconds)
    const uint8_t* g_spaBlob = nullptr;      // the SPA in the loaded image (for its PNG icons)
    std::atomic<uint64_t> g_unlockCount{0};  // new unlocks this run (the overlay's pop-up polls it)
    std::vector<uint16_t> g_unlockOrder;     // their ids, in the order they came (g_lock)

    fs::path storePath()
    {
        char name[16];
        std::snprintf(name, sizeof(name), "%08X.txt", g_titleId);
        return fs::path(g_dir) / name;
    }

    const kernel::spa::Achievement* findAchievement(uint32_t id)
    {
        for (auto& a : g_spa.achievements)
            if (a.id == id) return &a;
        return nullptr;
    }

    // Unlock store: one line per unlock, "<id> <unix seconds>"; '#' starts a comment.
    void loadStore()
    {
        std::ifstream f(storePath());
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            uint32_t id = 0;
            uint64_t when = 0;
            if (in >> id >> when && findAchievement(id))
                g_unlocked[uint16_t(id)] = when;
        }
    }

    void saveStore()
    {
        std::error_code ec;
        fs::create_directories(g_dir, ec);
        fs::path tmp = storePath();
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            f << "# Backbreaker achievements earned on this machine: <id> <unix time>\n";
            for (auto& [id, when] : g_unlocked)
            {
                const auto* a = findAchievement(id);
                f << id << ' ' << when << "  # " << (a ? a->label : "?") << '\n';
            }
        }
        fs::rename(tmp, storePath(), ec); // whole file or the old one, never half a file
        if (ec)
            warn("[xam] could not write %s: %s", storePath().string().c_str(), ec.message().c_str());
    }

    // The SPA resource is named after the title id; its data is in the loaded image.
    void loadLocked()
    {
        if (g_loaded) return;
        g_loaded = true;
        if (uint32_t exec = kernel::xexHeaderField(XEX_HEADER_EXECUTION_INFO))
            g_titleId = read32(exec + 0x0C);
        char want[9];
        std::snprintf(want, sizeof(want), "%08X", g_titleId);
        uint32_t res = kernel::xexHeaderField(XEX_HEADER_RESOURCE_INFO);
        uint32_t count = res ? (read32(res) - 4) / 16 : 0;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t e = res + 4 + i * 16;
            if (std::strncmp((const char*)host(e), want, 8) != 0) continue;
            uint32_t address = read32(e + 8), size = read32(e + 12);
            std::string error;
            g_spaBlob = host(address);
            if (!kernel::spa::parse(host(address), size, g_spa, error))
            {
                warn("[xam] achievement data (%s at %08X) unusable: %s", want, address, error.c_str());
                return;
            }
            loadStore();
            uint32_t total = 0, earned = 0;
            for (auto& a : g_spa.achievements)
            {
                total += a.gamerscore;
                if (g_unlocked.count(a.id)) earned += a.gamerscore;
            }
            info("[xam] achievements: %zu (%u G) for title %08X, %zu unlocked (%u G) in %s",
                 g_spa.achievements.size(), total, g_titleId, g_unlocked.size(), earned, storePath().string().c_str());
            g_shown = kernel::achievement_list::playerList(g_titleId, g_spa.achievements);
            if (kernel::achievement_list::matches(g_titleId, g_spa.achievements))
            {
                uint32_t shownTotal = 0;
                for (auto& a : g_shown) shownTotal += a.gamerscore;
                info("[xam] the player's list: %zu (%u G), without the online ones", g_shown.size(), shownTotal);
            }
            else
                info("[xam] the player's list is the title's own: not the list achievement_list.h was made for");
            return;
        }
        warn("[xam] no achievement data: the XEX has no resource named %s", want);
    }

    void unlock(uint32_t userIndex, uint32_t id)
    {
        std::lock_guard guard(g_lock);
        loadLocked();
        const auto* a = findAchievement(id);
        if (!a)
        {
            warn("[xam] the game reported achievement %u (user %u), which the title's list does not have", id, userIndex);
            return;
        }
        if (g_unlocked.count(uint16_t(id)))
            return; // already earned; the game reports some again
        g_unlocked[uint16_t(id)] = uint64_t(std::time(nullptr));
        g_unlockOrder.push_back(uint16_t(id));
        g_unlockCount.fetch_add(1, std::memory_order_release);
        saveStore();
        info("[xam] achievement unlocked: %s (%u G)", a->label.c_str(), a->gamerscore);
    }

    // FILETIME (100 ns since 1601) of a unix time
    uint64_t fileTime(uint64_t unixSeconds) { return (unixSeconds + 11644473600ull) * 10000000ull; }

    struct AchievementEnumerator : kernel::XamEnumerator
    {
        struct Item { kernel::spa::Achievement a; uint64_t unlockTime; bool unlocked; };
        std::vector<Item> items;
        size_t next = 0;
        uint32_t perCall = 1, flags = 0;

        uint32_t itemSize() const { return kDetailsSize + ((flags & 7) ? kStringBufferSize : 0); }

        uint32_t write(uint32_t buf, uint32_t length, uint32_t& count) override
        {
            count = uint32_t(std::min<size_t>({ items.size() - next, perCall, length / itemSize() }));
            if (!count)
                return ERROR_NO_MORE_FILES;
            // All the per-call details come first, then the strings (Xenia WriteItems).
            uint32_t strings = buf + perCall * kDetailsSize, end = buf + length;
            auto put = [&](const std::string& s) -> uint32_t {
                std::u16string w = kernel::spa::toUtf16(s);
                uint32_t bytes = uint32_t(w.size() + 1) * 2;
                if (strings + bytes > end) return 0;
                uint32_t at = strings;
                for (size_t i = 0; i < w.size(); i++) write16(at + uint32_t(i) * 2, uint16_t(w[i]));
                write16(at + uint32_t(w.size()) * 2, 0);
                strings += bytes;
                return at;
            };
            for (uint32_t i = 0; i < count; i++, next++)
            {
                const Item& it = items[next];
                uint32_t d = buf + i * kDetailsSize;
                write32(d + 0x00, it.a.id);
                write32(d + 0x04, (flags & 1) ? put(it.a.label) : 0);
                write32(d + 0x08, (flags & 2) ? put(it.a.description) : 0);
                write32(d + 0x0C, (flags & 4) ? put(it.a.unachieved) : 0);
                write32(d + 0x10, it.a.imageId);
                write32(d + 0x14, it.a.gamerscore);
                uint64_t ft = it.unlocked ? fileTime(it.unlockTime) : 0;
                write32(d + 0x18, uint32_t(ft >> 32));
                write32(d + 0x1C, uint32_t(ft));
                write32(d + 0x20, it.a.flags | (it.unlocked ? kernel::spa::kAchieved : 0));
            }
            return ERROR_SUCCESS;
        }
    };

    std::set<std::pair<uint32_t, uint32_t>> g_messagesSeen;
    std::mutex g_messagesLock;
}

namespace kernel
{
    void initAchievements(const std::string& dir)
    {
        std::lock_guard guard(g_lock);
        g_dir = dir;
        loadLocked();
    }

    std::vector<AchievementView> achievementList()
    {
        std::lock_guard guard(g_lock);
        loadLocked();
        std::vector<AchievementView> out;
        for (auto& a : g_shown)
        {
            AchievementView v;
            v.id = a.id;
            v.label = a.label;
            v.description = a.description;
            v.unachieved = a.unachieved;
            v.gamerscore = a.gamerscore;
            auto it = g_unlocked.find(a.id);
            v.unlocked = it != g_unlocked.end();
            v.unlockTime = v.unlocked ? it->second : 0;
            v.secret = !(a.flags & spa::kShowUnachieved);
            if (auto img = g_spa.images.find(a.imageId); img != g_spa.images.end() && g_spaBlob)
            {
                v.image = g_spaBlob + img->second.first;
                v.imageSize = img->second.second;
            }
            out.push_back(std::move(v));
        }
        return out;
    }

    uint64_t achievementUnlockCount() { return g_unlockCount.load(std::memory_order_acquire); }

    std::vector<uint16_t> unlockedSince(uint64_t index)
    {
        std::lock_guard guard(g_lock);
        if (index >= g_unlockOrder.size()) return {};
        return std::vector<uint16_t>(g_unlockOrder.begin() + std::ptrdiff_t(index), g_unlockOrder.end());
    }

    // --achievements: the player's list with what is earned, on stdout (the game has no screen for
    // it; on the console it was the Guide's).
    void printAchievements()
    {
        std::lock_guard guard(g_lock);
        loadLocked();
        uint32_t total = 0, earned = 0;
        size_t got = 0;
        for (auto& a : g_shown)
        {
            total += a.gamerscore;
            if (g_unlocked.count(a.id)) { earned += a.gamerscore; got++; }
        }
        std::printf("Backbreaker achievements (title %08X): %zu of %zu unlocked, %u of %u G\n",
                    g_titleId, got, g_shown.size(), earned, total);
        for (auto& a : g_shown)
        {
            auto it = g_unlocked.find(a.id);
            bool got = it != g_unlocked.end();
            char when[32] = "";
            if (got)
            {
                std::time_t t = std::time_t(it->second);
                std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&t));
            }
            bool secret = !got && !(a.flags & spa::kShowUnachieved);
            std::printf("  [%c] %-36s %4u G  %-16s %s\n", got ? 'x' : ' ', secret ? "(secret)" : a.label.c_str(), a.gamerscore,
                        when, secret ? "" : (got ? a.description : a.unachieved).c_str());
        }
        std::printf("Stored in %s\n", storePath().string().c_str());
    }
}

// u32 XMsgStartIORequest(u32 App, u32 Message, XOVERLAPPED* Overlapped, void* Buffer, u32 BufferLength)
// The XDK's asynchronous calls into system apps (Xenia xam_msg.cc). The result goes to the
// overlapped if there is one (the call then returns ERROR_IO_PENDING). The game's wrappers
// (recompiled code: sub_828F7A30 XUserWriteAchievements, sub_828F7AA8/7B38 set context/property,
// sub_828F7C70 read stats, sub_829475A8.. sessions) all go to the XGI app 0xFB. Messages not
// handled here succeed without doing anything, as the import did before it was implemented;
// each message is logged once so it is clear what the game asks for.
GUEST_IMPORT(XMsgStartIORequest)
{
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, overlapped = ctx.r5.u32, buf = ctx.r6.u32, length = ctx.r7.u32;
    uint32_t result = ERROR_SUCCESS;
    const char* what = nullptr;
    if (app == kAppXgi && msg == kMsgWriteAchievements)
    {
        what = "XUserWriteAchievements";
        if (buf)
        {
            uint32_t count = std::min<uint32_t>(read32(buf), 256), list = read32(buf + 4);
            for (uint32_t i = 0; i < count && list; i++)
                unlock(read32(list + i * 8), read32(list + i * 8 + 4));
        }
    }
    else if (app == kAppXgi && (msg == kMsgSetContext || msg == kMsgSetProperty))
        what = msg == kMsgSetContext ? "XUserSetContext, nothing to do offline" : "XUserSetPropertyEx, nothing to do offline";
    else if (app == kAppXgi && msg == kMsgReadStats)
    {
        what = "XUserReadStats: not signed in to LIVE (as Xenia)";
        result = X_ONLINE_E_LOGON_NOT_LOGGED_ON;
    }
    {
        std::lock_guard guard(g_messagesLock);
        if (g_messagesSeen.insert({ app, msg }).second)
            info("[xam] XMsgStartIORequest app %02X message %08X (%u bytes): %s", app, msg, length,
                 what ? what : "not handled, reported as done");
    }
    ctx.r3.u64 = kernel::completeOverlapped(overlapped, result);
}

// u32 XamUserCreateAchievementEnumerator(u32 TitleId, u32 UserIndex, u64 Xuid, u32 DetailFlags,
//                                        u32 StartingIndex, u32 Count, u32* BufferSize, HANDLE* Handle)
GUEST_IMPORT(XamUserCreateAchievementEnumerator)
{
    uint32_t titleId = ctx.r3.u32, userIndex = ctx.r4.u32, flags = ctx.r6.u32, offset = ctx.r7.u32,
             count = ctx.r8.u32, pSize = ctx.r9.u32, pHandle = ctx.r10.u32;
    if (!count || !pSize || !pHandle || userIndex >= 4)
    {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    auto* e = new AchievementEnumerator;
    e->perCall = count;
    e->flags = flags;
    write32(pSize, e->itemSize() * count);
    {
        std::lock_guard guard(g_lock);
        loadLocked();
        if (!titleId || titleId == g_titleId)
            for (auto& a : g_spa.achievements)
            {
                auto it = g_unlocked.find(a.id);
                e->items.push_back({ a, it == g_unlocked.end() ? 0 : it->second, it != g_unlocked.end() });
            }
    }
    e->next = std::min<size_t>(offset, e->items.size());
    write32(pHandle, kernel::registerObject(e));
    info("[xam] achievement enumerator: title %08X, user %u, flags %X, from %u, %u per call: %zu achievement(s)",
         titleId, userIndex, flags, offset, count, e->items.size());
    ctx.r3.u64 = ERROR_SUCCESS;
}

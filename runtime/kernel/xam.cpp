// XAM: system notifications and the signed-in user. One local profile ("Player") is signed in
// on controller 0; no Xbox LIVE. Behavior follows Xenia's xam_notify.cc / xam_user.cc.
#include "kernel.h"
#include "objects.h"
#include "import_support.h"
#include "../log.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>

using namespace mem;

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_NO_SUCH_USER = 0x525;

    constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull; // Xenia's default local XUID
    constexpr const char* kGamertag = "Player";

    // Notification IDs
    constexpr uint32_t XN_SYS_UI = 0x00000009;
    constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;

    struct Listener : kernel::Event
    {
        uint64_t mask;
        std::deque<std::pair<uint32_t, uint32_t>> queue; // (id, param)
        Listener(uint64_t m) : kernel::Event(true, false), mask(m) {}
    };

    bool g_startupNotified = false;

    void enqueue(Listener* l, uint32_t id, uint32_t param)
    {
        std::lock_guard lock(kernel::g_dispatchLock);
        l->queue.emplace_back(id, param);
        l->signaled = true;
        kernel::signalObject(l);
    }
}

// HANDLE XamNotifyCreateListener(u64 Mask, u32 MaxVersion)
GUEST_IMPORT(XamNotifyCreateListener)
{
    auto* l = new Listener(ctx.r3.u64);
    uint32_t handle = kernel::registerObject(l);
    info("[xam] notification listener %08X (mask %llX)", handle, (unsigned long long)l->mask);
    // Games expect a few notifications on startup, delivered to the first system listener.
    if (!g_startupNotified && (l->mask & 1))
    {
        g_startupNotified = true;
        enqueue(l, XN_SYS_UI, 1);
        enqueue(l, XN_SYS_UI, 0);
        enqueue(l, XN_SYS_SIGNINCHANGED, 1);
    }
    ctx.r3.u64 = handle;
}

// BOOL XNotifyGetNext(HANDLE Listener, u32 MatchId, u32* Id, u32* Param)
GUEST_IMPORT(XNotifyGetNext)
{
    uint32_t pId = ctx.r5.u32, pParam = ctx.r6.u32, match = ctx.r4.u32;
    auto* o = kernel::objectFromHandle(ctx.r3.u32);
    auto* l = dynamic_cast<Listener*>(o);
    if (!l) { ctx.r3.u64 = 0; return; }
    std::lock_guard lock(kernel::g_dispatchLock);
    for (auto it = l->queue.begin(); it != l->queue.end(); ++it)
    {
        if (match && it->first != match) continue;
        if (pId) write32(pId, it->first);
        if (pParam) write32(pParam, it->second);
        l->queue.erase(it);
        l->signaled = !l->queue.empty();
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}

namespace
{
    constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
    constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
    constexpr uint32_t ERROR_IO_PENDING = 997;

}

namespace kernel
{
    // XOVERLAPPED { u32 Result; u32 Length; u32 Context; HANDLE Event; void* Routine; u32 RoutineCtx; u32 ExtendedError; }
    uint32_t completeOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
    {
        if (!overlapped)
            return result;
        write32(overlapped + 0x00, result);
        write32(overlapped + 0x04, length);
        write32(overlapped + 0x18, result);
        if (uint32_t ev = read32(overlapped + 0x0C))
            if (auto* o = kernel::objectFromHandle(ev); o && o->type == kernel::Object::Type::Event)
                kernel::setEvent(static_cast<kernel::Event*>(o));
        return ERROR_IO_PENDING;
    }
}

// ---- Profile settings (issue #98) ----
// The game reads 3 dashboard settings (title FFFE07D1: the voice options) and 29 of its own (title
// 0) when a profile signs in: gamercard fields, the controller and gamer preferences, and its three
// 1000-byte title-specific blobs. It never writes any: it imports no XamUserWriteProfileSettings
// and sends no write message (it keeps its options in its own "Profile Options" save, BBOPT15.SAV).
// So the answer is the console's defaults for a fresh offline profile, as Xenia gives them
// (xenia-canary src/xenia/kernel/xam/user_settings.h default_setting_values / GetDefaultSetting,
// xam_user.cc XamUserReadProfileSettingsEx): source DEFAULT, value 0 or empty, except controller
// vibration = 3 (on) and the gamer picture key. Settings Xenia does not know keep source 0 (no value).
namespace
{
    constexpr uint32_t kSourceNoValue = 0, kSourceDefault = 1;

    struct KnownSetting { uint32_t id; const char* name; int32_t value; };
    // Setting ids are type << 28 | max size << 16 | number (Xenia user_settings.h SettingKey).
    constexpr KnownSetting kKnownSettings[] = {
        { 0x10040002, "GAMER_YAXIS_INVERSION", 0 },
        { 0x10040003, "OPTION_CONTROLLER_VIBRATION", 3 }, // 3 = on
        { 0x10040004, "GAMERCARD_ZONE", 0 },
        { 0x10040005, "GAMERCARD_REGION", 0 },
        { 0x10040006, "GAMERCARD_CRED", 0 },
        { 0x5004000B, "GAMERCARD_REP", 0 },
        { 0x1004000C, "OPTION_VOICE_MUTED", 0 },
        { 0x1004000D, "OPTION_VOICE_THRU_SPEAKERS", 0 },
        { 0x1004000E, "OPTION_VOICE_VOLUME", 0 },
        { 0x4064000F, "GAMERCARD_PICTURE_KEY", 0 },
        { 0x402C0011, "GAMERCARD_MOTTO", 0 },
        { 0x10040012, "GAMERCARD_TITLES_PLAYED", 0 },
        { 0x10040013, "GAMERCARD_ACHIEVEMENTS_EARNED", 0 },
        { 0x10040015, "GAMER_DIFFICULTY", 0 },
        { 0x10040018, "GAMER_CONTROL_SENSITIVITY", 0 },
        { 0x1004001D, "GAMER_PREFERRED_COLOR_FIRST", 0 },
        { 0x1004001E, "GAMER_PREFERRED_COLOR_SECOND", 0 },
        { 0x10040022, "GAMER_ACTION_AUTO_AIM", 0 },
        { 0x10040023, "GAMER_ACTION_AUTO_CENTER", 0 },
        { 0x10040024, "GAMER_ACTION_MOVEMENT_CONTROL", 0 },
        { 0x10040026, "GAMER_RACE_TRANSMISSION", 0 },
        { 0x10040027, "GAMER_RACE_CAMERA_LOCATION", 0 },
        { 0x10040028, "GAMER_RACE_BRAKE_CONTROL", 0 },
        { 0x10040029, "GAMER_RACE_ACCELERATOR_CONTROL", 0 },
        { 0x10040038, "GAMERCARD_TITLE_CRED_EARNED", 0 },
        { 0x10040039, "GAMERCARD_TITLE_ACHIEVEMENTS_EARNED", 0 },
        { 0x63E83FFF, "TITLE_SPECIFIC1", 0 },
        { 0x63E83FFE, "TITLE_SPECIFIC2", 0 },
        { 0x63E83FFD, "TITLE_SPECIFIC3", 0 },
    };
    constexpr const char16_t* kDefaultPictureKey = u"FFFE07D10002000200010002"; // Xenia default_setting_values

    const KnownSetting* knownSetting(uint32_t id)
    {
        for (auto& k : kKnownSettings)
            if (k.id == id) return &k;
        return nullptr;
    }

    // Xenia UserSetting::is_setting_valid: a standard id (number below 0x58) or a title-specific one
    bool settingValid(uint32_t id) { return (id & 0xFFFF) < 0x58 || (id & 0x3F00) == 0x3F00 || knownSetting(id); }

    std::mutex g_settingsLogLock;
    std::set<std::string> g_settingsLogged;
}

// u32 XamUserReadProfileSettings(u32 TitleId, u32 UserIndex, u32 XuidCount, u64* Xuids, u32 SettingCount,
//                                u32* SettingIds, u32* BufferSize, X_USER_READ_PROFILE_SETTINGS* Buffer, XOVERLAPPED*)
// X_USER_READ_PROFILE_SETTINGS { u32 count; X_USER_PROFILE_SETTING* settings }, then the settings:
// X_USER_PROFILE_SETTING (0x28) { u32 source; u64 xuid (or u32 user index); u32 id; X_USER_DATA
// { u8 type; ...; union at +8: i32 value / { u32 size; u32 pointer } for strings and blobs } },
// then the string/blob space (Xenia user_profile.h, user_data.h).
GUEST_IMPORT(XamUserReadProfileSettings)
{
    uint32_t titleId = ctx.r3.u32, userIndex = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32;
    uint32_t count = ctx.r7.u32, ids = ctx.r8.u32, pSize = ctx.r9.u32, buf = ctx.r10.u32;
    uint32_t overlapped = stackArg(ctx, 8);
    uint32_t users = xuidCount ? xuidCount : 1;

    uint32_t extra = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = read32(ids + i * 4), type = id >> 28;
        if (type == 4 || type == 6) extra += (((id >> 16) & 0xFFF) + 3) & ~3u;
    }
    uint32_t needed = 8 + users * count * 0x28 + users * extra;
    uint32_t have = pSize ? read32(pSize) : 0;
    if (!buf || have < needed)
    {
        if (!pSize) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return; }
        write32(pSize, needed);
        ctx.r3.u64 = ERROR_INSUFFICIENT_BUFFER;
        return;
    }

    memset(host(buf), 0, needed);
    uint32_t settings = buf + 8, data = settings + users * count * 0x28;
    write32(buf + 0, users * count);
    write32(buf + 4, settings);
    for (uint32_t u = 0; u < users; u++)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t s = settings + (u * count + i) * 0x28;
            uint32_t id = read32(ids + i * 4), type = id >> 28;
            const KnownSetting* known = knownSetting(id);
            write32(s + 0x00, settingValid(id) ? kSourceDefault : kSourceNoValue);
            if (xuidCount) write64(s + 0x08, read64(xuids + u * 8));
            else write32(s + 0x08, userIndex);
            write32(s + 0x10, id);
            write8(s + 0x18, uint8_t(type));
            if (type == 4 || type == 6)
            {
                uint32_t size = (id >> 16) & 0xFFF, used = 0;
                if (id == 0x4064000F) // the picture key: UTF-16BE with its terminator
                {
                    std::u16string key = kDefaultPictureKey;
                    for (size_t c = 0; c <= key.size(); c++)
                        write16(data + uint32_t(c) * 2, c < key.size() ? uint16_t(key[c]) : 0);
                    used = uint32_t(key.size() + 1) * 2;
                }
                write32(s + 0x20, used); // length in use
                write32(s + 0x24, data); // pointer (to zeroed space when empty)
                data += (size + 3) & ~3u;
            }
            else if (type == 1 && known)
                write32(s + 0x20, uint32_t(known->value));
        }
    }

    // Log each distinct request once, with names.
    std::string list;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = read32(ids + i * 4);
        const KnownSetting* known = knownSetting(id);
        char one[64];
        if (known) std::snprintf(one, sizeof(one), " %s", known->name);
        else std::snprintf(one, sizeof(one), " %08X%s", id, settingValid(id) ? "" : "(unknown)");
        list += one;
    }
    char head[64];
    std::snprintf(head, sizeof(head), "title %08X, %u setting(s):", titleId, count);
    {
        std::lock_guard guard(g_settingsLogLock);
        if (g_settingsLogged.insert(head + list).second)
            info("[xam] profile settings read, defaults returned, %s%s", head, list.c_str());
    }
    ctx.r3.u64 = kernel::completeOverlapped(overlapped, ERROR_SUCCESS, needed);
}

// ---- Users ----
// u32 XamUserGetSigninState(u32 UserIndex): 0 = not signed in, 1 = local, 2 = LIVE
GUEST_IMPORT(XamUserGetSigninState) { ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0; }

// u32 XamUserGetXUID(u32 UserIndex, u32 Type, u64* Xuid)
GUEST_IMPORT(XamUserGetXUID)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    write64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// u32 XamUserGetName(u32 UserIndex, char* Buffer, u32 Size)
GUEST_IMPORT(XamUserGetName)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    uint32_t buf = ctx.r4.u32, size = ctx.r5.u32;
    size_t n = std::min<size_t>(strlen(kGamertag), size ? size - 1 : 0);
    memcpy(host(buf), kGamertag, n);
    if (size) host(buf)[n] = 0;
    ctx.r3.u64 = ERROR_SUCCESS;
}

// u32 XamUserCheckPrivilege(u32 UserIndex, u32 Privilege, BOOL* Result)
GUEST_IMPORT(XamUserCheckPrivilege)
{
    write32(ctx.r5.u32, 1);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// u32 XamUserGetMembershipTierFromXUID(u64 Xuid) -> 0 = silver (offline profile)
GUEST_IMPORT(XamUserGetMembershipTierFromXUID) { ctx.r3.u64 = 0; }
GUEST_IMPORT(XamUserGetOnlineCountryFromXUID) { ctx.r3.u64 = 103; } // US
GUEST_IMPORT(XamUserAreUsersFriends) { if (ctx.r7.u32) write32(ctx.r7.u32, 0); ctx.r3.u64 = ERROR_SUCCESS; }

// u32 XamGetExecutionId(XEX_EXECUTION_ID** Info) -> the XEX's own execution info header
GUEST_IMPORT(XamGetExecutionId)
{
    uint32_t info = kernel::xexHeaderField(0x00040006); // XEX_HEADER_EXECUTION_INFO
    write32(ctx.r3.u32, info);
    ctx.r3.u64 = info ? 0 : 0x80004005;
}

// ---- Storage devices (behavior follows Xenia: one dummy hard drive, id 1) ----
namespace
{
    constexpr uint32_t kDummyDeviceId = 1;
}

// u32 XamShowDeviceSelectorUI(u32 UserIndex, u32 ContentType, u32 ContentFlags, u64 TotalRequested,
//                             u32* DeviceId, XOVERLAPPED* Overlapped)
GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    uint32_t pDevice = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (pDevice) write32(pDevice, kDummyDeviceId);
    info("[xam] device selector: picked the hard drive (device %u)", kDummyDeviceId);
    ctx.r3.u64 = kernel::completeOverlapped(overlapped, ERROR_SUCCESS);
}

// ---- In-process XMsg calls to system apps ----
// u32 XMsgInProcessCall(u32 App, u32 Message, void* Buffer, u32 BufferLength)
GUEST_IMPORT(XMsgInProcessCall)
{
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, buf = ctx.r5.u32;
    if (app == 0xFA && msg == 0x0007001B)
    {
        // XMP (music player) XMPGetPlaybackController { u32 client; u32* controller; u32* locked }:
        // the title controls playback, not the user's own music
        if (uint32_t p = read32(buf + 4)) write32(p, 0);
        if (uint32_t p = read32(buf + 8)) write32(p, 0);
        ctx.r3.u64 = 0;
        return;
    }
    warn("XMsgInProcessCall(app %02X, message %08X) not implemented", app, msg);
    ctx.r3.u64 = 0x80004005; // E_FAIL
}

// ---- Misc kernel bits the game touches during init ----
// void KeInitializeDpc(KDPC* Dpc, void* Routine, void* Context)
GUEST_IMPORT(KeInitializeDpc)
{
    uint32_t dpc = ctx.r3.u32;
    write16(dpc + 0x00, 19); // DpcObject
    write8(dpc + 0x02, 0);
    write8(dpc + 0x03, 0);
    write32(dpc + 0x04, 0);
    write32(dpc + 0x08, 0);
    write32(dpc + 0x0C, ctx.r4.u32); // DeferredRoutine
    write32(dpc + 0x10, ctx.r5.u32); // DeferredContext
}

GUEST_IMPORT(KiApcNormalRoutineNop) {}
GUEST_IMPORT(XMsgCancelIORequest) { ctx.r3.u64 = 0; }

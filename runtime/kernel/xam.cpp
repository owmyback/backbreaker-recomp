// XAM: system notifications and the signed-in user. One local profile ("Player") is signed in
// on controller 0; no Xbox LIVE. Behavior follows Xenia's xam_notify.cc / xam_user.cc.
#include "objects.h"
#include "import_support.h"
#include "../log.h"

#include <cstring>
#include <deque>

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
        kernel::signalObjectsChanged();
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

    // XOVERLAPPED { u32 Result; u32 Length; u32 Context; HANDLE Event; void* Routine; u32 RoutineCtx; u32 ExtendedError; }
    uint32_t completeOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0)
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

// u32 XamUserReadProfileSettings(u32 TitleId, u32 UserIndex, u32 XuidCount, u64* Xuids, u32 SettingCount,
//                                u32* SettingIds, u32* BufferSize, X_USER_READ_PROFILE_SETTINGS* Buffer, XOVERLAPPED*)
// Settings are returned typed but empty (we have no stored profile), like Xenia does for unknown IDs.
GUEST_IMPORT(XamUserReadProfileSettings)
{
    uint32_t titleId = ctx.r3.u32, userIndex = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32;
    uint32_t count = ctx.r7.u32, ids = ctx.r8.u32, pSize = ctx.r9.u32, buf = ctx.r10.u32;
    uint32_t overlapped = stackArg(ctx, 8);
    uint32_t users = xuidCount ? xuidCount : 1;

    // X_USER_READ_PROFILE_SETTINGS (8) + X_USER_PROFILE_SETTING (0x28) per setting per user + string/binary data
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
            write32(s + 0x00, 0); // from: not set
            if (xuidCount) write64(s + 0x08, read64(xuids + u * 8));
            else write32(s + 0x08, userIndex);
            write32(s + 0x10, id);
            write8(s + 0x18, uint8_t(type));
            if (type == 4 || type == 6)
            {
                uint32_t size = (id >> 16) & 0xFFF;
                write32(s + 0x20, 0);    // length in use
                write32(s + 0x24, data); // pointer
                data += (size + 3) & ~3u;
            }
        }
    }
    info("[xam] read %u profile settings for title %08X", count, titleId);
    ctx.r3.u64 = completeOverlapped(overlapped, ERROR_SUCCESS, needed);
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

// Networking (NetDll_*): the game runs offline. Startup succeeds, there is no Ethernet link, DNS
// lookups fail and sockets exist but never connect or carry data, so the game's online code gives up
// cleanly instead of crashing on unimplemented imports. Signatures, structure layouts and behaviour
// follow Xenia's xam_net.cc and xsocket.h (BSD; docs/licenses/xenia-LICENSE.txt), except wherever a
// host socket would need a network: no real socket is ever opened here.
#include "kernel.h"
#include "objects.h"
#include "import_support.h"
#include "../log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace mem;

namespace
{
    // Last error reported to the guest, per host thread (NetDll_WSAGetLastError reads it). Every
    // guest thread is its own host thread, and the game reads the error straight after the failing
    // call on the same thread, which is what makes thread_local enough here.
    thread_local uint32_t t_lastError = 0;
}

// int XNetStartup(u32 Caller, XNetStartupParams* Params)
GUEST_IMPORT(NetDll_XNetStartup)
{
    info("[net] XNetStartup: offline (no network)");
    ctx.r3.u64 = 0;
}

// int XNetCleanup(u32 Caller)
GUEST_IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }

// int WSAStartup(u32 Caller, u16 Version, X_WSADATA* Data)
// X_WSADATA { u16 version; u16 version_high; char description[257]; char system_status[129];
//             u16 max_sockets (0x186); u16 max_udpdg (0x188); u32 vendor_info_ptr }
// Values as Xenia's non-Windows path: the requested version, empty strings, 100 sockets, 1024 bytes.
GUEST_IMPORT(NetDll_WSAStartup)
{
    uint32_t version = ctx.r4.u32 & 0xFFFF, data = ctx.r5.u32;
    if (data)
    {
        write16(data + 0x000, uint16_t(version));
        write8(data + 0x004, 0);   // description
        write8(data + 0x105, 0);   // system_status
        write16(data + 0x186, 100);  // max_sockets
        write16(data + 0x188, 1024); // max_udpdg
    }
    ctx.r3.u64 = 0;
}

// int WSACleanup(u32 Caller)
GUEST_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }

// int WSAGetLastError()
GUEST_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = t_lastError; }

// WSAEVENT WSACreateEvent(): a manual-reset event, not signaled
GUEST_IMPORT(NetDll_WSACreateEvent) { ctx.r3.u64 = kernel::registerObject(new kernel::Event(true, false)); }

// u32 XNetGetEthernetLinkStatus(u32 Caller): 0 = no link
GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; }

// u32 XNetGetTitleXnAddr(u32 Caller, XNADDR* Address)
// XNADDR { u32 ina; u32 inaOnline; u16 wPortOnline; u8 abEnet[6]; u8 abOnline[20] }. As Xenia: the
// loopback address, a 0xCC MAC (some network libraries reject an all-zero one), XNET_GET_XNADDR_STATIC.
GUEST_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    uint32_t addr = ctx.r4.u32;
    if (addr)
    {
        write32(addr + 0x00, 0x7F000001); // INADDR_LOOPBACK
        write32(addr + 0x04, 0);
        write16(addr + 0x08, 0);
        std::memset(host(addr + 0x0A), 0xCC, 6);
        std::memset(host(addr + 0x10), 0, 20);
    }
    ctx.r3.u64 = 0x04; // XNET_GET_XNADDR_STATIC
}

// int XNetDnsLookup(u32 Caller, const char* Host, WSAEVENT Event, XNDNS** Result)
// XNDNS { s32 status; u32 cina; in_addr aina[8] }: the lookup completes at once with a non-zero
// (error) status and no addresses, and the event is signaled, as in Xenia.
GUEST_IMPORT(NetDll_XNetDnsLookup)
{
    uint32_t hostName = ctx.r4.u32, event = ctx.r5.u32, result = ctx.r6.u32;
    info("[net] DNS lookup of \"%s\": no network", hostName ? (const char*)host(hostName) : "");
    if (result)
    {
        uint32_t dns = virtualAlloc(40, 16); // zeroed
        write32(dns + 0, 1);                 // status: non-zero = error
        write32(result, dns);
    }
    if (event)
    {
        if (auto* o = kernel::objectFromHandle(event); o && o->type == kernel::Object::Type::Event)
            kernel::setEvent(static_cast<kernel::Event*>(o));
        else
            warn("[net] XNetDnsLookup: %08X is not an event", event);
    }
    ctx.r3.u64 = 0;
}

// int XNetDnsRelease(u32 Caller, XNDNS* Dns)
GUEST_IMPORT(NetDll_XNetDnsRelease)
{
    uint32_t dns = ctx.r4.u32;
    if (!dns) { ctx.r3.u64 = 0xC000000D; return; } // STATUS_INVALID_PARAMETER, as Xenia
    virtualFree(dns);
    ctx.r3.u64 = 0;
}

// ---- sockets -------------------------------------------------------------------------
//
// The game uses sockets in three places: a UDP socket it binds at startup (0x8266AFF8), a TCP
// socket that connects to "localhost" when a match loads (0x8243B148), and a TCP socket it binds
// and listens on (0x825ACB8C). Creating them has to succeed - with socket() failing the game crashes
// at startup - so they are real kernel objects whose handle means something, and everything that
// would need a network fails: connect is refused, receives would block, sends are dropped.
namespace
{
    // Winsock error codes, reported through t_lastError. Same numbers Xenia hands to
    // XThread::SetLastError, and the game compares against them (it checks for 10035 after a
    // failed receive at 0x82665B4C).
    constexpr uint32_t WSAEWOULDBLOCK = 10035, WSAECONNREFUSED = 10061, WSAENOTSOCK = 10038;

    // ioctlsocket commands. The title uses the BSD encoding (0x8004667E); 0x8004667B is the
    // Windows value Xenia's headers define. Both set non-blocking mode.
    constexpr uint32_t FIONBIO = 0x8004667E, FIONBIO_WIN = 0x8004667B;

    // select never waits longer than this, so a polling loop sleeps instead of spinning a core.
    constexpr uint32_t kSelectCapMs = 100;

    struct Socket : kernel::Object
    {
        uint32_t family, type, protocol;    // as passed to socket()
        bool nonBlocking = false;           // ioctlsocket(FIONBIO)
        bool listening = false;             // listen()
        uint32_t boundAddr = 0, boundPort = 0; // network order, exactly as bind() got them
        Socket(uint32_t f, uint32_t t, uint32_t p)
            : Object(Type::Socket), family(f), type(t), protocol(p) {}
    };

    // Guards the socket fields and the log bookkeeping below. Separate from the dispatcher lock in
    // objects.cpp; it is never held across a kernel:: call that takes that one. It is not
    // recursive: take it in a tight scope around the field access, and call shouldLog/logAddress
    // after releasing it.
    std::mutex g_netLock;
    std::map<std::string, uint64_t> g_callCounts;

    // Some of these are called every frame: log the first call, then every 1024th with the running
    // total. countOut always gets the call number.
    bool shouldLog(const char* what, uint64_t& countOut)
    {
        std::lock_guard guard(g_netLock);
        uint64_t& n = g_callCounts[what];
        n++;
        countOut = n;
        return n == 1 || (n & 0x3FF) == 0;
    }

    // Resolve the handle and check it is a live socket; anything else sets WSAENOTSOCK. The handle
    // stays valid until closesocket, which drops it from the kernel handle table.
    Socket* socketFromHandle(uint32_t handle)
    {
        kernel::Object* o = kernel::objectFromHandle(handle);
        if (!o || o->type != kernel::Object::Type::Socket)
        {
            t_lastError = WSAENOTSOCK;
            return nullptr;
        }
        return static_cast<Socket*>(o);
    }

    // Same question without the side effect: select checks its fd_sets for information only, and
    // must not leave WSAENOTSOCK behind for a later NetDll_WSAGetLastError.
    bool isSocket(uint32_t handle)
    {
        kernel::Object* o = kernel::objectFromHandle(handle);
        return o && o->type == kernel::Object::Type::Socket;
    }

    const char* sockTypeName(uint32_t t) { return t == 1 ? "STREAM" : t == 2 ? "DGRAM" : "raw"; }

    // "a.b.c.d" -> 0xAABBCCDD. The guest keeps addresses in network order (XNetGetTitleXnAddr
    // writes 0x7F000001 for loopback), so the parsed value is returned as-is.
    uint32_t parseIPv4(const char* s)
    {
        if (!s || !*s)
            return 0; // hardware inet_addr gives 0 for an empty string, not -1 (as Xenia noted)
        uint32_t value = 0, parts = 0;
        while (parts < 4)
        {
            char* end = nullptr;
            unsigned long part = std::strtoul(s, &end, 10);
            if (end == s || part > 255)
                return 0xFFFFFFFF; // INADDR_NONE
            value = (value << 8) | uint32_t(part);
            parts++;
            if (!*end)
                break;
            if (*end != '.')
                return 0xFFFFFFFF;
            s = end + 1;
        }
        return parts == 4 ? value : 0xFFFFFFFF; // strict dotted quad; no "1.2" shorthand
    }

    // Log the guest's XSOCKADDR_IN { be16 family; be16 port; be32 addr; char zero[8] }.
    void logAddress(const char* what, uint32_t name)
    {
        if (!name) { info("[net] %s: null address", what); return; }
        uint32_t addr = read32(name + 4), port = read16(name + 2);
        info("[net] %s: family %u, %u.%u.%u.%u:%u", what, read16(name),
             addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, port);
    }

    // fd_set as the guest lays it out (Xenia's x_fd_set): { be32 fd_count; be32 fd_array[64] }.
    // Returns the number of entries to look at, clamped the way Xenia clamps it.
    uint32_t fdSetCount(uint32_t set)
    {
        if (!set) return 0;
        return read32(set) > 64 ? 64 : read32(set);
    }
}

// int socket(u32 Caller, u32 Af, u32 Type, u32 Protocol): a new kernel object, so every socket gets
// a distinct handle. Creation never fails offline - the title crashes if it does.
GUEST_IMPORT(NetDll_socket)
{
    uint32_t family = ctx.r4.u32, type = ctx.r5.u32, protocol = ctx.r6.u32;
    uint32_t handle = kernel::registerObject(new Socket(family, type, protocol));
    info("[net] socket(family %u, type %s, protocol %u) -> handle %08X (no network: it will never connect)",
         family, sockTypeName(type), protocol, handle);
    ctx.r3.u64 = handle;
}

// int closesocket(u32 Caller, u32 S): releases the handle; later calls with it give WSAENOTSOCK.
GUEST_IMPORT(NetDll_closesocket)
{
    uint32_t handle = ctx.r4.u32;
    if (!socketFromHandle(handle)) { ctx.r3.s64 = -1; return; }
    kernel::closeHandle(handle);
    if (uint64_t n; shouldLog("closesocket", n))
        info("[net] closesocket(%08X)", handle);
    ctx.r3.u64 = 0;
}

// int shutdown(u32 Caller, u32 S, int How)
GUEST_IMPORT(NetDll_shutdown)
{
    uint32_t handle = ctx.r4.u32;
    Socket* s = socketFromHandle(handle);
    if (!s) { ctx.r3.s64 = -1; return; }
    {
        std::lock_guard guard(g_netLock);
        s->listening = false;
    }
    ctx.r3.u64 = 0;
    if (uint64_t n; shouldLog("shutdown", n))
        info("[net] shutdown(%08X, how %d)", handle, int(ctx.r5.u32));
}

// int setsockopt(u32 Caller, u32 S, u32 Level, u32 OptName, void* OptVal, u32 OptLen):
// accepted and remembered where it matters (see ioctlsocket for non-blocking mode). The game asks
// for SOL_SOCKET/0xFFFF (SO_BROADCAST, from 0x8266B02C and 0x825ACBAC) among others; with no
// network there is nothing to apply them to.
GUEST_IMPORT(NetDll_setsockopt)
{
    Socket* s = socketFromHandle(ctx.r4.u32);
    if (!s) { ctx.r3.s64 = -1; return; }
    ctx.r3.u64 = 0;
    if (uint64_t n; shouldLog("setsockopt", n))
        info("[net] setsockopt(level %u, option %04X, %u bytes) accepted (nothing to configure: no network)",
             ctx.r5.u32, ctx.r6.u32, ctx.r8.u32);
}

// int ioctlsocket(u32 Caller, u32 S, u32 Cmd, u32* Arg): FIONBIO is the one the game uses, twice,
// right after creating a socket (from 0x8266B07C and 0x8243B1A4).
GUEST_IMPORT(NetDll_ioctlsocket)
{
    uint32_t handle = ctx.r4.u32, cmd = ctx.r5.u32, arg = ctx.r6.u32;
    Socket* s = socketFromHandle(handle);
    if (!s) { ctx.r3.s64 = -1; return; }
    if (cmd == FIONBIO || cmd == FIONBIO_WIN)
    {
        uint32_t on = arg ? read32(arg) : 0;
        {
            std::lock_guard guard(g_netLock);
            s->nonBlocking = on != 0;
        }
        ctx.r3.u64 = 0;
        if (uint64_t n; shouldLog("ioctlsocket", n))
            info("[net] ioctlsocket(%08X, FIONBIO): %s", handle, s->nonBlocking ? "non-blocking" : "blocking");
        return;
    }
    ctx.r3.u64 = 0;
    if (uint64_t n; shouldLog("ioctlsocket?", n))
        warn("[net] ioctlsocket(%08X, command %08X): not FIONBIO, ignored", handle, cmd);
}

// int bind(u32 Caller, u32 S, XSOCKADDR_IN* Name, u32 NameLen): recorded, never given to a host
// stack (XSOCKADDR_IN is Xenia's layout: be16 family, be16 port, be32 address, 8 zero bytes).
GUEST_IMPORT(NetDll_bind)
{
    uint32_t handle = ctx.r4.u32, name = ctx.r5.u32;
    Socket* s = socketFromHandle(handle);
    if (!s) { ctx.r3.s64 = -1; return; }
    if (name)
    {
        std::lock_guard guard(g_netLock);
        s->boundPort = read16(name + 2);
        s->boundAddr = read32(name + 4);
    }
    ctx.r3.u64 = 0;
    if (uint64_t n; shouldLog("bind", n)) logAddress("bind", name);
}

// int connect(u32 Caller, u32 S, XSOCKADDR* Name, u32 NameLen): refused. The game's match-load path
// (0x8243B148) connects to "localhost" and, on SOCKET_ERROR here, closes the socket and moves on.
GUEST_IMPORT(NetDll_connect)
{
    Socket* s = socketFromHandle(ctx.r4.u32);
    if (!s) { ctx.r3.s64 = -1; return; }
    if (uint64_t n; shouldLog("connect", n)) logAddress("connect (refused: no network)", ctx.r5.u32);
    t_lastError = WSAECONNREFUSED;
    ctx.r3.s64 = -1;
}

// int listen(u32 Caller, u32 S, int Backlog): succeeds; nothing will ever arrive to accept.
GUEST_IMPORT(NetDll_listen)
{
    uint32_t handle = ctx.r4.u32;
    Socket* s = socketFromHandle(handle);
    if (!s) { ctx.r3.s64 = -1; return; }
    {
        std::lock_guard guard(g_netLock);
        s->listening = true;
    }
    ctx.r3.u64 = 0;
    if (uint64_t n; shouldLog("listen", n))
        info("[net] listen(%08X, backlog %d): listening, but nothing can connect (no network)", handle, int(ctx.r5.u32));
}

// int accept(u32 Caller, u32 S, XSOCKADDR* Addr, u32* AddrLen): would block - no connection is
// pending, and none can become one. Never blocks a guest thread.
GUEST_IMPORT(NetDll_accept)
{
    Socket* s = socketFromHandle(ctx.r4.u32);
    if (!s) { ctx.r3.s64 = -1; return; }
    t_lastError = WSAEWOULDBLOCK;
    if (uint64_t n; shouldLog("accept", n))
        info("[net] accept(%08X): no connection pending -> WSAEWOULDBLOCK", ctx.r4.u32);
    ctx.r3.s64 = -1;
}

// int select(u32 Caller, int Nfds, fd_set* Read, fd_set* Write, fd_set* Except, timeval* Timeout):
// no socket is ever ready. The sets come back empty (fd_count 0) after waiting the given timeout,
// capped at 100 ms, so the game's polling loop sleeps instead of spinning a core. A handle in a set
// that is not a live socket is skipped rather than failing the whole call, so a stale fd_set cannot
// wedge the loop.
GUEST_IMPORT(NetDll_select)
{
    uint32_t sets[3] = { ctx.r5.u32, ctx.r6.u32, ctx.r7.u32 };
    uint32_t timeout = ctx.r8.u32;

    uint64_t ms = timeout ? uint64_t(read32(timeout)) * 1000 + read32(timeout + 4) / 1000 : kSelectCapMs;
    if (ms > kSelectCapMs) ms = kSelectCapMs;

    for (uint32_t set : sets)
        for (uint32_t i = 0, n = fdSetCount(set); i < n; i++)
        {
            uint32_t handle = read32(set + 4 + i * 4);
            if (!isSocket(handle))
            {
                if (uint64_t n2; shouldLog("select!", n2))
                    warn("[net] select: %08X in an fd_set is not a socket, ignored", handle);
            }
        }

    for (uint32_t set : sets)
        if (set) write32(set, 0); // fd_count = 0: nothing ready

    if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    if (uint64_t n; shouldLog("select", n))
        info("[net] select: nothing ready after %llu ms (no network)", (unsigned long long)ms);
    ctx.r3.u64 = 0; // 0 = the timeout elapsed with no socket ready
}

// int __WSAFDIsSet(u32 S, fd_set* Set): the FD_ISSET helper, called alongside select while a match
// runs. Xenia walks the guest fd_set here; offline the answer is always no, because no socket is
// ever ready - which is what the loop over a set select just emptied would give anyway (select
// writes fd_count 0). Kept as a constant so it stays right for a set the game filled itself: a
// membership answer would tell the game a socket was readable and send it spinning on
// WSAEWOULDBLOCK. The handle is still read, so the call shape is unchanged if sockets ever carry data.
GUEST_IMPORT(NetDll___WSAFDIsSet)
{
    if (uint64_t n; shouldLog("__WSAFDIsSet", n))
        info("[net] __WSAFDIsSet(%08X): never ready (no network)", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// int recv(u32 Caller, u32 S, void* Buf, u32 Len, int Flags): nothing arrives, so it would block.
GUEST_IMPORT(NetDll_recv)
{
    if (!socketFromHandle(ctx.r4.u32)) { ctx.r3.s64 = -1; return; }
    t_lastError = WSAEWOULDBLOCK;
    if (uint64_t n; shouldLog("recv", n))
        info("[net] recv(%08X, %u bytes): nothing has arrived -> WSAEWOULDBLOCK", ctx.r4.u32, ctx.r6.u32);
    ctx.r3.s64 = -1;
}

// int recvfrom(u32 Caller, u32 S, void* Buf, u32 Len, int Flags, XSOCKADDR_IN* From, u32* FromLen)
GUEST_IMPORT(NetDll_recvfrom)
{
    if (!socketFromHandle(ctx.r4.u32)) { ctx.r3.s64 = -1; return; }
    t_lastError = WSAEWOULDBLOCK;
    if (uint64_t n; shouldLog("recvfrom", n))
        info("[net] recvfrom(%08X, %u bytes): nothing has arrived -> WSAEWOULDBLOCK", ctx.r4.u32, ctx.r6.u32);
    ctx.r3.s64 = -1;
}

// int WSARecvFrom(u32 Caller, u32 S, XWSABUF* Buffers, u32 Count, u32* NumRecv, u32* Flags,
//                 XSOCKADDR_IN* From, XWSAOVERLAPPED* Overlapped, void* CompletionRoutine)
// XWSABUF { be32 len; be32 buf_ptr }. Fails at once rather than leaving the game waiting on the
// overlapped event forever, which is what Xenia does for the same reason.
GUEST_IMPORT(NetDll_WSARecvFrom)
{
    if (!socketFromHandle(ctx.r4.u32)) { ctx.r3.s64 = -1; return; }
    t_lastError = WSAEWOULDBLOCK;
    if (uint64_t n; shouldLog("WSARecvFrom", n))
        info("[net] WSARecvFrom(%08X, %u buffers): nothing has arrived -> WSAEWOULDBLOCK", ctx.r4.u32, ctx.r6.u32);
    ctx.r3.s64 = -1;
}

// int send(u32 Caller, u32 S, const void* Buf, u32 Len, int Flags): succeeds and drops the data.
GUEST_IMPORT(NetDll_send)
{
    if (!socketFromHandle(ctx.r4.u32)) { ctx.r3.s64 = -1; return; }
    if (uint64_t n; shouldLog("send", n))
        info("[net] send(%08X, %u bytes): accepted and dropped (no network)", ctx.r4.u32, ctx.r6.u32);
    ctx.r3.u64 = ctx.r6.u32;
}

// int sendto(u32 Caller, u32 S, const void* Buf, u32 Len, int Flags, XSOCKADDR_IN* To, u32 ToLen)
GUEST_IMPORT(NetDll_sendto)
{
    if (!socketFromHandle(ctx.r4.u32)) { ctx.r3.s64 = -1; return; }
    if (uint64_t n; shouldLog("sendto", n))
        info("[net] sendto(%08X, %u bytes): accepted and dropped (no network)", ctx.r4.u32, ctx.r6.u32);
    ctx.r3.u64 = ctx.r6.u32;
}

// int WSASendTo(u32 Caller, u32 S, XWSABUF* Buffers, u32 Count, u32* NumSent, int Flags,
//               XSOCKADDR_IN* To, u32 ToLen, XWSAOVERLAPPED* Overlapped, void* CompletionRoutine):
// Xenia concatenates the buffers because its host socket takes one; here the total is what matters.
GUEST_IMPORT(NetDll_WSASendTo)
{
    uint32_t buffers = ctx.r5.u32, count = ctx.r6.u32, numSent = ctx.r7.u32;
    if (!socketFromHandle(ctx.r4.u32)) { ctx.r3.s64 = -1; return; }
    uint32_t total = 0;
    for (uint32_t i = 0; i < count && i < 64; i++) // bounded: a bogus count must not run past the array
        total += buffers ? read32(buffers + i * 8) : 0;
    if (numSent) write32(numSent, total);
    if (uint64_t n; shouldLog("WSASendTo", n))
        info("[net] WSASendTo(%08X, %u buffers, %u bytes): accepted and dropped (no network)", ctx.r4.u32, count, total);
    ctx.r3.u64 = 0;
}

// u32 inet_addr(const char* Address): dotted IPv4 -> network-order u32 (INADDR_NONE on garbage, 0
// for an empty string, as Xenia found on hardware). No caller argument, so the string is r3: read
// from r4 it pointed at guest address 1 and segfaulted the title on the way to the online menus.
GUEST_IMPORT(NetDll_inet_addr)
{
    const char* s = ctx.r3.u32 ? (const char*)host(ctx.r3.u32) : nullptr;
    uint32_t value = parseIPv4(s);
    if (uint64_t n; shouldLog("inet_addr", n))
        info("[net] inet_addr(\"%s\") -> %08X", s ? s : "", value);
    ctx.r3.u64 = value;
}

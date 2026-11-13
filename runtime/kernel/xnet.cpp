// Networking (NetDll_*): the game runs offline. Startup succeeds, there is no Ethernet link and DNS
// lookups fail, so the game's online code gives up cleanly instead of crashing on unimplemented
// imports. Signatures, structure layouts and behavior follow Xenia's xam_net.cc (BSD;
// docs/licenses/xenia-LICENSE.txt). Sockets are not implemented yet: the game creates and binds a
// UDP socket at startup and crashes if socket() fails, so those imports keep the default stub
// (returns 0) until a socket layer that works without a network is written.
#include "kernel.h"
#include "objects.h"
#include "import_support.h"
#include "../log.h"

#include <cstring>

using namespace mem;

namespace
{
    // Set by failing NetDll calls once sockets exist; nothing fails yet
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

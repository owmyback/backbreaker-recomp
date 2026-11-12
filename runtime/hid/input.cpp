// Controller input (XAM). Phase 1: one connected pad on user 0 with neutral state, no
// keystrokes. A host input backend (SDL, for the DualSense) will plug into readPad() later.
// Structures follow XInput / Xenia's xam_input.cc.
#include "../kernel/import_support.h"
#include "../log.h"

#include <atomic>

using namespace mem;

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    constexpr uint32_t ERROR_EMPTY = 0x10D2;
    constexpr uint32_t kAnyUser = 0xFF;

    struct Pad { uint16_t buttons = 0; uint8_t lt = 0, rt = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };

    std::atomic<uint32_t> g_packet{0};

    bool connected(uint32_t user) { return user == 0 || user == kAnyUser; }

    Pad readPad(uint32_t /*user*/) { return {}; }

    // XINPUT_GAMEPAD { u16 wButtons; u8 bLeftTrigger; u8 bRightTrigger; i16 sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
    void writeGamepad(uint32_t g, const Pad& p)
    {
        write16(g + 0, p.buttons);
        write8(g + 2, p.lt);
        write8(g + 3, p.rt);
        write16(g + 4, uint16_t(p.lx));
        write16(g + 6, uint16_t(p.ly));
        write16(g + 8, uint16_t(p.rx));
        write16(g + 10, uint16_t(p.ry));
    }
}

// u32 XamInputGetCapabilities(u32 UserIndex, u32 Flags, XINPUT_CAPABILITIES* Caps)
GUEST_IMPORT(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    if (!connected(user)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    write8(caps + 0, 1);  // Type: gamepad
    write8(caps + 1, 1);  // SubType: gamepad
    write16(caps + 2, 0); // Flags
    writeGamepad(caps + 4, { 0xFFFF, 0xFF, 0xFF, -1, -1, -1, -1 }); // every input is supported
    write16(caps + 16, 0xFFFF); // vibration motors
    write16(caps + 18, 0xFFFF);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// u32 XamInputGetState(u32 UserIndex, u32 Flags, XINPUT_STATE* State)
GUEST_IMPORT(XamInputGetState)
{
    uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
    if (!connected(user)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    write32(state, g_packet++);
    writeGamepad(state + 4, readPad(user));
    ctx.r3.u64 = ERROR_SUCCESS;
}

// u32 XamInputSetState(u32 UserIndex, u32 Unknown, XINPUT_VIBRATION* Vibration)
GUEST_IMPORT(XamInputSetState)
{
    ctx.r3.u64 = connected(ctx.r3.u32) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

// u32 XamInputGetKeystrokeEx(u32* UserIndex, u32 Flags, XINPUT_KEYSTROKE* Keystroke)
GUEST_IMPORT(XamInputGetKeystrokeEx) { ctx.r3.u64 = ERROR_EMPTY; }

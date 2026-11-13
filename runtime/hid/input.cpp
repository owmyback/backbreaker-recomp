// Controller input (XAM): one connected pad on user 0. Without a host backend the pad is
// neutral, except for scripted presses (--press). A host input backend (SDL, for the
// DualSense) will plug into readPad() later. Structures follow XInput / Xenia's xam_input.cc.
#include "hid.h"
#include "../kernel/import_support.h"
#include "../log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <vector>

using namespace mem;

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    constexpr uint32_t ERROR_EMPTY = 0x10D2;
    constexpr uint32_t kAnyUser = 0xFF;

    struct Pad { uint16_t buttons = 0; uint8_t lt = 0, rt = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };

    std::atomic<uint32_t> g_packet{0};

    // ---- Scripted presses ----
    struct Button { const char* name; uint16_t mask; uint16_t vk; };
    // XINPUT_GAMEPAD_* masks and VK_PAD_* virtual keys
    constexpr Button kButtons[] = {
        { "up", 0x0001, 0x5810 }, { "down", 0x0002, 0x5811 }, { "left", 0x0004, 0x5812 }, { "right", 0x0008, 0x5813 },
        { "start", 0x0010, 0x5814 }, { "back", 0x0020, 0x5815 }, { "ls", 0x0040, 0x5816 }, { "rs", 0x0080, 0x5817 },
        { "lb", 0x0100, 0x5805 }, { "rb", 0x0200, 0x5804 },
        { "a", 0x1000, 0x5800 }, { "b", 0x2000, 0x5801 }, { "x", 0x4000, 0x5802 }, { "y", 0x8000, 0x5803 },
    };
    constexpr double kHoldSeconds = 0.15;
    constexpr uint16_t XINPUT_KEYSTROKE_KEYDOWN = 0x0001, XINPUT_KEYSTROKE_KEYUP = 0x0002;

    struct Press { double at; const Button* button; };
    struct KeyEvent { double at; uint16_t vk; uint16_t flags; const char* name; };

    std::mutex g_scriptLock;
    std::vector<Press> g_presses;
    std::vector<KeyEvent> g_keyEvents; // sorted by time
    size_t g_nextKeyEvent = 0;
    const auto g_start = std::chrono::steady_clock::now();

    double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

    bool connected(uint32_t user) { return user == 0 || user == kAnyUser; }

    Pad readPad(uint32_t /*user*/)
    {
        Pad p{};
        double t = now();
        std::lock_guard guard(g_scriptLock);
        for (auto& press : g_presses)
            if (t >= press.at && t < press.at + kHoldSeconds)
                p.buttons |= press.button->mask;
        return p;
    }

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

namespace hid
{
    bool setScript(const std::string& script)
    {
        std::lock_guard guard(g_scriptLock);
        for (size_t p = 0; p < script.size();)
        {
            size_t q = script.find(',', p);
            std::string item = script.substr(p, q == std::string::npos ? std::string::npos : q - p);
            p = q == std::string::npos ? script.size() : q + 1;
            size_t at = item.find('@');
            const Button* button = nullptr;
            if (at != std::string::npos)
                for (auto& b : kButtons)
                    if (item.compare(0, at, b.name) == 0 && strlen(b.name) == at)
                        button = &b;
            if (!button)
            {
                warn("--press: bad item \"%s\" (use button@seconds; buttons: a b x y start back up down left right lb rb ls rs)", item.c_str());
                return false;
            }
            double t = std::atof(item.c_str() + at + 1);
            g_presses.push_back({ t, button });
            g_keyEvents.push_back({ t, button->vk, XINPUT_KEYSTROKE_KEYDOWN, button->name });
            g_keyEvents.push_back({ t + kHoldSeconds, button->vk, XINPUT_KEYSTROKE_KEYUP, button->name });
        }
        std::sort(g_keyEvents.begin(), g_keyEvents.end(), [](auto& a, auto& b) { return a.at < b.at; });
        info("[hid] scripted %zu button presses", g_presses.size());
        return true;
    }
}

// u32 XamInputGetKeystrokeEx(u32* UserIndex, u32 Flags, XINPUT_KEYSTROKE* Keystroke)
// XINPUT_KEYSTROKE { u16 VirtualKey; u16 Unicode; u16 Flags; u8 UserIndex; u8 HidCode; }
GUEST_IMPORT(XamInputGetKeystrokeEx)
{
    uint32_t pUser = ctx.r3.u32, ks = ctx.r5.u32;
    uint32_t user = pUser ? read32(pUser) : kAnyUser;
    if (!connected(user)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    std::lock_guard guard(g_scriptLock);
    if (g_nextKeyEvent >= g_keyEvents.size() || g_keyEvents[g_nextKeyEvent].at > now())
    {
        ctx.r3.u64 = ERROR_EMPTY;
        return;
    }
    const KeyEvent& e = g_keyEvents[g_nextKeyEvent++];
    write16(ks + 0, e.vk);
    write16(ks + 2, 0);
    write16(ks + 4, e.flags);
    write8(ks + 6, 0);
    write8(ks + 7, 0);
    if (pUser) write32(pUser, 0);
    if (e.flags & XINPUT_KEYSTROKE_KEYDOWN)
        info("[hid] scripted press: %s", e.name);
    ctx.r3.u64 = ERROR_SUCCESS;
}

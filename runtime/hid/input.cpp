// Controller input (XAM): up to 4 host controllers through SDL3 (users 0-3), plus scripted
// presses (--press) on user 0. Structures follow XInput / Xenia's xam_input.cc.
#include "hid.h"
#include "../kernel/import_support.h"
#include "../log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
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
    struct Button { const char* name; uint16_t mask; uint16_t vk; uint8_t trigger = 0; }; // trigger: 1 left, 2 right (pulled fully)
    // XINPUT_GAMEPAD_* masks and VK_PAD_* virtual keys
    constexpr Button kButtons[] = {
        { "up", 0x0001, 0x5810 }, { "down", 0x0002, 0x5811 }, { "left", 0x0004, 0x5812 }, { "right", 0x0008, 0x5813 },
        { "start", 0x0010, 0x5814 }, { "back", 0x0020, 0x5815 }, { "ls", 0x0040, 0x5816 }, { "rs", 0x0080, 0x5817 },
        { "lb", 0x0100, 0x5805 }, { "rb", 0x0200, 0x5804 },
        { "a", 0x1000, 0x5800 }, { "b", 0x2000, 0x5801 }, { "x", 0x4000, 0x5802 }, { "y", 0x8000, 0x5803 },
        { "lt", 0, 0x5806, 1 }, { "rt", 0, 0x5807, 2 },
    };
    constexpr double kHoldSeconds = 0.15;
    constexpr uint16_t XINPUT_KEYSTROKE_KEYDOWN = 0x0001, XINPUT_KEYSTROKE_KEYUP = 0x0002;

    struct Press { double at; const Button* button; };
    // Scripted stick holds: full deflection of one axis (0 lx, 1 ly, 2 rx, 3 ry) for `duration`
    struct StickHold { double at, duration; int axis; int16_t value; };
    struct Stick { const char* name; int axis; int16_t value; };
    constexpr Stick kSticks[] = { // XInput: +X right, +Y up
        { "ls_left", 0, -32768 }, { "ls_right", 0, 32767 }, { "ls_down", 1, -32768 }, { "ls_up", 1, 32767 },
        { "rs_left", 2, -32768 }, { "rs_right", 2, 32767 }, { "rs_down", 3, -32768 }, { "rs_up", 3, 32767 },
    };
    struct KeyEvent { double at; uint16_t vk; uint16_t flags; const char* name; };

    std::mutex g_scriptLock;
    std::vector<Press> g_presses;
    std::vector<StickHold> g_stickHolds;
    std::vector<KeyEvent> g_keyEvents; // sorted by time
    size_t g_nextKeyEvent = 0;
    const auto g_start = std::chrono::steady_clock::now();

    double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

    // ---- Host controllers (snapshot written by pollGamepads, read by the guest) ----
    constexpr int kMaxUsers = 4;
    struct HostPad
    {
        bool connected = false;
        Pad pad;
        uint32_t keys = 0;          // keystroke state: bit per entry of kKeys
        uint16_t rumbleLeft = 0, rumbleRight = 0;
        bool rumbleDirty = false;
        double rumbleSent = 0;
    };
    std::mutex g_hostLock;
    HostPad g_host[kMaxUsers];
    std::atomic<bool> g_virtualPad{false};
    struct RealKey { uint16_t vk, flags; uint8_t user; };
    std::deque<RealKey> g_realKeys; // keystrokes from real controllers, oldest first

    bool connected(uint32_t user)
    {
        std::lock_guard guard(g_hostLock);
        if (user == kAnyUser)
        {
            for (auto& h : g_host) if (h.connected) return true;
            return g_virtualPad;
        }
        if (user >= kMaxUsers) return false;
        return g_host[user].connected || (user == 0 && g_virtualPad);
    }

    Pad readPad(uint32_t user)
    {
        Pad p{};
        {
            std::lock_guard guard(g_hostLock);
            if (user < kMaxUsers && g_host[user].connected) p = g_host[user].pad;
        }
        if (user != 0) return p;
        double t = now();
        std::lock_guard guard(g_scriptLock);
        for (auto& press : g_presses)
            if (t >= press.at && t < press.at + kHoldSeconds)
            {
                p.buttons |= press.button->mask; // scripted presses add to the real pad
                if (press.button->trigger == 1) p.lt = 255;
                if (press.button->trigger == 2) p.rt = 255;
            }
        for (auto& h : g_stickHolds)
            if (t >= h.at && t < h.at + h.duration)
            {
                int16_t* axes[4] = { &p.lx, &p.ly, &p.rx, &p.ry };
                *axes[h.axis] = h.value; // scripted stick holds replace that axis
            }
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
// XINPUT_VIBRATION { u16 wLeftMotorSpeed; u16 wRightMotorSpeed; } (left = low frequency)
GUEST_IMPORT(XamInputSetState)
{
    uint32_t user = ctx.r3.u32, vib = ctx.r5.u32;
    if (!connected(user)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    if (vib && user < kMaxUsers)
    {
        std::lock_guard guard(g_hostLock);
        HostPad& h = g_host[user];
        uint16_t l = read16(vib), r = read16(vib + 2);
        if (l != h.rumbleLeft || r != h.rumbleRight) { h.rumbleLeft = l; h.rumbleRight = r; h.rumbleDirty = true; }
    }
    ctx.r3.u64 = ERROR_SUCCESS;
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
            const Stick* stick = nullptr;
            if (at != std::string::npos)
            {
                for (auto& b : kButtons)
                    if (item.compare(0, at, b.name) == 0 && strlen(b.name) == at)
                        button = &b;
                for (auto& s : kSticks)
                    if (item.compare(0, at, s.name) == 0 && strlen(s.name) == at)
                        stick = &s;
            }
            if (stick)
            {
                // stick@seconds[:duration]: hold the stick fully in that direction, then let go
                double t = std::atof(item.c_str() + at + 1);
                size_t colon = item.find(':', at);
                double duration = colon == std::string::npos ? kHoldSeconds : std::atof(item.c_str() + colon + 1);
                g_stickHolds.push_back({ t, duration, stick->axis, stick->value });
                continue;
            }
            if (!button)
            {
                warn("--press: bad item \"%s\" (use button@seconds or stick@seconds[:duration]; buttons: a b x y start "
                     "back up down left right lb rb lt rt ls rs; sticks: ls_/rs_ + left right up down)", item.c_str());
                return false;
            }
            double t = std::atof(item.c_str() + at + 1);
            g_presses.push_back({ t, button });
            g_keyEvents.push_back({ t, button->vk, XINPUT_KEYSTROKE_KEYDOWN, button->name });
            g_keyEvents.push_back({ t + kHoldSeconds, button->vk, XINPUT_KEYSTROKE_KEYUP, button->name });
        }
        std::sort(g_keyEvents.begin(), g_keyEvents.end(), [](auto& a, auto& b) { return a.at < b.at; });
        info("[hid] scripted %zu button presses, %zu stick holds", g_presses.size(), g_stickHolds.size());
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
    {
        std::lock_guard guard(g_hostLock);
        for (auto it = g_realKeys.begin(); it != g_realKeys.end(); ++it)
        {
            if (user != kAnyUser && it->user != user) continue;
            write16(ks + 0, it->vk);
            write16(ks + 2, 0);
            write16(ks + 4, it->flags);
            write8(ks + 6, it->user);
            write8(ks + 7, 0);
            if (pUser) write32(pUser, it->user);
            g_realKeys.erase(it);
            ctx.r3.u64 = ERROR_SUCCESS;
            return;
        }
    }
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

// ---- SDL3 backend ----
namespace
{
    // Keystrokes generated from real controllers: VK_PAD_* code and how the state is detected
    struct KeySource { uint16_t vk; uint16_t button; int axis; int sign; }; // button mask, or axis index (0 lx, 1 ly, 2 rx, 3 ry, 4 lt, 5 rt)
    constexpr KeySource kKeys[] = {
        { 0x5800, 0x1000, -1, 0 }, { 0x5801, 0x2000, -1, 0 }, { 0x5802, 0x4000, -1, 0 }, { 0x5803, 0x8000, -1, 0 }, // A B X Y
        { 0x5804, 0x0200, -1, 0 }, { 0x5805, 0x0100, -1, 0 },                                                   // RB LB
        { 0x5806, 0, 4, 1 }, { 0x5807, 0, 5, 1 },                                                               // LT RT
        { 0x5810, 0x0001, -1, 0 }, { 0x5811, 0x0002, -1, 0 }, { 0x5812, 0x0004, -1, 0 }, { 0x5813, 0x0008, -1, 0 }, // D-pad
        { 0x5814, 0x0010, -1, 0 }, { 0x5815, 0x0020, -1, 0 }, { 0x5816, 0x0040, -1, 0 }, { 0x5817, 0x0080, -1, 0 }, // start back ls rs
        { 0x5820, 0, 1, 1 }, { 0x5821, 0, 1, -1 }, { 0x5822, 0, 0, 1 }, { 0x5823, 0, 0, -1 },                       // left stick up down right left
        { 0x5830, 0, 3, 1 }, { 0x5831, 0, 3, -1 }, { 0x5832, 0, 2, 1 }, { 0x5833, 0, 2, -1 },                       // right stick
    };
    constexpr int kStickThreshold = 16384, kTriggerThreshold = 30; // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

    bool keyDown(const KeySource& k, const Pad& p)
    {
        if (k.axis < 0) return p.buttons & k.button;
        int v = k.axis == 0 ? p.lx : k.axis == 1 ? p.ly : k.axis == 2 ? p.rx : k.axis == 3 ? p.ry : k.axis == 4 ? p.lt : p.rt;
        if (k.axis >= 4) return v > kTriggerThreshold;
        return k.sign > 0 ? v > kStickThreshold : v < -kStickThreshold;
    }

    int16_t stick(SDL_Gamepad* g, SDL_GamepadAxis a, bool invert)
    {
        int v = SDL_GetGamepadAxis(g, a);
        if (invert) v = -v; // SDL: +Y is down; XInput: +Y is up
        return int16_t(std::clamp(v, -32768, 32767));
    }

    Pad readSdl(SDL_Gamepad* g)
    {
        static const std::pair<SDL_GamepadButton, uint16_t> map[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001 }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002 }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004 },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008 }, { SDL_GAMEPAD_BUTTON_START, 0x0010 }, { SDL_GAMEPAD_BUTTON_BACK, 0x0020 },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040 }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080 },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100 }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200 },
            { SDL_GAMEPAD_BUTTON_SOUTH, 0x1000 }, { SDL_GAMEPAD_BUTTON_EAST, 0x2000 }, { SDL_GAMEPAD_BUTTON_WEST, 0x4000 },
            { SDL_GAMEPAD_BUTTON_NORTH, 0x8000 } };
        Pad p{};
        for (auto& [b, mask] : map)
            if (SDL_GetGamepadButton(g, b)) p.buttons |= mask;
        p.lt = uint8_t(std::clamp(int(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) * 255 / 32767, 0, 255));
        p.rt = uint8_t(std::clamp(int(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) * 255 / 32767, 0, 255));
        p.lx = stick(g, SDL_GAMEPAD_AXIS_LEFTX, false);
        p.ly = stick(g, SDL_GAMEPAD_AXIS_LEFTY, true);
        p.rx = stick(g, SDL_GAMEPAD_AXIS_RIGHTX, false);
        p.ry = stick(g, SDL_GAMEPAD_AXIS_RIGHTY, true);
        return p;
    }

    SDL_Gamepad* g_open[kMaxUsers] = {};
    SDL_JoystickID g_openId[kMaxUsers] = {};
    bool g_sdlReady = false;

    // The overlay (achievements list, #101) takes the controllers while it is open: the game then
    // reads a neutral pad, and the overlay reads the button presses. kGuideMask is outside the
    // XINPUT_GAMEPAD_* buttons the game sees.
    constexpr uint16_t kGuideMask = 0x0400;
    std::atomic<bool> g_captured{false};
    uint16_t g_menuHeld[kMaxUsers] = {};
    std::atomic<uint32_t> g_menuPresses{0};
}

namespace hid
{
    void setVirtualPad(bool enabled) { g_virtualPad = enabled; }
    double secondsSinceLaunch() { return now(); }

    void initGamepads()
    {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // SIGINT/SIGTERM end the run (main.cpp)
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            warn("[hid] SDL gamepad support unavailable: %s", SDL_GetError());
            return;
        }
        g_sdlReady = true;
    }

    void pollGamepads()
    {
        if (!g_sdlReady) return;
        SDL_UpdateGamepads();
        // Users 0-3 = the first four controllers, in the order SDL lists them
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int u = 0; u < kMaxUsers; u++)
        {
            SDL_JoystickID id = u < count ? ids[u] : 0;
            if (g_open[u] && g_openId[u] != id)
            {
                info("[hid] controller on user %d disconnected", u);
                SDL_CloseGamepad(g_open[u]);
                g_open[u] = nullptr;
            }
            if (id && !g_open[u])
            {
                g_open[u] = SDL_OpenGamepad(id);
                g_openId[u] = id;
                if (g_open[u]) info("[hid] controller on user %d: %s", u, SDL_GetGamepadName(g_open[u]));
            }
        }
        SDL_free(ids);

        double t = now();
        std::lock_guard guard(g_hostLock);
        for (int u = 0; u < kMaxUsers; u++)
        {
            HostPad& h = g_host[u];
            if (!g_open[u])
            {
                if (h.connected) h = HostPad{}; // keys released, rumble off
                continue;
            }
            h.connected = true;
            Pad raw = readSdl(g_open[u]);
            // Menu presses for the overlay: button edges, the Guide (PS) button, and the left stick
            // pushed past half way counted as the d-pad
            uint16_t menu = raw.buttons;
            if (SDL_GetGamepadButton(g_open[u], SDL_GAMEPAD_BUTTON_GUIDE)) menu |= kGuideMask;
            if (raw.ly > 16384) menu |= 0x0001;
            if (raw.ly < -16384) menu |= 0x0002;
            g_menuPresses.fetch_or(uint32_t(menu & ~g_menuHeld[u]), std::memory_order_relaxed);
            g_menuHeld[u] = menu;
            h.pad = g_captured.load(std::memory_order_relaxed) ? Pad{} : raw; // keys below release too
            // Keystroke edges
            for (size_t k = 0; k < std::size(kKeys); k++)
            {
                bool down = keyDown(kKeys[k], h.pad), was = (h.keys >> k) & 1;
                if (down == was) continue;
                h.keys ^= 1u << k;
                g_realKeys.push_back({ kKeys[k].vk, uint16_t(down ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP), uint8_t(u) });
                if (g_realKeys.size() > 256) g_realKeys.pop_front();
            }
            // Rumble: XInput vibration lasts until changed; SDL's needs refreshing
            bool on = h.rumbleLeft || h.rumbleRight;
            if (h.rumbleDirty || (on && t - h.rumbleSent > 0.5))
            {
                SDL_RumbleGamepad(g_open[u], h.rumbleLeft, h.rumbleRight, on ? 1000 : 0);
                h.rumbleDirty = false;
                h.rumbleSent = t;
            }
        }
    }

    void setInputCaptured(bool captured) { g_captured = captured; }

    uint32_t takeMenuPresses() { return g_menuPresses.exchange(0, std::memory_order_relaxed); }

    void startInputThread()
    {
        std::thread([] {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // no window to have focus
            initGamepads();
            if (!g_sdlReady) return;
            for (;;)
            {
                SDL_PumpEvents();
                pollGamepads();
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }).detach();
    }
}

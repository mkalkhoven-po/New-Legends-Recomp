// XAPI controller functions (XPP) replaced by host XInput.
//
// The original XPP library drives the console's USB host controller, which
// is not emulated; the game only sees these entry points (addresses in
// config/overrides.txt). Original Xbox controllers map onto XInput pads
// almost one to one; Black and White go to the shoulder buttons.
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <math.h>
#include <string>
#include <vector>

#include "kernel.h"
#include "settings.h"

constexpr u32 XDEVICE_TYPE_GAMEPAD = 0x00221CC0;  // the game's gamepad type table
constexpr u32 ERROR_SUCCESS_ = 0, ERROR_DEVICE_NOT_CONNECTED_ = 1167;
constexpr int PORTS = 4;

// stdcall return: result in eax, pop return address and arguments.
static void hle_return(Ctx* c, u32 value, u32 argbytes) {
    c->eax = value;
    c->esp += 4 + argbytes;
}

struct Pad {
    bool connected = false;
    s64 next_probe_ms = 0;  // XInput is slow on empty ports; probe them rarely
    XINPUT_STATE last{};
    u32 packet = 0;
    bool open = false;
};
static Pad g_pads[PORTS];
static u32 g_reported_mask;

// Port 0 is always there: a real controller plus keyboard and mouse (from
// newlegends.ini), or keyboard and mouse alone. Scripted presses from
// NL_AUTOPRESS="seconds:button,..." and NL_MONKEY replace the real pad.
bool host_window_focused();
bool host_mouse_captured();
bool host_mouse_buttons_ok();

// ------------------------------------------------------------------ mouse
// Raw mouse movement and wheel notches, added up by the window thread.
static std::atomic<int> g_mouse_dx, g_mouse_dy, g_wheel;
void input_mouse_move(int dx, int dy) {
    g_mouse_dx += dx;
    g_mouse_dy += dy;
}
void input_mouse_wheel(int delta) { g_wheel += delta; }

// The game's right stick filter (0x16DBD0) scales the reading by the largest
// value seen so far on that side (starting at 28000), cuts a dead zone of 30%
// and turns the camera at a speed linear in what is left. The mouse sets the
// stick just past the dead zone plus a part proportional to its speed, so
// small movements turn the camera slowly instead of not at all.
constexpr float STICK_DEAD_ZONE = 0.3f;
constexpr s32 STICK_INITIAL_RANGE = 28000;
// Mouse speed (counts per second) that turns the camera at full stick speed,
// at Sensitivity=1.
constexpr float MOUSE_FULL_SPEED = 2500.0f;

// Largest values the game has been shown, per axis and side: its range.
static s32 g_seen_max[2] = {STICK_INITIAL_RANGE, STICK_INITIAL_RANGE};
static s32 g_seen_min[2] = {-STICK_INITIAL_RANGE, -STICK_INITIAL_RANGE};

static s32 mouse_axis(float speed, int axis) {
    if (speed == 0) return 0;
    float r = fabsf(speed) * settings().mouse_sensitivity / MOUSE_FULL_SPEED;
    if (r > 1) r = 1;
    float v = STICK_DEAD_ZONE + (1 - STICK_DEAD_ZONE) * r;
    return speed > 0 ? (s32)(v * g_seen_max[axis]) : (s32)(v * g_seen_min[axis]);
}

// Mouse look on top of the right stick, in the game's convention (positive y
// looks down). The value is kept for at least 8 ms, so a second read in the
// same frame does not see an empty mouse.
static void mouse_look(s32& rx, s32& ry) {
    static s64 last_us;
    static s32 mx, my;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    s64 us = now.QuadPart * 1000000 / freq.QuadPart;
    // NL_FAKE_MOUSE="from,to,x,y": mouse movement of x,y counts per second
    // between those seconds after start, for tests without a real mouse.
    static float fake[4];
    static bool faking = getenv("NL_FAKE_MOUSE") &&
                         sscanf(getenv("NL_FAKE_MOUSE"), "%f,%f,%f,%f", &fake[0], &fake[1], &fake[2], &fake[3]) == 4;
    if (faking) {
        static s64 t0 = us, prev = us;
        float t = (us - t0) / 1e6f;
        if (t >= fake[0] && t < fake[1]) {
            static float carry_x, carry_y;
            carry_x += fake[2] * (us - prev) / 1e6f;
            carry_y += fake[3] * (us - prev) / 1e6f;
            int ix = (int)carry_x, iy = (int)carry_y;
            carry_x -= ix, carry_y -= iy;
            input_mouse_move(ix, iy);
        }
        prev = us;
    }
    if (!settings().mouse || !(faking || host_mouse_captured())) {
        g_mouse_dx = 0;
        g_mouse_dy = 0;
        mx = my = 0;
        last_us = us;
        return;
    }
    s64 dt = us - last_us;
    if (dt >= 8000) {
        if (dt > 100000) dt = 100000;  // after a pause, do not spread the movement thin
        float sx = g_mouse_dx.exchange(0) * 1e6f / dt, sy = g_mouse_dy.exchange(0) * 1e6f / dt;
        if (settings().mouse_invert_y) sy = -sy;
        mx = mouse_axis(sx, 0);
        my = mouse_axis(sy, 1);
        last_us = us;
    }
    if (abs(mx) > abs(rx)) rx = mx;
    if (abs(my) > abs(ry)) ry = my;
}

// Wheel notches become short presses, one at a time, so the game sees each.
static bool wheel_down(int key) {
    static int active;  // KEY_WHEEL_* being pressed, or 0
    static s64 until;
    s64 now = now_ms();
    if (active && now >= until) {
        active = 0;
        until = now + 50;  // released for a few frames before the next press
    }
    if (!active && now >= until) {
        int w = g_wheel.load();
        if (w >= WHEEL_DELTA) active = KEY_WHEEL_UP, g_wheel -= WHEEL_DELTA;
        else if (w <= -WHEEL_DELTA) active = KEY_WHEEL_DOWN, g_wheel += WHEEL_DELTA;
        if (active) until = now + 70;
    }
    return active == key;
}

static bool is_mouse_button(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

struct ScriptPress {
    s64 at_ms;
    u16 buttons;
    u8 analog;  // index into the analog button array, 0xFF = none
};
static std::vector<ScriptPress> parse_script() {
    std::vector<ScriptPress> out;
    const char* s = getenv("NL_AUTOPRESS");
    if (!s) return out;
    static const struct {
        const char* name;
        u16 buttons;
        u8 analog;
    } names[] = {{"up", XINPUT_GAMEPAD_DPAD_UP, 0xFF}, {"down", XINPUT_GAMEPAD_DPAD_DOWN, 0xFF},
                 {"left", XINPUT_GAMEPAD_DPAD_LEFT, 0xFF}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT, 0xFF},
                 {"start", XINPUT_GAMEPAD_START, 0xFF}, {"back", XINPUT_GAMEPAD_BACK, 0xFF},
                 {"a", 0, 0}, {"b", 0, 1}, {"x", 0, 2}, {"y", 0, 3}};
    std::string all(s);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos);
        std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string b = item.substr(colon + 1);
            for (auto& n : names)
                if (b == n.name) out.push_back({(s64)(atof(item.c_str()) * 1000), n.buttons, n.analog});
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return out;
}

// NL_MONKEY=seed: random play for automated tests. Walks around holding a
// stick direction, looks around, and presses buttons (never Back or Start,
// which would leave the level for the menus).
static void monkey_state(XINPUT_GAMEPAD& g, s64 now) {
    static u32 rng = (u32)atoi(getenv("NL_MONKEY")) * 2654435761u + 1;
    static s64 next_walk, next_press, press_until;
    static SHORT lx, ly, rx;
    static u16 button;
    auto rnd = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    if (now >= next_walk) {
        next_walk = now + 500 + rnd() % 1500;
        lx = (SHORT)(rnd() % 65535 - 32767);
        ly = (SHORT)(rnd() % 32767);  // mostly forward: covers ground
        rx = (SHORT)(rnd() % 3 == 0 ? rnd() % 65535 - 32767 : 0);
    }
    if (now >= next_press) {
        static const u16 buttons[] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                      XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER};
        next_press = now + 200 + rnd() % 600;
        press_until = now + 100;
        button = buttons[rnd() % 6];
    }
    g.sThumbLX = lx;
    g.sThumbLY = ly;
    g.sThumbRX = rx;
    if (now < press_until) g.wButtons |= button;
}

// Keys from newlegends.ini, added on top of whatever `g` already holds.
static void keyboard_state(XINPUT_GAMEPAD& g) {
    const Settings& cfg = settings();
    if (!cfg.keyboard || !host_window_focused()) return;
    // Alt+Enter switches fullscreen; it must not also press what Enter is bound to.
    bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool buttons_ok = host_mouse_buttons_ok();
    auto down = [&](PadControl c) {
        for (int vk : cfg.keys[c]) {
            if (vk == KEY_WHEEL_UP || vk == KEY_WHEEL_DOWN) {
                if (wheel_down(vk)) return true;
                continue;
            }
            if (is_mouse_button(vk) && !buttons_ok) continue;
            if ((GetAsyncKeyState(vk) & 0x8000) && !(alt && vk == VK_RETURN)) return true;
        }
        return false;
    };
    static const struct {
        PadControl control;
        u16 button;
    } buttons[] = {{PAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},     {PAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
                   {PAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {PAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
                   {PAD_START, XINPUT_GAMEPAD_START},         {PAD_BACK, XINPUT_GAMEPAD_BACK},
                   {PAD_LEFT_THUMB, XINPUT_GAMEPAD_LEFT_THUMB}, {PAD_RIGHT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB},
                   {PAD_A, XINPUT_GAMEPAD_A},                 {PAD_B, XINPUT_GAMEPAD_B},
                   {PAD_X, XINPUT_GAMEPAD_X},                 {PAD_Y, XINPUT_GAMEPAD_Y},
                   {PAD_BLACK, XINPUT_GAMEPAD_RIGHT_SHOULDER}, {PAD_WHITE, XINPUT_GAMEPAD_LEFT_SHOULDER}};
    for (auto& b : buttons)
        if (down(b.control)) g.wButtons |= b.button;
    if (down(PAD_LEFT_TRIGGER)) g.bLeftTrigger = 255;
    if (down(PAD_RIGHT_TRIGGER)) g.bRightTrigger = 255;
    // A stick follows the keys only while one of its keys is held.
    auto axis = [&](SHORT& v, PadControl plus, PadControl minus) {
        int k = (down(plus) ? 32767 : 0) - (down(minus) ? 32767 : 0);
        if (k) v = (SHORT)k;
    };
    axis(g.sThumbLX, PAD_LSTICK_RIGHT, PAD_LSTICK_LEFT);
    axis(g.sThumbLY, PAD_LSTICK_UP, PAD_LSTICK_DOWN);
    axis(g.sThumbRX, PAD_RSTICK_RIGHT, PAD_RSTICK_LEFT);
    axis(g.sThumbRY, PAD_RSTICK_UP, PAD_RSTICK_DOWN);
}

static void virtual_state(XINPUT_STATE* st) {
    static std::vector<ScriptPress> script = parse_script();
    static bool monkey = getenv("NL_MONKEY") != nullptr;
    static u32 packet;
    XINPUT_GAMEPAD g{};
    s64 now = now_ms();
    if (monkey) monkey_state(g, now);
    for (auto& p : script)
        if (now >= p.at_ms && now < p.at_ms + 150) {
            g.wButtons |= p.buttons;
            if (p.analog == 0) g.wButtons |= XINPUT_GAMEPAD_A;
            if (p.analog == 1) g.wButtons |= XINPUT_GAMEPAD_B;
            if (p.analog == 2) g.wButtons |= XINPUT_GAMEPAD_X;
            if (p.analog == 3) g.wButtons |= XINPUT_GAMEPAD_Y;
        }
    keyboard_state(g);
    static XINPUT_GAMEPAD last;
    if (memcmp(&g, &last, sizeof g)) packet++;
    last = g;
    st->dwPacketNumber = packet;
    st->Gamepad = g;
}

static bool poll(int port, XINPUT_STATE* out) {
    Pad& p = g_pads[port];
    s64 now = now_ms();
    XINPUT_STATE st;
    // Scripted test input must not depend on whether a real controller is plugged in.
    static bool scripted = getenv("NL_MONKEY") || getenv("NL_AUTOPRESS");
    if (port == 0 && scripted) {
        if (out) virtual_state(out);
        return true;
    }
    bool keyboard = port == 0 && (settings().keyboard || settings().mouse);
    if (!p.connected && now < p.next_probe_ms) {
        if (keyboard) {
            if (out) virtual_state(out);
            return true;
        }
        return false;
    }
    if (XInputGetState(port, &st) != ERROR_SUCCESS) {
        p.connected = false;
        p.next_probe_ms = now + 1000;
        if (keyboard) {
            if (out) virtual_state(out);
            return true;
        }
        return false;
    }
    p.connected = true;
    if (keyboard) keyboard_state(st.Gamepad);
    if (out) *out = st;
    return true;
}

static u32 connected_mask() {
    u32 mask = 0;
    for (int i = 0; i < PORTS; i++)
        if (poll(i, nullptr)) mask |= 1u << i;
    return mask;
}

// XInitDevices(DWORD dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE PreallocTypes)
void hle_XInitDevices(Ctx* c) {
    g_reported_mask = 0;
    log_msg("input: XInitDevices, controllers on ports mask %X", connected_mask());
    hle_return(c, 0, 8);
}

// DWORD XGetDevices(PXPP_DEVICE_TYPE DeviceType)
void hle_XGetDevices(Ctx* c) {
    u32 type = rd32(c->esp + 4);
    u32 mask = type == XDEVICE_TYPE_GAMEPAD ? connected_mask() : 0;
    if (type == XDEVICE_TYPE_GAMEPAD) g_reported_mask = mask;
    hle_return(c, mask, 4);
}

// BOOL XGetDeviceChanges(PXPP_DEVICE_TYPE DeviceType, PDWORD pdwInsertions, PDWORD pdwRemovals)
void hle_XGetDeviceChanges(Ctx* c) {
    u32 type = rd32(c->esp + 4), ins_ptr = rd32(c->esp + 8), rem_ptr = rd32(c->esp + 12);
    u32 ins = 0, rem = 0;
    if (type == XDEVICE_TYPE_GAMEPAD) {
        u32 now = connected_mask();
        ins = now & ~g_reported_mask;
        rem = g_reported_mask & ~now;
        g_reported_mask = now;
    }
    if (ins_ptr) wr32(ins_ptr, ins);
    if (rem_ptr) wr32(rem_ptr, rem);
    hle_return(c, (ins | rem) != 0, 12);
}

// Handles are small tokens; the guest never looks inside them.
constexpr u32 HANDLE_BASE = 0x00ABC000;
static int handle_port(u32 h) {
    int port = (int)(h - HANDLE_BASE);
    return port >= 0 && port < PORTS && g_pads[port].open ? port : -1;
}

// HANDLE XInputOpen(PXPP_DEVICE_TYPE, DWORD dwPort, DWORD dwSlot, PXINPUT_POLLING_PARAMETERS)
void hle_XInputOpen(Ctx* c) {
    u32 type = rd32(c->esp + 4), port = rd32(c->esp + 8);
    if (type != XDEVICE_TYPE_GAMEPAD || port >= PORTS || !poll((int)port, nullptr)) return hle_return(c, 0, 16);
    g_pads[port].open = true;
    log_msg("input: controller opened on port %u", port);
    hle_return(c, HANDLE_BASE + port, 16);
}

// void XInputClose(HANDLE hDevice)
void hle_XInputClose(Ctx* c) {
    int port = handle_port(rd32(c->esp + 4));
    if (port >= 0) g_pads[port].open = false;
    hle_return(c, 0, 4);
}

static u8 analog(bool pressed) { return pressed ? 0xFF : 0; }

// DWORD XInputGetState(HANDLE hDevice, PXINPUT_STATE pState)
void hle_XInputGetState(Ctx* c) {
    int port = handle_port(rd32(c->esp + 4));
    u32 out = rd32(c->esp + 8);
    static u32 calls;
    if (getenv("NL_TRACE_INPUT") && (calls++ % 600) == 0)
        log_msg("input: XInputGetState(%08X) call %u from %08X", rd32(c->esp + 4), calls, rd32(c->esp));
    XINPUT_STATE st;
    if (port < 0 || !poll(port, &st)) return hle_return(c, ERROR_DEVICE_NOT_CONNECTED_, 8);
    XINPUT_GAMEPAD& g = st.Gamepad;
    // The game looks down when the right stick is pushed up; by default the
    // axis is flipped so up looks up, as PC players expect.
    s32 rx = g.sThumbRX, ry = settings().invert_aim ? g.sThumbRY : -(s32)g.sThumbRY;
    if (ry > 32767) ry = 32767;
    if (port == 0) {
        mouse_look(rx, ry);
        s32 v[2] = {rx, ry};
        for (int i = 0; i < 2; i++) {
            if (v[i] > g_seen_max[i]) g_seen_max[i] = v[i];
            if (v[i] < g_seen_min[i]) g_seen_min[i] = v[i];
        }
    }
    g.sThumbRX = (SHORT)rx;
    g.sThumbRY = (SHORT)ry;  // from here on in the game's convention
    Pad& p = g_pads[port];
    // Keyboard and mouse can change the state without XInput's packet number moving.
    if (st.dwPacketNumber != p.last.dwPacketNumber || memcmp(&st.Gamepad, &p.last.Gamepad, sizeof st.Gamepad)) p.packet++;
    p.last = st;
    // Digital buttons share bit positions for d-pad, start, back and sticks.
    u16 buttons = g.wButtons & 0x00FF;
    u8 analogs[8] = {
        analog(g.wButtons & XINPUT_GAMEPAD_A), analog(g.wButtons & XINPUT_GAMEPAD_B),
        analog(g.wButtons & XINPUT_GAMEPAD_X), analog(g.wButtons & XINPUT_GAMEPAD_Y),
        analog(g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER),  // Black
        analog(g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER),   // White
        g.bLeftTrigger, g.bRightTrigger,
    };
    static bool trace = getenv("NL_TRACE_INPUT") != nullptr;
    if (trace && (buttons || analogs[0])) log_msg("input: port %d buttons %04X A %02X packet %u", port, buttons, analogs[0], p.packet);
    wr32(out + 0, p.packet);
    wr16(out + 4, buttons);
    memcpy(gptr(out + 6), analogs, 8);
    wr16(out + 14, (u16)g.sThumbLX);
    wr16(out + 16, (u16)g.sThumbLY);
    wr16(out + 18, (u16)g.sThumbRX);
    wr16(out + 20, (u16)g.sThumbRY);
    hle_return(c, ERROR_SUCCESS_, 8);
}

// DWORD XInputSetState(HANDLE hDevice, PXINPUT_FEEDBACK pFeedback)
// Feedback header: dwStatus, hEvent, 58 reserved bytes; then the rumble motors.
void hle_XInputSetState(Ctx* c) {
    int port = handle_port(rd32(c->esp + 4));
    u32 fb = rd32(c->esp + 8);
    if (port < 0) return hle_return(c, ERROR_DEVICE_NOT_CONNECTED_, 8);
    XINPUT_VIBRATION v{rd16(fb + 66), rd16(fb + 68)};
    XInputSetState(port, &v);
    wr32(fb + 0, ERROR_SUCCESS_);  // completed immediately
    if (u32 ev = rd32(fb + 4)) {
        if (u32 obj = handle_object(ev)) {
            wr32(obj + DH_SIGNAL, 1);
            sched_signal();
        }
    }
    hle_return(c, ERROR_SUCCESS_, 8);
}

// DWORD XInputGetCapabilities(HANDLE hDevice, PXINPUT_CAPABILITIES pCapabilities)
// Packed: SubType @0, Reserved @1, In.Gamepad @3 (buttons, 8 analogs,
// 4 thumb axes), Out.Rumble @21 (two motors); 25 bytes.
void hle_XInputGetCapabilities(Ctx* c) {
    int port = handle_port(rd32(c->esp + 4));
    u32 caps = rd32(c->esp + 8);
    if (port < 0) return hle_return(c, ERROR_DEVICE_NOT_CONNECTED_, 8);
    memset(gptr(caps), 0, 25);
    wr8(caps + 0, 1);                                  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    wr16(caps + 3, 0x00FF);                            // all digital buttons
    memset(gptr(caps + 5), 0xFF, 8);                   // all analog buttons
    for (int i = 0; i < 4; i++) wr16(caps + 13 + 2 * i, 0xFFFF);  // thumb axes
    wr16(caps + 21, 0xFFFF);                           // rumble motors
    wr16(caps + 23, 0xFFFF);
    hle_return(c, ERROR_SUCCESS_, 8);
}

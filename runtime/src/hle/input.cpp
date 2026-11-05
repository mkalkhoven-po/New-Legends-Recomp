// XAPI controller functions (XPP) replaced by host XInput.
//
// The original XPP library drives the console's USB host controller, which
// is not emulated; the game only sees these entry points (addresses in
// config/overrides.txt). Original Xbox controllers map onto XInput pads
// almost one to one; Black and White go to the shoulder buttons.
#include <windows.h>
#include <xinput.h>

#include <string>
#include <vector>

#include "kernel.h"

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

// Port 0 falls back to a virtual pad (keyboard, plus scripted presses from
// NL_AUTOPRESS="seconds:button,...") when no controller is plugged in.
bool host_window_focused();
static bool g_virtual = getenv("NL_NO_KEYBOARD") == nullptr;

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

static void virtual_state(XINPUT_STATE* st) {
    static std::vector<ScriptPress> script = parse_script();
    static u32 packet;
    XINPUT_GAMEPAD g{};
    s64 now = now_ms();
    for (auto& p : script)
        if (now >= p.at_ms && now < p.at_ms + 150) {
            g.wButtons |= p.buttons;
            if (p.analog == 0) g.wButtons |= XINPUT_GAMEPAD_A;
            if (p.analog == 1) g.wButtons |= XINPUT_GAMEPAD_B;
            if (p.analog == 2) g.wButtons |= XINPUT_GAMEPAD_X;
            if (p.analog == 3) g.wButtons |= XINPUT_GAMEPAD_Y;
        }
    if (host_window_focused()) {
        auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        static const struct {
            int vk;
            u16 button;
        } keys[] = {{VK_UP, XINPUT_GAMEPAD_DPAD_UP},   {VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
                    {VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
                    {VK_RETURN, XINPUT_GAMEPAD_START}, {VK_BACK, XINPUT_GAMEPAD_BACK},
                    {VK_SPACE, XINPUT_GAMEPAD_A},      {'E', XINPUT_GAMEPAD_B},
                    {'Q', XINPUT_GAMEPAD_X},           {'F', XINPUT_GAMEPAD_Y},
                    {'C', XINPUT_GAMEPAD_RIGHT_SHOULDER}, {'Z', XINPUT_GAMEPAD_LEFT_SHOULDER}};
        for (auto& k : keys)
            if (key(k.vk)) g.wButtons |= k.button;
        g.sThumbLX = (SHORT)((key('D') ? 32767 : 0) - (key('A') ? 32767 : 0));
        g.sThumbLY = (SHORT)((key('W') ? 32767 : 0) - (key('S') ? 32767 : 0));
        g.sThumbRX = (SHORT)((key('L') ? 32767 : 0) - (key('J') ? 32767 : 0));
        g.sThumbRY = (SHORT)((key('I') ? 32767 : 0) - (key('K') ? 32767 : 0));
        g.bLeftTrigger = key(VK_TAB) ? 255 : 0;
        g.bRightTrigger = key('R') ? 255 : 0;
    }
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
    if (!p.connected && now < p.next_probe_ms) {
        if (port == 0 && g_virtual) {
            if (out) virtual_state(out);
            return true;
        }
        return false;
    }
    if (XInputGetState(port, &st) != ERROR_SUCCESS) {
        p.connected = false;
        p.next_probe_ms = now + 1000;
        if (port == 0 && g_virtual) {
            if (out) virtual_state(out);
            return true;
        }
        return false;
    }
    p.connected = true;
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
    Pad& p = g_pads[port];
    if (st.dwPacketNumber != p.last.dwPacketNumber) p.packet++;
    p.last = st;
    const XINPUT_GAMEPAD& g = st.Gamepad;
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

// Host-side hooks into the game's Unreal engine code (see config/overrides.txt).
#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <vector>

#include <windows.h>

#include "funcs.h"
#include "kernel.h"
#include "runtime.h"

// Unreal on Xbox uses 16-bit TCHARs.
static std::string read_tchar(u32 p) {
    std::string s;
    if (!p) return "(null)";
    for (u16 ch; s.size() < 4096 && (ch = rd16(p)) != 0; p += 2) s += ch < 0x80 ? (char)ch : '?';
    return s;
}

// FOutputDeviceError::Serialize(const TCHAR* Msg, EName Event), thiscall.
// Print the fatal error before the engine reports it and throws.
void hle_unreal_error_serialize(Ctx* c) {
    log_msg("[unreal] appError: %s", read_tchar(rd32(c->esp + 4)).c_str());
    f_00173A90(c);
}


// Test hook: NL_EXEC="seconds:command;..." runs console commands (god, ghost,
// "open 05Refinery") that many seconds after the player exists. Each process
// starts the list over, so it also applies after a level change.
namespace {
struct ExecCmd {
    double at;
    std::string text;
};
constexpr u32 VIEWPORT_OUTPUT = 0x28;  // FOutputDevice subobject
constexpr u32 VIEWPORT_EXEC = 0x2C;    // FExec subobject
constexpr u32 PLAYER_ACTOR = 0x30;     // UPlayer::Actor
constexpr u32 XBOX_VIEWPORT_EXEC = 0x16E600;

bool g_in_exec;

std::vector<ExecCmd> parse_exec_list() {
    std::vector<ExecCmd> out;
    const char* e = getenv("NL_EXEC");
    std::string s = e ? e : "";
    for (size_t pos = 0; pos < s.size();) {
        size_t end = s.find(';', pos), colon = s.find(':', pos);
        if (end == std::string::npos) end = s.size();
        if (colon != std::string::npos && colon < end)
            out.push_back({atof(s.substr(pos, colon - pos).c_str()), s.substr(colon + 1, end - colon - 1)});
        pos = end + 1;
    }
    return out;
}

void exec_console(Ctx* c, u32 viewport, const std::string& text) {
    u32 esp = c->esp, ecx = c->ecx, eax = c->eax, edx = c->edx;
    c->esp = (c->esp - 2 * ((u32)text.size() + 1)) & ~3u;
    u32 cmd = c->esp;
    for (size_t i = 0; i <= text.size(); i++) wr16(cmd + 2 * (u32)i, i < text.size() ? (u8)text[i] : 0);
    c->ecx = viewport + VIEWPORT_EXEC;
    g_in_exec = true;
    u32 handled = guest_call(c, XBOX_VIEWPORT_EXEC, {cmd, viewport + VIEWPORT_OUTPUT}, true);
    g_in_exec = false;
    log_msg("[unreal] exec \"%s\" -> %u", text.c_str(), handled);
    c->esp = esp, c->ecx = ecx, c->eax = eax, c->edx = edx;
}
}  // namespace

// Test hook: NL_TOUR=seconds puts the player on the next navigation point or
// trigger of the level every so many seconds, in a fixed random order, so a
// run reaches the whole level and sets off its scripts.
namespace {
constexpr u32 NAMES_DATA = 0x381030;   // FName::Names.Data
constexpr u32 NAME_TEXT = 0xC;         // FNameEntry::Name
constexpr u32 OBJ_OUTER = 0x18;
constexpr u32 OBJ_NAME = 0x20;
constexpr u32 OBJ_CLASS = 0x24;
constexpr u32 FIELD_SUPER = 0x28;
constexpr u32 STRUCT_CHILDREN = 0x38;
constexpr u32 FIELD_NEXT = 0x2C;
constexpr u32 PROPERTY_OFFSET = 0x48;
constexpr u32 LEVEL_ACTORS = 0x28;     // TTransArray: data, num
constexpr u32 ACTOR_XLEVEL = 0x1FC;
constexpr u32 ACTOR_LOCATION = 0x26C;
constexpr u32 ACTOR_COL_LOCATION = 0x2A0;
constexpr u32 ACTOR_FLAGS_3E4 = 0x3E4;
constexpr u32 LEVEL_FAR_MOVE_ACTOR = 0xA4690;
constexpr float TOUR_LIFT = 32.0f;     // above the point, so the player lands on it

std::string obj_name(u32 obj) {
    return read_tchar(rd32(rd32(NAMES_DATA) + 4 * rd32(obj + OBJ_NAME)) + NAME_TEXT);
}

bool is_a(u32 obj, const char* name) {
    for (u32 cl = rd32(obj + OBJ_CLASS); cl; cl = rd32(cl + FIELD_SUPER))
        if (obj_name(cl) == name) return true;
    return false;
}

// Offset of a script property, from the class's field list.
u32 find_property(u32 obj, const char* name) {
    for (u32 cl = rd32(obj + OBJ_CLASS); cl; cl = rd32(cl + FIELD_SUPER))
        for (u32 f = rd32(cl + STRUCT_CHILDREN); f; f = rd32(f + FIELD_NEXT))
            if (obj_name(f) == name) return rd32(f + PROPERTY_OFFSET);
    return 0;
}

struct TourStop {
    u32 actor;
};

std::vector<TourStop> tour_stops(u32 xlevel) {
    std::vector<TourStop> out;
    u32 data = rd32(xlevel + LEVEL_ACTORS);
    u32 num = rd32(xlevel + LEVEL_ACTORS + 4);
    for (u32 i = 0; i < num; i++) {
        u32 a = rd32(data + 4 * i);
        if (a && (is_a(a, "NavigationPoint") || is_a(a, "Trigger"))) out.push_back({a});
    }
    const char* seed = getenv("NL_TOUR_SEED");
    std::mt19937 rng(seed ? atoi(seed) : 1);
    std::shuffle(out.begin(), out.end(), rng);
    return out;
}

void tour_step(Ctx* c, u32 viewport) {
    using Clock = std::chrono::steady_clock;
    static const char* env = getenv("NL_TOUR");
    if (!env) return;
    static double interval = atof(env);
    static Clock::time_point next_time;
    static std::vector<TourStop> stops;
    static size_t next;
    static u32 tour_level;
    static u32 health_offset;
    u32 player = rd32(viewport + PLAYER_ACTOR);
    if (!player) return;
    u32 xlevel = rd32(player + ACTOR_XLEVEL);
    Clock::time_point now = Clock::now();
    if (xlevel != tour_level) {
        tour_level = xlevel;
        stops = tour_stops(xlevel);
        // A failed mission restarts the process; go on where this one stopped.
        const char* pos = getenv("NL_TOUR_POS");
        std::string level = obj_name(rd32(xlevel + OBJ_OUTER)) + ":";
        next = pos && !strncmp(pos, level.c_str(), level.size()) ? strtoul(pos + level.size(), nullptr, 10) : 0;
        next_time = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval));
        health_offset = find_property(player, "Health");
        log_msg("[tour] %s: %u stops, player %s, health at +0x%X", obj_name(rd32(xlevel + OBJ_OUTER)).c_str(),
                (u32)stops.size(), obj_name(player).c_str(), health_offset);
    }
    if (now < next_time || next >= stops.size()) return;
    next_time = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval));
    u32 target = stops[next++].actor;
    std::string pos = obj_name(rd32(xlevel + OBJ_OUTER)) + ":" + std::to_string(next);
    SetEnvironmentVariableA("NL_TOUR_POS", pos.c_str());
    float x = rdf32(target + ACTOR_LOCATION), y = rdf32(target + ACTOR_LOCATION + 4);
    float z = rdf32(target + ACTOR_LOCATION + 8) + TOUR_LIFT;
    u32 esp = c->esp, ecx = c->ecx, eax = c->eax, edx = c->edx;
    c->ecx = xlevel;
    u32 x_bits, y_bits, z_bits;
    memcpy(&x_bits, &x, 4), memcpy(&y_bits, &y, 4), memcpy(&z_bits, &z, 4);
    u32 ok = guest_call(c, LEVEL_FAR_MOVE_ACTOR, {player, x_bits, y_bits, z_bits, 0, 1}, true);
    log_msg("[tour] %u/%u %s (%.0f %.0f %.0f) moved %u health %d", (u32)next, (u32)stops.size(),
            obj_name(target).c_str(), x, y, z, ok, health_offset ? (int)rd32(player + health_offset) : 0);
    c->esp = esp, c->ecx = ecx, c->eax = eax, c->edx = edx;
}
}  // namespace

// NL_TRACE_LOG: the retail build sends its log to a device whose Serialize is
// an empty stub (ret 8), shared with other functions that take two arguments.
// Anything that reads as text is shown.
void hle_log_stub(Ctx* c) {
    static bool trace = getenv("NL_TRACE_LOG") != nullptr;
    if (trace) {
        u32 msg = rd32(c->esp + 4);
        if (msg >= 0x10000 && msg < 0x84000000 && !(msg & 1)) {
            std::string text = read_tchar(msg);
            bool ok = text.size() >= 3;
            for (char ch : text) ok = ok && (u8)ch >= 0x20 && (u8)ch < 0x7F && ch != '?';
            if (ok) log_msg("[log] %s", text.c_str());
        }
    }
    f_00011210(c);
}

// AActor::SetCollision(NewCollideActors, NewBlockActors, NewBlockPlayers),
// thiscall. ULevel::CheckEncroachment (0xA59C9) puts a blocked mover on its
// target location while it pushes the pawn away. A pawn that bumps the door then
// sets off the door's script, which calls this, and the collision hash stops the
// game with "moved without proper hashing": Location is not ColLocation, where
// the actor was hashed. The game has this bug on the console too. Do the update
// at the hashed location; CheckEncroachment puts Location back afterwards.
void hle_actor_set_collision(Ctx* c) {
    u32 actor = c->ecx;
    u32 location[3];
    bool swapped = false;
    for (int i = 0; i < 3; i++) {
        location[i] = rd32(actor + ACTOR_LOCATION + 4 * i);
        swapped |= rdf32(actor + ACTOR_LOCATION + 4 * i) != rdf32(actor + ACTOR_COL_LOCATION + 4 * i);
    }
    swapped = swapped && (rd32(actor + ACTOR_FLAGS_3E4) & 1);  // bCollideActors: it is in the hash
    if (swapped) {
        log_msg("[unreal] SetCollision on %s while it is being moved", obj_name(actor).c_str());
        for (int i = 0; i < 3; i++) wr32(actor + ACTOR_LOCATION + 4 * i, rd32(actor + ACTOR_COL_LOCATION + 4 * i));
    }
    f_0006A540(c);
    if (swapped)
        for (int i = 0; i < 3; i++) wr32(actor + ACTOR_LOCATION + 4 * i, location[i]);
}

// UViewport::Serialize(const TCHAR* Msg, EName Event), thiscall: the output of
// a test command goes to the log.
void hle_viewport_serialize(Ctx* c) {
    if (g_in_exec) log_msg("[unreal]   %s", read_tchar(rd32(c->esp + 4)).c_str());
    f_000827F0(c);
}

// UXboxViewport::UpdateInput(UBOOL Reset), thiscall: once per tick on the game
// thread, a safe place to enter the engine.
// Mouse look: camera events of its own after the controller's, in stick
// units, through UViewport's input event (key, state, delta). JoyU / JoyV are
// what the right stick sends; [pads + 0xE4] is the right stick speed factor.
bool input_mouse_camera(float* x, float* y);
namespace {
constexpr u32 VIEWPORT_PADS = 0x18;
constexpr u32 PADS_RIGHT_SPEED = 0xE4;
constexpr u32 VIEWPORT_INPUT_EVENT = 0x16DEF0;
constexpr u32 IK_JOY_U = 0xE8, IK_JOY_V = 0xE9, IST_AXIS = 4;

void mouse_camera(Ctx* c, u32 viewport) {
    float x, y;
    if (!input_mouse_camera(&x, &y)) return;
    u32 pads = rd32(viewport + VIEWPORT_PADS);
    if (!pads) return;
    u32 speed_bits = rd32(pads + PADS_RIGHT_SPEED);
    float speed;
    memcpy(&speed, &speed_bits, 4);
    u32 esp = c->esp, ecx = c->ecx, eax = c->eax, edx = c->edx;
    const struct {
        u32 key;
        float v;
    } axes[] = {{IK_JOY_U, x * speed}, {IK_JOY_V, y * speed}};
    for (auto& a : axes) {
        if (a.v == 0) continue;
        u32 bits;
        memcpy(&bits, &a.v, 4);
        c->ecx = viewport;
        guest_call(c, VIEWPORT_INPUT_EVENT, {a.key, IST_AXIS, bits}, true);
    }
    c->esp = esp, c->ecx = ecx, c->eax = eax, c->edx = edx;
}
}  // namespace

void hle_viewport_update_input(Ctx* c) {
    using Clock = std::chrono::steady_clock;
    static std::vector<ExecCmd> cmds = parse_exec_list();
    static size_t next;
    static Clock::time_point start;
    static bool started;
    if (next < cmds.size() && rd32(c->ecx + PLAYER_ACTOR)) {
        if (!started) start = Clock::now(), started = true;
        double t = std::chrono::duration<double>(Clock::now() - start).count();
        while (next < cmds.size() && cmds[next].at <= t) exec_console(c, c->ecx, cmds[next++].text);
    }
    tour_step(c, c->ecx);
    u32 viewport = c->ecx;
    f_0016ED00(c);
    mouse_camera(c, viewport);
}

// CRT _JumpToContinuation(void* target, EHRegistrationNode* frame): leave a
// finished catch block. Take the dispatcher's marker frame off the chain, restore
// the frame's esp (saved just below it) and ebp (the frame sits 12 bytes under
// it), and go on at the continuation, which is in an outer guest function.
void hle_JumpToContinuation(Ctx* c) {
    u32 target = rd32(c->esp + 4), frame = rd32(c->esp + 8);
    wr32(c->fs_base, rd32(rd32(c->fs_base)));
    c->esp = rd32(frame - 4);
    c->ebp = frame + 12;
    throw GuestResume{target};
}

// newlegends.ini: display, controller, mouse and keyboard settings.
#include "settings.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>

#include "runtime.h"

static const char* INI_PATH = "newlegends.ini";

static const char* DEFAULT_INI = R"(; New Legends settings. Delete this file to get the defaults back.

[Display]
; Start in fullscreen (1) or in a window (0). Alt+Enter or F11 switches while playing.
Fullscreen=0
; Window size in pixels when not in fullscreen.
WindowWidth=1280
WindowHeight=960
; Picture shape: Original keeps the 4:3 picture with black bars at the sides,
; Stretch fills the whole window or screen.
Aspect=Original
; Smooth=1 scales the picture smoothly, Smooth=0 keeps sharp pixels.
Smooth=1

[Controller]
; Aiming and looking up and down. InvertAim=0: push the stick up to look up.
; InvertAim=1: push up to look down, as the game does on the Xbox.
InvertAim=0

[Mouse]
; Look around with the mouse. The game window keeps the mouse pointer while
; you play; Alt+Tab or clicking another window gives it back.
Enabled=1
; How fast the camera turns for a given mouse movement (1.0 = default).
Sensitivity=1.0
; InvertY=1: move the mouse up to look down.
InvertY=0

[Keyboard]
; Keyboard and mouse buttons work as a controller, together with a real one
; if it is plugged in. The defaults follow the game's own controller layout:
;   A = jump, B = right hand attack, X = left hand attack, Y = parry,
;   Black = next weapon, White = use, Left trigger = aim (hold),
;   Right trigger = camera behind the player, Back = inventory, Start = menu,
;   D-pad left/right = change weapon.
;   Jump + left attack = dash attack, parry + right attack = super attack.
Enabled=1
; Controller input = one or more keys, separated by commas.
; Key names: A-Z, 0-9, F1-F12, Space, Enter, Tab, Backspace, Escape, Shift,
; Ctrl, Alt, Up, Down, Left, Right, Insert, Delete, Home, End, PageUp,
; PageDown, Numpad0-Numpad9, MouseLeft, MouseRight, MouseMiddle, MouseBack,
; MouseForward, WheelUp, WheelDown, or a Windows virtual-key code such as
; 0xBA. Leave empty for no key.
A=Space
B=MouseRight,Backspace
X=MouseLeft
Y=Shift
Black=Q
White=E
LeftTrigger=Ctrl,MouseBack
RightTrigger=MouseMiddle,R
Start=Escape,Enter
Back=Tab
LeftThumb=
RightThumb=
DPadUp=Up
DPadDown=Down
DPadLeft=Left,WheelUp
DPadRight=Right,WheelDown
LeftStickUp=W
LeftStickDown=S
LeftStickLeft=A
LeftStickRight=D
RightStickUp=I
RightStickDown=K
RightStickLeft=J
RightStickRight=L
)";

static const char* CONTROL_NAMES[PAD_CONTROL_COUNT] = {
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Start", "Back", "LeftThumb", "RightThumb",
    "A", "B", "X", "Y", "Black", "White", "LeftTrigger", "RightTrigger",
    "LeftStickUp", "LeftStickDown", "LeftStickLeft", "LeftStickRight",
    "RightStickUp", "RightStickDown", "RightStickLeft", "RightStickRight",
};

static const struct {
    const char* name;
    int vk;
} KEY_NAMES[] = {
    {"Space", VK_SPACE},     {"Enter", VK_RETURN},       {"Tab", VK_TAB},          {"Backspace", VK_BACK},
    {"Escape", VK_ESCAPE},   {"Shift", VK_SHIFT},        {"LShift", VK_LSHIFT},    {"RShift", VK_RSHIFT},
    {"Ctrl", VK_CONTROL},    {"LCtrl", VK_LCONTROL},     {"RCtrl", VK_RCONTROL},   {"Alt", VK_MENU},
    {"Up", VK_UP},           {"Down", VK_DOWN},          {"Left", VK_LEFT},        {"Right", VK_RIGHT},
    {"Insert", VK_INSERT},   {"Delete", VK_DELETE},      {"Home", VK_HOME},        {"End", VK_END},
    {"PageUp", VK_PRIOR},    {"PageDown", VK_NEXT},      {"MouseLeft", VK_LBUTTON}, {"MouseRight", VK_RBUTTON},
    {"MouseMiddle", VK_MBUTTON}, {"MouseBack", VK_XBUTTON1}, {"MouseForward", VK_XBUTTON2},
    {"WheelUp", KEY_WHEEL_UP}, {"WheelDown", KEY_WHEEL_DOWN},
};

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static int key_code(const std::string& name) {
    if (name.size() == 1 && isalnum((unsigned char)name[0])) return toupper((unsigned char)name[0]);
    for (auto& k : KEY_NAMES)
        if (!_stricmp(name.c_str(), k.name)) return k.vk;
    if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3) {
        int n = atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (!_strnicmp(name.c_str(), "Numpad", 6) && name.size() == 7 && isdigit((unsigned char)name[6]))
        return VK_NUMPAD0 + name[6] - '0';
    if (!_strnicmp(name.c_str(), "0x", 2)) return (int)strtol(name.c_str(), nullptr, 16);
    return 0;
}

static bool to_bool(const std::string& v) { return v == "1" || !_stricmp(v.c_str(), "true") || !_stricmp(v.c_str(), "yes"); }

static void apply(Settings& s, const std::string& section, const std::string& key, const std::string& value, int line) {
    const char* k = key.c_str();
    if (!_stricmp(section.c_str(), "Display")) {
        if (!_stricmp(k, "Fullscreen")) return void(s.fullscreen = to_bool(value));
        if (!_stricmp(k, "WindowWidth")) return void(s.window_width = atoi(value.c_str()));
        if (!_stricmp(k, "WindowHeight")) return void(s.window_height = atoi(value.c_str()));
        if (!_stricmp(k, "Smooth")) return void(s.smooth = to_bool(value));
        if (!_stricmp(k, "Aspect")) {
            if (!_stricmp(value.c_str(), "Stretch")) s.aspect = Aspect::Stretch;
            else if (!_stricmp(value.c_str(), "Original")) s.aspect = Aspect::Original;
            else log_msg("settings: line %d: Aspect must be Original or Stretch", line);
            return;
        }
    } else if (!_stricmp(section.c_str(), "Controller")) {
        if (!_stricmp(k, "InvertAim")) return void(s.invert_aim = to_bool(value));
    } else if (!_stricmp(section.c_str(), "Mouse")) {
        if (!_stricmp(k, "Enabled")) return void(s.mouse = to_bool(value));
        if (!_stricmp(k, "InvertY")) return void(s.mouse_invert_y = to_bool(value));
        if (!_stricmp(k, "Sensitivity")) {
            double v = atof(value.c_str());
            if (v > 0 && v <= 20) s.mouse_sensitivity = (float)v;
            else log_msg("settings: line %d: Sensitivity must be between 0 and 20", line);
            return;
        }
    } else if (!_stricmp(section.c_str(), "Keyboard")) {
        if (!_stricmp(k, "Enabled")) return void(s.keyboard = to_bool(value));
        for (int c = 0; c < PAD_CONTROL_COUNT; c++) {
            if (_stricmp(k, CONTROL_NAMES[c])) continue;
            s.keys[c].clear();
            size_t pos = 0;
            while (pos <= value.size()) {
                size_t end = value.find(',', pos);
                std::string name = trim(value.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
                if (!name.empty()) {
                    if (int vk = key_code(name)) s.keys[c].push_back(vk);
                    else log_msg("settings: line %d: unknown key \"%s\"", line, name.c_str());
                }
                if (end == std::string::npos) break;
                pos = end + 1;
            }
            return;
        }
    }
    log_msg("settings: line %d: unknown setting [%s] %s", line, section.c_str(), key.c_str());
}

static void parse(Settings& s, const char* text) {
    std::string section;
    int line_no = 0;
    for (const char* p = text; *p;) {
        const char* nl = strchr(p, '\n');
        std::string line = trim(std::string(p, nl ? nl - p : strlen(p)));
        p = nl ? nl + 1 : p + strlen(p);
        line_no++;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            log_msg("settings: line %d is not key=value", line_no);
            continue;
        }
        apply(s, section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)), line_no);
    }
}

static Settings load() {
    Settings s;
    parse(s, DEFAULT_INI);  // every key gets its default, even if the file lacks it
    std::string text;
    if (FILE* f = fopen(INI_PATH, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
        parse(s, text.c_str());
    } else if (FILE* f = fopen(INI_PATH, "wb")) {
        fputs(DEFAULT_INI, f);
        fclose(f);
        log_msg("settings: wrote defaults to %s", INI_PATH);
    }
    if (s.window_width < 320) s.window_width = 320;
    if (s.window_height < 240) s.window_height = 240;
    // Session state handed down from the process before a level change.
    if (const char* v = getenv("NL_SESSION_FULLSCREEN")) s.fullscreen = *v == '1';
    if (const char* v = getenv("NL_SESSION_WINDOW"))
        sscanf(v, "%d,%d,%d,%d", &s.window_x, &s.window_y, &s.window_width, &s.window_height);
    if (getenv("NL_NO_KEYBOARD")) s.keyboard = s.mouse = false;
    return s;
}

static Settings& mutable_settings() {
    static Settings s = load();
    return s;
}

const Settings& settings() { return mutable_settings(); }

// Child processes inherit the environment, so these carry over a level change.
void settings_set_fullscreen(bool on) {
    mutable_settings().fullscreen = on;
    SetEnvironmentVariableA("NL_SESSION_FULLSCREEN", on ? "1" : "0");
}

void settings_set_window(int x, int y, int w, int h) {
    Settings& s = mutable_settings();
    s.window_x = x, s.window_y = y, s.window_width = w, s.window_height = h;
    char v[64];
    snprintf(v, sizeof v, "%d,%d,%d,%d", x, y, w, h);
    SetEnvironmentVariableA("NL_SESSION_WINDOW", v);
}

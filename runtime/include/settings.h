// Player settings from newlegends.ini (written with defaults on first run).
#pragma once
#include <vector>

enum class Aspect { Original, Stretch };

// Controller inputs the keyboard can drive.
enum PadControl {
    PAD_DPAD_UP, PAD_DPAD_DOWN, PAD_DPAD_LEFT, PAD_DPAD_RIGHT,
    PAD_START, PAD_BACK, PAD_LEFT_THUMB, PAD_RIGHT_THUMB,
    PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BLACK, PAD_WHITE,
    PAD_LEFT_TRIGGER, PAD_RIGHT_TRIGGER,
    PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT,
    PAD_RSTICK_UP, PAD_RSTICK_DOWN, PAD_RSTICK_LEFT, PAD_RSTICK_RIGHT,
    PAD_CONTROL_COUNT
};

struct Settings {
    bool fullscreen = false;
    int window_width = 1280, window_height = 960;
    int window_x = -1, window_y = -1;  // -1: let Windows place it
    Aspect aspect = Aspect::Original;
    bool smooth = true;  // linear filtering when scaling the picture
    bool keyboard = true;
    std::vector<int> keys[PAD_CONTROL_COUNT];  // Windows virtual-key codes
};

// Loaded once, before the window opens. Environment overrides from a running
// session (fullscreen toggle, window position) are applied on top, so a
// level change, which starts a new process, keeps them.
const Settings& settings();
void settings_set_fullscreen(bool on);
void settings_set_window(int x, int y, int w, int h);

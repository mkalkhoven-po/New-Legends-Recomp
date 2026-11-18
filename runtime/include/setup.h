// Game data: checking it, installing it from a disc image or a folder, the
// setup window shown when it is missing, and shortcuts.
#pragma once
#include <atomic>
#include <filesystem>
#include <string>

// Empty when the folder holds the right game files, else what is wrong, in
// words for the player.
std::string game_data_problem(const std::string& xbe_path);

struct InstallProgress {
    std::atomic<unsigned long long> done{0}, total{0};
    std::atomic<bool> cancel{false};
    std::atomic<int> stage{0};  // 0 checking, 1 copying, 2 finishing
};

// Copies the game files from a disc image (.iso / .xiso, trimmed or full) or
// a folder into "game" in the current folder. An existing "game" folder with
// the wrong files is kept as "game.old". False with *error set on failure.
bool install_game_files(const std::filesystem::path& source, InstallProgress& progress, std::string* error);

// Desktop shortcut and Start menu entry for this program.
bool create_shortcuts(bool desktop, bool start_menu, std::string* error);

// The setup window. `problem` is shown when not empty; `files_ok` means the
// game files are already fine (opened with --setup). True: start the game.
bool run_setup_window(const std::string& problem, bool files_ok);

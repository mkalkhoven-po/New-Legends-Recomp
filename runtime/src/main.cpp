#include <windows.h>
#include <shellapi.h>

#include <stdio.h>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "func_table.h"
#include "gpu.h"
#include "kernel.h"
#include "setup.h"
#include "version.h"

// Return address pushed for the boot thread; reaching it means the XBE entry
// point returned to the "kernel".
constexpr u32 BOOT_RETURN = 0xFE00FFF0u;

void crash_init();
[[noreturn]] void show_user_error(const char* text);
bool host_interactive();

static bool file_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The game files live in a "game" folder next to the exe (or in the current
// folder, the developer layout). Settings, saves and caches go there too.
static void find_home() {
    if (file_exists("game/default.xbe")) return;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string dir(exe);
    dir.resize(dir.find_last_of("\\/") + 1);
    SetCurrentDirectoryA(dir.c_str());
}

static const char* MISSING_TEXT =
    "The game files were not found.\n\n"
    "Start newlegends.exe by double-clicking it to set them up from your disc image or a folder, or run "
    "newlegends.exe --install <disc image or folder>. See README.md.";

// newlegends.exe --install <image or folder>: setup without the window.
static int install_from_command_line() {
    int n = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (n < 3) {
        fprintf(stderr, "Usage: newlegends.exe --install <disc image (.iso, .xiso) or folder with the game files>\n");
        return 2;
    }
    std::filesystem::path source(wargv[2]);
    InstallProgress progress;
    std::string error;
    bool ok = false, done = false;
    std::thread worker([&] {
        ok = install_game_files(source, progress, &error);
        done = true;
    });
    int last = -1;
    while (!done) {
        Sleep(200);
        unsigned long long total = progress.total;
        int pct = total ? (int)(progress.done * 100 / total) : 0;
        if (progress.stage == 1 && pct / 10 != last / 10) {
            printf("Copying the game files: %d%%\n", pct);
            fflush(stdout);
            last = pct;
        }
    }
    worker.join();
    if (!ok) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    printf("The game files are set up.\n");
    return 0;
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    bool setup_flag = mode == "--setup", install_flag = mode == "--install";
    bool dev_path = argc > 1 && !setup_flag && !install_flag;  // developers: the xbe to run
    if (!dev_path) find_home();
    std::string xbe_path = dev_path ? argv[1] : "game/default.xbe";
    std::string game_dir = xbe_path.substr(0, xbe_path.find_last_of("/\\") + 1);
    if (game_dir.empty()) game_dir = ".";

    crash_init();
    log_msg("%s %s", NL_PRODUCT_NAME, NL_VERSION_STRING);
    if (install_flag) return install_from_command_line();
    // Missing or wrong game files: the setup window, when someone is there to
    // see it (not for test runs with their output in a file).
    bool missing = !file_exists(xbe_path);
    std::string problem = game_data_problem(xbe_path);
    if (missing || !problem.empty() || setup_flag) {
        if (dev_path || !host_interactive()) show_user_error(missing ? MISSING_TEXT : problem.c_str());
        if (!run_setup_window(problem, !missing && problem.empty())) return 0;
        missing = !file_exists(xbe_path);
        problem = game_data_problem(xbe_path);
        if (missing || !problem.empty()) show_user_error(missing ? MISSING_TEXT : problem.c_str());
    }
    mem_init();
    XbeInfo xbe = xbe_load(xbe_path.c_str());
    if (xbe.entry != g_entry_point)
        fatal("%s has entry %08X but the recompiled code expects %08X", xbe_path.c_str(), xbe.entry, g_entry_point);
    dispatch_init();
    kernel_init(xbe);
    file_init(game_dir.c_str());

    // The boot thread runs the XBE entry point the way the kernel does after
    // loading a title; XAPI then starts the game's main thread from there.
    GuestThread* boot = thread_create(0x10000, 0);
    gil_lock();
    thread_bind(boot);
    kernel_start_services();
    video_init();

    GuestFunc entry = dispatch_lookup(xbe.entry);
    if (!entry) fatal("no recompiled function at entry %08X", xbe.entry);
    log_msg("starting guest at %08X", xbe.entry);
    Ctx* c = &boot->ctx;
    GUEST_CALL(entry, BOOT_RETURN);
    // Only the boot thread ends here; the game runs on the threads it made.
    log_msg("boot thread finished (eax %08X)", c->eax);
    gil_unlock();
    ExitThread(0);
}

#include <windows.h>

#include <string>

#include "func_table.h"
#include "gpu.h"
#include "kernel.h"

// Return address pushed for the boot thread; reaching it means the XBE entry
// point returned to the "kernel".
constexpr u32 BOOT_RETURN = 0xFE00FFF0u;

void crash_init();

int main(int argc, char** argv) {
    std::string xbe_path = argc > 1 ? argv[1] : "game/default.xbe";
    std::string game_dir = xbe_path.substr(0, xbe_path.find_last_of("/\\") + 1);
    if (game_dir.empty()) game_dir = ".";

    crash_init();
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

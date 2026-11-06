// Host-side runtime services shared by the runtime modules.
#pragma once
#include <stdio.h>

#include "x86_recomp.h"

// ------------------------------------------------------------------ logging
void log_msg(const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);

// ------------------------------------------------------------------- memory
// Guest physical RAM (64 MB, as on retail hardware) at 0x00000000, and a
// kernel-private area for structures the HLE kernel hands to the game.
constexpr u32 RAM_SIZE = 64u << 20;
constexpr u32 KERNEL_AREA = 0x8F000000u;
constexpr u32 KERNEL_AREA_SIZE = 16u << 20;

void mem_init();
// Kernel-area bump allocator for KPCR/KTHREAD/objects; zeroed, 16-aligned.
u32 kernel_alloc(u32 size);

// ------------------------------------------------------------------- loader
struct XbeInfo {
    u32 base, entry, kernel_thunk, tls_dir, stack_commit;
};
XbeInfo xbe_load(const char* path);

// --------------------------------------------------------------- dispatch
void dispatch_init();
GuestFunc dispatch_lookup(u32 addr);
// Print likely return addresses from the guest stack and the registers.
void guest_backtrace(Ctx* c);
// Thrown (host C++) to leave nested guest calls for the guest frame a catch
// block continues in. esp and ebp already hold that frame's values.
struct GuestResume {
    u32 target;
};

// Kernel import thunks are patched to KERNEL_THUNK_BASE | ordinal; an
// indirect call to such a value lands in the HLE kernel.
constexpr u32 KERNEL_THUNK_BASE = 0xFE000000u;
void kernel_init(const XbeInfo& xbe);
void kernel_call(Ctx* c, u32 ordinal);
// Re-copy one section from the XBE file (XeLoadSection after an unload).
void xbe_reload_section(u32 header);

// ------------------------------------------------------------------ kernel
void kernel_start_services();
void file_init(const char* game_dir);

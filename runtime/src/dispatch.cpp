// Address -> recompiled function lookup for indirect calls and jumps.
#include <mutex>
#include <unordered_set>

#include "func_table.h"
#include "runtime.h"

static u32 g_lo, g_hi;
static GuestFunc* g_lookup;  // dense table over [g_lo, g_hi)

void dispatch_init() {
    g_lo = 0xFFFFFFFFu;
    g_hi = 0;
    for (unsigned i = 0; i < g_func_count; i++) {
        if (g_func_table[i].addr < g_lo) g_lo = g_func_table[i].addr;
        if (g_func_table[i].addr >= g_hi) g_hi = g_func_table[i].addr + 1;
    }
    g_lookup = (GuestFunc*)calloc(g_hi - g_lo, sizeof(GuestFunc));
    for (unsigned i = 0; i < g_func_count; i++) g_lookup[g_func_table[i].addr - g_lo] = g_func_table[i].fn;
    log_msg("dispatch: %u functions in %08X-%08X", g_func_count, g_lo, g_hi);
}

GuestFunc dispatch_lookup(u32 addr) {
    if (addr < g_lo || addr >= g_hi) return nullptr;
    return g_lookup[addr - g_lo];
}

// Likely return addresses on the guest stack: values in the code range that
// directly follow a call instruction. Good enough to see who called whom.
void guest_backtrace(Ctx* c) {
    fputs("guest stack (likely return addresses):", stderr);
    int n = 0;
    for (u32 a = c->esp; a < c->esp + 0x10000 && n < 24; a += 4) {
        u32 v = rd32(a);
        if (v < g_lo + 7 || v >= g_hi + 0x100) continue;
        bool call = rd8(v - 5) == 0xE8 || (rd8(v - 2) == 0xFF && (rd8(v - 1) & 0x38) == 0x10) ||
                    (rd8(v - 3) == 0xFF && (rd8(v - 2) & 0x38) == 0x10) ||
                    (rd8(v - 6) == 0xFF && (rd8(v - 5) & 0x38) == 0x10) ||
                    (rd8(v - 7) == 0xFF && (rd8(v - 6) & 0x38) == 0x10);
        if (!call) continue;
        fprintf(stderr, "%s %08X@+%X", n % 6 ? "" : "\n   ", v, a - c->esp);
        n++;
    }
    fprintf(stderr, "\n  eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X\n", c->eax, c->ecx,
            c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
}

void rt_icall(Ctx* c, u32 target) {
    if (GuestFunc fn = dispatch_lookup(target)) return fn(c);
    if ((target & 0xFFFF0000u) == KERNEL_THUNK_BASE) return kernel_call(c, target & 0xFFFF);
    guest_backtrace(c);
    fatal("indirect call to unknown address %08X (return address %08X, esp %08X)", target, rd32(c->esp), c->esp);
}

// Guest code is nested C calls, so continuing in an outer guest frame means
// unwinding the host stack to it. The guest stack grows down: the frame the
// catch resumes in is the innermost one whose entry esp is above the
// continuation's esp; deeper instances let the exception pass. What is left
// of the function runs as its own entry, including its return.
void rt_eh_frame(Ctx* c, GuestFunc body) {
    u32 entry_esp = c->esp, target = 0;
    for (;;) {
        try {
            if (target) rt_icall(c, target);
            else body(c);
            return;
        } catch (const GuestResume& r) {
            if (c->esp >= entry_esp) throw;
            target = r.target;
        }
    }
}

// _except_handler3 calls the block and never gets it back: the block starts
// by restoring esp, saved 8 bytes below the frame; ebp sits 16 above it.
void rt_except_resume(Ctx* c, u32 target, u32 frame) {
    c->esp = rd32(frame - 8);
    c->ebp = frame + 16;
    throw GuestResume{target};
}

void rt_bad_jump(Ctx* c, u32 from, u32 target) {
    guest_backtrace(c);
    fatal("jump table at %08X went to unknown target %08X", from, target);
}

void rt_trap(Ctx* c, u32 addr) {
    guest_backtrace(c);
    fatal("reached code at %08X that analysis marked unreachable (esp %08X)", addr, c->esp);
}

void rt_unimpl(Ctx* c, u32 addr, const char* text) {
    static std::mutex lock;
    static std::unordered_set<u32> seen;
    std::lock_guard<std::mutex> g(lock);
    if (seen.insert(addr).second) log_msg("unimplemented instruction at %08X: %s", addr, text);
}

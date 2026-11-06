// Crash reporting: turn a host exception inside recompiled code into guest
// terms (guest fault address, the chain of f_XXXXXXXX functions, registers).
#include <windows.h>

#include <dbghelp.h>

#include <mutex>
#include <vector>

#include "kernel.h"

static const char* describe(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW: return "host stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    default: return "exception";
    }
}

static void print_host_stack(CONTEXT* ctx, HANDLE thread = GetCurrentThread()) {
    HANDLE proc = GetCurrentProcess();
    CONTEXT c = *ctx;
    STACKFRAME64 f{};
    f.AddrPC.Offset = c.Rip;
    f.AddrPC.Mode = AddrModeFlat;
    f.AddrStack.Offset = c.Rsp;
    f.AddrStack.Mode = AddrModeFlat;
    f.AddrFrame.Offset = c.Rbp;
    f.AddrFrame.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = (SYMBOL_INFO*)buf;
    fputs("host call stack (innermost first):\n", stderr);
    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, &c, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            !f.AddrPC.Offset)
            break;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, f.AddrPC.Offset, &disp, sym))
            fprintf(stderr, "  %s+0x%llX\n", sym->Name, (unsigned long long)disp);
        else
            fprintf(stderr, "  %016llX\n", (unsigned long long)f.AddrPC.Offset);
    }
}

// The game reads past the end of allocations (a texture whose data is smaller
// than its header claims). On the console the next pages are usually mapped;
// here a block can end at the edge of the address space in use. Give reads in
// the virtual allocation range a zero page and carry on.
constexpr u32 VIRT_LO = 0x10000000u, VIRT_HI = 0x80000000u, PAGE_MASK = 0xFFFu;
static bool map_overread(EXCEPTION_RECORD* r) {
    u64 addr = r->ExceptionInformation[1], base = (u64)GUEST_BASE;
    if (r->ExceptionInformation[0] != 0 || addr < base + VIRT_LO || addr >= base + VIRT_HI) return false;
    if (!VirtualAlloc((void*)(addr & ~(u64)PAGE_MASK), PAGE_MASK + 1, MEM_COMMIT, PAGE_READWRITE)) return false;
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 8) log_msg("[memory] guest read past allocations at %08llX: mapped a zero page", addr - base);
    return true;
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Only fatal hardware exceptions; C++/debugger exceptions pass through.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_INT_OVERFLOW && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION && map_overread(ep->ExceptionRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    static LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;

    fprintf(stderr, "\n*** host %s (%08lX)", describe(code), code);
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        u64 addr = ep->ExceptionRecord->ExceptionInformation[1];
        const char* what = kind == 0 ? "read" : kind == 1 ? "write" : "execute";
        u64 base = (u64)GUEST_BASE;
        if (addr >= base && addr < base + 0x100000000ull)
            fprintf(stderr, ": guest %s of %08llX", what, addr - base);
        else
            fprintf(stderr, ": host %s of %016llX", what, addr);
    }
    fputc('\n', stderr);
    print_host_stack(ep->ContextRecord);
    if (GuestThread* t = cur_thread()) {
        fprintf(stderr, "guest thread %u\n", t->id);
        guest_backtrace(&t->ctx);
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Host threads running guest code, for the watchdog dump.
static std::mutex g_threads_m;
static std::vector<std::pair<u32, HANDLE>> g_threads;

void crash_register_thread(u32 id) {
    HANDLE h;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard<std::mutex> lk(g_threads_m);
    g_threads.push_back({id, h});
}

// Print where every guest thread is (host stack with f_XXXXXXXX frames).
static void dump_threads() {
    std::lock_guard<std::mutex> lk(g_threads_m);
    for (auto& t : g_threads) {
        if (SuspendThread(t.second) == (DWORD)-1) continue;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t.second, &c)) {
            fprintf(stderr, "=== guest thread %u\n", t.first);
            print_host_stack(&c, t.second);
        }
        ResumeThread(t.second);
    }
    fflush(stderr);
}

// NL_WATCHDOG=seconds: when no frame was presented for that long, dump all
// guest threads, to see where a hang sits.
u32 vkb_frame_no();
static volatile bool g_watchdog_off;
void crash_watchdog_stop() { g_watchdog_off = true; }

static DWORD WINAPI watchdog(void* p) {
    DWORD ms = (DWORD)(uintptr_t)p;
    u32 last = ~0u;
    for (;;) {
        Sleep(ms);
        u32 now = vkb_frame_no();
        if (g_watchdog_off) return 0;
        if (now != last) {
            last = now;
            continue;
        }
        fprintf(stderr, "\n*** watchdog: no frames for %lu s (frame %u)\n", ms / 1000, now);
        dump_threads();
    }
}

void crash_init() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, on_exception);
    if (const char* w = getenv("NL_WATCHDOG"))
        CloseHandle(CreateThread(nullptr, 0, watchdog, (void*)(uintptr_t)(atoi(w) * 1000), 0, nullptr));
}

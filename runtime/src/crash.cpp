// Crash reporting: turn a host exception inside recompiled code into guest
// terms (guest fault address, the chain of f_XXXXXXXX functions, registers).
#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
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

// Hash of the innermost host return addresses, to tell call paths apart.
static u64 stack_key(CONTEXT* ctx) {
    CONTEXT c = *ctx;
    STACKFRAME64 f{};
    f.AddrPC.Offset = c.Rip;
    f.AddrPC.Mode = AddrModeFlat;
    f.AddrStack.Offset = c.Rsp;
    f.AddrStack.Mode = AddrModeFlat;
    f.AddrFrame.Offset = c.Rbp;
    f.AddrFrame.Mode = AddrModeFlat;
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < 8; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &f, &c, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            !f.AddrPC.Offset)
            break;
        h = (h ^ f.AddrPC.Offset) * 1099511628211ull;
    }
    return h;
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

static bool g_overread_stack = getenv("NL_OVERREAD_STACK") != nullptr;
u32 vkb_frame_no();


// Raw guest stack words, to find a function's arguments while debugging.
static void dump_guest_stack() {
    GuestThread* t = cur_thread();
    if (!t) return;
    u32 sp = t->ctx.esp;
    for (u32 i = 0; i < 0x800; i += 32)
        log_msg("  stack %08X: %08X %08X %08X %08X %08X %08X %08X %08X", sp + i, rd32(sp + i), rd32(sp + i + 4),
                rd32(sp + i + 8), rd32(sp + i + 12), rd32(sp + i + 16), rd32(sp + i + 20), rd32(sp + i + 24), rd32(sp + i + 28));
}

// NL_WATCH_WRITE=addr (hex): once that guest page is allocated, every write
// to it logs the writer's call stack the first time that stack is seen
// (debugging; the page is re-protected after each write by single-stepping).
static u32 g_watch_page;
static bool g_watch_rearm;

static void watch_protect(DWORD prot) {
    DWORD old;
    VirtualProtect(gptr(g_watch_page), PAGE_MASK + 1, prot, &old);
    // Physical RAM has a second view: 0x80000000 and the write-combined 0xF0000000.
    if (g_watch_page >= 0x80000000u && g_watch_page < 0x84000000u)
        VirtualProtect(gptr(g_watch_page + 0x70000000u), PAGE_MASK + 1, prot, &old);
}

void crash_watch_page(u32 a, u32 size) {
    static u32 watch = getenv("NL_WATCH_WRITE") ? (u32)strtoul(getenv("NL_WATCH_WRITE"), nullptr, 16) : 0;
    if (!watch || watch < a || watch >= a + size) return;
    g_watch_page = watch & ~PAGE_MASK;
    watch_protect(PAGE_READONLY);
    log_msg("[watch] page %08X protected (allocation %08X+%X)", g_watch_page, a, size);
}

static bool watch_hit(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && g_watch_rearm) {
        g_watch_rearm = false;
        watch_protect(PAGE_READONLY);
        return true;
    }
    u64 addr = r->ExceptionInformation[1], base = (u64)GUEST_BASE;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_watch_page || r->ExceptionInformation[0] != 1) return false;
    u32 g = (u32)(addr - base);
    if ((g & ~PAGE_MASK) != g_watch_page && (g & ~PAGE_MASK) != g_watch_page + 0x70000000u) return false;
    watch_protect(PAGE_READWRITE);
    g_watch_rearm = true;
    ep->ContextRecord->EFlags |= 0x100;  // trap after this instruction to re-protect
    // Log each distinct host call stack once.
    static std::vector<u64> seen;
    static u32 watch_lo = getenv("NL_WATCH_WRITE") ? (u32)strtoul(getenv("NL_WATCH_WRITE"), nullptr, 16) : 0;
    if ((g & 0x0FFFFFFF) < (watch_lo & 0x0FFFFFFF)) return true;  // only at or above the watched address
    u64 key = stack_key(ep->ContextRecord);
    for (u64 k : seen)
        if (k == key) return true;
    if (seen.size() >= 40) return true;
    seen.push_back(key);
    // NL_WATCH_DUMP=addr:len (hex): save that guest memory at the first hit.
    static const char* dump = getenv("NL_WATCH_DUMP");
    if (dump) {
        u32 da = 0, dl = 0;
        sscanf(dump, "%x:%x", &da, &dl);
        dump = nullptr;
        if (FILE* f = fopen("watch_dump.bin", "wb")) {
            fwrite(gptr(da), 1, dl, f);
            fclose(f);
        }
    }
    // NL_WATCH_FIND=file: at the first hit, list where the middle 1 KB of that
    // file's bytes sits in committed guest memory.
    static const char* find = getenv("NL_WATCH_FIND");
    if (find) {
        std::vector<u8> pat;
        if (FILE* f = fopen(find, "rb")) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            pat.resize(n);
            fseek(f, 0, SEEK_SET);
            fread(pat.data(), 1, n, f);
            fclose(f);
        }
        find = nullptr;
        if (pat.size() >= 0x800) {
            const u8* needle = pat.data() + pat.size() / 2;
            u64 a = (u64)GUEST_BASE + 0x10000000u, end = (u64)GUEST_BASE + 0x90000000u;
            while (a < end) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
                u64 next = (u64)mbi.BaseAddress + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
                    const u8* p = (const u8*)mbi.BaseAddress;
                    for (u64 i = 0; i + 0x400 <= mbi.RegionSize; i += 2)
                        if (p[i] == needle[0] && !memcmp(p + i, needle, 0x400))
                            log_msg("[watch] pattern (middle of %s) at %08llX", getenv("NL_WATCH_FIND"),
                                    (u64)(p + i) - (u64)GUEST_BASE - pat.size() / 2);
                }
                a = next;
            }
        }
    }
    fprintf(stderr, "[watch] write to %08X (frame %u)\n", g, vkb_frame_no());
    print_host_stack(ep->ContextRecord);
    dump_guest_stack();
    fflush(stderr);
    return true;
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_SINGLE_STEP) && watch_hit(ep))
        return EXCEPTION_CONTINUE_EXECUTION;
    // Only fatal hardware exceptions; C++/debugger exceptions pass through.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_INT_OVERFLOW && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION && map_overread(ep->ExceptionRecord)) {
        if (g_overread_stack) {
            g_overread_stack = false;
            print_host_stack(ep->ContextRecord, GetCurrentThread());
            dump_guest_stack();
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
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

// NL_PROFILE=id: sample that thread (1000 = the GPU/FIFO thread, otherwise a
// guest thread id) about 1000 times a second and every 10 s print the
// functions it spends the most time in: "self" counts the function the thread
// was in, "total" also counts the functions that called it.
static DWORD WINAPI profiler(void* p) {
    u32 id = (u32)(uintptr_t)p;
    HANDLE th = nullptr;
    while (!th) {
        Sleep(100);
        std::lock_guard<std::mutex> lk(g_threads_m);
        for (auto& t : g_threads)
            if (t.first == id) th = t.second;
    }
    timeBeginPeriod(1);
    HANDLE proc = GetCurrentProcess();
    std::unordered_map<DWORD64, u32> self, total;  // by function start address
    u32 samples = 0;
    DWORD last_report = GetTickCount();
    auto func_of = [&](DWORD64 pc) -> DWORD64 {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        return SymFromAddr(proc, pc, &disp, sym) ? sym->Address : pc;
    };
    for (;;) {
        Sleep(1);
        if (SuspendThread(th) == (DWORD)-1) return 0;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        DWORD64 pcs[16];
        int n = 0;
        if (GetThreadContext(th, &c)) {
            STACKFRAME64 f{};
            f.AddrPC.Offset = c.Rip;
            f.AddrStack.Offset = c.Rsp;
            f.AddrFrame.Offset = c.Rbp;
            f.AddrPC.Mode = f.AddrStack.Mode = f.AddrFrame.Mode = AddrModeFlat;
            while (n < 16 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &f, &c, nullptr, SymFunctionTableAccess64,
                                         SymGetModuleBase64, nullptr) &&
                   f.AddrPC.Offset)
                pcs[n++] = f.AddrPC.Offset;
        }
        ResumeThread(th);
        if (!n) continue;
        samples++;
        DWORD64 seen[16];
        int ns = 0;
        for (int i = 0; i < n; i++) {
            DWORD64 fn = func_of(pcs[i]);
            if (i == 0) self[fn]++;
            if (std::find(seen, seen + ns, fn) == seen + ns) {
                seen[ns++] = fn;
                total[fn]++;
            }
        }
        if (GetTickCount() - last_report < 10000) continue;
        last_report = GetTickCount();
        auto print = [&](const char* title, std::unordered_map<DWORD64, u32>& m) {
            std::vector<std::pair<u32, DWORD64>> v;
            for (auto& e : m) v.push_back({e.second, e.first});
            std::sort(v.rbegin(), v.rend());
            fprintf(stderr, "--- profile thread %u, %u samples, %s:\n", id, samples, title);
            for (size_t i = 0; i < v.size() && i < 25; i++) {
                char buf[sizeof(SYMBOL_INFO) + 256];
                SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                const char* name = SymFromAddr(proc, v[i].second, &disp, sym) ? sym->Name : "?";
                fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * v[i].first / samples, name);
            }
        };
        print("self", self);
        print("total", total);
        fflush(stderr);
        self.clear();
        total.clear();
        samples = 0;
    }
}

void crash_init() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, on_exception);
    if (const char* p = getenv("NL_PROFILE"))
        CloseHandle(CreateThread(nullptr, 0, profiler, (void*)(uintptr_t)atoi(p), 0, nullptr));
    if (const char* w = getenv("NL_WATCHDOG"))
        CloseHandle(CreateThread(nullptr, 0, watchdog, (void*)(uintptr_t)(atoi(w) * 1000), 0, nullptr));
}

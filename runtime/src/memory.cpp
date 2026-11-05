#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

#include <mutex>

#include "runtime.h"

void log_msg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(1);
}

static void commit(u32 addr, u32 size) {
    if (!VirtualAlloc(GUEST_BASE + addr, size, MEM_COMMIT, PAGE_READWRITE))
        fatal("cannot commit guest memory %08X+%X (error %lu)", addr, size, GetLastError());
}

// Physical RAM as the contiguous (cached) mapping at 0x80000000 and the
// write-combined alias at 0xF0000000: two views of one section.
constexpr u32 PHYS_MAP = 0x80000000u, PHYS_MAP_WC = 0xF0000000u, PHYS_SIZE = 64u << 20;

static void* place(u64 off, u64 size) {
    // Carve [off, off+size) out of the placeholder reservation.
    if (!VirtualFree(GUEST_BASE + off, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        fatal("cannot split guest placeholder at %llX (error %lu)", off, GetLastError());
    return GUEST_BASE + off;
}

// Turn one of the remaining placeholders into an ordinary reservation.
static void reserve_plain(u64 off, u64 size) {
    if (!VirtualAlloc2(nullptr, GUEST_BASE + off, size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0))
        fatal("cannot reserve guest range %llX+%llX (error %lu)", off, size, GetLastError());
}

static void map_phys(HANDLE section, u64 off) {
    place(off, PHYS_SIZE);
    if (!MapViewOfFile3(section, nullptr, GUEST_BASE + off, 0, PHYS_SIZE, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
        fatal("cannot map physical RAM at guest %llX (error %lu)", off, GetLastError());
}

void mem_init() {
    // Reserve the whole 32-bit guest space at a fixed host address so the
    // generated code can address it as GUEST_BASE + guest address. It starts
    // as a placeholder so the physical RAM views can be placed inside it.
    void* p = VirtualAlloc2(nullptr, GUEST_BASE, 0x100000000ull, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                            nullptr, 0);
    if (p != GUEST_BASE) fatal("cannot reserve 4 GB of guest address space at %p (error %lu)", GUEST_BASE, GetLastError());
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, PHYS_SIZE, nullptr);
    if (!section) fatal("cannot create physical RAM section (error %lu)", GetLastError());
    map_phys(section, PHYS_MAP);
    map_phys(section, PHYS_MAP_WC);
    reserve_plain(0, PHYS_MAP);
    reserve_plain(PHYS_MAP + PHYS_SIZE, PHYS_MAP_WC - PHYS_MAP - PHYS_SIZE);
    reserve_plain(PHYS_MAP_WC + PHYS_SIZE, 0x100000000ull - PHYS_MAP_WC - PHYS_SIZE);
    commit(0, RAM_SIZE);
    commit(KERNEL_AREA, KERNEL_AREA_SIZE);
}

static std::mutex g_kernel_alloc_lock;
static u32 g_kernel_next = KERNEL_AREA;

u32 kernel_alloc(u32 size) {
    std::lock_guard<std::mutex> lock(g_kernel_alloc_lock);
    u32 a = g_kernel_next;
    g_kernel_next += (size + 15) & ~15u;
    if (g_kernel_next > KERNEL_AREA + KERNEL_AREA_SIZE) fatal("kernel area exhausted");
    memset(gptr(a), 0, size);
    return a;
}

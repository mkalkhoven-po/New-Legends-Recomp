// NV2A register file. The XDK's D3D library programs the GPU directly; this
// models the registers it touches: a plain store for configuration
// registers, live values for status/timer registers, and the PFIFO/PGRAPH
// side effects that drive the command processor (pfifo.cpp).
#include <windows.h>

#include <mutex>

#include "gpu.h"

constexpr u32 NV2A_BASE = 0xFD000000u;
constexpr double NV2A_CORE_HZ = 233333333.0;
static u32* g_regs;  // 16 MB register space, one u32 per word
static bool g_trace;
std::recursive_mutex g_nv_lock;

u32& nv_reg(u32 off) { return g_regs[(off & 0xFFFFFF) >> 2]; }

// PRAMIN (instance memory) is the top MB of RAM, also reachable through
// the 0xFD700000 window.
u8* ramin_ptr(u32 off) { return gptr(RAMIN_BASE + (off & 0xFFFFF)); }

static u64 timer_ns() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)((double)t.QuadPart * (1e9 / (double)f.QuadPart));
}

// PTIMER counts core clocks scaled by DENOMINATOR/NUMERATOR, in units of 32
// (the low 5 bits read as zero). The driver sets it, so keep an offset.
static s64 g_ptimer_offset;
static u64 ptimer_now() {
    u32 num = nv_reg(NV_PTIMER_NUMERATOR) ? nv_reg(NV_PTIMER_NUMERATOR) : 1;
    double clocks = (double)timer_ns() * (NV2A_CORE_HZ / 1e9) * nv_reg(NV_PTIMER_DENOMINATOR) / num;
    return (((u64)clocks << 5) + g_ptimer_offset) & 0x1FFFFFFFFFFFFFFFull;
}
static void ptimer_set(u64 v) { g_ptimer_offset += (s64)(v - ptimer_now()); }

void nv2a_init() {
    g_regs = (u32*)calloc(1, 16u << 20);
    g_trace = getenv("NL_TRACE_MMIO") != nullptr;
    nv_reg(NV_PMC_BOOT_0) = 0x02A000A1;            // NV2A, revision A1
    nv_reg(NV_PBUS_PCI + 0x00) = 0x02A010DE;       // PCI vendor/device
    nv_reg(NV_PBUS_PCI + 0x08) = 0x030000A1;       // VGA class, revision
    nv_reg(NV_PFB_CSTATUS) = RAM_SIZE;
    nv_reg(NV_PFB_CFG0) = 0x03070103;
    nv_reg(NV_PFIFO_CACHE1_STATUS) = 1 << 4;       // LOW_MARK: empty
    nv_reg(NV_PFIFO_RUNOUT_STATUS) = 1 << 4;       // LOW_MARK: empty
    nv_reg(NV_PTIMER_NUMERATOR) = 1;
    nv_reg(NV_PTIMER_DENOMINATOR) = 1;
    nv_reg(NV_PRAMDAC_NVPLL_COEFF) = 0x00011C01;   // 233 MHz core
    nv_reg(NV_PRAMDAC_MPLL_COEFF) = 0x00007702;
    nv_reg(NV_PRAMDAC_VPLL_COEFF) = 0x0003C20D;
    pfifo_init();
}

static u32 read_reg(u32 off) {
    switch (off) {
    case NV_PTIMER_TIME_0: return (u32)ptimer_now();
    case NV_PTIMER_TIME_1: return (u32)(ptimer_now() >> 32) & 0x1FFFFFFF;
    case NV_PGRAPH_STATUS: return 0;  // idle
    case NV_PFB_WBC: return nv_reg(off) & ~NV_PFB_WBC_FLUSH;  // flushes complete at once
    case NV_PCRTC_RASTER: {
        // Scanline counter: 525 lines at 60 Hz.
        return (u32)((timer_ns() / 31746) % 525);
    }
    case NV_PMC_INTR_0: {
        u32 v = 0;
        if (nv_reg(NV_PFIFO_INTR_0) & nv_reg(NV_PFIFO_INTR_EN_0)) v |= 1 << 8;
        if (nv_reg(NV_PGRAPH_INTR) & nv_reg(NV_PGRAPH_INTR_EN)) v |= 1 << 12;
        if (nv_reg(NV_PCRTC_INTR_0) & nv_reg(NV_PCRTC_INTR_EN_0)) v |= 1 << 24;
        return v;
    }
    }
    if (off >= NV_PRAMIN && off < NV_PRAMIN + 0x100000) {
        u32 v;
        memcpy(&v, ramin_ptr(off - NV_PRAMIN), 4);
        return v;
    }
    if (off >= NV_USER && off < NV_USER + 0x10000) return pfifo_user_read(off - NV_USER);
    if (off >= NV_PFIFO && off < NV_PFIFO + 0x2000) return pfifo_read(off);
    return nv_reg(off);
}

static void write_reg(u32 off, u32 v) {
    switch (off) {
    // Interrupt status registers: write 1 to clear.
    case NV_PFIFO_INTR_0:
    case NV_PGRAPH_INTR:
    case NV_PCRTC_INTR_0:
    case NV_PTIMER_INTR_0:
        nv_reg(off) &= ~v;
        nv2a_update_irq();
        if (off == NV_PGRAPH_INTR) pgraph_intr_cleared();
        return;
    case NV_PFIFO_INTR_EN_0:
    case NV_PGRAPH_INTR_EN:
    case NV_PCRTC_INTR_EN_0:
    case NV_PMC_INTR_EN_0:
        nv_reg(off) = v;
        nv2a_update_irq();
        return;
    case NV_PTIMER_TIME_0: ptimer_set((ptimer_now() & ~0xFFFFFFFFull) | (v & ~0x1Fu)); return;
    case NV_PTIMER_TIME_1: ptimer_set((ptimer_now() & 0xFFFFFFFFull) | ((u64)(v & 0x1FFFFFFF) << 32)); return;
    case NV_PCRTC_START:
        nv_reg(off) = v;
        video_set_scanout(v);
        return;
    }
    if (off >= NV_PRAMIN && off < NV_PRAMIN + 0x100000) {
        memcpy(ramin_ptr(off - NV_PRAMIN), &v, 4);
        return;
    }
    if (off >= NV_USER && off < NV_USER + 0x10000) return pfifo_user_write(off - NV_USER, v);
    if (off >= NV_PFIFO && off < NV_PFIFO + 0x2000) return pfifo_write(off, v);
    if (off >= NV_PGRAPH && off < NV_PGRAPH + 0x2000) return pgraph_reg_write(off, v);
    nv_reg(off) = v;
}

static u32 nv2a_read(u32 off, u32 size) {
    std::lock_guard<std::recursive_mutex> lk(g_nv_lock);
    u32 v = read_reg(off & ~3u) >> ((off & 3) * 8);
    if (size < 4) v &= (1u << (size * 8)) - 1;
    if (g_trace) log_msg("[nv2a] rd%u %06X -> %08X", size * 8, off, v);
    return v;
}

static void nv2a_write(u32 off, u32 v, u32 size) {
    std::lock_guard<std::recursive_mutex> lk(g_nv_lock);
    if (g_trace) log_msg("[nv2a] wr%u %06X <- %08X", size * 8, off, v);
    if (size < 4) {
        u32 sh = (off & 3) * 8, mask = ((1u << (size * 8)) - 1) << sh;
        v = (read_reg(off & ~3u) & ~mask) | ((v << sh) & mask);
    }
    write_reg(off & ~3u, v);
}

// ------------------------------------------------------------ MMIO entry
u32 mmio_read(u32 a, u32 size) { return nv2a_read(a - NV2A_BASE, size); }
void mmio_write(u32 a, u32 v, u32 size) { nv2a_write(a - NV2A_BASE, v, size); }

// NV2A GPU emulation: register file (nv2a.cpp), command FIFO (pfifo.cpp),
// graphics engine methods (pgraph.cpp) and display/presentation (video.cpp).
#pragma once
#include <mutex>

#include "runtime.h"

// Instance memory: top MB of guest RAM as seen through the contiguous
// (0x80000000 | physical) mapping.
constexpr u32 RAMIN_BASE = 0x83F00000u;

// GPU-visible physical address -> guest address.
static inline u32 gpu_addr(u32 phys) { return 0x80000000u | (phys & (RAM_SIZE - 1)); }

// ------------------------------------------------------------ registers
enum : u32 {
    NV_PMC_BOOT_0 = 0x000000,
    NV_PMC_INTR_0 = 0x000100,
    NV_PMC_INTR_EN_0 = 0x000140,
    NV_PBUS_PCI = 0x001800,
    NV_PFIFO = 0x002000,
    NV_PFIFO_INTR_0 = 0x002100,
    NV_PFIFO_INTR_EN_0 = 0x002140,
    NV_PFIFO_RAMHT = 0x002210,
    NV_PFIFO_RAMFC = 0x002214,
    NV_PFIFO_RUNOUT_STATUS = 0x002400,
    NV_PFIFO_MODE = 0x002504,
    NV_PFIFO_CACHE1_PUSH0 = 0x003200,
    NV_PFIFO_CACHE1_PUSH1 = 0x003204,
    NV_PFIFO_CACHE1_STATUS = 0x003214,
    NV_PFIFO_CACHE1_DMA_PUSH = 0x003220,
    NV_PFIFO_CACHE1_DMA_STATE = 0x003228,
    NV_PFIFO_CACHE1_DMA_INSTANCE = 0x00322C,
    NV_PFIFO_CACHE1_DMA_PUT = 0x003240,
    NV_PFIFO_CACHE1_DMA_GET = 0x003244,
    NV_PFIFO_CACHE1_REF = 0x003248,
    NV_PFIFO_CACHE1_DMA_SUBROUTINE = 0x00324C,
    NV_PFIFO_CACHE1_PULL0 = 0x003250,
    NV_PTIMER_INTR_0 = 0x009100,
    NV_PTIMER_NUMERATOR = 0x009200,
    NV_PTIMER_DENOMINATOR = 0x009210,
    NV_PTIMER_TIME_0 = 0x009400,
    NV_PTIMER_TIME_1 = 0x009410,
    NV_PFB_CFG0 = 0x100200,
    NV_PFB_CSTATUS = 0x10020C,
    NV_PFB_WBC = 0x100410,
    NV_PFB_WBC_FLUSH = 1 << 16,
    NV_PGRAPH = 0x400000,
    NV_PGRAPH_INTR = 0x400100,
    NV_PGRAPH_INTR_ERROR = 1 << 20,
    NV_PGRAPH_NSOURCE = 0x400108,
    NV_PGRAPH_INTR_EN = 0x400140,
    NV_PGRAPH_STATUS = 0x400700,
    NV_PGRAPH_TRAPPED_ADDR = 0x400704,
    NV_PGRAPH_TRAPPED_DATA_LOW = 0x400708,
    NV_PGRAPH_INCREMENT = 0x40071C,
    NV_PCRTC_INTR_0 = 0x600100,
    NV_PCRTC_INTR_EN_0 = 0x600140,
    NV_PCRTC_START = 0x600800,
    NV_PCRTC_RASTER = 0x600808,
    NV_PRAMDAC_NVPLL_COEFF = 0x680500,
    NV_PRAMDAC_MPLL_COEFF = 0x680504,
    NV_PRAMDAC_VPLL_COEFF = 0x680508,
    NV_PRAMIN = 0x700000,
    NV_USER = 0x800000,
};

extern std::recursive_mutex g_nv_lock;  // guards the register file
u32& nv_reg(u32 off);
u8* ramin_ptr(u32 off);
void nv2a_init();
void nv2a_update_irq();

// ----------------------------------------------------------------- PFIFO
void pfifo_init();
u32 pfifo_read(u32 off);
void pfifo_write(u32 off, u32 v);
u32 pfifo_user_read(u32 off);
void pfifo_user_write(u32 off, u32 v);

// Object lookup through RAMHT: handle -> instance offset in RAMIN, or ~0.
u32 ramht_lookup(u32 handle);
struct DmaObject {
    u32 base, limit;  // physical
};
DmaObject dma_object(u32 instance);

// ---------------------------------------------------------------- PGRAPH
void pgraph_reg_write(u32 off, u32 v);
void pgraph_intr_cleared();
// One method from the FIFO for the object bound to `subch`.
void pgraph_method(u32 subch, u32 obj_instance, u32 method, u32 value);

// ----------------------------------------------------------------- video
void video_init();
void video_set_scanout(u32 phys);

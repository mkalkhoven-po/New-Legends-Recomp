// PFIFO: the DMA pusher that walks the pushbuffer D3D writes into guest
// memory, and the puller that routes methods to the bound objects. Runs on
// its own host thread, woken when the guest moves the DMA PUT pointer.
#include <windows.h>

#include <atomic>
#include <condition_variable>

#include "gpu.h"

static std::atomic<u32> g_put, g_get, g_ref, g_busy;
static std::mutex g_kick_m;
static std::condition_variable g_kick_cv;
static bool g_kicked;

// Pusher state that survives between kicks: a method's data may straddle
// a PUT boundary.
static u32 g_dcount, g_method, g_subch, g_noninc, g_subroutine, g_sub_active;
static u32 g_subch_obj[8];

u32 ramht_lookup(u32 handle) {
    u32 r = nv_reg(NV_PFIFO_RAMHT);
    u32 base = ((r >> 4) & 0x1F) << 12, size = 1u << (((r >> 16) & 3) + 12);
    // The hash is only an accelerator; a linear scan finds the same entry.
    for (u32 o = 0; o < size; o += 8) {
        u32 h, ctx;
        memcpy(&h, ramin_ptr(base + o), 4);
        memcpy(&ctx, ramin_ptr(base + o + 4), 4);
        if (h == handle && (ctx & 0x80000000u)) return (ctx & 0xFFFF) << 4;
    }
    log_msg("[pfifo] RAMHT: no object for handle %08X", handle);
    return ~0u;
}

DmaObject dma_object(u32 instance) {
    if (instance == ~0u) return {0, 0};
    u32 flags, limit, frame;
    memcpy(&flags, ramin_ptr(instance), 4);
    memcpy(&limit, ramin_ptr(instance + 4), 4);
    memcpy(&frame, ramin_ptr(instance + 8), 4);
    return {(frame & 0xFFFFF000u) | (flags >> 20), limit};
}

static void puller(u32 subch, u32 method, u32 value) {
    if (method == 0) {  // SET_OBJECT
        g_subch_obj[subch] = ramht_lookup(value);
        return;
    }
    pgraph_method(subch, g_subch_obj[subch], method, value);
}

static void run_pusher() {
    u32 inst;
    {
        std::lock_guard<std::recursive_mutex> lk(g_nv_lock);
        inst = (nv_reg(NV_PFIFO_CACHE1_DMA_INSTANCE) & 0xFFFF) << 4;
    }
    DmaObject dma = dma_object(inst);
    u32 get = g_get;
    while (get != g_put) {
        u32 word = rd32(gpu_addr(dma.base + get));
        get += 4;
        if (g_dcount) {
            puller(g_subch, g_method, word);
            if (!g_noninc) g_method += 4;
            g_dcount--;
            if (!g_dcount) g_get = get;
            continue;
        }
        if ((word & 0xE0000003u) == 0x20000000u) {  // old jump
            get = word & 0x1FFFFFFC;
        } else if ((word & 3) == 1) {  // jump
            get = word & 0xFFFFFFFC;
        } else if ((word & 3) == 2) {  // call
            if (g_sub_active) fatal("[pfifo] nested pushbuffer call at %08X", get - 4);
            g_subroutine = get;
            g_sub_active = 1;
            get = word & 0xFFFFFFFC;
        } else if (word == 0x00020000u) {  // return
            if (!g_sub_active) fatal("[pfifo] pushbuffer return without call at %08X", get - 4);
            get = g_subroutine;
            g_sub_active = 0;
        } else if ((word & 0xE0030003u) == 0 || (word & 0xE0030003u) == 0x40000000u) {
            g_method = word & 0x1FFC;
            g_subch = (word >> 13) & 7;
            g_dcount = (word >> 18) & 0x7FF;
            g_noninc = (word & 0x40000000u) != 0;
        } else {
            fatal("[pfifo] bad pushbuffer command %08X at %08X", word, get - 4);
        }
        g_get = get;
    }
}

void pgraph_init_backend();

void crash_register_thread(u32 id);

// NL_TRACE_FPS: time the FIFO thread had nothing to do (waiting for the game).
static double g_idle_ms;
double pfifo_idle_ms() { return g_idle_ms; }

static DWORD WINAPI gpu_thread(void*) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    crash_register_thread(1000);  // shows as "guest thread 1000" in watchdog dumps
    pgraph_init_backend();
    for (;;) {
        {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            std::unique_lock<std::mutex> lk(g_kick_m);
            g_kick_cv.wait(lk, [] { return g_kicked; });
            g_kicked = false;
            QueryPerformanceCounter(&t1);
            g_idle_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        }
        g_busy = 1;
        run_pusher();
        g_busy = 0;
    }
}

static void kick() {
    {
        std::lock_guard<std::mutex> lk(g_kick_m);
        g_kicked = true;
    }
    g_kick_cv.notify_one();
}

void pfifo_init() { CloseHandle(CreateThread(nullptr, 0, gpu_thread, nullptr, 0, nullptr)); }

u32 pfifo_read(u32 off) {
    switch (off) {
    case NV_PFIFO_CACHE1_DMA_PUT: return g_put;
    case NV_PFIFO_CACHE1_DMA_GET: return g_get;
    case NV_PFIFO_CACHE1_REF: return g_ref;
    case NV_PFIFO_CACHE1_DMA_PUSH:
        // BUSY while the pusher has work.
        return (nv_reg(off) & ~0x100u) | (g_get != g_put || g_busy ? 0x100u : 0);
    case NV_PFIFO_CACHE1_STATUS: return 1 << 4;  // CACHE1 empty
    }
    return nv_reg(off);
}

void pfifo_write(u32 off, u32 v) {
    switch (off) {
    case NV_PFIFO_CACHE1_DMA_PUT: g_put = v; kick(); return;
    case NV_PFIFO_CACHE1_DMA_GET: g_get = v; return;
    case NV_PFIFO_CACHE1_REF: g_ref = v; return;
    }
    nv_reg(off) = v;
}

// NV_USER: per-channel doorbell registers (channel 0 only).
u32 pfifo_user_read(u32 off) {
    switch (off) {
    case 0x40: return g_put;
    case 0x44: return g_get;
    case 0x48: return g_ref;
    }
    return 0;
}

void pfifo_user_write(u32 off, u32 v) {
    switch (off) {
    case 0x40: g_put = v; kick(); return;
    case 0x44: g_get = v; return;
    case 0x48: g_ref = v; return;
    }
    log_msg("[pfifo] user write %04X <- %08X", off, v);
}

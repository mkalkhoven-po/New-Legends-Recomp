// Display timing: a 60 Hz vertical blank that raises the PCRTC interrupt.
#include <windows.h>

#include <condition_variable>

#include "gpu.h"

void kernel_set_irq(u32 line, bool level);
constexpr u32 GPU_IRQ = 3;

static u32 g_scanout;
static std::mutex g_vblank_m;
static std::condition_variable g_vblank_cv;
static u64 g_vblanks;

// The FIFO waits here at a flip: frames go out at the console's 60 Hz.
static double g_vblank_wait_ms;
double video_vblank_wait_ms() { return g_vblank_wait_ms; }

void video_wait_vblank() {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    std::unique_lock<std::mutex> lk(g_vblank_m);
    u64 seen = g_vblanks;
    g_vblank_cv.wait(lk, [&] { return g_vblanks != seen; });
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    g_vblank_wait_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}

void video_set_scanout(u32 phys) { g_scanout = phys; }

void nv2a_update_irq() {
    std::lock_guard<std::recursive_mutex> lk(g_nv_lock);
    bool pending = false;
    if (nv_reg(NV_PFIFO_INTR_0) & nv_reg(NV_PFIFO_INTR_EN_0)) pending = true;
    if (nv_reg(NV_PGRAPH_INTR) & nv_reg(NV_PGRAPH_INTR_EN)) pending = true;
    if (nv_reg(NV_PCRTC_INTR_0) & nv_reg(NV_PCRTC_INTR_EN_0)) pending = true;
    kernel_set_irq(GPU_IRQ, pending && (nv_reg(NV_PMC_INTR_EN_0) & 1));
}

static DWORD WINAPI vblank_thread(void*) {
    timeBeginPeriod(1);
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (u64 frame = 1;; frame++) {
        // Sleep until the next 1/60 s boundary.
        for (;;) {
            QueryPerformanceCounter(&t);
            double due = (double)frame / 60.0 - (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
            if (due <= 0) break;
            Sleep(due > 0.002 ? (DWORD)(due * 1000) - 1 : 0);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_nv_lock);
            nv_reg(NV_PCRTC_INTR_0) |= 1;  // VBLANK
        }
        nv2a_update_irq();
        {
            std::lock_guard<std::mutex> lk(g_vblank_m);
            g_vblanks++;
        }
        g_vblank_cv.notify_all();
    }
}

void video_init() {
    nv2a_init();
    CloseHandle(CreateThread(nullptr, 0, vblank_thread, nullptr, 0, nullptr));
}

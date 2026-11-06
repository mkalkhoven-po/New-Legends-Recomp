// Out-of-line helpers for instructions the generated code calls into.
#include <windows.h>

#include "runtime.h"

// ---------------------------------------------------------------- x87 80-bit
double rdf80(u32 a) {
    u64 mant;
    u16 se;
    memcpy(&mant, gptr(a), 8);
    memcpy(&se, gptr(a + 8), 2);
    int sign = se >> 15, exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void wrf80(u32 a, double v) {
    u64 mant = 0;
    u16 se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else if (isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (v != 0.0) {
        int e;
        double m = frexp(fabs(v), &e);  // m in [0.5, 1)
        mant = (u64)ldexp(m, 64);
        se |= (u16)(e - 1 + 16383);
    }
    memcpy(gptr(a), &mant, 8);
    memcpy(gptr(a + 8), &se, 2);
}

void fxam(Ctx* c) {
    double v = ST(0);
    u16 s = c->fsw & ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (signbit(v)) s |= FSW_C1;
    switch (fpclassify(v)) {
    case FP_NAN: s |= FSW_C0; break;
    case FP_INFINITE: s |= FSW_C2 | FSW_C0; break;
    case FP_ZERO: s |= FSW_C3; break;
    case FP_SUBNORMAL: s |= FSW_C3 | FSW_C2; break;
    default: s |= FSW_C2; break;
    }
    c->fsw = s;
}

void fprem(Ctx* c, int ieee) {
    double a = ST(0), b = ST(1);
    double q = ieee ? nearbyint(a / b) : trunc(a / b);
    ST(0) = ieee ? remainder(a, b) : fmod(a, b);
    u32 qi = (u32)(s64)fabs(q);
    u16 s = c->fsw & ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (qi & 1) s |= FSW_C1;
    if (qi & 2) s |= FSW_C3;
    if (qi & 4) s |= FSW_C0;
    c->fsw = s;  // C2 clear: reduction complete
}

void fxtract(Ctx* c) {
    double v = ST(0);
    int e;
    double m = frexp(v, &e);
    ST(0) = (double)(e - 1);
    fpush(c, m * 2.0);
}

// Protected-mode 32-bit layout: control, status and tag words in 4-byte
// slots, then instruction/operand pointers (not modelled, stored as 0).
void fenv(Ctx* c, u32 a, int store) {
    if (store) {
        memset(gptr(a), 0, 28);
        wr16(a + 0, c->fcw);
        wr16(a + 4, fsw_read(c));
        wr16(a + 8, 0);  // all registers tagged valid
        c->fcw |= 0x3F;  // fnstenv masks all exceptions
    } else {
        c->fcw = rd16(a + 0);
        u16 sw = rd16(a + 4);
        c->fsw = sw & ~0x3800;
        c->ftop = (sw >> 11) & 7;
    }
}

void fsave(Ctx* c, u32 a, int store) {
    if (store) {
        u16 fcw = c->fcw;
        fenv(c, a, 1);
        wr16(a + 0, fcw);
        for (int i = 0; i < 8; i++) wrf80(a + 28 + 10 * i, ST(i));
        c->fcw = 0x037F;  // fninit
        c->fsw = 0;
        c->ftop = 0;
    } else {
        fenv(c, a, 0);
        for (int i = 0; i < 8; i++) ST(i) = rdf80(a + 28 + 10 * i);
    }
}

// ------------------------------------------------------------------- system
void rt_cpuid(Ctx* c) {
    // Report the retail Xbox CPU: a 733 MHz Pentium III (Coppermine).
    switch (c->eax) {
    case 0:
        c->eax = 2;
        c->ebx = 0x756E6547;  // "Genu"
        c->edx = 0x49656E69;  // "ineI"
        c->ecx = 0x6C65746E;  // "ntel"
        break;
    case 1:
        c->eax = 0x00000683;
        c->ebx = 0;
        c->ecx = 0;
        c->edx = 0x0383F9FF;  // FPU TSC CX8 CMOV MMX FXSR SSE ...
        break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
        break;
    }
}

u64 rt_rdtsc(Ctx*) {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Scale the host counter to the 733.33 MHz Xbox TSC.
    return (u64)((double)now.QuadPart * (733333333.0 / (double)freq.QuadPart));
}

constexpr u32 INT_DEBUG_SERVICE = 0x2D, DEBUG_SERVICE_PRINT = 1;

void rt_int(Ctx* c, u32 addr, u32 vector) {
    if (vector == 3) return;  // int3: debugger breakpoint, no debugger attached
    if (vector == INT_DEBUG_SERVICE) {
        // Kernel debug service, how OutputDebugString reaches a debugger:
        // eax = service, ecx = ANSI_STRING {length, maximum, buffer}.
        if (c->eax == DEBUG_SERVICE_PRINT) {
            u32 len = rd16(c->ecx), text = rd32(c->ecx + 4);
            log_msg("[debug] %.*s", (int)len, (const char*)gptr(text));
        }
        return;
    }
    fatal("software interrupt %02X at %08X", vector, addr);
}

u32 rt_in(Ctx*, u32 port, u32 size) {
    log_msg("in port %04X (%u bytes) -> 0", port, size);
    return 0;
}

void rt_out(Ctx*, u32 port, u32 value, u32 size) { log_msg("out port %04X <- %X (%u bytes)", port, value, size); }

// ------------------------------------------------------------------ strings
void rt_rep_movs(Ctx* c, u32 size) {
    u32 n = c->ecx;
    if (!n) return;
    u32 bytes = n * size;
    if (!c->df) {
        // rep movs copies element by element, so a forward overlap repeats a
        // pattern; only that case needs the slow path.
        if (c->edi > c->esi && c->edi - c->esi < bytes) {
            for (u32 i = 0; i < bytes; i++) gptr(c->edi)[i] = gptr(c->esi)[i];
        } else {
            memmove(gptr(c->edi), gptr(c->esi), bytes);
        }
        c->esi += bytes;
        c->edi += bytes;
    } else {
        for (u32 i = 0; i < n; i++) {
            memmove(gptr(c->edi), gptr(c->esi), size);
            c->esi -= size;
            c->edi -= size;
        }
    }
    c->ecx = 0;
}

void rt_rep_stos(Ctx* c, u32 size) {
    u32 n = c->ecx;
    if (!n) return;
    if (size == 1 && !c->df) {
        memset(gptr(c->edi), (u8)c->eax, n);
        c->edi += n;
    } else {
        s32 step = c->df ? -(s32)size : (s32)size;
        for (u32 i = 0; i < n; i++) {
            memcpy(gptr(c->edi), &c->eax, size);
            c->edi += step;
        }
    }
    c->ecx = 0;
}

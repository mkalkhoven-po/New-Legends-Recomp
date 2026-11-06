// Contract between the generated code and the runtime.
//
// Every recompiled guest function is `void f_XXXXXXXX(Ctx* c)`. Guest state
// (registers, x87 stack, SSE/MMX) lives in Ctx; guest memory is a flat 4 GB
// reservation at a fixed host address so a guest address converts to a host
// pointer with a single add of a constant.
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <emmintrin.h>
#include <intrin.h>
#include <xmmintrin.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

// Host address of guest address 0. Must match the reservation in memory.cpp.
// 16 TiB: far above where Windows randomizes bottom-up allocations (heaps,
// stacks) and below the top-down region with the DLLs, so it is always free.
#define GUEST_BASE ((u8*)0x100000000000ull)

union Xmm {
    __m128 v;
    float f[4];
    u32 u[4];
    u64 q[2];
};

struct Ctx {
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 fs_base;  // guest address the fs segment points at
    u32 df;       // direction flag, 0 or 1
    u32 eflags_sys;  // AC/ID bits: only observable through pushfd/popfd
    // Arithmetic flags at function boundaries (inside a function they are
    // C locals; see FLAGS_IN/FLAGS_OUT).
    u32 f_cf, f_zf, f_sf, f_of, f_pf, f_af;

    // x87: stack of doubles; physical slot of ST(i) is (ftop + i) & 7.
    double st[8];
    u32 ftop;
    u16 fcw, fsw;  // fsw holds C0-C3 only, TOP is merged on read

    u64 mm[8];
    Xmm xmm[8];
    u32 mxcsr;

    void* host_thread;  // runtime-owned per-thread data
};

typedef void (*GuestFunc)(Ctx*);

// Run a function that can be resumed after a guest catch block (dispatch.cpp).
void rt_eh_frame(Ctx* c, GuestFunc body);
// Enter a __except block of the function that owns SEH frame `frame`.
[[noreturn]] void rt_except_resume(Ctx* c, u32 target, u32 frame);

// ------------------------------------------------------------------ memory
static inline u8* gptr(u32 a) { return GUEST_BASE + a; }
static inline u8 rd8(u32 a) { return *gptr(a); }
static inline u16 rd16(u32 a) { u16 v; memcpy(&v, gptr(a), 2); return v; }
static inline u32 rd32(u32 a) { u32 v; memcpy(&v, gptr(a), 4); return v; }
static inline u64 rd64(u32 a) { u64 v; memcpy(&v, gptr(a), 8); return v; }
static inline void wr8(u32 a, u8 v) { *gptr(a) = v; }
static inline void wr16(u32 a, u16 v) { memcpy(gptr(a), &v, 2); }
static inline void wr32(u32 a, u32 v) { memcpy(gptr(a), &v, 4); }
static inline void wr64(u32 a, u64 v) { memcpy(gptr(a), &v, 8); }
// NV2A GPU registers (0xFD000000, 16 MB) are emulated by the runtime. Code
// from the XDK's D3D library uses these checked accessors (the emitter maps
// rd32 -> io_rd32 there).
u32 mmio_read(u32 a, u32 size);
void mmio_write(u32 a, u32 v, u32 size);
static inline bool is_mmio(u32 a) { return a - 0xFD000000u < 0x01000000u; }
static inline u8 io_rd8(u32 a) { return __builtin_expect(is_mmio(a), 0) ? (u8)mmio_read(a, 1) : rd8(a); }
static inline u16 io_rd16(u32 a) { return __builtin_expect(is_mmio(a), 0) ? (u16)mmio_read(a, 2) : rd16(a); }
static inline u32 io_rd32(u32 a) { return __builtin_expect(is_mmio(a), 0) ? mmio_read(a, 4) : rd32(a); }
static inline void io_wr8(u32 a, u8 v) { if (__builtin_expect(is_mmio(a), 0)) mmio_write(a, v, 1); else wr8(a, v); }
static inline void io_wr16(u32 a, u16 v) { if (__builtin_expect(is_mmio(a), 0)) mmio_write(a, v, 2); else wr16(a, v); }
static inline void io_wr32(u32 a, u32 v) { if (__builtin_expect(is_mmio(a), 0)) mmio_write(a, v, 4); else wr32(a, v); }

static inline float rdf32(u32 a) { float v; memcpy(&v, gptr(a), 4); return v; }
static inline double rdf64(u32 a) { double v; memcpy(&v, gptr(a), 8); return v; }
static inline void wrf32(u32 a, float v) { memcpy(gptr(a), &v, 4); }
static inline void wrf64(u32 a, double v) { memcpy(gptr(a), &v, 8); }
static inline __m128 rdx128(u32 a) { return _mm_loadu_ps((const float*)gptr(a)); }
static inline void wrx128(u32 a, __m128 v) { _mm_storeu_ps((float*)gptr(a), v); }

// MMX is emulated on the low 64 bits of SSE2 registers.
#define M2X(v) _mm_cvtsi64_si128((long long)(v))
#define X2M(x) ((u64)_mm_cvtsi128_si64(x))
static inline __m128 rdx64(__m128 d, u32 a) { return _mm_castpd_ps(_mm_loadl_pd(_mm_castps_pd(d), (const double*)gptr(a))); }
static inline __m128 rdxh64(__m128 d, u32 a) { return _mm_castpd_ps(_mm_loadh_pd(_mm_castps_pd(d), (const double*)gptr(a))); }
static inline void wrx64(u32 a, __m128 v) { _mm_storel_pd((double*)gptr(a), _mm_castps_pd(v)); }
static inline void wrxh64(u32 a, __m128 v) { _mm_storeh_pd((double*)gptr(a), _mm_castps_pd(v)); }
static inline __m128 rdx32(u32 a) { return _mm_load_ss((const float*)gptr(a)); }
static inline __m128 x_from_u64(u64 v) { return _mm_castsi128_ps(M2X(v)); }
static inline u64 x_low_u64(__m128 v) { return X2M(_mm_castps_si128(v)); }

// lock-prefixed read-modify-write on 32-bit memory. Each returns the old value.
static inline u32 atomic_xadd32(u32 a, u32 v) { return (u32)_InterlockedExchangeAdd((volatile long*)gptr(a), (long)v); }
static inline u32 atomic_xchg32(u32 a, u32 v) { return (u32)_InterlockedExchange((volatile long*)gptr(a), (long)v); }
static inline u32 atomic_or32(u32 a, u32 v) { return (u32)_InterlockedOr((volatile long*)gptr(a), (long)v); }
static inline u32 atomic_and32(u32 a, u32 v) { return (u32)_InterlockedAnd((volatile long*)gptr(a), (long)v); }
static inline u32 atomic_xor32(u32 a, u32 v) { return (u32)_InterlockedXor((volatile long*)gptr(a), (long)v); }
static inline u32 atomic_cmpxchg32(u32 a, u32 v, u32 cmp) {
    return (u32)_InterlockedCompareExchange((volatile long*)gptr(a), (long)v, (long)cmp);
}

// ---------------------------------------------------------- partial registers
#define SET16(r, v) ((r) = ((r) & 0xFFFF0000u) | (u16)(v))
#define SET8L(r, v) ((r) = ((r) & 0xFFFFFF00u) | (u8)(v))
#define SET8H(r, v) ((r) = ((r) & 0xFFFF00FFu) | ((u32)(u8)(v) << 8))

// --------------------------------------------------------------------- flags
// Flags are locals of each generated function (cf, zf, sf, of, pf, af) so the
// C compiler drops the ones nothing reads.
#define PAR(r) ((u32)(~__builtin_parity((u32)(r) & 0xFFu)) & 1u)
#define MSB(x, n) ((u32)((x) >> ((n) - 1)) & 1u)
#define FL_SZP(r, n) (zf = (r) == 0, sf = MSB(r, n), pf = PAR(r))
#define FL_LOGIC(r, n) (cf = 0, of = 0, af = 0, FL_SZP(r, n))
#define FL_ADD(a, b, r, n) \
    (cf = (r) < (a), of = MSB(((a) ^ (r)) & ((b) ^ (r)), n), af = (((a) ^ (b) ^ (r)) >> 4) & 1, FL_SZP(r, n))
#define FL_SUB(a, b, r, n) \
    (cf = (a) < (b), of = MSB(((a) ^ (b)) & ((a) ^ (r)), n), af = (((a) ^ (b) ^ (r)) >> 4) & 1, FL_SZP(r, n))
// add/sub with carry-in: carry-out comes from the widened sum.
#define FL_ADC(a, b, r, ci, n) \
    (cf = ((u64)(a) + (u64)(b) + (ci)) >> (n) & 1, of = MSB(((a) ^ (r)) & ((b) ^ (r)), n), \
     af = (((a) ^ (b) ^ (r)) >> 4) & 1, FL_SZP(r, n))
#define FL_SBB(a, b, r, ci, n) \
    (cf = (u64)(a) < (u64)(b) + (ci), of = MSB(((a) ^ (b)) & ((a) ^ (r)), n), \
     af = (((a) ^ (b) ^ (r)) >> 4) & 1, FL_SZP(r, n))

#define FLAGS_IN() (cf = c->f_cf, zf = c->f_zf, sf = c->f_sf, of = c->f_of, pf = c->f_pf, af = c->f_af)
#define FLAGS_OUT() (c->f_cf = cf, c->f_zf = zf, c->f_sf = sf, c->f_of = of, c->f_pf = pf, c->f_af = af)

// Rotate through carry, one bit at a time (rare enough not to matter).
static inline u32 rcl_n(u32 a, u32 n, u32 bits, u32* cf) {
    u32 mask = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    for (u32 i = 0; i < n; i++) {
        u32 out = (a >> (bits - 1)) & 1;
        a = ((a << 1) | *cf) & mask;
        *cf = out;
    }
    return a;
}
static inline u32 rcr_n(u32 a, u32 n, u32 bits, u32* cf) {
    for (u32 i = 0; i < n; i++) {
        u32 out = a & 1;
        a = (a >> 1) | (*cf << (bits - 1));
        *cf = out;
    }
    return a;
}

// Pack/unpack the arithmetic flags in EFLAGS layout (pushfd/popfd/lahf/sahf).
#define FLAGS_PACK() \
    ((cf) | 2u | ((pf) << 2) | ((af) << 4) | ((zf) << 6) | ((sf) << 7) | (c->df << 10) | ((of) << 11) | 0x200u | c->eflags_sys)
#define FLAGS_UNPACK(v)                                                                         \
    (cf = (v) & 1, pf = ((v) >> 2) & 1, af = ((v) >> 4) & 1, zf = ((v) >> 6) & 1, sf = ((v) >> 7) & 1, \
     c->df = ((v) >> 10) & 1, of = ((v) >> 11) & 1, c->eflags_sys = (v) & 0x00240000u)

// ---------------------------------------------------------------------- x87
#define ST(i) c->st[(c->ftop + (i)) & 7]
static inline void fpush(Ctx* c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline void fpop(Ctx* c) { c->ftop = (c->ftop + 1) & 7; }

#define FSW_C0 0x0100
#define FSW_C1 0x0200
#define FSW_C2 0x0400
#define FSW_C3 0x4000

static inline void fcom(Ctx* c, double a, double b) {
    u16 s = c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3);
    if (a != a || b != b) s |= FSW_C0 | FSW_C2 | FSW_C3;
    else if (a < b) s |= FSW_C0;
    else if (a == b) s |= FSW_C3;
    c->fsw = s;
}
static inline u16 fsw_read(Ctx* c) { return (u16)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)); }

// fcomi family writes ZF/PF/CF directly.
#define FCOMI(a, b)                                    \
    do {                                               \
        double fa_ = (a), fb_ = (b);                   \
        int un_ = fa_ != fa_ || fb_ != fb_;            \
        zf = un_ || fa_ == fb_;                        \
        pf = un_;                                      \
        cf = un_ || fa_ < fb_;                         \
        of = sf = af = 0;                              \
    } while (0)

static inline double frnd(Ctx* c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline s16 fist16(Ctx* c, double v) {
    double r = frnd(c, v);
    return (r >= -32768.0 && r <= 32767.0) ? (s16)r : (s16)0x8000;
}
static inline s32 fist32(Ctx* c, double v) {
    double r = frnd(c, v);
    return (r >= -2147483648.0 && r <= 2147483647.0) ? (s32)r : (s32)0x80000000;
}
static inline s64 fist64(Ctx* c, double v) {
    double r = frnd(c, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (s64)r : (s64)0x8000000000000000ll;
}

double rdf80(u32 a);
void wrf80(u32 a, double v);
void fxam(Ctx* c);
void fprem(Ctx* c, int ieee);
void fxtract(Ctx* c);
// 28-byte FPU environment (store = fnstenv, else fldenv) and the 108-byte
// full state (store = fnsave, which also resets the FPU, else frstor).
void fenv(Ctx* c, u32 a, int store);
void fsave(Ctx* c, u32 a, int store);

// ------------------------------------------------------------ runtime calls
// Indirect call/jump through a guest address (vtables, function pointers,
// kernel import thunks).
void rt_icall(Ctx* c, u32 target);
// A computed jump inside a function resolved to an unknown target.
[[noreturn]] void rt_bad_jump(Ctx* c, u32 from, u32 target);
// Reached code the analysis considered unreachable (padding, noreturn call).
[[noreturn]] void rt_trap(Ctx* c, u32 addr);
// Instruction the recompiler cannot translate yet.
void rt_unimpl(Ctx* c, u32 addr, const char* text);

void rt_cpuid(Ctx* c);
u64 rt_rdtsc(Ctx* c);
void rt_int(Ctx* c, u32 addr, u32 vector);
u32 rt_in(Ctx* c, u32 port, u32 size);
void rt_out(Ctx* c, u32 port, u32 value, u32 size);

// Preemption: the runtime sets g_yield periodically and the recompiler checks
// it on loop back-edges, so a spinning guest thread lets others run.
extern volatile u32 g_yield;
void rt_yield(Ctx* c);

// rep movs/stos; size is the element size in bytes.
void rt_rep_movs(Ctx* c, u32 size);
void rt_rep_stos(Ctx* c, u32 size);

// Guest calls a host callback inside the runtime by pushing a return address.
#define GUEST_CALL(fn, ret) (c->esp -= 4, wr32(c->esp, (ret)), fn(c))

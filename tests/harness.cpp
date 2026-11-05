// Runs recompiled instruction sequences from difftest.py on given initial
// states and writes the resulting states, for comparison with Unicorn.
#include <windows.h>

#include <vector>

#include "runtime.h"

typedef u32 (*TestFn)(Ctx*, u32);
extern const TestFn g_tests[];
extern const unsigned g_ntests;

#pragma pack(push, 1)
struct State {
    u32 id;
    u32 regs[8];
    u32 eflags;
    u32 ftop;
    u16 fsw, fcw;
    double st[8];
    u64 mm[8];
    u8 xmm[8][16];
    u32 crc;  // output only: CRC32 of the compared memory ranges
    u32 fault;
};
#pragma pack(pop)

constexpr u32 IMAGE_LO = 0x00010000u, IMAGE_HI = 0x004A8000u;
constexpr u32 SCRATCH = 0x03000000u, SCRATCH_SIZE = 0x00800000u;

// Stubs for runtime entry points the test sequences never reach.
void rt_icall(Ctx*, u32 t) { fatal("rt_icall %08X in test", t); }
void rt_bad_jump(Ctx*, u32, u32) { fatal("rt_bad_jump in test"); }
void rt_trap(Ctx*, u32 a) { fatal("rt_trap %08X in test", a); }
void rt_unimpl(Ctx*, u32 a, const char* t) { fatal("rt_unimpl %08X %s in test", a, t); }

static u32 crc32(const u8* p, size_t n, u32 crc = 0) {
    static u32 table[256];
    if (!table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static int run_case(TestFn fn, Ctx* c, u32 flags_in, u32* flags_out) {
    __try {
        *flags_out = fn(c, flags_in);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int main(int argc, char** argv) {
    if (argc != 5) fatal("usage: harness <xbe> <scratch.bin> <in.bin> <out.bin>");
    mem_init();
    xbe_load(argv[1]);
    FILE* f = fopen(argv[2], "rb");
    if (!f || fread(gptr(SCRATCH), 1, SCRATCH_SIZE, f) != SCRATCH_SIZE) fatal("bad scratch file");
    fclose(f);
    std::vector<u8> image(gptr(IMAGE_LO), gptr(IMAGE_HI));
    std::vector<u8> scratch(gptr(SCRATCH), gptr(SCRATCH) + SCRATCH_SIZE);

    FILE* in = fopen(argv[3], "rb");
    FILE* out = fopen(argv[4], "wb");
    State s;
    while (fread(&s, sizeof(s), 1, in) == 1) {
        memcpy(gptr(IMAGE_LO), image.data(), image.size());
        memcpy(gptr(SCRATCH), scratch.data(), scratch.size());
        static Ctx ctx;
        Ctx* c = &ctx;
        memset(c, 0, sizeof(*c));
        memcpy(&c->eax, s.regs, sizeof(s.regs));
        c->ftop = s.ftop;
        c->fsw = s.fsw;
        c->fcw = s.fcw;
        memcpy(c->st, s.st, sizeof(s.st));
        memcpy(c->mm, s.mm, sizeof(s.mm));
        memcpy(c->xmm, s.xmm, sizeof(s.xmm));
        c->mxcsr = 0x1F80;

        u32 fl = 0;
        s.fault = run_case(g_tests[s.id], c, s.eflags, &fl);
        memcpy(s.regs, &c->eax, sizeof(s.regs));
        s.eflags = fl;
        s.ftop = c->ftop;
        s.fsw = c->fsw;
        s.fcw = c->fcw;
        memcpy(s.st, c->st, sizeof(s.st));
        memcpy(s.mm, c->mm, sizeof(s.mm));
        memcpy(s.xmm, c->xmm, sizeof(s.xmm));
        s.crc = crc32(gptr(SCRATCH), SCRATCH_SIZE, crc32(gptr(IMAGE_LO), IMAGE_HI - IMAGE_LO));
        fwrite(&s, sizeof(s), 1, out);
    }
    fclose(out);
    return 0;
}

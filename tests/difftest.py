"""Differential test of the x86 -> C translation against Unicorn.

Takes straight-line instruction runs from the game's own basic blocks, runs
each on random initial states in Unicorn (reference CPU) and in the
recompiled C (tests/harness.cpp), and compares registers, flags, x87, MMX,
SSE and a CRC of the touched memory.

Usage:
    python -I tests/difftest.py [--cases N] [--seed S] [--filter MNEMONIC]
"""
import argparse
import collections
import math
import os
import random
import struct
import subprocess
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, ROOT)

from capstone import x86  # noqa: E402
from unicorn import UC_ARCH_X86, UC_MODE_32, Uc, UcError  # noqa: E402
from unicorn import x86_const as U  # noqa: E402

from recompiler.analysis import Analyzer, Image  # noqa: E402
from recompiler.emit import PREFIXES, InsnEmitter, Unsupported  # noqa: E402
from recompiler.xbe import Xbe  # noqa: E402

XBE = os.path.join(ROOT, "game", "default.xbe")
OUT = os.path.join(ROOT, "build", "difftest")
CLANG = r"C:\Program Files\LLVM\bin\clang-cl.exe"
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

IMAGE_LO, IMAGE_HI = 0x00010000, 0x004A8000
SCRATCH, SCRATCH_SIZE = 0x03000000, 0x00800000
PROLOGUE = 0x03E00000  # x87 stack loader code
FPDATA = 0x03E01000
RAM = 0x04000000

STATE = struct.Struct("<I8IIIHH8d8Q128sII")

# fyl2x/fyl2xp1: Unicorn does not pop on a non-positive operand (hardware does).
CONTROL = {"fyl2x", "fyl2xp1", "call", "jmp", "ret", "loop", "loope", "loopne", "jecxz", "jcxz", "int3", "int", "hlt", "ud2",
           "cpuid", "rdtsc", "in", "out", "iretd", "int1", "into", "wait"}
FLAG_BITS = {"cf": 0, "pf": 2, "af": 4, "zf": 6, "sf": 7, "of": 11}
EFL = x86


def flag_effects(ins):
    """Return {flag: True if left undefined, False if defined} for flags written."""
    out = {}
    e = ins.eflags
    for f in FLAG_BITS:
        F = f.upper()
        und = getattr(EFL, f"X86_EFLAGS_UNDEFINED_{F}", 0)
        mod = (getattr(EFL, f"X86_EFLAGS_MODIFY_{F}", 0) | getattr(EFL, f"X86_EFLAGS_RESET_{F}", 0)
               | getattr(EFL, f"X86_EFLAGS_SET_{F}", 0))
        if e & und:
            out[f] = True
        elif e & mod:
            out[f] = False
    m = ins.mnemonic
    if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
        cnt = ins.operands[-1] if len(ins.operands) > 1 else None
        if cnt is not None and not (cnt.type == x86.X86_OP_IMM and cnt.imm == 1):
            out["of"] = True
        if m in ("shl", "sal", "shr", "sar", "shld", "shrd"):
            out["af"] = True
    if m in ("mul", "imul"):
        for f in ("zf", "sf", "pf", "af"):
            out[f] = True
    if m in ("bsf", "bsr"):
        for f in ("cf", "of", "sf", "af", "pf"):
            out[f] = True
    if m in ("and", "or", "xor", "test"):
        out["af"] = True
    return out


def is_testable(ins):
    m = ins.mnemonic.split()[-1]
    if m in CONTROL or m in JCC_NAMES:
        return False
    for op in ins.operands:
        if op.type == x86.X86_OP_MEM and op.mem.segment:
            return False  # fs-relative: thread environment, not data
        if op.type == x86.X86_OP_REG and ins.reg_name(op.reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
            return False
    raw = bytes(ins.bytes)
    if raw[:1] == b"\x9b" or any(b in (0xD9, 0xDD) for b in raw[:2]) and len(raw) > 1:
        modrm = raw[1] if raw[0] in (0xD9, 0xDD) else None
        if modrm is not None and modrm < 0xC0 and (modrm >> 3) & 7 in (4, 6):
            return False  # fldenv/fnstenv/frstor/fnsave
    return True


JCC_NAMES = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"}


def collect_sequences(an, limit_len=12):
    seqs = []
    for fn in an.functions.values():
        for b in fn.blocks.values():
            run = []
            for ins in b.insns:
                if not is_testable(ins):
                    break
                run.append(ins)
                if len(run) >= limit_len:
                    break
            if run:
                seqs.append(run)
    return seqs


def translate(seq):
    lines = []
    for ins in seq:
        ie = InsnEmitter(None, ins)
        body = ie.translate()
        lines.append(f"    // {ins.address:08X} {ins.mnemonic} {ins.op_str}")
        lines.append("    { " + " ".join(body) + " }")
    return lines


def is_x87(ins):
    raw = bytes(ins.bytes)
    i = 0
    while i < len(raw) and raw[i] in PREFIXES:
        i += 1
    return i < len(raw) and 0xD8 <= raw[i] <= 0xDF


def x87_used(seq):
    return any(is_x87(ins) for ins in seq) and not mmx_used(seq)


def mmx_used(seq):
    return any(ins.reg_name(op.reg).startswith("mm") for ins in seq for op in ins.operands if op.type == x86.X86_OP_REG)


def fcomp_used(seq):
    names = ("fcom", "fucom", "ftst", "fxam", "fprem", "ficom", "fsin", "fcos", "fptan", "fsincos")
    return any(ins.mnemonic.startswith(names) for ins in seq)


def f80_to_double(t):
    mant, se = t
    sign = -1.0 if se & 0x8000 else 1.0
    exp = se & 0x7FFF
    if exp == 0 and mant == 0:
        return math.copysign(0.0, sign)
    if exp == 0x7FFF:
        return math.nan if mant & 0x7FFFFFFFFFFFFFFF else sign * math.inf
    try:
        return sign * math.ldexp(mant, exp - 16383 - 63)
    except OverflowError:
        return sign * math.inf


def close(a, b, tol):
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    if a == b:
        return True
    if math.isinf(a) or math.isinf(b):
        return False
    return abs(a - b) <= tol * max(abs(a), abs(b), 1e-300) or abs(a - b) < 1e-300


class Case:
    def __init__(self, cid, seq, rng):
        self.id, self.seq = cid, seq
        r = []
        for i in range(8):
            if i == 4:  # esp
                r.append(SCRATCH + 0x400000 + rng.randrange(0, 0x1000) * 4)
            else:
                k = rng.random()
                if k < 0.6:
                    r.append(SCRATCH + 0x10000 + rng.randrange(0, 0x100000) * 4)
                elif k < 0.85:
                    r.append(rng.randrange(0, 64))
                else:
                    r.append(rng.getrandbits(32))
        self.regs = r
        self.eflags = 0x202 | sum(rng.getrandbits(1) << b for b in FLAG_BITS.values())
        self.fp = [rng.choice([rng.uniform(-1000, 1000), rng.uniform(-1, 1), float(rng.randrange(-100, 100)),
                               rng.uniform(0.001, 10.0)]) for _ in range(4)]
        self.mm = [rng.getrandbits(64) for _ in range(8)]
        self.xmm = [rng.getrandbits(128) if rng.random() < 0.3 else
                    int.from_bytes(struct.pack("<4f", *[rng.uniform(-100, 100) for _ in range(4)]), "little")
                    for _ in range(8)]
        self.use_x87 = x87_used(seq)
        self.use_mmx = mmx_used(seq)


def run_unicorn(mu, case, image, scratch):
    mu.mem_write(IMAGE_LO, image)
    mu.mem_write(SCRATCH, scratch)
    regs = [U.UC_X86_REG_EAX, U.UC_X86_REG_ECX, U.UC_X86_REG_EDX, U.UC_X86_REG_EBX,
            U.UC_X86_REG_ESP, U.UC_X86_REG_EBP, U.UC_X86_REG_ESI, U.UC_X86_REG_EDI]
    # Reset the FPU, then load four values (top ends at 4) unless MMX is used.
    mu.reg_write(U.UC_X86_REG_FPSW, 0)
    mu.reg_write(U.UC_X86_REG_FPCW, 0x37F)
    mu.reg_write(U.UC_X86_REG_FPTAG, 0xFFFF)
    if case.use_mmx:
        for i in range(8):
            mu.reg_write(U.UC_X86_REG_FP0 + i, (case.mm[i], 0xFFFF))
    else:
        code = b""
        for i in range(4):
            mu.mem_write(FPDATA + 8 * i, struct.pack("<d", case.fp[3 - i]))
            code += b"\xDD\x05" + struct.pack("<I", FPDATA + 8 * i)
        mu.mem_write(PROLOGUE, code)
        mu.emu_start(PROLOGUE, PROLOGUE + len(code))
    for i in range(8):
        mu.reg_write(U.UC_X86_REG_XMM0 + i, case.xmm[i])
    for reg, v in zip(regs, case.regs):
        mu.reg_write(reg, v)
    mu.reg_write(U.UC_X86_REG_EFLAGS, case.eflags)
    start = case.seq[0].address
    end = case.seq[-1].address + case.seq[-1].size
    mu.emu_start(start, end, count=len(case.seq))
    out = {
        "regs": [mu.reg_read(r) for r in regs],
        "eflags": mu.reg_read(U.UC_X86_REG_EFLAGS),
        "fsw": mu.reg_read(U.UC_X86_REG_FPSW),
        "ftag": mu.reg_read(U.UC_X86_REG_FPTAG),
        "fp": [f80_to_double(mu.reg_read(U.UC_X86_REG_FP0 + i)) for i in range(8)],
        "mm": [mu.reg_read(U.UC_X86_REG_FP0 + i)[0] for i in range(8)],
        "xmm": [mu.reg_read(U.UC_X86_REG_XMM0 + i) for i in range(8)],
    }
    out["crc"] = zlib.crc32(bytes(mu.mem_read(SCRATCH, SCRATCH_SIZE)),
                            zlib.crc32(bytes(mu.mem_read(IMAGE_LO, IMAGE_HI - IMAGE_LO))))
    return out


def pack_state(case):
    st = [0.0] * 8
    if not case.use_mmx:
        for i, v in enumerate(case.fp):
            st[(4 + i) & 7] = v
    xmm = b"".join(v.to_bytes(16, "little") for v in case.xmm)
    return STATE.pack(case.id, *case.regs, case.eflags, 0 if case.use_mmx else 4, 0x2000 if not case.use_mmx else 0,
                      0x37F, *st, *case.mm, xmm, 0, 0)


def build_harness(cases_src):
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(OUT, "cases.cpp")
    open(src, "w").write(cases_src)
    exe = os.path.join(OUT, "harness.exe")
    files = [os.path.join(ROOT, p) for p in ("tests/harness.cpp", "runtime/src/memory.cpp",
                                              "runtime/src/loader.cpp", "runtime/src/cpu.cpp")] + [src]
    bat = os.path.join(OUT, "build_harness.bat")
    with open(bat, "w") as f:
        f.write(f'@call "{VCVARS}" >nul\n')
        f.write(f'@"{CLANG}" /nologo /O1 /EHsc /std:c++17 -w /I "{os.path.join(ROOT, "runtime", "include")}" '
                + " ".join(f'"{x}"' for x in files) + f' onecore.lib /Fe"{exe}" /Fo"{OUT}\\\\"\n')
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-6000:], r.stderr[-3000:])
        sys.exit("harness build failed")
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--filter", default=None, help="only sequences containing this mnemonic")
    ap.add_argument("--show", type=int, default=15)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    xbe = Xbe(open(XBE, "rb").read())
    img = Image(xbe)
    an = Analyzer(img)
    an.discover(xbe.entry, log=lambda *_: None)
    seqs = collect_sequences(an)
    if args.filter:
        seqs = [s for s in seqs if any(i.mnemonic.split()[-1] == args.filter for i in s)]
    rng.shuffle(seqs)

    image = bytes(xbe.data[0:0])  # placeholder; real image comes from guest layout below
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    # Only map what is reset per case, so stray accesses skip the case
    # instead of reading leftovers from earlier cases.
    mu.mem_map(IMAGE_LO, IMAGE_HI - IMAGE_LO)
    mu.mem_map(SCRATCH, SCRATCH_SIZE)
    mu.mem_map(PROLOGUE, 0x2000)
    # Lay out the image exactly like runtime/src/loader.cpp.
    buf = bytearray(IMAGE_HI - IMAGE_LO)
    buf[0 : xbe.size_headers] = xbe.data[: xbe.size_headers]
    for i, s in enumerate(xbe.sections):
        off = s["vaddr"] - IMAGE_LO
        n = min(s["rawsize"], s["vsize"])
        buf[off : off + s["vsize"]] = b"\0" * s["vsize"]
        buf[off : off + n] = xbe.data[s["raw"] : s["raw"] + n]
        # The loader counts one reference on every preloaded section.
        if s["flags"] & 2:
            hdr = xbe.section_hdrs_addr - IMAGE_LO + i * 0x38
            buf[hdr + 0x18 : hdr + 0x1C] = struct.pack("<I", 1)
    image = bytes(buf)
    scratch = rng.randbytes(SCRATCH_SIZE)
    scratch_path = os.path.join(OUT, "scratch.bin")
    os.makedirs(OUT, exist_ok=True)
    open(scratch_path, "wb").write(scratch)

    cases, funcs, expected = [], [], {}
    skipped = collections.Counter()
    for seq in seqs:
        if len(cases) >= args.cases:
            break
        try:
            body = translate(seq)
        except Unsupported as e:
            skipped[f"unsupported {e}"] += 1
            continue
        cid = len(funcs)
        case = None
        for _attempt in range(4):
            cand = Case(cid, seq, rng)
            try:
                res = run_unicorn(mu, cand, image, scratch)
            except UcError as e:
                skipped[f"unicorn {e}"] += 1
                continue
            case = cand
            break
        if case is None:
            continue
        funcs.append(["u32 t_%d(Ctx* c, u32 fl) {" % cid,
                      "    u32 cf, zf, sf, of, pf, af; FLAGS_UNPACK(fl);"] + body + ["    return FLAGS_PACK();", "}"])
        cases.append(case)
        expected[cid] = res

    src = ['#include "x86_recomp.h"\n']
    for f in funcs:
        src.append("\n".join(f))
    src.append("typedef u32 (*TestFn)(Ctx*, u32);\nextern const TestFn g_tests[] = {"
               + ", ".join(f"t_{i}" for i in range(len(funcs))) + "};")
    src.append(f"extern const unsigned g_ntests = {len(funcs)};")
    exe = build_harness("\n\n".join(src) + "\n")

    in_path, out_path = os.path.join(OUT, "in.bin"), os.path.join(OUT, "out.bin")
    with open(in_path, "wb") as f:
        for case in cases:
            f.write(pack_state(case))
    r = subprocess.run([exe, XBE, scratch_path, in_path, out_path], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-3000:])
        sys.exit("harness failed")

    data = open(out_path, "rb").read()
    fails = collections.Counter()
    shown = 0
    for k in range(len(cases)):
        v = STATE.unpack_from(data, k * STATE.size)
        cid, regs, efl, ftop, fsw, fcw = v[0], v[1:9], v[9], v[10], v[11], v[12]
        st, mm, xmmb, crc, fault = v[13:21], v[21:29], v[29], v[30], v[31]
        case, exp = cases[cid], expected[cid]
        problems = []
        if fault:
            problems.append("host fault")
        names = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
        for n, a, b in zip(names, regs, exp["regs"]):
            if a != b:
                problems.append(f"{n}={a:08X} want {b:08X}")
        undefined = set()
        for ins in case.seq:
            for f, und in flag_effects(ins).items():
                (undefined.add if und else undefined.discard)(f)
        for f, bit in FLAG_BITS.items():
            if f not in undefined and (efl >> bit & 1) != (exp["eflags"] >> bit & 1):
                problems.append(f"{f}={efl >> bit & 1}")
        if case.use_x87:
            utop = (exp["fsw"] >> 11) & 7
            if ftop != utop:
                problems.append(f"ftop={ftop} want {utop}")
            else:
                transc = any(i.mnemonic in ("fsin", "fcos", "fptan", "fpatan", "fyl2x", "f2xm1", "fsincos")
                             for i in case.seq)
                for i in range(8):
                    phys = (utop + i) & 7
                    if (exp["ftag"] >> (2 * phys)) & 3 == 3:
                        continue  # empty slot
                    a, b = st[(ftop + i) & 7], exp["fp"][phys]
                    if not close(a, b, 1e-9 if transc else 1e-12):
                        problems.append(f"ST{i}={a!r} want {b!r}")
            if fcomp_used(case.seq) and (fsw & 0x4500) != (exp["fsw"] & 0x4500):
                problems.append(f"fsw C-bits {fsw & 0x4700:04X} want {exp['fsw'] & 0x4700:04X}")
        if case.use_mmx:
            for i in range(8):
                if mm[i] != exp["mm"][i]:
                    problems.append(f"mm{i}={mm[i]:016X} want {exp['mm'][i]:016X}")
        for i in range(8):
            got = int.from_bytes(xmmb[16 * i : 16 * i + 16], "little")
            if got != exp["xmm"][i]:
                problems.append(f"xmm{i}={got:032X} want {exp['xmm'][i]:032X}")
        if crc != exp["crc"]:
            problems.append("memory differs")
        if problems:
            key = " / ".join(sorted({i.mnemonic for i in case.seq}))
            fails[key] += 1
            if shown < args.show:
                shown += 1
                print(f"FAIL case {cid}:")
                for ins in case.seq:
                    print(f"    {ins.address:08X}  {ins.mnemonic} {ins.op_str}")
                print("    regs in: " + " ".join(f"{x:08X}" for x in case.regs) + f" eflags {case.eflags:X}")
                print("    " + "; ".join(problems[:8]))
    print(f"\n{len(cases)} cases, {sum(fails.values())} failing; skipped {sum(skipped.values())}")
    for k, n in skipped.most_common(8):
        print(f"  skipped {n:5d}  {k}")
    if fails:
        print("failing sequences by mnemonic set (top 25):")
        for k, n in fails.most_common(25):
            print(f"  {n:5d}  {k}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

"""x86-32 -> C++ translation of analysed functions.

Each guest function becomes `void f_XXXXXXXX(Ctx* c)`; each basic block a
label. Guest calls push the real return address on the guest stack, so stack
layouts (arguments, `ret n`, return-address reads) match the original.
Instructions are emitted in their own `{}` scope so temporaries never cross a
label.
"""
from capstone import x86

from .analysis import EXCEPT_BLOCK_CALL, JCC

REG32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
UT = {1: "u8", 2: "u16", 4: "u32", 8: "u64"}
STY = {1: "s8", 2: "s16", 4: "s32", 8: "s64"}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}

COND = {
    "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf",
    "be": "(cf|zf)", "a": "!(cf|zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf",
    "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf|(sf!=of))", "g": "(!zf&&sf==of)",
}
PREFIXES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x9B}

# MMX ops implemented on the low 64 bits of an SSE2 register.
MMX_BINOP = {
    "paddb": "_mm_add_epi8", "paddw": "_mm_add_epi16", "paddd": "_mm_add_epi32", "paddq": "_mm_add_epi64",
    "paddsb": "_mm_adds_epi8", "paddsw": "_mm_adds_epi16", "paddusb": "_mm_adds_epu8", "paddusw": "_mm_adds_epu16",
    "psubb": "_mm_sub_epi8", "psubw": "_mm_sub_epi16", "psubd": "_mm_sub_epi32", "psubq": "_mm_sub_epi64",
    "psubsb": "_mm_subs_epi8", "psubsw": "_mm_subs_epi16", "psubusb": "_mm_subs_epu8", "psubusw": "_mm_subs_epu16",
    "pmullw": "_mm_mullo_epi16", "pmulhw": "_mm_mulhi_epi16", "pmulhuw": "_mm_mulhi_epu16",
    "pmaddwd": "_mm_madd_epi16", "pmuludq": "_mm_mul_epu32",
    "pand": "_mm_and_si128", "pandn": "_mm_andnot_si128", "por": "_mm_or_si128", "pxor": "_mm_xor_si128",
    "pcmpeqb": "_mm_cmpeq_epi8", "pcmpeqw": "_mm_cmpeq_epi16", "pcmpeqd": "_mm_cmpeq_epi32",
    "pcmpgtb": "_mm_cmpgt_epi8", "pcmpgtw": "_mm_cmpgt_epi16", "pcmpgtd": "_mm_cmpgt_epi32",
    "pavgb": "_mm_avg_epu8", "pavgw": "_mm_avg_epu16", "pmaxsw": "_mm_max_epi16", "pminsw": "_mm_min_epi16",
    "pmaxub": "_mm_max_epu8", "pminub": "_mm_min_epu8", "psadbw": "_mm_sad_epu8",
}
MMX_SHIFT = {
    "psllw": "epi16", "pslld": "epi32", "psllq": "epi64", "psrlw": "epi16", "psrld": "epi32", "psrlq": "epi64",
    "psraw": "epi16", "psrad": "epi32",
}
MMX_UNPACK = {
    "punpcklbw": ("epi8", False), "punpcklwd": ("epi16", False), "punpckldq": ("epi32", False),
    "punpckhbw": ("epi8", True), "punpckhwd": ("epi16", True), "punpckhdq": ("epi32", True),
}
MMX_PACK = {"packsswb": "_mm_packs_epi16", "packssdw": "_mm_packs_epi32", "packuswb": "_mm_packus_epi16"}

# SSE1 packed/scalar single ops: dst = op(dst, src).
SSE_PS = {
    "addps": "_mm_add_ps", "subps": "_mm_sub_ps", "mulps": "_mm_mul_ps", "divps": "_mm_div_ps",
    "minps": "_mm_min_ps", "maxps": "_mm_max_ps", "andps": "_mm_and_ps", "andnps": "_mm_andnot_ps",
    "orps": "_mm_or_ps", "xorps": "_mm_xor_ps", "unpcklps": "_mm_unpacklo_ps", "unpckhps": "_mm_unpackhi_ps",
}
SSE_SS = {
    "addss": "_mm_add_ss", "subss": "_mm_sub_ss", "mulss": "_mm_mul_ss", "divss": "_mm_div_ss",
    "minss": "_mm_min_ss", "maxss": "_mm_max_ss",
}
SSE_UNARY = {"sqrtps": "_mm_sqrt_ps", "rcpps": "_mm_rcp_ps", "rsqrtps": "_mm_rsqrt_ps",
             "sqrtss": "_mm_sqrt_ss", "rcpss": "_mm_rcp_ss", "rsqrtss": "_mm_rsqrt_ss"}
SSE_CMP_PRED = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]
SSE_CMP = {f"cmp{p}{k}": (p, k) for p in SSE_CMP_PRED for k in ("ps", "ss")}
SSE_PS.update({k: v for k, v in SSE_UNARY.items() if k.endswith("ps")})
SSE_SS.update({k: v for k, v in SSE_UNARY.items() if k.endswith("ss")})


YIELD = "if (g_yield) rt_yield(c);"


class Unsupported(Exception):
    pass


def fname(va):
    return f"f_{va:08X}"


def hexu(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class InsnEmitter:
    """Translates one instruction into C statements."""

    def __init__(self, fn_emitter, ins):
        self.fe = fn_emitter
        self.ins = ins
        self.out = []
        self.has_ea = False

    # ------------------------------------------------------------- operands
    def reg_name(self, reg):
        return self.ins.reg_name(reg)

    def addr_expr(self, mem, with_segment=True):
        parts = []
        if mem.base:
            parts.append(self.reg_read(self.reg_name(mem.base)))
        if mem.index:
            idx = self.reg_read(self.reg_name(mem.index))
            parts.append(idx if mem.scale == 1 else f"{idx}*{mem.scale}")
        if mem.disp or not parts:
            parts.append(hexu(mem.disp))
        if with_segment and mem.segment:
            seg = self.reg_name(mem.segment)
            if seg == "fs":
                parts.append("c->fs_base")
            elif seg not in ("ds", "es", "ss", "cs"):
                raise Unsupported(f"segment {seg}")
        return "(u32)(" + " + ".join(parts) + ")" if len(parts) > 1 else parts[0]

    def mem_op(self):
        for op in self.ins.operands:
            if op.type == x86.X86_OP_MEM:
                return op
        return None

    def setup_ea(self):
        if self.has_ea:
            return
        op = self.mem_op()
        if op is not None:
            if self.ins.prefix[3] == 0x67:
                raise Unsupported("16-bit addressing")
            self.out.append(f"u32 ea = {self.addr_expr(op.mem)};")
            self.has_ea = True

    def reg_read(self, name):
        if name in REG32:
            return f"c->{name}"
        if name in REG16:
            return f"(u16)c->{REG16[name]}"
        if name in REG8L:
            return f"(u8)c->{REG8L[name]}"
        if name in REG8H:
            return f"(u8)(c->{REG8H[name]} >> 8)"
        raise Unsupported(f"register {name}")

    def reg_write(self, name, v):
        if name in REG32:
            return f"c->{name} = {v};"
        if name in REG16:
            return f"SET16(c->{REG16[name]}, {v});"
        if name in REG8L:
            return f"SET8L(c->{REG8L[name]}, {v});"
        if name in REG8H:
            return f"SET8H(c->{REG8H[name]}, {v});"
        raise Unsupported(f"register {name}")

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            return self.reg_read(self.reg_name(op.reg))
        if op.type == x86.X86_OP_IMM:
            return f"({UT[size]}){hexu(op.imm & MASK[size])}"
        if op.type == x86.X86_OP_MEM:
            self.setup_ea()
            return f"rd{size * 8}(ea)"
        raise Unsupported("operand")

    def wr(self, op, v):
        if op.type == x86.X86_OP_REG:
            return self.reg_write(self.reg_name(op.reg), v)
        if op.type == x86.X86_OP_MEM:
            self.setup_ea()
            return f"wr{op.size * 8}(ea, {v});"
        raise Unsupported("write operand")

    def emit(self, line):
        self.out.append(line)

    # ----------------------------------------------------------- dispatch
    def translate(self):
        ins = self.ins
        m = ins.mnemonic
        for p in ("rep ", "repe ", "repne ", "lock ", "repz ", "repnz "):
            if m.startswith(p):
                m = m[len(p):]
        raw = bytes(ins.bytes)
        i = 0
        while i < len(raw) and raw[i] in PREFIXES:
            i += 1
        if i < len(raw) and 0xD8 <= raw[i] <= 0xDF:
            self.x87(raw[i], raw[i + 1])
            return self.out
        if m == "wait":
            return self.out
        h = getattr(self, "i_" + m, None)
        if h is None:
            h = self.generic_lookup(m)
        if h is None:
            raise Unsupported(m)
        h(m)
        return self.out

    def generic_lookup(self, m):
        if m in ("add", "or", "adc", "sbb", "and", "sub", "xor", "cmp", "test"):
            return self.alu
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            return self.shift
        if m.startswith("set") and m[3:] in COND:
            return self.setcc
        if m.startswith("cmov") and m[4:] in COND:
            return self.cmov
        if m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd",
                 "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd"):
            if all(op.type == x86.X86_OP_MEM or op.type == x86.X86_OP_REG and self.reg_name(op.reg) in ("al", "ax", "eax")
                   for op in self.ins.operands):
                return self.string_op
        if m in ("nop", "fnop", "pause", "cli", "sti", "wbinvd", "invd", "prefetchnta", "prefetcht0",
                 "prefetcht1", "prefetcht2", "emms", "sfence", "lfence", "mfence"):
            return lambda m: None
        if m in MMX_BINOP or m in MMX_SHIFT or m in MMX_UNPACK or m in MMX_PACK:
            return self.mmx_op
        if m in SSE_PS or m in SSE_SS or m in SSE_CMP:
            return self.sse_op
        return None

    @property
    def lock(self):
        return self.ins.prefix[0] == 0xF0

    # ------------------------------------------------------------- integer
    def i_mov(self, m):
        d, s = self.ins.operands
        if d.type == x86.X86_OP_REG and self.reg_name(d.reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
            raise Unsupported("mov to segment")
        if s.type == x86.X86_OP_REG and self.reg_name(s.reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
            raise Unsupported("mov from segment")
        self.emit(self.wr(d, self.rd(s, d.size)))

    def i_movzx(self, m):
        d, s = self.ins.operands
        self.emit(self.wr(d, f"({UT[d.size]}){self.rd(s)}"))

    def i_movsx(self, m):
        d, s = self.ins.operands
        self.emit(self.wr(d, f"({UT[d.size]})({STY[d.size]})({STY[s.size]}){self.rd(s)}"))

    def i_lea(self, m):
        d, s = self.ins.operands
        self.emit(self.wr(d, f"({UT[d.size]}){self.addr_expr(s.mem, with_segment=False)}"))

    def i_xchg(self, m):
        a, b = self.ins.operands
        n = a.size
        if a.type == x86.X86_OP_MEM or b.type == x86.X86_OP_MEM:
            mem, reg = (a, b) if a.type == x86.X86_OP_MEM else (b, a)
            self.setup_ea()
            if n == 4:
                self.emit(f"u32 o = atomic_xchg32(ea, {self.rd(reg)}); {self.wr(reg, 'o')}")
            else:
                self.emit(f"{UT[n]} o = {self.rd(mem)}; {self.wr(mem, self.rd(reg))} {self.wr(reg, 'o')}")
            return
        self.emit(f"{UT[n]} t = {self.rd(a)}; {self.wr(a, self.rd(b))} {self.wr(b, 't')}")

    def alu(self, m):
        d, s = self.ins.operands
        n, t = d.size, UT[d.size]
        bits = n * 8
        if s.size != n and s.type == x86.X86_OP_IMM:
            src = f"({t}){hexu(s.imm & MASK[n])}"
        else:
            src = self.rd(s, n)
        self.setup_ea()
        if self.lock and d.type == x86.X86_OP_MEM and n == 4 and m in ("add", "sub", "or", "and", "xor"):
            self.emit(f"u32 b = {src};")
            if m == "sub":
                self.emit("u32 a = atomic_xadd32(ea, 0u - b);")
            else:
                self.emit(f"u32 a = atomic_{'xadd' if m == 'add' else m}32(ea, b);")
            op = {"add": "+", "sub": "-", "or": "|", "and": "&", "xor": "^"}[m]
            self.emit(f"u32 r = a {op} b; {self.alu_flags(m, bits)}")
            return
        self.emit(f"{t} a = {self.rd(d)}; {t} b = {src};")
        if m == "adc":
            self.emit(f"u32 ci = cf; {t} r = ({t})(a + b + ci); FL_ADC(a, b, r, ci, {bits});")
        elif m == "sbb":
            self.emit(f"u32 ci = cf; {t} r = ({t})(a - b - ci); FL_SBB(a, b, r, ci, {bits});")
        else:
            op = {"add": "+", "or": "|", "and": "&", "sub": "-", "xor": "^", "cmp": "-", "test": "&"}[m]
            self.emit(f"{t} r = ({t})(a {op} b); {self.alu_flags(m, bits)}")
        if m not in ("cmp", "test"):
            self.emit(self.wr(d, "r"))

    @staticmethod
    def alu_flags(m, bits):
        if m in ("add",):
            return f"FL_ADD(a, b, r, {bits});"
        if m in ("sub", "cmp"):
            return f"FL_SUB(a, b, r, {bits});"
        return f"FL_LOGIC(r, {bits});"

    def i_inc(self, m, delta="+"):
        (d,) = self.ins.operands
        n, t = d.size, UT[d.size]
        self.setup_ea()
        if self.lock and d.type == x86.X86_OP_MEM and n == 4:
            v = "1u" if delta == "+" else "0xFFFFFFFFu"
            self.emit(f"u32 a = atomic_xadd32(ea, {v}); u32 b = 1u; u32 r = a {delta} 1u;")
        else:
            self.emit(f"{t} a = {self.rd(d)}; {t} b = 1; {t} r = ({t})(a {delta} b);")
            self.emit(self.wr(d, "r"))
        fl = "FL_ADD" if delta == "+" else "FL_SUB"
        self.emit(f"u32 c0 = cf; {fl}(a, b, r, {n * 8}); cf = c0;")

    def i_dec(self, m):
        self.i_inc(m, "-")

    def i_neg(self, m):
        (d,) = self.ins.operands
        t = UT[d.size]
        self.emit(f"{t} a = 0; {t} b = {self.rd(d)}; {t} r = ({t})(a - b); FL_SUB(a, b, r, {d.size * 8});")
        self.emit(self.wr(d, "r"))

    def i_not(self, m):
        (d,) = self.ins.operands
        self.emit(self.wr(d, f"({UT[d.size]})~{self.rd(d)}"))

    def shift(self, m):
        ops = self.ins.operands
        d = ops[0]
        n, t, bits = d.size, UT[d.size], d.size * 8
        if len(ops) == 1:
            cnt, const = 1, True
        elif ops[1].type == x86.X86_OP_IMM:
            cnt, const = ops[1].imm & 31, True
        else:
            cnt, const = None, False
        if const and cnt == 0:
            return
        self.emit(f"{t} a = {self.rd(d)}; u32 n = {cnt if const else '(u32)(u8)c->ecx & 31u'};")
        body = []
        if m in ("shl", "sal"):
            body.append(f"{t} r = ({t})((u32)a << n); cf = n <= {bits} ? ((u32)a >> ({bits} - n)) & 1u : 0u;")
            body.append(f"of = MSB(r, {bits}) ^ cf; af = 0; FL_SZP(r, {bits});")
        elif m == "shr":
            body.append(f"{t} r = ({t})((u32)a >> n); cf = ((u32)a >> (n - 1)) & 1u; of = MSB(a, {bits}); af = 0; FL_SZP(r, {bits});")
        elif m == "sar":
            body.append(f"u32 k = n < {bits} ? n : {bits - 1}; {t} r = ({t})(({STY[n]})a >> k);")
            body.append(f"cf = (u32)((({STY[n]})a >> (n - 1 < {bits} ? n - 1 : {bits - 1})) & 1); of = 0; af = 0; FL_SZP(r, {bits});")
        elif m == "rol":
            body.append(f"u32 k = n % {bits}; {t} r = ({t})(k ? ((u32)a << k) | ((u32)a >> ({bits} - k)) : a);")
            body.append(f"cf = r & 1u; of = MSB(r, {bits}) ^ cf;")
        elif m == "ror":
            body.append(f"u32 k = n % {bits}; {t} r = ({t})(k ? ((u32)a >> k) | ((u32)a << ({bits} - k)) : a);")
            body.append(f"cf = MSB(r, {bits}); of = MSB(r, {bits}) ^ MSB(r, {bits - 1});")
        elif m in ("rcl", "rcr"):
            k = f"n % {bits + 1}" if bits < 32 else "n"
            body.append(f"u32 a0 = a; {t} r = ({t}){m}_n(a, {k}, {bits}, &cf);")
            if m == "rcl":
                body.append(f"of = MSB(r, {bits}) ^ cf;")
            else:
                body.append(f"of = MSB(r, {bits}) ^ MSB(r, {bits - 1});")
        body.append(self.wr(d, "r"))
        if const:
            self.out.extend(body)
        else:
            self.emit("if (n) {")
            self.out.extend("  " + b for b in body)
            self.emit("}")

    def i_shld(self, m, left=True):
        d, s, cntop = self.ins.operands
        if d.size != 4:
            raise Unsupported("16-bit shld/shrd")
        cnt = f"{cntop.imm & 31}u" if cntop.type == x86.X86_OP_IMM else "((u32)(u8)c->ecx & 31u)"
        self.emit(f"u32 a = {self.rd(d)}; u32 b = {self.rd(s)}; u32 n = {cnt};")
        self.emit("if (n) {")
        if left:
            self.emit("  u32 r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1u;")
        else:
            self.emit("  u32 r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1u;")
        self.emit(f"  of = MSB(r ^ a, 32); af = 0; FL_SZP(r, 32); {self.wr(d, 'r')}")
        self.emit("}")

    def i_shrd(self, m):
        self.i_shld(m, left=False)

    def i_mul(self, m):
        (s,) = self.ins.operands
        n = s.size
        if n == 4:
            self.emit(f"u64 p = (u64)c->eax * {self.rd(s)}; c->eax = (u32)p; c->edx = (u32)(p >> 32); cf = of = c->edx != 0;")
        elif n == 2:
            self.emit(f"u32 p = (u32)(u16)c->eax * {self.rd(s)}; SET16(c->eax, p); SET16(c->edx, p >> 16); cf = of = (p >> 16) != 0;")
        else:
            self.emit(f"u32 p = (u32)(u8)c->eax * {self.rd(s)}; SET16(c->eax, p); cf = of = (p >> 8) != 0;")

    def i_imul(self, m):
        ops = self.ins.operands
        if len(ops) == 1:
            (s,) = ops
            n = s.size
            if n == 4:
                self.emit(f"s64 p = (s64)(s32)c->eax * (s32){self.rd(s)}; c->eax = (u32)p; c->edx = (u32)((u64)p >> 32); cf = of = p != (s32)p;")
            elif n == 2:
                self.emit(f"s32 p = (s32)(s16)c->eax * (s16){self.rd(s)}; SET16(c->eax, p); SET16(c->edx, (u32)p >> 16); cf = of = p != (s16)p;")
            else:
                self.emit(f"s32 p = (s32)(s8)c->eax * (s8){self.rd(s)}; SET16(c->eax, p); cf = of = p != (s8)p;")
            return
        d = ops[0]
        a, b = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
        n = d.size
        st = STY[n]
        bv = f"({st}){hexu(b.imm & MASK[n])}" if b.type == x86.X86_OP_IMM else f"({st}){self.rd(b)}"
        self.emit(f"s64 p = (s64)({st}){self.rd(a)} * (s64){bv}; {UT[n]} r = ({UT[n]})p; cf = of = p != ({st})r; FL_SZP(r, {n * 8});")
        self.emit(self.wr(d, "r"))

    def i_div(self, m):
        (s,) = self.ins.operands
        n = s.size
        if n == 4:
            self.emit(f"u64 a = ((u64)c->edx << 32) | c->eax; u32 b = {self.rd(s)}; c->eax = (u32)(a / b); c->edx = (u32)(a % b);")
        elif n == 2:
            self.emit(f"u32 a = ((u32)(u16)c->edx << 16) | (u16)c->eax; u32 b = {self.rd(s)}; SET16(c->eax, a / b); SET16(c->edx, a % b);")
        else:
            self.emit(f"u32 a = (u16)c->eax; u32 b = {self.rd(s)}; SET8L(c->eax, a / b); SET8H(c->eax, a % b);")

    def i_idiv(self, m):
        (s,) = self.ins.operands
        n = s.size
        if n == 4:
            self.emit(f"s64 a = (s64)(((u64)c->edx << 32) | c->eax); s32 b = (s32){self.rd(s)}; c->eax = (u32)(s32)(a / b); c->edx = (u32)(s32)(a % b);")
        elif n == 2:
            self.emit(f"s32 a = (s32)(((u32)(u16)c->edx << 16) | (u16)c->eax); s32 b = (s16){self.rd(s)}; SET16(c->eax, a / b); SET16(c->edx, a % b);")
        else:
            self.emit(f"s32 a = (s16)c->eax; s32 b = (s8){self.rd(s)}; SET8L(c->eax, a / b); SET8H(c->eax, a % b);")

    def i_cdq(self, m):
        self.emit("c->edx = (u32)((s32)c->eax >> 31);")

    def i_cwde(self, m):
        self.emit("c->eax = (u32)(s32)(s16)c->eax;")

    def i_cbw(self, m):
        self.emit("SET16(c->eax, (s16)(s8)c->eax);")

    def i_cwd(self, m):
        self.emit("SET16(c->edx, (c->eax & 0x8000u) ? 0xFFFFu : 0u);")

    def i_bswap(self, m):
        (d,) = self.ins.operands
        self.emit(self.wr(d, f"__builtin_bswap32({self.rd(d)})"))

    def i_xadd(self, m):
        d, s = self.ins.operands
        n, t = d.size, UT[d.size]
        self.setup_ea()
        if d.type == x86.X86_OP_MEM and n == 4 and self.lock:
            self.emit(f"u32 b = {self.rd(s)}; u32 a = atomic_xadd32(ea, b); u32 r = a + b;")
        else:
            self.emit(f"{t} a = {self.rd(d)}; {t} b = {self.rd(s)}; {t} r = ({t})(a + b); {self.wr(d, 'r')}")
        self.emit(f"FL_ADD(a, b, r, {n * 8}); {self.wr(s, 'a')}")

    def i_cmpxchg(self, m):
        d, s = self.ins.operands
        n, t = d.size, UT[d.size]
        acc = {4: "eax", 2: "ax", 1: "al"}[n]
        self.setup_ea()
        if d.type == x86.X86_OP_MEM and n == 4:
            self.emit(f"u32 a = c->eax; u32 b = atomic_cmpxchg32(ea, {self.rd(s)}, a); u32 r = a - b; FL_SUB(a, b, r, 32);")
            self.emit("if (!zf) c->eax = b;")
            return
        self.emit(f"{t} a = {self.reg_read(acc)}; {t} b = {self.rd(d)}; {t} r = ({t})(a - b); FL_SUB(a, b, r, {n * 8});")
        self.emit(f"if (zf) {{ {self.wr(d, self.rd(s))} }} else {{ {self.reg_write(acc, 'b')} }}")

    def bit_op(self, m):
        d, s = self.ins.operands
        n, t, bits = d.size, UT[d.size], d.size * 8
        if d.type == x86.X86_OP_MEM and s.type == x86.X86_OP_REG:
            # Register bit offsets address a bit string beyond the operand.
            self.setup_ea()
            self.emit(f"s32 off = (s32){self.rd(s)}; ea += (u32)((off >> 5) * 4); u32 n = (u32)off & 31u;")
            t, bits, rd, wr = "u32", 32, "rd32(ea)", lambda v: f"wr32(ea, {v});"
        else:
            nexpr = hexu(s.imm & (bits - 1)) if s.type == x86.X86_OP_IMM else f"((u32){self.rd(s)} & {bits - 1}u)"
            self.emit(f"u32 n = {nexpr};")
            rd, wr = self.rd(d), lambda v: self.wr(d, v)
        self.emit(f"{t} a = {rd}; cf = (a >> n) & 1u;")
        if m == "bts":
            self.emit(wr(f"({t})(a | (1u << n))"))
        elif m == "btr":
            self.emit(wr(f"({t})(a & ~(1u << n))"))
        elif m == "btc":
            self.emit(wr(f"({t})(a ^ (1u << n))"))

    i_bt = i_bts = i_btr = i_btc = bit_op

    def i_bsf(self, m):
        d, s = self.ins.operands
        fn = "__builtin_ctz" if m == "bsf" else "31 - __builtin_clz"
        self.emit(f"u32 a = {self.rd(s)}; zf = a == 0; if (a) {{ {self.wr(d, f'({UT[d.size]})({fn}(a))')} }}")

    i_bsr = i_bsf

    def setcc(self, m):
        (d,) = self.ins.operands
        self.emit(self.wr(d, f"(u8){COND[m[3:]]}"))

    def cmov(self, m):
        d, s = self.ins.operands
        self.emit(f"{UT[d.size]} v = {self.rd(s)}; if ({COND[m[4:]]}) {{ {self.wr(d, 'v')} }}")

    # ------------------------------------------------------------- stack
    def i_push(self, m):
        (s,) = self.ins.operands
        if s.type == x86.X86_OP_REG and self.reg_name(s.reg) in SEGMENT_SELECTORS:
            # Flat model: segment registers hold fixed selectors.
            self.emit(f"c->esp -= 4; wr32(c->esp, {hexu(SEGMENT_SELECTORS[self.reg_name(s.reg)])});")
            return
        if s.size == 2:
            self.emit(f"u16 v = {self.rd(s)}; c->esp -= 2; wr16(c->esp, v);")
        else:
            v = hexu(s.imm) if s.type == x86.X86_OP_IMM else self.rd(s)
            self.emit(f"u32 v = {v}; c->esp -= 4; wr32(c->esp, v);")

    def i_pop(self, m):
        (d,) = self.ins.operands
        if d.type == x86.X86_OP_REG and self.reg_name(d.reg) in SEGMENT_SELECTORS:
            self.emit("c->esp += 4;")  # selector discarded: segments stay flat
            return
        n = d.size
        self.emit(f"{UT[n]} v = rd{n * 8}(c->esp); c->esp += {n};")
        if d.type == x86.X86_OP_MEM:
            # The destination address uses esp after the increment.
            self.emit(f"u32 ea = {self.addr_expr(d.mem)}; wr{n * 8}(ea, v);")
            self.has_ea = True
        else:
            self.emit(self.wr(d, "v"))

    def i_pushal(self, m):
        self.emit("u32 s0 = c->esp;")
        for r in ("eax", "ecx", "edx", "ebx", "s0", "ebp", "esi", "edi"):
            v = r if r == "s0" else f"c->{r}"
            self.emit(f"c->esp -= 4; wr32(c->esp, {v});")

    def i_popal(self, m):
        for r in ("edi", "esi", "ebp", None, "ebx", "edx", "ecx", "eax"):
            if r:
                self.emit(f"c->{r} = rd32(c->esp);")
            self.emit("c->esp += 4;")

    i_pushad, i_popad = i_pushal, i_popal

    def i_pushfd(self, m):
        self.emit("c->esp -= 4; wr32(c->esp, FLAGS_PACK());")

    def i_popfd(self, m):
        self.emit("u32 v = rd32(c->esp); c->esp += 4; FLAGS_UNPACK(v);")

    i_pushf, i_popf = i_pushfd, i_popfd

    def i_leave(self, m):
        self.emit("c->esp = c->ebp; c->ebp = rd32(c->esp); c->esp += 4;")

    def i_enter(self, m):
        size, level = (op.imm for op in self.ins.operands)
        if level:
            raise Unsupported("enter with nesting")
        self.emit(f"c->esp -= 4; wr32(c->esp, c->ebp); c->ebp = c->esp; c->esp -= {hexu(size)};")

    # ------------------------------------------------------------- flags/misc
    def i_sahf(self, m):
        self.emit("u32 v = (c->eax >> 8) & 0xFFu; cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1;")

    def i_lahf(self, m):
        self.emit("SET8H(c->eax, cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7));")

    def i_clc(self, m):
        self.emit("cf = 0;")

    def i_stc(self, m):
        self.emit("cf = 1;")

    def i_cmc(self, m):
        self.emit("cf ^= 1;")

    def i_cld(self, m):
        self.emit("c->df = 0;")

    def i_std(self, m):
        self.emit("c->df = 1;")

    def i_xlatb(self, m):
        self.emit("SET8L(c->eax, rd8(c->ebx + (u8)c->eax));")

    def i_int3(self, m):
        pass  # the block terminator emits the trap

    def i_cpuid(self, m):
        self.emit("rt_cpuid(c);")

    def i_rdtsc(self, m):
        self.emit("u64 t = rt_rdtsc(c); c->eax = (u32)t; c->edx = (u32)(t >> 32);")

    def i_int(self, m):
        self.emit(f"rt_int(c, {hexu(self.ins.address)}, {self.ins.operands[0].imm & 0xFF}u);")

    def i_in(self, m):
        d, p = self.ins.operands
        port = hexu(p.imm) if p.type == x86.X86_OP_IMM else "(u16)c->edx"
        self.emit(self.wr(d, f"({UT[d.size]})rt_in(c, {port}, {d.size})"))

    def i_out(self, m):
        p, s = self.ins.operands
        port = hexu(p.imm) if p.type == x86.X86_OP_IMM else "(u16)c->edx"
        self.emit(f"rt_out(c, {port}, {self.rd(s)}, {s.size});")

    # ------------------------------------------------------------- strings
    def string_op(self, m):
        kind, n = m[:4], {"b": 1, "w": 2, "d": 4}[m[4]]
        t, bits = UT[n], n * 8
        acc = {1: "al", 2: "ax", 4: "eax"}[n]
        rep = self.ins.prefix[0] in (0xF3, 0xF2)
        repne = self.ins.prefix[0] == 0xF2
        step = f"(c->df ? (u32)-{n} : {n}u)"
        if kind == "movs" and rep:
            self.emit(f"rt_rep_movs(c, {n});")
            return
        if kind == "stos" and rep:
            self.emit(f"rt_rep_stos(c, {n});")
            return
        if kind == "movs":
            body = f"wr{bits}(c->edi, rd{bits}(c->esi)); c->esi += {step}; c->edi += {step};"
        elif kind == "stos":
            body = f"wr{bits}(c->edi, {self.reg_read(acc)}); c->edi += {step};"
        elif kind == "lods":
            body = f"{self.reg_write(acc, f'rd{bits}(c->esi)')} c->esi += {step};"
        elif kind == "scas":
            body = (f"{t} a = {self.reg_read(acc)}; {t} b = rd{bits}(c->edi); {t} r = ({t})(a - b); "
                    f"FL_SUB(a, b, r, {bits}); c->edi += {step};")
        elif kind == "cmps":
            body = (f"{t} a = rd{bits}(c->esi); {t} b = rd{bits}(c->edi); {t} r = ({t})(a - b); "
                    f"FL_SUB(a, b, r, {bits}); c->esi += {step}; c->edi += {step};")
        else:
            raise Unsupported(m)
        if not rep:
            self.emit(body)
            return
        cond = ""
        if kind in ("scas", "cmps"):
            cond = f" if ({'zf' if repne else '!zf'}) break;"
        self.emit(f"while (c->ecx) {{ {body} c->ecx--;{cond} }}")

    # ------------------------------------------------------------- MMX/SSE
    def is_mm(self, op):
        return op.type == x86.X86_OP_REG and self.reg_name(op.reg).startswith("mm")

    def is_xmm(self, op):
        return op.type == x86.X86_OP_REG and self.reg_name(op.reg).startswith("xmm")

    def mm(self, op):
        return f"c->mm[{self.reg_name(op.reg)[2:]}]"

    def xmm(self, op):
        return f"c->xmm[{self.reg_name(op.reg)[3:]}].v"

    def q(self, op):
        """64-bit source: mm register, memory, or the low half of an xmm."""
        if self.is_mm(op):
            return self.mm(op)
        if self.is_xmm(op):
            return f"x_low_u64({self.xmm(op)})"
        if op.type == x86.X86_OP_MEM:
            self.setup_ea()
            return "rd64(ea)"
        raise Unsupported("mmx operand")

    def xsrc(self, op, size=16):
        """xmm source; memory operands load `size` bytes (16, 8 or 4)."""
        if self.is_xmm(op):
            return self.xmm(op)
        if op.type == x86.X86_OP_MEM:
            self.setup_ea()
            return {16: "rdx128(ea)", 8: "rdx64(_mm_setzero_ps(), ea)", 4: "rdx32(ea)"}[size]
        raise Unsupported("sse operand")

    def mmx_op(self, m):
        ops = self.ins.operands
        d = ops[0]
        dst = self.mm(d)
        if m in MMX_SHIFT:
            lanes = MMX_SHIFT[m]
            kind = {"psl": "sll", "psr": "srl" if m[3] == "l" else "sra"}[m[:3]]
            if ops[1].type == x86.X86_OP_IMM:
                self.emit(f"{dst} = X2M(_mm_{kind}i_{lanes}(M2X({dst}), {ops[1].imm & 0xFF}));")
            else:
                self.emit(f"u64 b = {self.q(ops[1])}; {dst} = X2M(_mm_{kind}_{lanes}(M2X({dst}), M2X(b)));")
            return
        self.emit(f"u64 b = {self.q(ops[1])}; __m128i A = M2X({dst}), B = M2X(b);")
        if m in MMX_BINOP:
            self.emit(f"{dst} = X2M({MMX_BINOP[m]}(A, B));")
        elif m in MMX_UNPACK:
            lanes, high = MMX_UNPACK[m]
            x = f"_mm_unpacklo_{lanes}(A, B)"
            self.emit(f"{dst} = X2M({'_mm_srli_si128(' + x + ', 8)' if high else x});")
        elif m in MMX_PACK:
            self.emit(f"__m128i X = _mm_unpacklo_epi64(A, B); {dst} = X2M({MMX_PACK[m]}(X, X));")

    def i_movq(self, m):
        d, s = self.ins.operands
        if self.is_mm(d):
            self.emit(f"{self.mm(d)} = {self.q(s)};")
        elif self.is_xmm(d):
            self.emit(f"{self.xmm(d)} = x_from_u64({self.q(s)});")
        else:
            self.setup_ea()
            self.emit(f"wr64(ea, {self.q(s)});")

    i_movntq = i_movq

    def i_movd(self, m):
        d, s = self.ins.operands
        if self.is_mm(d):
            self.emit(f"{self.mm(d)} = (u64){self.rd(s, 4)};")
        elif self.is_xmm(d):
            self.emit(f"{self.xmm(d)} = _mm_castsi128_ps(_mm_cvtsi32_si128((int){self.rd(s, 4)}));")
        elif self.is_mm(s):
            self.emit(self.wr(d, f"(u32){self.mm(s)}"))
        else:
            self.emit(self.wr(d, f"(u32)_mm_cvtsi128_si32(_mm_castps_si128({self.xmm(s)}))"))

    def i_pshufw(self, m):
        d, s, imm = self.ins.operands
        self.emit(f"{self.mm(d)} = X2M(_mm_shufflelo_epi16(M2X({self.q(s)}), {imm.imm & 0xFF}));")

    def i_pmovmskb(self, m):
        d, s = self.ins.operands
        self.emit(self.wr(d, f"(u32)(_mm_movemask_epi8(M2X({self.q(s)})) & 0xFF)"))

    def i_pextrw(self, m):
        d, s, imm = self.ins.operands
        self.emit(self.wr(d, f"(u32)(u16)({self.q(s)} >> {16 * (imm.imm & 3)})"))

    def i_pinsrw(self, m):
        d, s, imm = self.ins.operands
        sh = 16 * (imm.imm & 3)
        self.emit(f"u64 v = (u16){self.rd(s, 2)}; {self.mm(d)} = ({self.mm(d)} & ~(0xFFFFull << {sh})) | (v << {sh});")

    def i_movaps(self, m):
        d, s = self.ins.operands
        if self.is_xmm(d):
            self.emit(f"{self.xmm(d)} = {self.xsrc(s)};")
        else:
            self.setup_ea()
            self.emit(f"wrx128(ea, {self.xmm(s)});")

    i_movups = i_movntps = i_movaps

    def i_movss(self, m):
        d, s = self.ins.operands
        if self.is_xmm(d) and self.is_xmm(s):
            self.emit(f"{self.xmm(d)} = _mm_move_ss({self.xmm(d)}, {self.xmm(s)});")
        elif self.is_xmm(d):
            self.emit(f"{self.xmm(d)} = {self.xsrc(s, 4)};")
        else:
            self.setup_ea()
            self.emit(f"_mm_store_ss((float*)gptr(ea), {self.xmm(s)});")

    def i_movlps(self, m, high=False):
        d, s = self.ins.operands
        self.setup_ea()
        if self.is_xmm(d):
            self.emit(f"{self.xmm(d)} = rdx{'h' if high else ''}64({self.xmm(d)}, ea);")
        else:
            self.emit(f"wrx{'h' if high else ''}64(ea, {self.xmm(s)});")

    def i_movhps(self, m):
        self.i_movlps(m, high=True)

    def i_movlhps(self, m):
        d, s = self.ins.operands
        self.emit(f"{self.xmm(d)} = _mm_movelh_ps({self.xmm(d)}, {self.xmm(s)});")

    def i_movhlps(self, m):
        d, s = self.ins.operands
        self.emit(f"{self.xmm(d)} = _mm_movehl_ps({self.xmm(d)}, {self.xmm(s)});")

    def i_shufps(self, m):
        d, s, imm = self.ins.operands
        self.emit(f"{self.xmm(d)} = _mm_shuffle_ps({self.xmm(d)}, {self.xsrc(s)}, {imm.imm & 0xFF});")

    def sse_op(self, m):
        d, s = self.ins.operands[:2]
        dst = self.xmm(d)
        if m in SSE_CMP:
            pred, kind = SSE_CMP[m]
            src = self.xsrc(s, 16 if kind == "ps" else 4)
            self.emit(f"{dst} = _mm_cmp{pred}_{kind}({dst}, {src});")
            return
        if m in SSE_UNARY:
            if m.endswith("ss"):
                self.emit(f"{dst} = _mm_move_ss({dst}, {SSE_UNARY[m]}({self.xsrc(s, 4)}));")
            else:
                self.emit(f"{dst} = {SSE_UNARY[m]}({self.xsrc(s)});")
            return
        if m in SSE_SS:
            self.emit(f"{dst} = {SSE_SS[m]}({dst}, {self.xsrc(s, 4)});")
            return
        self.emit(f"{dst} = {SSE_PS[m]}({dst}, {self.xsrc(s)});")

    def i_comiss(self, m):
        a, b = self.ins.operands
        self.emit(f"FCOMI(_mm_cvtss_f32({self.xmm(a)}), _mm_cvtss_f32({self.xsrc(b, 4)}));")

    i_ucomiss = i_comiss

    def i_cvtps2pi(self, m, trunc=False):
        d, s = self.ins.operands
        fn = "_mm_cvttps_epi32" if trunc else "_mm_cvtps_epi32"
        self.emit(f"{self.mm(d)} = X2M({fn}({self.xsrc(s, 8)}));")

    def i_cvttps2pi(self, m):
        self.i_cvtps2pi(m, trunc=True)

    def i_cvtpi2ps(self, m):
        d, s = self.ins.operands
        self.emit(f"__m128 f = _mm_cvtepi32_ps(M2X({self.q(s)})); {self.xmm(d)} = _mm_shuffle_ps(f, {self.xmm(d)}, 0xE4);")

    def i_cvtsi2ss(self, m):
        d, s = self.ins.operands
        self.emit(f"{self.xmm(d)} = _mm_cvtsi32_ss({self.xmm(d)}, (int){self.rd(s, 4)});")

    def i_cvtss2si(self, m, trunc=False):
        d, s = self.ins.operands
        fn = "_mm_cvttss_si32" if trunc else "_mm_cvtss_si32"
        self.emit(self.wr(d, f"(u32){fn}({self.xsrc(s, 4)})"))

    def i_cvttss2si(self, m):
        self.i_cvtss2si(m, trunc=True)

    def i_movmskps(self, m):
        d, s = self.ins.operands
        self.emit(self.wr(d, f"(u32)_mm_movemask_ps({self.xmm(s)})"))

    def i_ldmxcsr(self, m):
        self.setup_ea()
        self.emit("c->mxcsr = rd32(ea);")

    def i_stmxcsr(self, m):
        self.setup_ea()
        self.emit("wr32(ea, c->mxcsr);")

    # ------------------------------------------------------------- x87
    def fmem(self, kind):
        self.setup_ea()
        return {
            "f32": "(double)rdf32(ea)", "f64": "rdf64(ea)", "f80": "rdf80(ea)",
            "i16": "(double)(s16)rd16(ea)", "i32": "(double)(s32)rd32(ea)", "i64": "(double)(s64)rd64(ea)",
        }[kind]

    def farith(self, op, dst, a, b):
        """dst = a op b for op index 0..7 of the D8 group (0..1, 4..7)."""
        e = self.emit
        if op == 0:
            e(f"{dst} = {a} + {b};")
        elif op == 1:
            e(f"{dst} = {a} * {b};")
        elif op == 4:
            e(f"{dst} = {a} - {b};")
        elif op == 5:
            e(f"{dst} = {b} - {a};")
        elif op == 6:
            e(f"{dst} = {a} / {b};")
        elif op == 7:
            e(f"{dst} = {b} / {a};")

    def x87(self, op, modrm):
        e = self.emit
        mod, r, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        if mod != 3:
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                kind = {0xD8: "f32", 0xDC: "f64", 0xDA: "i32", 0xDE: "i16"}[op]
                e(f"double m = {self.fmem(kind)};")
                if r in (2, 3):
                    e("fcom(c, ST(0), m);")
                    if r == 3:
                        e("fpop(c);")
                else:
                    self.farith(r, "ST(0)", "ST(0)", "m")
                return
            if op == 0xD9:
                if r == 0:
                    return e(f"fpush(c, {self.fmem('f32')});")
                if r in (2, 3):
                    self.setup_ea()
                    e("wrf32(ea, (float)ST(0));" + (" fpop(c);" if r == 3 else ""))
                    return
                if r == 5:
                    self.setup_ea()
                    return e("c->fcw = rd16(ea);")
                if r == 7:
                    self.setup_ea()
                    return e("wr16(ea, c->fcw);")
                if r in (4, 6):  # fldenv / fnstenv
                    self.setup_ea()
                    return e(f"fenv(c, ea, {1 if r == 6 else 0});")
                raise Unsupported(f"x87 D9 /{r}")
            if op == 0xDD:
                if r == 0:
                    return e(f"fpush(c, {self.fmem('f64')});")
                if r in (2, 3):
                    self.setup_ea()
                    e("wrf64(ea, ST(0));" + (" fpop(c);" if r == 3 else ""))
                    return
                if r == 7:
                    self.setup_ea()
                    return e("wr16(ea, fsw_read(c));")
                if r in (4, 6):  # frstor / fnsave
                    self.setup_ea()
                    return e(f"fsave(c, ea, {1 if r == 6 else 0});")
                raise Unsupported(f"x87 DD /{r}")
            if op == 0xDB:
                if r == 0:
                    return e(f"fpush(c, {self.fmem('i32')});")
                if r in (2, 3):
                    self.setup_ea()
                    e("wr32(ea, (u32)fist32(c, ST(0)));" + (" fpop(c);" if r == 3 else ""))
                    return
                if r == 5:
                    return e(f"fpush(c, {self.fmem('f80')});")
                if r == 7:
                    self.setup_ea()
                    return e("wrf80(ea, ST(0)); fpop(c);")
                raise Unsupported(f"x87 DB /{r}")
            if op == 0xDF:
                if r == 0:
                    return e(f"fpush(c, {self.fmem('i16')});")
                if r in (2, 3):
                    self.setup_ea()
                    e("wr16(ea, (u16)fist16(c, ST(0)));" + (" fpop(c);" if r == 3 else ""))
                    return
                if r == 5:
                    return e(f"fpush(c, {self.fmem('i64')});")
                if r == 7:
                    self.setup_ea()
                    return e("wr64(ea, (u64)fist64(c, ST(0))); fpop(c);")
                raise Unsupported(f"x87 DF /{r}")
            raise Unsupported(f"x87 {op:02X} /{r}")

        i = rm
        if op == 0xD8:
            if r in (2, 3):
                e(f"fcom(c, ST(0), ST({i}));" + (" fpop(c);" if r == 3 else ""))
            else:
                self.farith(r, "ST(0)", "ST(0)", f"ST({i})")
            return
        if op in (0xDC, 0xDE):
            pop = " fpop(c);" if op == 0xDE else ""
            if op == 0xDE and modrm == 0xD9:
                return e("fcom(c, ST(0), ST(1)); fpop(c); fpop(c);")
            if r in (2, 3):
                e(f"fcom(c, ST(0), ST({i}));" + (" fpop(c);" if r == 3 else ""))
                return
            # Register forms here store into ST(i); the sub/div pairs are
            # mirrored relative to D8 (see Intel SDM FSUB/FSUBR).
            dst = f"ST({i})"
            if r == 0:
                e(f"{dst} = ST({i}) + ST(0);")
            elif r == 1:
                e(f"{dst} = ST({i}) * ST(0);")
            elif r == 4:
                e(f"{dst} = ST(0) - ST({i});")
            elif r == 5:
                e(f"{dst} = ST({i}) - ST(0);")
            elif r == 6:
                e(f"{dst} = ST(0) / ST({i});")
            elif r == 7:
                e(f"{dst} = ST({i}) / ST(0);")
            if pop:
                e(pop.strip())
            return
        if op == 0xD9:
            simple = {
                0xE0: "ST(0) = -ST(0);", 0xE1: "ST(0) = fabs(ST(0));", 0xE4: "fcom(c, ST(0), 0.0);",
                0xE5: "fxam(c);", 0xE8: "fpush(c, 1.0);", 0xE9: "fpush(c, 3.321928094887362347870);",
                0xEA: "fpush(c, 1.442695040888963407360);", 0xEB: "fpush(c, 3.141592653589793238463);",
                0xEC: "fpush(c, 0.301029995663981195214);", 0xED: "fpush(c, 0.693147180559945309417);",
                0xEE: "fpush(c, 0.0);", 0xF0: "ST(0) = exp2(ST(0)) - 1.0;",
                0xF1: "ST(1) = ST(1) * log2(ST(0)); fpop(c);",
                0xF2: "ST(0) = tan(ST(0)); fpush(c, 1.0); c->fsw &= ~FSW_C2;",
                0xF3: "ST(1) = atan2(ST(1), ST(0)); fpop(c);", 0xF4: "fxtract(c);",
                0xF5: "fprem(c, 1);", 0xF6: "c->ftop = (c->ftop - 1) & 7;", 0xF7: "c->ftop = (c->ftop + 1) & 7;",
                0xF8: "fprem(c, 0);", 0xF9: "ST(1) = ST(1) * log2(ST(0) + 1.0); fpop(c);",
                0xFA: "ST(0) = sqrt(ST(0));",
                0xFB: "{ double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); c->fsw &= ~FSW_C2; }",
                0xFC: "ST(0) = frnd(c, ST(0));", 0xFD: "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));",
                0xFE: "ST(0) = sin(ST(0)); c->fsw &= ~FSW_C2;", 0xFF: "ST(0) = cos(ST(0)); c->fsw &= ~FSW_C2;",
                0xD0: "",
            }
            if modrm in simple:
                return e(simple[modrm]) if simple[modrm] else None
            if r == 0:
                return e(f"{{ double v = ST({i}); fpush(c, v); }}")
            if r == 1:
                return e(f"{{ double t = ST(0); ST(0) = ST({i}); ST({i}) = t; }}")
            if r == 3:
                return e(f"ST({i}) = ST(0); fpop(c);")
            raise Unsupported(f"x87 D9 {modrm:02X}")
        if op == 0xDA:
            if modrm == 0xE9:
                return e("fcom(c, ST(0), ST(1)); fpop(c); fpop(c);")
            cc = {0: "cf", 1: "zf", 2: "(cf|zf)", 3: "pf"}.get(r)
            if cc:
                return e(f"if ({cc}) ST(0) = ST({i});")
            raise Unsupported(f"x87 DA {modrm:02X}")
        if op == 0xDB:
            if modrm == 0xE2:
                return e("c->fsw &= 0x7F00;")
            if modrm == 0xE3:
                return e("c->fcw = 0x37F; c->fsw = 0; c->ftop = 0;")
            cc = {0: "!cf", 1: "!zf", 2: "!(cf|zf)", 3: "!pf"}.get(r)
            if cc:
                return e(f"if ({cc}) ST(0) = ST({i});")
            if r in (5, 6):
                return e(f"FCOMI(ST(0), ST({i}));")
            raise Unsupported(f"x87 DB {modrm:02X}")
        if op == 0xDD:
            if r == 0:
                return  # ffree: tags are not modelled
            if r == 2:
                return e(f"ST({i}) = ST(0);")
            if r == 3:
                return e(f"ST({i}) = ST(0); fpop(c);")
            if r in (4, 5):
                return e(f"fcom(c, ST(0), ST({i}));" + (" fpop(c);" if r == 5 else ""))
            raise Unsupported(f"x87 DD {modrm:02X}")
        if op == 0xDF:
            if modrm == 0xE0:
                return e("SET16(c->eax, fsw_read(c));")
            if r in (5, 6):
                return e(f"FCOMI(ST(0), ST({i})); fpop(c);")
            raise Unsupported(f"x87 DF {modrm:02X}")
        raise Unsupported(f"x87 {op:02X} {modrm:02X}")


# Xbox kernel GDT selectors (flat 4 GB code/data, fs = KPCR).
SEGMENT_SELECTORS = {"cs": 0x08, "ds": 0x10, "es": 0x10, "ss": 0x10, "fs": 0x28, "gs": 0x10}

# Accessors routed through the hardware register check in XDK hardware
# layers (see io_rd32 in x86_recomp.h).
MMIO_ACCESSORS = ("rd8", "rd16", "rd32", "wr8", "wr16", "wr32")


class FunctionEmitter:
    def __init__(self, fn, known, overrides=None, mmio=False, eh_frame=False):
        self.fn = fn
        self.known = known  # set of function starts that have a definition
        self.overrides = overrides or {}
        self.mmio = mmio  # may touch hardware registers: checked accessors
        self.eh_frame = eh_frame  # a catch block resumes in this function's frame
        self.unsupported = []

    def callee(self, va):
        return self.overrides.get(va, fname(va))

    def insn(self, ins):
        try:
            return InsnEmitter(self, ins).translate()
        except Unsupported as ex:
            text = f"{ins.mnemonic} {ins.op_str}".replace('"', "'")
            self.unsupported.append((ins.address, ins.mnemonic, str(ex)))
            return [f'rt_unimpl(c, {hexu(ins.address)}, "{text}");']

    def call(self, ins):
        ret = hexu(ins.address + ins.size)
        op = ins.operands[0]
        if ins.address == EXCEPT_BLOCK_CALL:
            ie = InsnEmitter(self, ins)
            tgt = ie.rd(op, 4)
            return ie.out + [f"rt_except_resume(c, {tgt}, c->ebx);"]
        if op.type == x86.X86_OP_IMM:
            tgt = op.imm & 0xFFFFFFFF
            if tgt == ins.address + ins.size:  # call $+5: pushes eip
                return [f"c->esp -= 4; wr32(c->esp, {ret});"]
            # Flags come back through Ctx, for asm helpers that return in CF.
            if tgt in self.known or tgt in self.overrides:
                return [f"c->esp -= 4; wr32(c->esp, {ret}); {self.callee(tgt)}(c); FLAGS_IN();"]
            return [f"c->esp -= 4; wr32(c->esp, {ret}); rt_icall(c, {hexu(tgt)}); FLAGS_IN();"]
        ie = InsnEmitter(self, ins)
        tgt = ie.rd(op, 4)
        return ie.out + [f"u32 t = {tgt}; c->esp -= 4; wr32(c->esp, {ret}); rt_icall(c, t); FLAGS_IN();"]

    def emit(self):
        fn = self.fn
        blocks = sorted(fn.blocks.values(), key=lambda b: b.start)
        targets = {fn.start}
        for b in blocks:
            if b.end in ("jcc", "jmp", "table"):
                targets.update(b.succs)
        # Flags arrive through Ctx: a function entered by a jump or by
        # running off the end of another one sees the flags it left.
        L = [f"void {fname(fn.start)}(Ctx* c) {{", "    u32 cf, zf, sf, of, pf, af; FLAGS_IN();"]
        if blocks[0].start != fn.start:
            L.append(f"    goto L_{fn.start:08X};")
        for idx, b in enumerate(blocks):
            nxt = blocks[idx + 1].start if idx + 1 < len(blocks) else None
            # A block that is only reached by fallthrough from the block
            # emitted just before it needs no label.
            if b.start in targets or idx == 0 or blocks[idx - 1].succs != [b.start] or blocks[idx - 1].end != "fall":
                L.append(f"L_{b.start:08X}:;")
            body = b.insns
            term = body[-1] if b.end in ("jcc", "jmp", "ret", "tail", "itail", "table") else None
            for ins in body:
                if ins is term:
                    break
                L.append(f"    // {ins.address:08X} {ins.mnemonic} {ins.op_str}")
                lines = self.call(ins) if ins.mnemonic == "call" else self.insn(ins)
                if lines:
                    L.append("    { " + " ".join(lines) + " }" if len(lines) <= 2 else "    {\n" + "\n".join("        " + x for x in lines) + "\n    }")
            if term is not None:
                L.append(f"    // {term.address:08X} {term.mnemonic} {term.op_str}")
            L.extend("    " + x for x in self.terminator(b, term, nxt))
        L.append("}")
        if self.eh_frame:
            # The body runs under a host frame a thrown exception can resume at.
            name = fname(fn.start)
            L[0] = f"static void body_{name}(Ctx* c) {{"
            L.append(f"void {name}(Ctx* c) {{ rt_eh_frame(c, body_{name}); }}")
        if self.mmio:
            L = [f"#define {n} io_{n}" for n in MMIO_ACCESSORS] + L + [f"#undef {n}" for n in MMIO_ACCESSORS]
        return "\n".join(L)

    def terminator(self, b, term, nxt):
        end = b.end
        if end == "fall":
            return [] if b.succs == [nxt] or not b.succs else [f"goto L_{b.succs[0]:08X};"]
        if end == "jmp":
            tgt = b.succs[0]
            out = [YIELD] if tgt <= term.address else []
            return out if tgt == nxt else out + [f"goto L_{tgt:08X};"]
        if end == "jcc":
            m = term.mnemonic
            taken, fall = b.succs
            if m in JCC:
                cond = COND[m[1:]]
            elif m == "loop":
                cond = "--c->ecx != 0"
            elif m == "loope":
                cond = "--c->ecx != 0 && zf"
            elif m == "loopne":
                cond = "--c->ecx != 0 && !zf"
            elif m == "jecxz":
                cond = "c->ecx == 0"
            elif m == "jcxz":
                cond = "(u16)c->ecx == 0"
            else:
                raise Unsupported(m)
            if taken <= term.address:  # loop back-edge: allow preemption
                out = [f"if ({cond}) {{ {YIELD} goto L_{taken:08X}; }}"]
            else:
                out = [f"if ({cond}) goto L_{taken:08X};"]
            if fall != nxt:
                out.append(f"goto L_{fall:08X};")
            return out
        if end == "ret":
            n = term.operands[0].imm if term.operands else 0
            return [f"FLAGS_OUT(); c->esp += {4 + n}; return;"]
        if end in ("tail", "falltail"):
            if b.target in self.known or b.target in self.overrides:
                return [f"FLAGS_OUT(); {self.callee(b.target)}(c); return;"]
            return [f"FLAGS_OUT(); rt_icall(c, {hexu(b.target)}); return;"]
        if end == "itail":
            ie = InsnEmitter(self, term)
            t = ie.rd(term.operands[0], 4)
            return ["{ " + " ".join(ie.out) + f" u32 t = {t}; FLAGS_OUT(); rt_icall(c, t); return; }}"]
        if end == "table":
            ie = InsnEmitter(self, term)
            t = ie.rd(term.operands[0], 4)
            cases = " ".join(f"case {hexu(s)}: goto L_{s:08X};" for s in b.succs)
            return ["{ " + " ".join(ie.out) + f" u32 t = {t};",
                    f"  switch (t) {{ {cases} default: rt_bad_jump(c, {hexu(term.address)}, t); }} }}"]
        if end == "stop":
            last = b.insns[-1]
            if last.mnemonic == "call":
                return [f"rt_trap(c, {hexu(last.address + last.size)});"]
            return [f"rt_trap(c, {hexu(last.address)});"]
        raise Unsupported(end)

"""Static control-flow analysis of an XBE: function discovery, basic blocks
and jump tables.

Discovery is recursive descent seeded from the entry point, then grown with
every direct call target, every immediate operand that lands on code, and
every aligned pointer in the data sections that lands on code (vtables,
callbacks, static initialiser tables). Data-pointer candidates are kept only
if they decode cleanly, since plain integers can fall in the code range.
"""
import struct
from dataclasses import dataclass, field

import capstone
from capstone import x86

# Sections that carry the X flag on Xbox but hold data, not code.
DATA_SECTIONS = {".rdata", ".data", "DOLBY", "BINKDATA", "$$XTIMAGE", "$$XSIMAGE"}

JCC = {
    "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja",
    "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg",
}
LOOPS = {"loop", "loope", "loopne", "jecxz", "jcxz"}
STOPS = {"int3", "hlt", "ud2"}
MAX_TABLE = 1024
# Instructions compiled game code never contains; a pointer-seeded candidate
# that decodes into one of these is data, not a function.
SUSPICIOUS = {
    "in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "hlt", "iretd", "les", "lds", "into",
    "aam", "aad", "aaa", "aas", "daa", "das", "arpl", "bound", "retf", "ljmp", "lcall", "int1", "salc",
}


class DecodeError(Exception):
    pass


@dataclass
class Block:
    start: int
    insns: list = field(default_factory=list)
    # Successor addresses inside the same function.
    succs: list = field(default_factory=list)
    # How the block ends: "fall", "jmp", "jcc", "ret", "tail", "itail", "table", "stop",
    # or "falltail" (its last instruction flows into another function's entry).
    end: str = "fall"
    # For "tail"/"falltail": target function. For "table": list of targets.
    target: object = None


@dataclass
class Function:
    start: int
    blocks: dict = field(default_factory=dict)
    calls: set = field(default_factory=set)
    imm_refs: set = field(default_factory=set)
    tables: dict = field(default_factory=dict)  # jmp va -> (table va, [targets])
    warnings: list = field(default_factory=list)

    @property
    def end(self):
        last = max(self.blocks.values(), key=lambda b: b.start)
        i = last.insns[-1]
        return i.address + i.size


class Image:
    def __init__(self, xbe):
        self.xbe = xbe
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.code = [
            (s["vaddr"], s["vaddr"] + s["rawsize"], s)
            for s in xbe.sections
            if s["name"] not in DATA_SECTIONS
        ]
        self._cache = {}

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi, _ in self.code)

    def section_of(self, va):
        for lo, hi, s in self.code:
            if lo <= va < hi:
                return lo, hi
        return None

    def read(self, va, n):
        off = self.xbe.va_to_off(va)
        if off is None:
            return None
        return self.xbe.data[off : off + n]

    def u32(self, va):
        b = self.read(va, 4)
        return None if b is None or len(b) < 4 else struct.unpack("<I", b)[0]

    def u8(self, va):
        b = self.read(va, 1)
        return None if not b else b[0]

    def decode(self, va):
        ins = self._cache.get(va)
        if ins is not None:
            return ins
        rng = self.section_of(va)
        if rng is None:
            raise DecodeError(f"{va:08X}: outside code")
        buf = self.read(va, min(15, rng[1] - va))
        ins = next(self.md.disasm(buf, va, 1), None)
        if ins is None:
            raise DecodeError(f"{va:08X}: invalid instruction")
        self._cache[va] = ins
        return ins


def mem_operand(ins):
    for op in ins.operands:
        if op.type == x86.X86_OP_MEM:
            return op.mem
    return None


class Analyzer:
    def __init__(self, image, known_functions=()):
        self.img = image
        self.functions = {}
        self.rejected = {}
        self.starts = set(known_functions)
        self.accepted_insns = {}  # address -> size, instructions of accepted functions

    # ----------------------------------------------------------------- tables
    def _prev_insns(self, preds, va, limit=12):
        """Walk back along unique predecessors, yielding instructions."""
        seen = 0
        while seen < limit:
            p = preds.get(va)
            if not p or len(p) != 1:
                return
            va = next(iter(p))
            yield self.img.decode(va)
            seen += 1

    def _table_bound(self, preds, jmp, index_reg):
        """Find the entry count of a jump table from the guarding cmp/ja."""
        reg = index_reg
        byte_table = None
        cmp_seen = False
        for ins in self._prev_insns(preds, jmp.address):
            m = ins.mnemonic
            ops = ins.operands
            if m == "movzx" and ops[0].type == x86.X86_OP_REG and ops[0].reg == reg and ops[1].type == x86.X86_OP_MEM:
                mm = ops[1].mem
                if ops[1].size == 1 and mm.base != 0 and mm.index == 0:
                    # movzx reg, byte [idx + bytetable]
                    byte_table = (mm.disp & 0xFFFFFFFF, mm.base)
                    reg = mm.base
                    continue
                return None
            if m == "mov" and ops[0].type == x86.X86_OP_REG and ops[0].reg == reg and ops[1].type == x86.X86_OP_REG:
                reg = ops[1].reg
                continue
            if m in ("ja", "jae", "jbe", "jb", "jg", "jle"):
                cmp_seen = True
                continue
            if m == "and" and not byte_table and ops[0].type == x86.X86_OP_REG and ops[0].reg == reg and ops[1].type == x86.X86_OP_IMM:
                n = (ops[1].imm & 0xFFFFFFFF) + 1
                return (n, None) if n <= 256 else None
            if m == "cmp" and cmp_seen and ops[0].type == x86.X86_OP_REG and ops[1].type == x86.X86_OP_IMM:
                if ops[0].reg != reg and not self._same_reg(ops[0].reg, reg):
                    cmp_seen = False  # guards some other value
                    continue
                n = (ops[1].imm & 0xFFFFFFFF) + 1
                if n > 4096:
                    return None
                if byte_table:
                    idx = [self.img.u8(byte_table[0] + i) for i in range(n)]
                    if None in idx:
                        return None
                    return max(idx) + 1, (byte_table[0], n)
                return n, None
            if ins.group(x86.X86_GRP_JUMP) or ins.group(x86.X86_GRP_CALL) or m == "ret":
                return None
        return None

    @staticmethod
    def _same_reg(a, b):
        groups = [
            {x86.X86_REG_EAX, x86.X86_REG_AX, x86.X86_REG_AL},
            {x86.X86_REG_ECX, x86.X86_REG_CX, x86.X86_REG_CL},
            {x86.X86_REG_EDX, x86.X86_REG_DX, x86.X86_REG_DL},
            {x86.X86_REG_EBX, x86.X86_REG_BX, x86.X86_REG_BL},
        ]
        return any(a in g and b in g for g in groups)

    def _resolve_table(self, fn, preds, ins):
        mm = mem_operand(ins)
        if mm is None or mm.index == 0 or mm.scale != 4 or mm.base != 0 or mm.segment != 0:
            return None
        table = mm.disp & 0xFFFFFFFF
        bound = self._table_bound(preds, ins, mm.index)
        rng = self.img.section_of(ins.address)
        targets = []
        def near(t):
            return t is not None and rng[0] <= t < rng[1] and abs(t - ins.address) < 0x4000

        negated = self._index_negated(preds, ins, mm.index)
        if bound:
            # Some slots can be unused (e.g. index 0 after `and eax, 3` on a
            # path where eax is never 0); keep the ones that are code. A
            # negated index walks the table downwards.
            step = -4 if negated else 4
            targets = [t for t in (self.img.u32(table + step * i) for i in range(bound[0])) if near(t)]
        if not targets:
            # Heuristic: entries continue while they point at code close to
            # the jump. Hand-written code (memcpy) also indexes backwards
            # from the table base, so scan that way when the forward scan
            # does not start cleanly or the index was negated.
            fn.warnings.append(f"{ins.address:08X}: unbounded jump table at {table:08X}")
            first_ok = near(self.img.u32(table))
            for i in range(0 if first_ok else 1, MAX_TABLE):
                t = self.img.u32(table + 4 * i)
                if not near(t):
                    break
                targets.append(t)
            if not first_ok or negated:
                for i in range(1, MAX_TABLE):
                    t = self.img.u32(table - 4 * i)
                    if not near(t):
                        break
                    targets.append(t)
            if not targets:
                return None
        return table, targets

    def _index_negated(self, preds, jmp, reg):
        for ins in self._prev_insns(preds, jmp.address, limit=4):
            ops = ins.operands
            if ins.mnemonic == "neg" and ops[0].type == x86.X86_OP_REG and ops[0].reg == reg:
                return True
        return False

    # -------------------------------------------------------------- functions
    def explore(self, start):
        """Recursive-descent decode of one function into basic blocks."""
        fn = Function(start)
        insn_at = {}  # va -> ins, for every decoded instruction
        preds = {}  # va -> set of predecessor instruction vas
        leaders = {start}
        edges = {}  # last insn va -> (end kind, succs, target)
        work = [start]
        visited = set()

        def add_edge(src, dst):
            preds.setdefault(dst, set()).add(src)

        while work:
            va = work.pop()
            if va in visited:
                continue
            while True:
                if va in visited:
                    break
                visited.add(va)
                ins = self.img.decode(va)
                insn_at[va] = ins
                m = ins.mnemonic
                nxt = va + ins.size

                if m in ("mov", "push", "lea", "cmp"):
                    for op in ins.operands:
                        if op.type == x86.X86_OP_IMM and self.img.is_code(op.imm & 0xFFFFFFFF):
                            fn.imm_refs.add(op.imm & 0xFFFFFFFF)

                if m in STOPS:
                    edges[va] = ("stop", [], None)
                    break
                if m == "ret":
                    edges[va] = ("ret", [], None)
                    break
                if m == "call":
                    op = ins.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        tgt = op.imm & 0xFFFFFFFF
                        if tgt != nxt:
                            if not self.img.is_code(tgt):
                                raise DecodeError(f"{va:08X}: call outside code {tgt:08X}")
                            fn.calls.add(tgt)
                    elif op.type == x86.X86_OP_MEM and op.mem.index and op.mem.scale == 4:
                        # call [idx*4 + table]: a handler table, possibly
                        # embedded in a code section where the data pointer
                        # scan does not look.
                        fn.imm_refs.update(self.call_table(op.mem.disp & 0xFFFFFFFF))
                    # Do not run off into padding or the next function after
                    # a call that never returns.
                    try:
                        after = self.img.decode(nxt)
                    except DecodeError:
                        edges[va] = ("stop", [], None)
                        break
                    if after.mnemonic in STOPS:
                        edges[va] = ("stop", [], None)
                        break
                    if nxt in self.starts and nxt != start:
                        # The return lands on another function's entry (an
                        # SEH handler inside this function, or the next
                        # function after a noreturn call): continue there.
                        edges[va] = ("falltail", [], nxt)
                        break
                    add_edge(va, nxt)
                    va = nxt
                    continue
                if m in JCC or m in LOOPS:
                    tgt = ins.operands[0].imm & 0xFFFFFFFF
                    if not self.img.is_code(tgt):
                        raise DecodeError(f"{va:08X}: branch outside code")
                    edges[va] = ("jcc", [tgt, nxt], None)
                    leaders.update((tgt, nxt))
                    add_edge(va, tgt)
                    add_edge(va, nxt)
                    work.append(tgt)
                    work.append(nxt)
                    break
                if m == "jmp":
                    op = ins.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        tgt = op.imm & 0xFFFFFFFF
                        if not self.img.is_code(tgt):
                            raise DecodeError(f"{va:08X}: jmp outside code")
                        if tgt in self.starts and tgt != start:
                            edges[va] = ("tail", [], tgt)
                        else:
                            edges[va] = ("jmp", [tgt], None)
                            leaders.add(tgt)
                            add_edge(va, tgt)
                            work.append(tgt)
                        break
                    res = self._resolve_table(fn, preds, ins)
                    if res:
                        table, targets = res
                        fn.tables[va] = (table, targets)
                        uniq = list(dict.fromkeys(targets))
                        edges[va] = ("table", uniq, targets)
                        for t in uniq:
                            leaders.add(t)
                            add_edge(va, t)
                            work.append(t)
                    else:
                        edges[va] = ("itail", [], None)
                    break
                # Ordinary instruction: fall through.
                if nxt in self.starts and nxt != start:
                    # Runs straight into another function: treat as tail call.
                    edges[va] = ("falltail", [], nxt)
                    break
                add_edge(va, nxt)
                va = nxt

        # Split the linear runs into basic blocks at every leader and at
        # every instruction that is entered other than by plain fallthrough.
        cur = None
        prev = None
        for va in sorted(insn_at):
            ins = insn_at[va]
            falls_in = prev is not None and prev.address + prev.size == va and prev.address in preds.get(va, ())
            if cur is None or va in leaders or not falls_in or len(preds.get(va, ())) != 1:
                if cur is not None and falls_in:
                    cur.succs = [va]
                cur = Block(va)
                fn.blocks[va] = cur
            cur.insns.append(ins)
            prev = ins
            if va in edges:
                kind, succs, tgt = edges[va]
                cur.end = kind
                cur.succs = list(succs)
                cur.target = tgt
                cur = None
        return fn

    def add(self, start):
        fn = self.explore(start)
        self.functions[start] = fn
        for b in fn.blocks.values():
            for i in b.insns:
                self.accepted_insns[i.address] = i.size
        return fn

    def discover(self, entry, extra=(), log=print):
        self.starts.add(entry)
        self.starts.update(extra)
        work = sorted(self.starts)
        imm_candidates = set()
        while True:
            while work:
                va = work.pop()
                if va in self.functions or va in self.rejected:
                    continue
                try:
                    fn = self.add(va)
                except DecodeError as e:
                    self.rejected[va] = str(e)
                    continue
                for t in fn.calls:
                    if t not in self.starts:
                        self.starts.add(t)
                        work.append(t)
                imm_candidates |= fn.imm_refs
            # Immediates and data pointers are lower-confidence seeds.
            new = [c for c in imm_candidates | self.data_pointers() if c not in self.starts and c not in self.rejected]
            new = [c for c in new if self.plausible_start(c)]
            if not new:
                break
            log(f"  +{len(new)} pointer-seeded functions")
            self.starts.update(new)
            work = new

        # Pointer-seeded starts that sit inside another function's body (SEH
        # handlers, labels that a data value happens to hit) still get an
        # entry of their own, but must not split the function around them:
        # control (and flags) would cross a C function boundary there.
        strong = {entry} | set(extra) | {t for f in self.functions.values() for t in f.calls}
        strong &= set(self.functions)
        self.starts = set(strong)
        interior = set()
        for va in strong:
            try:
                fn = self.explore(va)
            except DecodeError:
                continue
            for b in fn.blocks.values():
                interior.update(i.address for i in b.insns if i.address != va)
        # Same for pointer-seeded functions among themselves; a function's
        # entry comes before its interior labels, so go in address order.
        weak = set(self.functions) - strong
        for va in sorted(weak):
            if va in interior:
                continue
            self.starts.add(va)
            try:
                fn = self.explore(va)
            except DecodeError:
                continue
            for b in fn.blocks.values():
                interior.update(i.address for i in b.insns if i.address != va and i.address not in strong)
        log(f"  {len(strong)} called functions, {len(weak & interior)} pointer-seeded entries inside other functions")

        # Final pass: rebuild every function with the final start set so jumps
        # to other functions become tail calls consistently.
        for va in list(self.functions):
            try:
                self.functions[va] = self.explore(va)
            except DecodeError as e:
                self.rejected[va] = str(e)
                del self.functions[va]
        return self.functions

    _data_ptr_cache = None

    def call_table(self, base, limit=256):
        """Function addresses in a call table. Where the table begins in
        terms of the index is unknown, so take every 16-aligned code
        address (compiler function alignment) among the next entries."""
        out = set()
        for i in range(limit):
            v = self.img.u32(base + 4 * i)
            if v is None:
                break
            if v & 0xF == 0 and self.img.is_code(v):
                out.add(v)
        return out

    def data_pointers(self):
        if self._data_ptr_cache is None:
            out = set()
            x = self.img.xbe
            for s in x.sections:
                if s["name"] not in DATA_SECTIONS:
                    continue
                raw = x.data[s["raw"] : s["raw"] + s["rawsize"]]
                for i in range(0, len(raw) - 3, 4):
                    v = struct.unpack_from("<I", raw, i)[0]
                    if self.img.is_code(v):
                        out.add(v)
            self._data_ptr_cache = out
        return self._data_ptr_cache

    def plausible_start(self, va):
        """Reject pointer candidates that land inside a decoded instruction or
        decode into padding/garbage before reaching a return."""
        if self._inside_known_insn(va):
            return False
        try:
            fn = self.explore(va)
        except DecodeError as e:
            self.rejected[va] = str(e)
            return False
        for b in fn.blocks.values():
            for ins in b.insns:
                if ins.mnemonic in STOPS and ins.address == va:
                    self.rejected[va] = "starts with padding"
                    return False
                if ins.mnemonic in SUSPICIOUS or any(
                        op.type == x86.X86_OP_REG and ins.reg_name(op.reg) in ("cs", "ds", "es", "ss", "gs")
                        for op in ins.operands):
                    self.rejected[va] = f"suspicious {ins.mnemonic}"
                    return False
        return True

    _insn_index = None

    def _inside_known_insn(self, va):
        # Only instructions of accepted functions count: decodes of rejected
        # pointer candidates are often garbage that overlaps real code.
        for back in range(1, 15):
            size = self.accepted_insns.get(va - back)
            if size is not None and size > back:
                return True
        return False

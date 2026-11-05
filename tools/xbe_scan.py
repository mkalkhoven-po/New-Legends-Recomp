"""Rough code statistics for the executable sections of an XBE.

Linear sweep only, so numbers are estimates: it is meant to size the
recompilation work (function count, x87/SSE use, indirect control flow),
not to drive it.

Usage:
    python tools/xbe_scan.py <default.xbe>
"""
import collections
import os
import sys

import capstone
from capstone import x86

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from recompiler.xbe import Xbe  # noqa: E402

INTERESTING = {"rdtsc", "cpuid", "in", "out", "int", "int3", "hlt", "cli", "sti",
               "wrmsr", "rdmsr", "invlpg", "wbinvd", "lgdt", "lidt", "iretd"}


def scan(path):
    x = Xbe(open(path, "rb").read())
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True

    totals = collections.Counter()
    call_targets = set()
    special = collections.Counter()

    for s in x.sections:
        if not s["flags"] & 4 or s["name"] in (".rdata", ".data", "BINKDATA", "DOLBY") or s["name"].startswith("$$"):
            continue
        code = x.data[s["raw"] : s["raw"] + s["rawsize"]]
        c = collections.Counter()
        for ins in md.disasm(code, s["vaddr"]):
            if ins.id == 0:  # skipdata
                c["bad"] += 1
                continue
            c["ins"] += 1
            m = ins.mnemonic
            groups = set(ins.groups)
            if m.startswith("f") and m not in ("femms",):
                c["x87"] += 1
            if x86.X86_GRP_SSE1 in groups or x86.X86_GRP_SSE2 in groups or m.endswith("ps") and m.startswith(("mov", "add", "mul", "sub", "shuf", "rcp", "rsqrt", "min", "max", "and", "xor", "cmp")):
                c["sse"] += 1
            if x86.X86_GRP_MMX in groups or m.startswith(("pf", "pi2f", "pmulhrw")):
                c["mmx/3dnow"] += 1
            if m == "call":
                op = ins.operands[0]
                if op.type == x86.X86_OP_IMM:
                    call_targets.add(op.imm)
                    c["call_direct"] += 1
                else:
                    c["call_indirect"] += 1
            elif m == "jmp" and ins.operands[0].type != x86.X86_OP_IMM:
                c["jmp_indirect"] += 1
            elif m in ("ret", "retn"):
                c["ret"] += 1
            if m in INTERESTING:
                special[m] += 1
        totals.update(c)
        print(f"{s['name']:<10} {s['rawsize']:>8} bytes  ins={c['ins']:>7} x87={c['x87']:>6} "
              f"sse={c['sse']:>5} mmx={c['mmx/3dnow']:>4} icall={c['call_indirect']:>5} ijmp={c['jmp_indirect']:>4}")

    print("\nTotals:", dict(totals))
    print("Unique direct call targets (~function count lower bound):", len(call_targets))
    print("Privileged/special instructions:", dict(special))


if __name__ == "__main__":
    scan(sys.argv[1])

"""Decide whether odd .text pointers really are Thumb entry points.

An odd ARM function pointer means "Thumb code at address & ~1". Ghidra absorbed most of
these addresses into ARM functions, which would be silently wrong disassembly. This
scores each candidate by decoding it both ways and checking for a plausible function
prologue, so the question is settled from the bytes rather than from Ghidra's guess.
"""
import struct
import sys

from capstone import (CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs)

TEXT_VA, TEXT_SZ = 0x100000, 0x593000
REGIONS = {'rodata': (0x593000, 0x6C000, 0x693000), 'data': (0x5FF000, 0x29000, 0x6FF000)}
NINSN = 12

code = open(sys.argv[1] if len(sys.argv) > 1 else 'rom-extract/code.bin', 'rb').read()
text_end = TEXT_VA + TEXT_SZ

slots = []
for fo, sz, va in REGIONS.values():
    for off in range(fo, fo + sz - 3, 4):
        w = struct.unpack_from('<I', code, off)[0]
        if TEXT_VA <= w < text_end:
            slots.append((va + (off - fo), w))

runs, cur, prev = set(), [], None
for src, _ in slots:
    if prev is not None and src == prev + 4:
        cur.append(src)
    else:
        if len(cur) >= 3:
            runs |= set(cur)
        cur = [src]
    prev = src
if len(cur) >= 3:
    runs |= set(cur)

odd = {}
for src, tgt in slots:
    if tgt & 1:
        odd.setdefault(tgt, []).append(src)

md_arm = Cs(CS_ARCH_ARM, CS_MODE_ARM)
md_thumb = Cs(CS_ARCH_ARM, CS_MODE_THUMB)


def score(md, va, is_thumb):
    """Count decodable instructions and detect a function prologue."""
    off = va - TEXT_VA
    buf = code[off:off + NINSN * 4]
    insns = list(md.disasm(buf, va, count=NINSN))
    if not insns:
        return 0, False, ''
    # A prologue saving lr is the strongest signal of a real entry point.
    first = insns[0]
    m, op = first.mnemonic, first.op_str
    prologue = ('lr' in op or 'sp' in op) and m.startswith(('push', 'stm', 'sub', 'stmdb'))
    if is_thumb and m.startswith('push'):
        prologue = True
    return len(insns), prologue, f'{m} {op}'


rows = []
for tgt, srcs in sorted(odd.items()):
    t_va, a_va = tgt & ~1, tgt & ~3
    tn, tp, tfirst = score(md_thumb, t_va, True)
    an, ap, afirst = score(md_arm, a_va, False)
    in_run = any(s in runs for s in srcs)
    rows.append((tgt, in_run, tn, tp, tfirst, an, ap, afirst))

thumb_pro = sum(1 for r in rows if r[3])
arm_pro = sum(1 for r in rows if r[6])
both = sum(1 for r in rows if r[3] and r[6])
neither = sum(1 for r in rows if not r[3] and not r[6])
in_run = sum(1 for r in rows if r[1])
full_thumb = sum(1 for r in rows if r[2] == NINSN)
full_arm = sum(1 for r in rows if r[5] == NINSN)

print(f'odd targets                     : {len(rows)}')
print(f'  referenced from a vtable run  : {in_run}')
print(f'  decode all {NINSN} as thumb        : {full_thumb}')
print(f'  decode all {NINSN} as arm          : {full_arm}')
print(f'  thumb prologue                : {thumb_pro}')
print(f'  arm prologue                  : {arm_pro}')
print(f'  both look like prologues      : {both}')
print(f'  neither                       : {neither}')
print()
print('sample (target, in_run, thumb-first-insn | arm-first-insn):')
for tgt, run, tn, tp, tf, an, ap, af in rows[:20]:
    flag = 'T' if tp and not ap else ('A' if ap and not tp else '?')
    print(f'  0x{tgt:08X} run={str(run):<5} [{flag}] thumb={tf:<28} arm={af}')

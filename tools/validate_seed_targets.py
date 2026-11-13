"""Measure the false-positive rate of vtable-derived function seeds.

The seeding pass treated every .rodata/.data word landing in the .text range as a
function pointer. But 16-bit pair data such as (0x0014, 0x0133) reads as 0x00140133,
which is inside .text, so some "pointers" are not pointers at all. Odd targets turned
out to be almost entirely this kind of noise.

This scores each candidate against a random control sample: real ARM function entries
save lr or adjust sp in their first instructions, random addresses do not. The gap
between candidates and control is the signal that the seeds are genuine.
"""
import random
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs

TEXT_VA, TEXT_SZ = 0x100000, 0x593000
REGIONS = {'rodata': (0x593000, 0x6C000, 0x693000), 'data': (0x5FF000, 0x29000, 0x6FF000)}
NINSN = 6

code = open(sys.argv[1] if len(sys.argv) > 1 else 'rom-extract/code.bin', 'rb').read()
text_end = TEXT_VA + TEXT_SZ
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)

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

in_run, isolated = set(), set()
for src, tgt in slots:
    if tgt & 3:
        continue
    (in_run if src in runs else isolated).add(tgt)
isolated -= in_run


def is_entry(va):
    """True if the first instructions look like an ARM function prologue."""
    off = va - TEXT_VA
    if off < 0 or off + NINSN * 4 > len(code):
        return False
    insns = list(md.disasm(code[off:off + NINSN * 4], va, count=NINSN))
    if len(insns) < NINSN:
        return False
    for ins in insns[:3]:
        m, op = ins.mnemonic, ins.op_str
        if m.startswith(('push', 'stmdb', 'stmfd')) and 'lr' in op:
            return True
        if m.startswith('sub') and op.startswith('sp, sp'):
            return True
        if m.startswith('mov') and op in ('ip, sp', 'r12, sp'):
            return True
        if m.startswith('bx') and op == 'lr':
            return True     # trivial stub / getter
    return False


def decodes(va):
    off = va - TEXT_VA
    if off < 0 or off + NINSN * 4 > len(code):
        return False
    return len(list(md.disasm(code[off:off + NINSN * 4], va, count=NINSN))) == NINSN


random.seed(1)
control = {TEXT_VA + 4 * random.randrange(TEXT_SZ // 4) for _ in range(4000)}

for name, s in (('in vtable run', in_run), ('isolated', isolated), ('random control', control)):
    n = len(s)
    ent = sum(1 for a in s if is_entry(a))
    dec = sum(1 for a in s if decodes(a))
    print(f'{name:<16} n={n:<6} prologue={ent:<6} ({100.0*ent/n:5.1f}%)   '
          f'decodes={100.0*dec/n:5.1f}%')

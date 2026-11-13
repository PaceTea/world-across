"""Histogram the instructions the boot-path slice actually uses.

The recompiler only has to translate what the slice contains, so this bounds the codegen
work before any of it is written. Counts base mnemonics separately from condition codes
and the flag-setting S bit, because in ARM those are orthogonal: nearly every instruction
can be predicated, and predication plus NZCV is what a naive translator gets wrong.
"""
import collections
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_CC_AL, ARM_CC_INVALID

TEXT_VA = 0x100000
TSV = sys.argv[1] if len(sys.argv) > 1 else 'ghidra/export/functions.tsv'
BIN = sys.argv[2] if len(sys.argv) > 2 else 'rom-extract/code.bin'
MAX_DEPTH = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9

code = open(BIN, 'rb').read()

md_arm = Cs(CS_ARCH_ARM, CS_MODE_ARM)
md_thumb = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
for m in (md_arm, md_thumb):
    m.detail = True

funcs = []
with open(TSV) as fh:
    next(fh)
    for line in fh:
        a, end, size, thumb, depth, thunk, name = line.rstrip('\n').split('\t')
        depth = int(depth)
        if 0 <= depth <= MAX_DEPTH:
            funcs.append((int(a, 16), int(end, 16), int(size), int(thumb), depth, name))

mnem = collections.Counter()
ccs = collections.Counter()
groups = collections.Counter()
total = pred = sets_flags = writes_pc = 0
failed_funcs = []
undecoded_bytes = 0

BRANCH = {'b', 'bl', 'blx', 'bx', 'bxj'}
LOAD = {'ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'ldrd', 'ldm', 'ldmia', 'ldmib',
        'ldmda', 'ldmdb', 'pop'}
STORE = {'str', 'strb', 'strh', 'strd', 'stm', 'stmia', 'stmib', 'stmda', 'stmdb', 'push'}
VFP_PREFIX = ('v', 'f')

for addr, end, size, thumb, depth, name in funcs:
    off = addr - TEXT_VA
    # size is the real byte count; end overshoots badly for fragmented bodies.
    buf = code[off:off + size]
    md = md_thumb if thumb else md_arm
    n = 0
    consumed = 0
    for ins in md.disasm(buf, addr):
        n += 1
        consumed += ins.size
        total += 1
        base = ins.mnemonic.split('.')[0]
        # Capstone folds the condition into the mnemonic; strip it via insn.cc.
        cc = ins.cc
        if cc not in (ARM_CC_AL, ARM_CC_INVALID):
            pred += 1
            ccs[cc] += 1
            for suf in ('eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc',
                        'hi', 'ls', 'ge', 'lt', 'gt', 'le'):
                if base.endswith(suf) and len(base) > len(suf):
                    base = base[:-len(suf)]
                    break
        if ins.update_flags:
            sets_flags += 1
            if base.endswith('s') and len(base) > 1:
                base = base[:-1]    # fold movs/subs/ands into their base mnemonic
        mnem[base] += 1

        if base in BRANCH:
            groups['branch'] += 1
        elif base in LOAD:
            groups['load'] += 1
        elif base in STORE:
            groups['store'] += 1
        elif base.startswith(VFP_PREFIX) and base not in ('vmov',):
            groups['vfp'] += 1
        elif base.startswith('vmov'):
            groups['vfp'] += 1
        elif base.startswith('svc') or base.startswith('swi'):
            groups['syscall'] += 1
        else:
            groups['alu/other'] += 1

    if consumed < len(buf):
        undecoded_bytes += len(buf) - consumed
        failed_funcs.append((addr, name, len(buf) - consumed))

print(f'functions in slice     : {len(funcs)}')
print(f'instructions           : {total}')
print(f'predicated (cond != al): {pred} ({100.0*pred/max(total,1):.1f}%)')
print(f'set flags (S bit)      : {sets_flags} ({100.0*sets_flags/max(total,1):.1f}%)')
print(f'bytes not decoded      : {undecoded_bytes} in {len(failed_funcs)} functions')
print()
print('by group:')
for g, c in groups.most_common():
    print(f'  {g:<12} {c:>7} ({100.0*c/max(total,1):5.1f}%)')
print()
print(f'distinct base mnemonics: {len(mnem)}')
cum = 0
print('mnemonic       count   cumulative%')
for m, c in mnem.most_common():
    cum += c
    print(f'  {m:<12} {c:>6}   {100.0*cum/max(total,1):5.1f}%')

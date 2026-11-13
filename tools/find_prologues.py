"""Find undiscovered functions by their prologue, in the .text that no function covers.

Ghidra reaches about 86% of .text. The rest is real code whose only callers are indirect,
so nothing in the binary points at it in a way a scan can follow, and the recompiler only
learns it exists when a dispatch finds no function there. Chasing those one at a time is
hopeless; this sweeps the uncovered bytes in one pass.

DO NOT SEED FROM THIS OUTPUT WITHOUT CHECKING IT FIRST. The prologue heuristic scores
78-83% against a 5.6% control on vtable-derived targets, but on *this* population -
uncovered .text - it measured 7.9% against a 4.9% control, which is noise. Most of what it
returns is not a function.

The reason is structural rather than fixable: many real functions here are leaf
constructors that set a few fields and return, like 0x00689E08, which is three stores and
a bx lr. They have no prologue at all, so no test of an entry point's *shape* can separate
them from data. Use tools/discover_loop.sh instead, which learns entry points from
addresses the running guest actually branches to. This script is kept for measuring how
much of .text looks function-shaped, not as a source of seeds.
"""
import argparse
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs

TEXT_VA, TEXT_SZ = 0x100000, 0x593000
NINSN = 8


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--tsv', default='ghidra/export/functions.tsv')
    ap.add_argument('--out', default='ghidra/export/prologue_targets.txt')
    a = ap.parse_args()

    code = open(a.bin, 'rb').read()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True

    covered = bytearray(TEXT_SZ)
    with open(a.tsv) as fh:
        next(fh)
        for line in fh:
            f = line.rstrip('\n').split('\t')
            for part in f[6].split(';'):
                lo, hi = (int(x, 16) for x in part.split('-'))
                lo = max(lo, TEXT_VA)
                hi = min(hi, TEXT_VA + TEXT_SZ - 1)
                if lo <= hi:
                    covered[lo - TEXT_VA:hi - TEXT_VA + 1] = b'\1' * (hi - lo + 1)

    uncovered = TEXT_SZ - sum(covered)
    print(f'.text uncovered: 0x{uncovered:X} bytes '
          f'({100.0 * uncovered / TEXT_SZ:.1f}%)')

    hits = []
    va = TEXT_VA
    while va < TEXT_VA + TEXT_SZ - NINSN * 4:
        off = va - TEXT_VA
        if covered[off]:
            va += 4
            continue

        insns = list(md.disasm(code[off:off + NINSN * 4], va, count=NINSN))
        if len(insns) < NINSN:
            va += 4
            continue

        first = insns[0]
        m, ops = first.mnemonic, first.op_str
        strong = ((m.startswith(('push', 'stmdb', 'stmfd')) and 'lr' in ops)
                  or (m.startswith('sub') and ops.startswith('sp, sp'))
                  or (m.startswith('mov') and ops in ('ip, sp', 'r12, sp')))
        if not strong:
            va += 4
            continue

        hits.append(va)
        # Skip past what we just decoded; the body starts here, not four bytes on.
        va += NINSN * 4

    with open(a.out, 'w') as fh:
        for h in hits:
            fh.write(f'{h:08X}\n')
    print(f'prologue candidates: {len(hits)}')
    print(f'wrote {a.out}')


if __name__ == '__main__':
    main()

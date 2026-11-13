"""Find the first instruction where the recompiled run and the reference run disagree.

Both sides write one fixed record per instruction, captured before that instruction
executes: pc, r0-r14, packed NZCV. The first differing record names the guest instruction
whose translation is wrong, which is the whole point of keeping a reference run at all.

A pc mismatch means control flow already diverged, so the real fault is the last
instruction before it - the context lines are what to read.
"""
import argparse
import struct
import sys

REC = struct.Struct('<17I')
FLAGS = 'NZCV'


def records(path):
    with open(path, 'rb') as fh:
        while True:
            b = fh.read(REC.size * 4096)
            if not b:
                return
            for i in range(0, len(b) - REC.size + 1, REC.size):
                yield REC.unpack_from(b, i)


def fmt(rec):
    pc = rec[0]
    fl = ''.join(f if rec[16] >> (3 - k) & 1 else '-' for k, f in enumerate(FLAGS))
    regs = ' '.join(f'{rec[1 + i]:08x}' for i in range(15))
    return f'{pc:08x} [{fl}] {regs}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mine')
    ap.add_argument('reference')
    ap.add_argument('--context', type=int, default=6)
    a = ap.parse_args()

    ring = []
    n = 0
    for mine, ref in zip(records(a.mine), records(a.reference)):
        if mine != ref:
            print(f'first divergence at instruction {n}\n')
            for r in ring[-a.context:]:
                print(f'  both   {fmt(r)}')
            print()
            print(f'  mine   {fmt(mine)}')
            print(f'  ref    {fmt(ref)}')
            print()
            if mine[0] != ref[0]:
                print(f'  pc differs: mine 0x{mine[0]:08x} vs reference 0x{ref[0]:08x}'
                      ' - control flow already diverged, so the fault is in the'
                      ' instruction above')
            diffs = [f'r{i}: {mine[1+i]:08x} != {ref[1+i]:08x}'
                     for i in range(15) if mine[1 + i] != ref[1 + i]]
            if mine[16] != ref[16]:
                diffs.append(f'flags: {mine[16]:04b} != {ref[16]:04b}')
            for d in diffs:
                print(f'  {d}')
            return 1
        ring.append(mine)
        if len(ring) > a.context + 1:
            ring.pop(0)
        n += 1

    print(f'no divergence in the first {n} instructions compared')
    return 0


if __name__ == '__main__':
    sys.exit(main())

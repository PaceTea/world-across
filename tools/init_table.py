"""Decode the static initialiser table and emit its targets as addresses.

ALBW's CRT walks a table of C++ constructors and calls each one indirectly. The table
holds **PC-relative offsets**, not absolute pointers: each target is the address of its
own slot plus the word stored there. A scan for words that look like .text addresses
therefore misses all of them, which is why Ghidra never found most of these functions and
why the recompiled boot path had no function to dispatch to.

Bounds come from the initialiser itself rather than being hardcoded, so this stays
correct if the surrounding code is re-analysed.
"""
import argparse
import struct

TEXT_VA = 0x100000

# The Thumb initialiser: two pc-relative literals, each added to its own pc.
#   00100926  ldr r4, [pc, #0x18]   /  00100928  add r4, pc
#   0010092a  ldr r5, [pc, #0x18]   /  0010092c  add r5, pc
INIT_FN = 0x00100924
LIT_START, PC_START = 0x00100940, 0x0010092C
LIT_END, PC_END = 0x00100944, 0x00100930


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--tsv', default='ghidra/export/functions.tsv')
    ap.add_argument('--out', default='ghidra/export/init_targets.txt')
    a = ap.parse_args()

    code = open(a.bin, 'rb').read()

    def word(va):
        return struct.unpack_from('<I', code, va - TEXT_VA)[0]

    start = (word(LIT_START) + PC_START) & 0xFFFFFFFF
    end = (word(LIT_END) + PC_END) & 0xFFFFFFFF
    if not (TEXT_VA <= start < end <= 0x00800000):
        raise SystemExit(f'implausible table bounds 0x{start:08X}..0x{end:08X}; '
                         f'the initialiser at 0x{INIT_FN:08X} may have moved')

    targets = []
    for slot in range(start, end, 4):
        targets.append((word(slot) + slot) & 0xFFFFFFFF)

    known = set()
    try:
        with open(a.tsv) as fh:
            next(fh)
            for line in fh:
                known.add(int(line.split('\t')[0], 16))
    except FileNotFoundError:
        pass

    with open(a.out, 'w') as fh:
        for t in targets:
            fh.write(f'{t:08X}\n')

    thumb = sum(1 for t in targets if t & 1)
    unknown = [t for t in targets if (t & ~1) not in known]
    print(f'table          : 0x{start:08X}..0x{end:08X}, {len(targets)} entries')
    print(f'  arm / thumb  : {len(targets) - thumb} / {thumb}')
    print(f'  target range : 0x{min(t & ~1 for t in targets):08X}'
          f'..0x{max(t & ~1 for t in targets):08X}')
    print(f'  not yet known as functions: {len(unknown)}')
    print(f'wrote {a.out}')


if __name__ == '__main__':
    main()

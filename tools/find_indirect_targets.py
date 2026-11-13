"""Find function pointers by how the code uses them, not by how they look.

Some of ALBW's functions are reached only through a literal pool: `ldr rX, [pc, #n]`
followed by `blx rX`. The pointer sits inside .text, so a scan of .rodata/.data never sees
it, and the target is often a leaf function with no stack prologue, so the prologue
heuristic cannot recognise it either - measured at 7.9% against a 4.9% control, which is
noise.

Usage is the sound discriminator. A word that gets loaded into a register which is then
used as a branch target is a function pointer, whatever it looks like in isolation. This
tracks that pattern within a short window and reports only what it actually sees used.

In practice it finds almost nothing yet - two pointers - and the reason is worth keeping:
the code doing the loading is usually itself in the undiscovered 12%, so the pattern is
invisible for the same reason its target is. It becomes useful as coverage rises. Until
then tools/discover_loop.sh is what actually finds these, by asking the running guest.
"""
import argparse
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_MEM, ARM_OP_REG

TEXT_VA, TEXT_SZ = 0x100000, 0x593000
WINDOW = 12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--tsv', default='ghidra/export/functions.tsv')
    ap.add_argument('--out', default='ghidra/export/indirect_targets.txt')
    a = ap.parse_args()

    code = open(a.bin, 'rb').read()
    text_end = TEXT_VA + TEXT_SZ
    md_arm, md_thumb = Cs(CS_ARCH_ARM, CS_MODE_ARM), Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for m in (md_arm, md_thumb):
        m.detail = True

    funcs = []
    known = set()
    with open(a.tsv) as fh:
        next(fh)
        for line in fh:
            f = line.rstrip('\n').split('\t')
            known.add(int(f[0], 16))
            rs = [tuple(int(x, 16) for x in p.split('-')) for p in f[6].split(';')]
            funcs.append((rs, int(f[3])))

    def word(va):
        off = va - TEXT_VA
        if off < 0 or off + 4 > len(code):
            return None
        return struct.unpack_from('<I', code, off)[0]

    found = {}
    for ranges, thumb in funcs:
        md = md_thumb if thumb else md_arm
        for lo, hi in ranges:
            if lo < TEXT_VA or hi >= len(code) + TEXT_VA:
                continue
            insns = list(md.disasm(code[lo - TEXT_VA:hi - TEXT_VA + 1], lo))

            # register -> literal value loaded into it, and where
            pending = {}
            for idx, ins in enumerate(insns):
                m = ins.mnemonic.split('.')[0]
                ops = ins.operands

                if m == 'ldr' and len(ops) >= 2 and ops[1].type == ARM_OP_MEM:
                    mem = ops[1].mem
                    if mem.base and md.reg_name(mem.base) == 'pc' and mem.index == 0:
                        if thumb:
                            lit = ((ins.address + 4) & ~3) + mem.disp
                        else:
                            lit = ins.address + 8 + mem.disp
                        v = word(lit)
                        d = md.reg_name(ops[0].reg)
                        if v is not None and d:
                            pending[d] = (v, idx)
                        continue

                if m in ('blx', 'bx') and ops and ops[0].type == ARM_OP_REG:
                    r = md.reg_name(ops[0].reg)
                    got = pending.get(r)
                    if got and idx - got[1] <= WINDOW:
                        v = got[0]
                        if TEXT_VA <= (v & ~1) < text_end:
                            found.setdefault(v & ~1 | (v & 1), ins.address)
                    continue

                # Any other write to a register invalidates what we recorded for it.
                for op in ops[:1]:
                    if op.type == ARM_OP_REG:
                        pending.pop(md.reg_name(op.reg), None)

    new = {t: site for t, site in found.items() if (t & ~1) not in known}
    with open(a.out, 'w') as fh:
        for t in sorted(new):
            fh.write(f'{t:08X}\n')

    print(f'literal-pool pointers actually branched to : {len(found)}')
    print(f'  not already functions                   : {len(new)}')
    print(f'  thumb among those                       : {sum(1 for t in new if t & 1)}')
    print(f'wrote {a.out}')


if __name__ == '__main__':
    main()

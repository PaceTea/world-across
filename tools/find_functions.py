#!/usr/bin/env python3
"""find_functions.py -- the function database, from the card image alone.

The recompiler needs to know where the game's functions start and which addresses make up each
body. That used to come from a Ghidra export (ghidra/export/functions.tsv), which made Ghidra --
an installation, a Java runtime, an analysis that takes an hour -- part of building the port. This
finds the same things from the code itself, and writes them in the same format:

    python3 tools/find_functions.py            # writes rom-extract/functions.tsv

Where a function starts is known from what refers to it, and every kind of reference this game
uses can be read off the image without running it:

  - the entry point at the start of .text, and the entry points a running game has reported
    (entries/<product code>.txt), which is how the ones below were checked;
  - calls (`bl`, `blx`), and branches to an address that already starts a function (tail calls);
  - words holding an address in .text: virtual tables and callback tables in .rodata and .data,
    and the literal pools the code loads its function pointers from;
  - the C++ static initialisers. RVCT's .init_array holds them as offsets from the entry itself,
    not as addresses, so no word in the image points at them; 1,230 functions, nearly all of them
    run at startup, are reached only that way.

A body is what control flow reaches from the start -- following branches, jump tables and the
fall-through, but not calls -- so loops and shared tails stay inside one function, as they did with
Ghidra's bodies. The flow stops where it reaches another function's start, a literal the code
loads as data, or a word that cannot be an ARM11 instruction (a coprocessor the console does not
have, an ARMv7 encoding, a service call number that does not exist): after a call that does not
return, what follows is often data, and without that the body would run on through it.

The output stays in rom-extract/, beside the code it is read from: like everything derived from
the card, it is not part of the repository.
"""
import argparse
import bisect
import collections
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recompile as R  # noqa: E402

from capstone.arm import (ARM_CC_AL, ARM_CC_INVALID, ARM_OP_IMM, ARM_OP_MEM,  # noqa: E402
                          ARM_OP_REG)

TEXT_VA = R.TEXT_VA

# Words that decode, but not as anything the ARM11 runs. Data does decode, nearly all of it, so
# these are what tells a body that it has walked off the end of the code. (ARMv6K, not ARMv6:
# the exclusive loads and stores of a byte, a halfword and a doubleword are real, and in use.)
NOT_ARM11 = {
    'cdp', 'cdp2', 'ldc', 'ldc2', 'ldcl', 'ldc2l', 'stc', 'stc2', 'stcl', 'stc2l', 'mcr2', 'mrc2',
    'mcrr', 'mcrr2', 'mrrc', 'mrrc2', 'hvc', 'smc', 'udf', 'bkpt', 'eret',
    'movw', 'movt', 'bfi', 'bfc', 'ubfx', 'sbfx', 'rbit', 'mls', 'sdiv', 'udiv', 'dmb', 'dsb',
    'isb', 'pldw', 'pli', 'dbg', 'tbb', 'tbh', 'cbz', 'cbnz', 'it',
    'ldaex', 'stlex', 'lda', 'stl', 'crc32b', 'crc32h', 'crc32w', 'sevl', 'hlt', 'vsel',
    'srs', 'srsda', 'srsdb', 'srsia', 'srsib', 'rfe', 'rfeda', 'rfedb', 'rfeia', 'rfeib',
    'ldrt', 'strt', 'ldrbt', 'strbt', 'ldrht', 'strht', 'ldrsbt', 'ldrsht',
}
VFP_V2 = ('vadd', 'vsub', 'vmul', 'vnmul', 'vdiv', 'vmla', 'vmls', 'vnmla', 'vnmls', 'vneg',
          'vabs', 'vsqrt', 'vmov', 'vcmp', 'vcmpe', 'vcvt', 'vldr', 'vstr', 'vpush', 'vpop',
          'vldm', 'vstm', 'vldmia', 'vstmia', 'vldmdb', 'vstmdb', 'vmrs', 'vmsr')


def layout(exheader):
    """The three segments, from the extended header: (va, size) for text, rodata, data."""
    ex = open(exheader, 'rb').read()
    text = struct.unpack_from('<I', ex, 0x10)[0], struct.unpack_from('<I', ex, 0x18)[0]
    ro = struct.unpack_from('<I', ex, 0x20)[0], struct.unpack_from('<I', ex, 0x28)[0]
    data = struct.unpack_from('<I', ex, 0x30)[0], struct.unpack_from('<I', ex, 0x38)[0]
    return text, ro, data


class Finder:
    def __init__(self, code, text_end):
        self.code = code
        self.text_end = text_end
        self.tr = R.Translator(code, {})
        self.starts = {}            # address -> (thumb, kind)
        self.bodies = {}            # address -> (ranges, edges)
        self.literals = set()       # words the walked code loads as data
        self.tables = set()         # words of jump tables

    def word(self, addr):
        return struct.unpack_from('<I', self.code, addr - TEXT_VA)[0]

    def in_text(self, addr):
        return TEXT_VA <= addr < self.text_end

    def decode(self, pc, thumb):
        md = self.tr.md_thumb if thumb else self.tr.md_arm
        off = pc - TEXT_VA
        return next(md.disasm(self.code[off:off + 4], pc, 1), None)

    def not_code(self, ins, thumb):
        base = R.base_mnemonic(ins)
        if base in NOT_ARM11:
            return True
        # The ARM11's Thumb is the 16-bit one: of the 32-bit encodings it has only the call.
        if thumb and ins.size == 4 and base not in ('bl', 'blx'):
            return True
        if base.startswith('v') and not base.startswith(VFP_V2):
            return True                         # NEON, or VFP beyond version 2
        if '.16' in ins.mnemonic or 'f16' in ins.mnemonic or '.f64.f16' in ins.mnemonic:
            return True
        if base in ('svc', 'swi') and ins.operands and ins.operands[0].imm > 0xFF:
            return True
        if base in ('mcr', 'mrc') and 'p15' not in ins.op_str:
            return True
        # The unconditional space (condition 1111): capstone reports those as AL, so the word
        # itself has to say. The ARM11 has only a handful of them.
        if not thumb and ins.bytes[3] >> 4 == 0xF and base not in ('blx', 'pld', 'cps', 'cpsie',
                                                                    'cpsid', 'setend', 'clrex'):
            return True
        return False

    def writes_pc(self, ins):
        ops = ins.operands
        base = R.base_mnemonic(ins)
        if base in ('pop', 'ldm', 'ldmia', 'ldmib', 'ldmda', 'ldmdb', 'ldmfd'):
            return any(op.type == ARM_OP_REG and ins.reg_name(op.reg) == 'pc' for op in ops)
        if base in ('str', 'strb', 'strh', 'push', 'stm', 'stmia', 'stmdb', 'stmfd', 'stmib',
                    'stmda', 'cmp', 'cmn', 'tst', 'teq', 'b', 'bl', 'blx', 'bx'):
            return False
        return bool(ops) and ops[0].type == ARM_OP_REG and ins.reg_name(ops[0].reg) == 'pc'

    def literal(self, ins, thumb):
        """The words a pc-relative load reads, which are data."""
        base = R.base_mnemonic(ins)
        if base not in ('ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'ldrd', 'vldr'):
            return ()
        for op in ins.operands:
            if op.type == ARM_OP_MEM and op.mem.base and ins.reg_name(op.mem.base) == 'pc' \
                    and not op.mem.index:
                pc = (ins.address + 4) & ~3 if thumb else ins.address + 8
                at = (pc + op.mem.disp) & 0xFFFFFFFF
                n = 8 if base == 'ldrd' or (base == 'vldr' and
                                            ins.reg_name(ins.operands[0].reg)[0] == 'd') else 4
                return tuple(range(at & ~3, at + n, 4))
        return ()

    def walk(self, entry, thumb):
        """The body of the function at entry: (ranges, edges) or None if it does not decode.

        A literal pool is data the body itself names, by loading from it, but the walk can reach
        the pool before the load that names it -- after a call that does not return, say -- and
        run on through it into whatever follows. Where that happened the body is walked a second
        time with the pool known. (Only the body's own loads count: one bogus body loading from
        the middle of real code must not cut every other body short there.)
        """
        body = self._walk(entry, thumb, set())
        if body is not None and body[3]:
            body = self._walk(entry, thumb, body[2])
        if body is None:
            return None
        ranges, edges, data, _, rough = body
        self.literals |= data
        self.rough = rough
        return ranges, edges

    def _walk(self, entry, thumb, known_data):
        seen = {}
        edges = []
        lits = set(known_data)
        tables = set()
        rough = False               # some path ran into a word that is not code
        work = [entry]
        while work:
            pc = work.pop()
            run = []
            while True:
                if pc in seen:
                    break
                if not self.in_text(pc):
                    rough = True
                    break
                # Only a start in the same instruction set ends the flow: the same bytes cannot be
                # both, so a Thumb start inside ARM code (or the reverse) is a word that was not a
                # function pointer after all.
                if pc != entry and self.starts.get(pc, (None,))[0] == thumb:
                    edges.append((pc, thumb, 'fall'))
                    break
                if pc in lits or pc in tables:
                    break
                ins = self.decode(pc, thumb)
                if ins is None or self.not_code(ins, thumb):
                    rough = True
                    break
                seen[pc] = ins.size
                run.append(ins)
                if len(run) > 20:
                    del run[0]
                lits.update(self.literal(ins, thumb))
                base = R.base_mnemonic(ins)
                cond = ins.cc not in (ARM_CC_AL, ARM_CC_INVALID)
                ops = ins.operands
                imm = ops[0].imm & 0xFFFFFFFF if ops and ops[0].type == ARM_OP_IMM else None
                if base == 'b' and imm is not None:
                    if imm != entry and self.starts.get(imm, (None,))[0] == thumb:
                        edges.append((imm, thumb, 'tail'))
                    else:
                        work.append(imm)
                    if not cond:
                        break
                elif base == 'bl' and imm is not None:
                    edges.append((imm, thumb, 'called'))
                    if not thumb and not cond and self.tr.inline_arg_helper(imm):
                        # The word after the call is the helper's argument, and the helper never
                        # comes back here: it continues at one of up to four slots after the
                        # word, the ones the word's actions name (recompile.py, inline_arg_helper).
                        if self.in_text(pc + 4):
                            arg = self.word(pc + 4)
                            lits.add(pc + 4)
                            for k in range(9):
                                act = (arg >> (3 * k)) & 7
                                if act < 4:
                                    edges.append((pc + 8 + 4 * act, 0, 'continuation'))
                        break
                elif base == 'blx' and imm is not None:
                    edges.append((imm & ~1, 0 if thumb else 1, 'called'))
                elif base in ('bx', 'bxj'):
                    if not cond:
                        break
                elif self.writes_pc(ins):
                    v = None if thumb else self.tr.find_veneer(ins)
                    jt = None
                    if v is not None:
                        edges.append((v & ~1, v & 1, 'veneer'))
                        lits.add(ins.address + 8 + ops[1].mem.disp)
                    elif not thumb:
                        self.tr.md, self.tr.thumb = self.tr.md_arm, False
                        jt = self.tr.find_jump_table(run, len(run) - 1, None)
                        if jt is None and len(run) < 32:
                            # The bound check may sit before a branch that this stretch of the
                            # walk started from; read the instructions before it in address order.
                            back = self.tr.disasm_span(max(TEXT_VA, pc - 128), pc, self.tr.md_arm)
                            if back and back[-1].address == pc:
                                jt = self.tr.find_jump_table(back, len(back) - 1, None)
                        if jt is None:
                            # `add pc, pc, rX, lsl #2`: the cases are the branches from pc + 8 on.
                            entries = {}
                            at = pc + 8
                            while self.in_text(at) and len(entries) < 256:
                                e = self.decode(at, False)
                                if e is None or R.base_mnemonic(e) != 'b' or \
                                        e.cc not in (ARM_CC_AL, ARM_CC_INVALID):
                                    break
                                entries[at] = e
                                at += 4
                            if entries and self.tr.find_branch_table(ins, entries):
                                work.extend(entries)
                                work.append(at)     # the case after the last branch
                    if jt is not None:
                        tbase, _reg, targets = jt
                        tables.update(range(tbase, tbase + 4 * len(targets), 4))
                        work.extend(t for t in targets if self.in_text(t) and not t & 3)
                    if not cond:
                        break
                pc += ins.size
        # A word walked before the load that names it as data was found is still data.
        ran_into = False
        for a in lits | tables:
            if seen.pop(a, None) is not None:
                ran_into = True
        if entry not in seen:
            return None
        self.tables |= tables
        ranges = []
        for a in sorted(seen):
            end = a + seen[a] - 1
            if ranges and a == ranges[-1][1] + 1:
                ranges[-1][1] = end
            else:
                ranges.append([a, end])
        return [tuple(r) for r in ranges], edges, lits | tables, ran_into, rough

    def add(self, addr, thumb, kind, clean=False):
        """Take addr as a function start if a body can be walked from it.

        With clean, only if every path through the body ends the way code ends -- a return, a
        branch, another function -- and none runs into a word that is not code. A start read off
        a word is held to that. It matters most for Thumb, which decodes nearly any pair of bytes:
        the few real Thumb pointers in the data are outnumbered by odd numbers that are not
        pointers at all.
        """
        if addr in self.starts or not self.in_text(addr) or addr & (1 if thumb else 3):
            return False
        self.starts[addr] = (thumb, kind)
        body = self.walk(addr, thumb)
        if body is None or (clean and self.rough):
            del self.starts[addr]
            return False
        self.bodies[addr] = body
        return True

    def close(self):
        """Follow every edge until no body names a start that is not known yet."""
        pending = list(self.bodies)
        added = 0
        while pending:
            nxt = []
            for a in pending:
                for t, th, kind in self.bodies[a][1]:
                    if kind == 'fall' and t in self.starts:
                        continue
                    if self.add(t, th, kind):
                        added += 1
                        nxt.append(t)
            pending = nxt
        return added

    def rewalk(self):
        """Walk every body again now that more starts are known, and say how many changed.

        A body walked before a later-found start lies inside it runs through that start; walked
        again, it stops there and falls through to it instead.
        """
        changed = 0
        for a, (thumb, _kind) in list(self.starts.items()):
            body = self.walk(a, thumb)
            if body is None:
                continue
            if body[0] != self.bodies[a][0]:
                changed += 1
            self.bodies[a] = body
        return changed

    def covered(self, arm_only=False):
        ivs = sorted(r for a, (rs, _) in self.bodies.items()
                     if not (arm_only and self.starts[a][0]) for r in rs)
        merged = []
        for lo, hi in ivs:
            if merged and lo <= merged[-1][1] + 1:
                merged[-1][1] = max(merged[-1][1], hi)
            else:
                merged.append([lo, hi])
        return merged


def image_words(code, text_end, ro, data):
    """Candidate starts from the words in the image: (address, thumb, kind, where)."""
    out = []
    n = len(code) // 4
    words = struct.unpack_from(f'<{n}I', code)

    def arm_start(t):
        return not t & 3 and TEXT_VA <= t < text_end and words[(t - TEXT_VA) // 4] >> 28 == 0xE

    for i, w in enumerate(words):
        at = TEXT_VA + 4 * i
        t = w & ~1
        if not TEXT_VA <= t < text_end:
            continue
        where = 'text' if at < text_end else 'data'
        if w & 1:
            out.append((t, 1, 'word', where, at))
        elif arm_start(t):
            out.append((t, 0, 'word', where, at))

    # .init_array: a run of offsets, each from its own address to a function. One such word is
    # nothing -- any small number in a table plus its address lands somewhere in the code -- so
    # only runs of eight or more count, and only outside .text, where the literal pools are.
    lo = (ro[0] - TEXT_VA) // 4
    run = []
    for i in range(lo, n):
        at = TEXT_VA + 4 * i
        t = (at + words[i]) & 0xFFFFFFFF
        if arm_start(t):
            run.append(t)
            continue
        if len(run) >= 8:
            out.extend((x, 0, 'init', 'data', None) for x in run)
        run = []
    if len(run) >= 8:
        out.extend((x, 0, 'init', 'data', None) for x in run)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--exheader', default='rom-extract/exheader.bin')
    ap.add_argument('--known', action='append', default=None,
                    help='entry points the running game reported (default: entries/ for the '
                         "card's product code, which tools/extract_rom.py records)")
    ap.add_argument('--out', default='rom-extract/functions.tsv')
    ap.add_argument('--compare', help='a Ghidra functions.tsv to compare the result with')
    a = ap.parse_args()
    t0 = time.time()

    code = open(a.bin, 'rb').read()
    text, ro, data = layout(a.exheader)
    text_end = text[0] + text[1]
    f = Finder(code, text_end)

    f.add(text[0], 0, 'entry')
    known = a.known
    if known is None:
        # The addresses are one release's: another release's code is laid out differently, and
        # its addresses would be noise. So each release has a file of its own, by product code.
        try:
            product = open(os.path.join(os.path.dirname(a.bin), 'product_code.txt')).read().strip()
            known = [os.path.join('entries', f'{product}.txt')]
        except FileNotFoundError:
            print('no product code beside the code (run tools/extract_rom.py again); '
                  'going without the entry points play has found')
            known = []
    for path in known:
        try:
            lines = open(path).read().splitlines()
        except FileNotFoundError:
            print(f'{path} is not there; skipped')
            continue
        for line in lines:
            if line.strip() and not line.startswith('#'):
                w = int(line, 16)
                f.add(w & ~1, w & 1, 'discovered')
    cands = image_words(code, text_end, ro, data)

    rounds = 0
    while True:
        rounds += 1
        by_edges = f.close()
        spans = {False: f.covered(), True: f.covered(arm_only=True)}
        starts_of = {k: [m[0] for m in v] for k, v in spans.items()}

        def inside(x, arm_only=False):
            merged, los = spans[arm_only], starts_of[arm_only]
            i = bisect.bisect_right(los, x) - 1
            return i >= 0 and merged[i][0] <= x <= merged[i][1]

        by_words = 0
        for t, thumb, kind, where, at in cands:
            if t in f.starts:
                continue
            if where == 'text':
                # In .text a word is a function pointer only if the code loads it: the rest of
                # what lives there is jump tables and data, and constants that look like addresses.
                if at not in f.literals or at in f.tables:
                    continue
                if inside(t, arm_only=not thumb):
                    continue
            if thumb and inside(t):
                continue
            # A word may be any number; an .init_array entry is known to be a function.
            if f.add(t, thumb, kind if kind == 'init' else f'word_{where}', clean=kind != 'init'):
                by_words += 1
        changed = f.rewalk()
        print(f'round {rounds}: {by_edges} by edges, {by_words} by words, {changed} bodies '
              f'changed, {len(f.starts)} functions ({time.time() - t0:.0f} s)')
        if not by_words and not changed:
            more = f.close()
            if not more:
                break

    # A Thumb start read off a word, found inside a body of ARM code walked later, was an odd
    # number and not a pointer.
    arm = f.covered(arm_only=True)
    arm_los = [m[0] for m in arm]
    dropped = 0
    for addr, (thumb, kind) in list(f.starts.items()):
        if thumb and kind.startswith('word'):
            i = bisect.bisect_right(arm_los, addr) - 1
            if i >= 0 and arm[i][0] <= addr <= arm[i][1]:
                del f.starts[addr], f.bodies[addr]
                dropped += 1
    if dropped:
        print(f'{dropped} Thumb starts from words dropped: they lie inside ARM code')

    kinds = collections.Counter(k for _, k in f.starts.values())
    merged = f.covered()
    cov = sum(hi - lo + 1 for lo, hi in merged)
    print(f'{len(f.starts)} functions, {cov} bytes of .text in bodies '
          f'({100 * cov / text[1]:.1f}%); by kind: ' +
          ', '.join(f'{k} {n}' for k, n in kinds.most_common()))

    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    with open(a.out, 'w') as fh:
        fh.write('addr\tend\tsize\tthumb\tboot_depth\tthunk\tranges\tname\n')
        for addr in sorted(f.starts):
            thumb, kind = f.starts[addr]
            ranges, _ = f.bodies[addr]
            size = sum(hi - lo + 1 for lo, hi in ranges)
            end = max(hi for _, hi in ranges)
            rs = ';'.join(f'{lo:08X}-{hi:08X}' for lo, hi in ranges)
            fh.write(f'{addr:08X}\t{end:08X}\t{size}\t{thumb}\t0\t0\t{rs}\t{kind}_{addr:08x}\n')
    print(f'written to {a.out}')

    if a.compare:
        G = R.load_functions(a.compare)
        los = [m[0] for m in merged]

        def inside(x):
            i = bisect.bisect_right(los, x) - 1
            return i >= 0 and merged[i][0] <= x <= merged[i][1]
        gs, fs = set(G), set(f.starts)
        gonly = gs - fs
        bare = [x for x in gonly if not inside(x)]
        gb = cb = 0
        for g in G.values():
            for lo, hi in g.ranges:
                for x in range(lo, hi + 1, 4):
                    gb += 1
                    cb += inside(x)
        print(f'compared with {a.compare}: {len(gs & fs)} starts in both, {len(gonly)} only '
              f'there ({len(bare)} in no body here), {len(fs - gs)} only here; '
              f'{100 * cb / gb:.2f}% of its body words are in a body here')
        with open(a.out + '.only-there', 'w') as fh:
            for x in sorted(bare):
                fh.write(f'{x:08X}\t{G[x].size}\t{G[x].thumb}\n')


if __name__ == '__main__':
    main()

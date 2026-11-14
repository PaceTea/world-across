"""Translate ARM11 functions from code.bin into C, one C function per guest function.

Anything not handled emits a call to albw_unimplemented() instead of approximate code:
a static recompiler that guesses produces a game that hangs with no suspect, so the
first gap has to abort loudly. Run with --report to see which mnemonics still do that.

Intra-function control flow becomes goto over labelled basic blocks, which handles
arbitrary ARM branching without needing structured control flow recovered first.
"""
import argparse
import re
import collections
import os
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import (ARM_CC_AL, ARM_CC_INVALID, ARM_CC_LS, ARM_OP_IMM, ARM_OP_MEM,
                          ARM_OP_REG, ARM_SFT_ASR, ARM_SFT_LSL, ARM_SFT_LSR,
                          ARM_SFT_ROR, ARM_SFT_RRX)

TEXT_VA = 0x100000

CC_MACRO = {
    1: 'ALBW_EQ', 2: 'ALBW_NE', 3: 'ALBW_CS', 4: 'ALBW_CC', 5: 'ALBW_MI',
    6: 'ALBW_PL', 7: 'ALBW_VS', 8: 'ALBW_VC', 9: 'ALBW_HI', 10: 'ALBW_LS',
    11: 'ALBW_GE', 12: 'ALBW_LT', 13: 'ALBW_GT', 14: 'ALBW_LE',
}

REGNAME = {'sp': 13, 'lr': 14, 'pc': 15, 'fp': 11, 'ip': 12, 'sl': 10, 'sb': 9,
           'tr': 9, 'v1': 4, 'v2': 5, 'v3': 6, 'v4': 7, 'v5': 8, 'v6': 9, 'v7': 10,
           'v8': 11, 'a1': 0, 'a2': 1, 'a3': 2, 'a4': 3}


# Data-processing instructions whose immediate operand uses ARM's modified-immediate
# encoding, which capstone may present unfolded.
DP_ROTATABLE = {'mov', 'mvn', 'add', 'sub', 'rsb', 'and', 'orr', 'eor', 'bic',
                'adc', 'sbc', 'rsc', 'cmp', 'cmn', 'tst', 'teq'}

S_ABLE = {'mov', 'mvn', 'add', 'adc', 'sub', 'sbc', 'rsb', 'rsc', 'and', 'orr', 'eor',
          'bic', 'lsl', 'lsr', 'asr', 'ror', 'rrx', 'mul', 'mla', 'umull', 'smull'}


def decode_mnemonic(ins):
    """Split the printed mnemonic into its base and whether it sets the flags.

    The S bit is read off the mnemonic rather than from capstone's update_flags, which
    is not a reliable answer to that question: `adc r0, r2, r2` encodes S as 0 and prints
    without the suffix, yet reports update_flags=True - presumably because ADC *reads*
    the carry. Trusting it makes the translation clobber NZCV that the guest still needs.

    Stripping the condition first matters too: in `subseq` both suffixes are present, and
    taking the S off first would leave `subs` behind.
    """
    base = ins.mnemonic.split('.')[0]
    if ins.cc not in (ARM_CC_AL, ARM_CC_INVALID):
        for suf in ('eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc',
                    'hi', 'ls', 'ge', 'lt', 'gt', 'le'):
            if base.endswith(suf) and len(base) > len(suf):
                base = base[:-len(suf)]
                break
    sets = base.endswith('s') and base[:-1] in S_ABLE
    if sets:
        base = base[:-1]
    return base, sets


def base_mnemonic(ins):
    """Mnemonic with condition and flag suffixes removed.

    Needed wherever the label pass and emit() must agree: `bls`/`blt`/`ble` start with the
    letters of `bl` but are plain conditional branches, so a textual prefix test
    misclassifies them and their targets never become labels.
    """
    return decode_mnemonic(ins)[0]


class Unsupported(Exception):
    pass


def reg_index(md, rid):
    n = md.reg_name(rid)
    if n is None:
        raise Unsupported('unknown register')
    if n in REGNAME:
        return REGNAME[n]
    if n.startswith('r') and n[1:].isdigit():
        return int(n[1:])
    raise Unsupported(f'register {n}')


class Fn:
    def __init__(self, addr, size, name, thumb, depth, ranges):
        self.addr, self.size, self.name = addr, size, name
        self.thumb, self.depth = thumb, depth
        # Every address range of the body. Bodies are often non-contiguous, so a single
        # addr..addr+size read would miss later fragments and pull in the unrelated
        # functions that sit between them.
        self.ranges = ranges

    def contains(self, addr):
        return any(lo <= addr <= hi for lo, hi in self.ranges)

    @property
    def sym(self):
        return f'f_{self.addr:08x}'


def load_functions(tsv):
    out = {}
    with open(tsv) as fh:
        next(fh)
        for line in fh:
            (a, end, size, thumb, depth, thunk,
             ranges, name) = line.rstrip('\n').split('\t')
            addr = int(a, 16)
            rs = []
            for part in ranges.split(';'):
                lo, hi = part.split('-')
                rs.append((int(lo, 16), int(hi, 16)))
            out[addr] = Fn(addr, int(size), name, int(thumb), int(depth), rs)
    return out


class Translator:
    def __init__(self, code, funcs):
        self.code = code
        self.funcs = funcs
        self.md_arm = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        self.md_thumb = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        for m in (self.md_arm, self.md_thumb):
            m.detail = True
        self.md = self.md_arm
        self.thumb = False
        self.gaps = collections.Counter()
        self.slice_addrs = set()
        # Direct-call targets that exist but are not being translated (thumb functions,
        # or callees Ghidra's reference analysis missed). They get stubs so the build
        # stays closed and leaving the slice is visible at runtime.
        self.external = set()

    # ---- operand helpers -------------------------------------------------

    def reg(self, ins, rid, dest=False):
        i = reg_index(self.md, rid)
        if i == 15:
            if dest:
                raise Unsupported('pc as destination')
            # Reading pc is a constant at translate time, but the offset differs by
            # state: instruction address plus 8 in ARM, plus 4 in Thumb.
            off = 4 if self.thumb else 8
            return f'0x{(ins.address + off) & 0xFFFFFFFF:x}u'
        return f'c->r[{i}]'

    def vreg(self, rid):
        """VFP register as a C lvalue; s and d views alias in the union."""
        n = self.md.reg_name(rid)
        if n and n[0] in 'sd' and n[1:].isdigit():
            return f'c->v.{n[0]}[{int(n[1:])}]', n[0]
        raise Unsupported(f'vfp register {n}')

    def shifted(self, ins, op):
        """A register operand with an optional shift, plus its carry-out expression."""
        base = self.reg(ins, op.reg)
        st, sv = op.shift.type, op.shift.value
        if st == 0:
            return base, None
        if st == ARM_SFT_RRX:
            # Rotate right through carry by one: the old carry becomes bit 31 and bit 0
            # becomes the new carry.
            return f'albw_rrx({base}, c->cf, &co)', True
        fn = {ARM_SFT_LSL: 'albw_lsl_c', ARM_SFT_LSR: 'albw_lsr_c',
              ARM_SFT_ASR: 'albw_asr_c', ARM_SFT_ROR: 'albw_ror_c'}.get(st)
        if fn is None:
            # Register-controlled shifts carry the amount in a register.
            fn = {ARM_SFT_LSL + 5: 'albw_lsl_c', ARM_SFT_LSR + 5: 'albw_lsr_c',
                  ARM_SFT_ASR + 5: 'albw_asr_c', ARM_SFT_ROR + 5: 'albw_ror_c'}.get(st)
            if fn is None:
                raise Unsupported(f'shift type {st}')
            amount = f'({self.reg(ins, sv)} & 0xff)'
        else:
            amount = str(sv)
        return f'{fn}({base}, {amount}, c->cf, &co)', True

    def dp_src(self, ins, base, ops, idx):
        """Source operand of a data-processing instruction, with rotation folded in.

        An ARM modified immediate is an 8-bit value rotated right by an even amount, and
        capstone hands that back as two operands rather than one: `add r2, pc, #200, #30`
        means 200 ROR 30, which is 0x320. Reading ops[-1] as the source therefore uses the
        rotation amount as the value and is wrong by whatever the rotation was.
        """
        if base in DP_ROTATABLE and len(ops) >= 2:
            if ops[-1].type == ARM_OP_IMM and ops[-2].type == ARM_OP_IMM:
                rot = ops[-1].imm & 31
                val = ops[-2].imm & 0xFFFFFFFF
                if rot:
                    val = ((val >> rot) | (val << (32 - rot))) & 0xFFFFFFFF
                self.imm_carry = (val >> 31) & 1 if rot else None
                return f'0x{val:x}u'
            if ops[idx].type == ARM_OP_IMM:
                val = ops[idx].imm & 0xFFFFFFFF
                # A rotated immediate also produces a shifter carry-out, which logical
                # operations with the S bit copy into C. capstone folds most of these, so
                # the rotation has to be inferred: an 8-bit field cannot express anything
                # above 0xFF without one.
                self.imm_carry = (val >> 31) & 1 if val > 0xFF else None
                return f'0x{val:x}u'
        return self.src(ins, ops[idx])

    def shifter_carry_stmt(self):
        """C update for a logical operation with the S bit, if the shifter produced one.

        Logical operations leave C alone unless the operand went through the barrel
        shifter. A register shift gives a computed carry; a *rotated* immediate gives
        bit 31 of that immediate, which is easy to miss because the rotation is invisible
        in the folded operand.
        """
        if self.shift_carry:
            return ['c->cf = co;']
        if self.imm_carry is not None:
            return [f'c->cf = {self.imm_carry}u;']
        return []

    def src(self, ins, op):
        """C expression for a source operand; sets self.shift_carry if a shift applied."""
        if op.type == ARM_OP_IMM:
            return f'0x{op.imm & 0xFFFFFFFF:x}u'
        if op.type == ARM_OP_REG:
            expr, carry = self.shifted(ins, op)
            if carry:
                self.shift_carry = True
            return expr
        raise Unsupported('operand type')

    def mem_addr(self, ins, op):
        """Address expression for a memory operand, plus a writeback statement.

        The writeback statement may reference t_ea, so callers must bind
        `uint32_t t_ea = <addr>;` before emitting it.

        Post-indexed addressing is the trap here: capstone puts that offset in a separate
        trailing operand and leaves mem.disp at 0, so a handler that only looks at
        mem.disp sees no offset, drops the base update, and turns a copy loop into an
        infinite one. This raises rather than returning no writeback when the instruction
        says there is one.
        """
        m = op.mem
        if m.base == 0:
            raise Unsupported('mem without base')
        bi = reg_index(self.md, m.base)

        if bi == 15:
            # Literal pool: PC reads as the instruction address plus 8 in ARM state.
            if m.index != 0:
                raise Unsupported('pc-relative indexed')
            if ins.writeback:
                raise Unsupported('pc-relative with writeback')
            # Thumb literal loads align the base down to 4 first, which matters because
            # a Thumb instruction can sit at a 2-mod-4 address.
            if self.thumb:
                addr = (((ins.address + 4) & ~3) + m.disp) & 0xFFFFFFFF
            else:
                addr = (ins.address + 8 + m.disp) & 0xFFFFFFFF
            return f'0x{addr:x}u', None

        base = f'c->r[{bi}]'

        # An operand following the memory operand is a post-indexed offset.
        ops = ins.operands
        mem_i = next((i for i, o in enumerate(ops) if o.type == ARM_OP_MEM), None)
        post = ops[mem_i + 1] if mem_i is not None and mem_i + 1 < len(ops) else None

        if post is not None:
            if not ins.writeback:
                raise Unsupported('trailing operand without writeback')
            if post.type == ARM_OP_IMM:
                # A post-indexed immediate carries its direction in `subtracted`, not in the
                # sign of the value: `str r1, [r0], #-4` arrives as imm 4 with subtracted set.
                # Reading only the sign walked pointers the wrong way, which corrupted a list
                # by storing eight bytes from where the guest meant to.
                # (Memory-operand displacements are the other way round: capstone gives those
                # a negative disp, which is why the offset path below uses the sign.)
                d = abs(post.imm)
                stmt = f'{base} {"-" if post.subtracted or post.imm < 0 else "+"}= {d};'
            elif post.type == ARM_OP_REG:
                stmt = f'{base} {"-" if post.subtracted else "+"}= {self.reg(ins, post.reg)};'
            else:
                raise Unsupported('post-index operand type')
            return base, stmt

        if m.index != 0:
            idx = self.reg(ins, m.index)
            st, sv = op.shift.type, op.shift.value
            # An immediate LSR or ASR by 32 is real ARM -- it is how the encoding spells a shift of
            # zero -- and shifting a 32-bit value by 32 in C is undefined. x86 shifts by the count
            # modulo 32, so `r8 >> 32` came out as r8 itself instead of 0. The compiler said so
            # in a warning, which is how this was found.
            if st == ARM_SFT_LSR and sv >= 32:
                idx = '0u'
            elif st == ARM_SFT_ASR and sv >= 32:
                idx = f'((uint32_t)((int32_t){idx} >> 31))'
            elif st == ARM_SFT_LSL:
                idx = f'({idx} << {sv})'
            elif st == ARM_SFT_LSR:
                idx = f'({idx} >> {sv})'
            elif st == ARM_SFT_ASR:
                idx = f'((uint32_t)((int32_t){idx} >> {sv}))'
            elif st != 0:
                raise Unsupported(f'index shift {st}')
            addr = f'({base} {"-" if op.subtracted else "+"} {idx})'
        elif m.disp:
            addr = f'({base} + {m.disp})' if m.disp > 0 else f'({base} - {-m.disp})'
        else:
            addr = base

        if not ins.writeback:
            return addr, None
        if addr == base:
            raise Unsupported('writeback with no offset to apply')
        return addr, f'{base} = t_ea;'

    # ---- instruction emission -------------------------------------------

    def emit(self, ins, fn, labels, out):
        self.shift_carry = False
        self.imm_carry = None
        cc = ins.cc
        base, sets = decode_mnemonic(ins)

        body = self.translate(ins, base, sets, fn, labels)

        guard = ''
        if cc not in (ARM_CC_AL, ARM_CC_INVALID):
            m = CC_MACRO.get(cc)
            if m is None:
                raise Unsupported(f'cc {cc}')
            guard = f'{m}(c)'

        # Each instruction gets its own block so the t_* temporaries are scoped to it;
        # at function scope they would collide between instructions.
        out.append(f'    /* {ins.address:08x}  {ins.mnemonic} {ins.op_str} */')
        # Outside the predication guard: the reference run traces every instruction it
        # fetches, including ones whose condition turns out false.
        out.append(f'    ALBW_TRACE_PC(c, 0x{ins.address:08x}u);')
        out.append(f'    if ({guard}) {{' if guard else '    {')
        for s in body:
            out.append(f'        {s}')
        out.append('    }')

    def translate(self, ins, base, sets, fn, labels):
        ops = ins.operands
        nxt = ins.address + ins.size
        # A call made from Thumb state sets bit 0 of lr, so the callee's `bx lr` returns
        # to Thumb. The value is observable to the guest, not just a hint to dispatch.
        lr_val = nxt | 1 if self.thumb else nxt

        if base == 'nop':
            return ['(void)0;']

        if base in ('mov', 'mvn'):
            d = self.reg(ins, ops[0].reg, dest=True)
            s = self.dp_src(ins, base, ops, 1)
            e = f'~({s})' if base == 'mvn' else s
            st = [f'{d} = {e};']
            if sets:
                st.append(f'albw_nz(c, {d});')
                st += self.shifter_carry_stmt()
            return st

        bt = getattr(self, 'branch_tables', {}).get(ins.address)
        if bt is not None:
            treg, targets = bt
            st = [f'switch (c->r[{treg}]) {{']
            for k, t in enumerate(targets):
                if t in labels:
                    st.append(f'case {k}u: goto L_{t:08x};')
            st += [f'default: albw_dispatch(c, 0x{ins.address + 8:x}u + c->r[{treg}] * 4u);'
                   ' return;', '}']
            return st

        if base in ('add', 'sub', 'rsb', 'and', 'orr', 'eor', 'bic'):
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg) if len(ops) > 2 else d
            b = self.dp_src(ins, base, ops, -1)
            if base == 'add':
                expr, flags = f'{a} + ({b})', 'albw_add_flags'
            elif base == 'sub':
                expr, flags = f'{a} - ({b})', 'albw_sub_flags'
            elif base == 'rsb':
                expr, flags = f'({b}) - {a}', 'rsb'
            else:
                oper = {'and': '&', 'orr': '|', 'eor': '^'}.get(base)
                expr = f'{a} & ~({b})' if base == 'bic' else f'{a} {oper} ({b})'
                flags = None
            st = [f'uint32_t t_a = {a};', f'uint32_t t_b = {b};']
            if base == 'add':
                st.append(f'{d} = t_a + t_b;')
                if sets:
                    st.append(f'albw_add_flags(c, t_a, t_b, {d});')
            elif base == 'sub':
                st.append(f'{d} = t_a - t_b;')
                if sets:
                    st.append(f'albw_sub_flags(c, t_a, t_b, {d});')
            elif base == 'rsb':
                st.append(f'{d} = t_b - t_a;')
                if sets:
                    st.append(f'albw_sub_flags(c, t_b, t_a, {d});')
            else:
                st.append(f'{d} = {expr.replace(a, "t_a").replace(b, "t_b")};')
                if sets:
                    st.append(f'albw_nz(c, {d});')
                    st += self.shifter_carry_stmt()
            return st

        if base in ('cmp', 'cmn', 'tst', 'teq'):
            a = self.reg(ins, ops[0].reg)
            b = self.dp_src(ins, base, ops, 1)
            if base == 'cmp':
                return [f'uint32_t t_a = {a}, t_b = {b};',
                        'albw_sub_flags(c, t_a, t_b, t_a - t_b);']
            if base == 'cmn':
                return [f'uint32_t t_a = {a}, t_b = {b};',
                        'albw_add_flags(c, t_a, t_b, t_a + t_b);']
            oper = '&' if base == 'tst' else '^'
            st = [f'albw_nz(c, {a} {oper} ({b}));']
            st += self.shifter_carry_stmt()
            return st

        if base in ('lsl', 'lsr', 'asr', 'ror'):
            d = self.reg(ins, ops[0].reg, dest=True)
            # Two encodings reach here. Usually capstone folds the amount into the
            # source operand's shift field and ops[2] does not exist, so reading it
            # silently shifts by zero. The register-amount form instead gives three
            # plain operands with no shift field at all.
            fname = {'lsl': 'albw_lsl_c', 'lsr': 'albw_lsr_c',
                     'asr': 'albw_asr_c', 'ror': 'albw_ror_c'}[base]
            if len(ops) >= 3:
                a = self.reg(ins, ops[1].reg)
                amt = self.src(ins, ops[2])
                st = [f'{d} = {fname}({a}, ({amt}) & 0xff, c->cf, &co);']
            else:
                v = self.src(ins, ops[1])
                if not self.shift_carry:
                    raise Unsupported(f'{base} whose source operand carries no shift')
                st = [f'{d} = {v};']
            if sets:
                st += [f'albw_nz(c, {d});', 'c->cf = co;']
            return st

        if base in ('mul', 'mla'):
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            b = self.reg(ins, ops[2].reg)
            if base == 'mul':
                st = [f'{d} = {a} * {b};']
            else:
                acc = self.reg(ins, ops[3].reg)
                st = [f'{d} = {a} * {b} + {acc};']
            if sets:
                st.append(f'albw_nz(c, {d});')
            return st

        # Loading pc straight off the stack is a return, the same as `pop {r4, pc}`: the
        # continuation already lives on the C call stack, and dispatching to the popped value
        # would re-enter the caller mid-body. The loaded value is therefore discarded, but the
        # stack adjustment is not -- the epilogue is expected to leave sp above the frame.
        if (base == 'ldr' and len(ops) >= 2
                and ops[0].type == ARM_OP_REG and reg_index(self.md, ops[0].reg) == 15
                and ops[1].type == ARM_OP_MEM and ops[1].mem.base
                and reg_index(self.md, ops[1].mem.base) == 13):
            _, wb = self.mem_addr(ins, ops[1])
            return ([wb] if wb else []) + ['return;']

        veneer = getattr(self, 'veneers', {}).get(ins.address)
        if veneer is not None:
            t = veneer & ~1
            if t in self.funcs:
                if t not in self.slice_addrs:
                    self.external.add(t)
                return [f'{self.funcs[t].sym}(c);', 'return;']
            # An address the linker put in a veneer is a function entry, but if no body was
            # built for it the table is still the honest way through.
            return [f'albw_dispatch(c, 0x{veneer:x}u);', 'return;']

        table = getattr(self, 'jump_tables', {}).get(ins.address)
        if table is not None:
            tbase, treg, targets = table
            st = [f'uint32_t t_t = albw_r32(0x{tbase:x}u + c->r[{treg}] * 4);',
                  'switch (t_t) {']
            # Only targets that actually got a label can be reached by goto; anything
            # else leaves the function and has to go through dispatch.
            for t in sorted(set(targets)):
                if t in labels:
                    st.append(f'case 0x{t:x}u: goto L_{t:08x};')
            st += ['default: albw_dispatch(c, t_t); return;', '}']
            return st

        if base in ('ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh'):
            d = self.reg(ins, ops[0].reg, dest=True)
            addr, wb = self.mem_addr(ins, ops[1])
            rd = {'ldr': 'albw_r32(t_ea)',
                  'ldrb': '(uint32_t)albw_r8(t_ea)',
                  'ldrh': '(uint32_t)albw_r16(t_ea)',
                  'ldrsb': '(uint32_t)(int32_t)(int8_t)albw_r8(t_ea)',
                  'ldrsh': '(uint32_t)(int32_t)(int16_t)albw_r16(t_ea)'}[base]
            st = [f'uint32_t t_ea = {addr};', f'{d} = {rd};']
            if wb:
                st.append(wb)
            return st

        if base in ('str', 'strb', 'strh'):
            s = self.reg(ins, ops[0].reg)
            addr, wb = self.mem_addr(ins, ops[1])
            st = [f'uint32_t t_ea = {addr};']
            wr = {'str': f'albw_w32(t_ea, {s});',
                  'strb': f'albw_w8(t_ea, (uint8_t){s});',
                  'strh': f'albw_w16(t_ea, (uint16_t){s});'}[base]
            st.append(wr)
            if wb:
                st.append(wb)
            return st

        if base in ('push', 'pop', 'stm', 'ldm', 'stmia', 'stmib', 'stmda', 'stmdb',
                    'ldmia', 'ldmib', 'ldmda', 'ldmdb', 'stmfd', 'ldmfd'):
            return self.block_transfer(ins, base)

        if base == 'b':
            t = ops[0].imm & 0xFFFFFFFF
            # A backward branch is a loop edge, and the only place a thread that never enters
            # the kernel can be taken off the processor; the guest busy-waits for its own
            # interrupt handler, so without this the run deadlocks with one thread spinning.
            # See albw_preempt.
            #
            # It counts wherever it goes, including out of the function. The reference run
            # recognises a backward branch from the instruction alone and cannot tell those
            # apart, so counting only the ones that stay inside made the two runs give up the
            # processor at different instructions -- which the comparison then reports as a
            # translation bug.
            back = ['ALBW_MAYBE_PREEMPT(c);'] if t <= ins.address else []
            if t in labels:
                return back + [f'goto L_{t:08x};']
            if t in self.funcs:
                if t not in self.slice_addrs:
                    self.external.add(t)
                return back + [f'{self.funcs[t].sym}(c);', 'return;']
            raise Unsupported('branch outside function and not a known function')

        if base == 'bl':
            t = ops[0].imm & 0xFFFFFFFF
            if t not in self.funcs:
                raise Unsupported('bl to unknown function')
            if t not in self.slice_addrs:
                self.external.add(t)
            call = [f'c->r[14] = 0x{lr_val:x}u;', f'{self.funcs[t].sym}(c);']
            # A helper that takes the word after the call as its argument finishes the caller's
            # job itself (see inline_arg_helper), so the caller is done once it returns.
            return call + ['return;'] if self.inline_arg_helper(t) else call

        if base == 'bx':
            i = reg_index(self.md, ops[0].reg)
            if i == 14:
                return ['return;']
            return [f'albw_dispatch(c, c->r[{i}]);', 'return;']

        if base == 'blx':
            if ops[0].type == ARM_OP_REG:
                i = reg_index(self.md, ops[0].reg)
                return [f'c->r[14] = 0x{lr_val:x}u;', f'albw_dispatch(c, c->r[{i}]);']
            # blx with an immediate always switches instruction set. Each guest function
            # is translated in its own state, so the switch needs nothing at runtime.
            t = ops[0].imm & 0xFFFFFFFE
            if t not in self.funcs:
                raise Unsupported('blx to unknown function')
            if t not in self.slice_addrs:
                self.external.add(t)
            return [f'c->r[14] = 0x{lr_val:x}u;', f'{self.funcs[t].sym}(c);']

        if base == 'svc':
            return [f'albw_svc(c, 0x{ops[0].imm:x}u);']

        if base == 'mcr':
            # The only coprocessor writes in real code are the barriers
            # `mcr p15,0,Rn,c7,c10,{4,5}`: drain write buffer and data memory barrier.
            # One core and one flat mapping make both no-ops. Anything else stays a gap
            # rather than being quietly ignored.
            if 'p15' in ins.op_str and 'c7' in ins.op_str and 'c10' in ins.op_str:
                return ['(void)0;']
            raise Unsupported(f'mcr other than a cache barrier: {ins.op_str}')

        if base == 'mrc':
            # Only TPIDRURO appears in this binary: p15, 0, Rd, c13, c0, 3.
            d = self.reg(ins, ops[2].reg, dest=True)
            if 'c13' in ins.op_str and 'c0' in ins.op_str:
                return [f'{d} = c->tls;']
            raise Unsupported('mrc other than TPIDRURO')

        if base in ('adc', 'sbc', 'rsc'):
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg) if len(ops) > 2 else d
            b = self.dp_src(ins, base, ops, -1)
            st = [f'uint32_t t_a = {a}, t_b = {b};']
            if base == 'adc':
                st += ['uint64_t t_w = (uint64_t)t_a + t_b + c->cf;',
                       f'{d} = (uint32_t)t_w;']
                if sets:
                    st += [f'albw_nz(c, {d});', 'c->cf = (uint32_t)(t_w >> 32);',
                           f'c->vf = ((~(t_a ^ t_b) & (t_a ^ {d})) >> 31);']
            else:
                if base == 'rsc':
                    st.append('uint32_t t_x = t_b, t_y = t_a;')
                else:
                    st.append('uint32_t t_x = t_a, t_y = t_b;')
                st += ['uint64_t t_w = (uint64_t)t_x - t_y - (1u - c->cf);',
                       f'{d} = (uint32_t)t_w;']
                if sets:
                    st += [f'albw_nz(c, {d});',
                           'c->cf = (t_w >> 32) == 0;',
                           f'c->vf = (((t_x ^ t_y) & (t_x ^ {d})) >> 31);']
            return st

        if base in ('umull', 'smull', 'umlal', 'smlal'):
            lo = self.reg(ins, ops[0].reg, dest=True)
            hi = self.reg(ins, ops[1].reg, dest=True)
            a = self.reg(ins, ops[2].reg)
            b = self.reg(ins, ops[3].reg)
            if base[0] == 'u':
                prod = f'((uint64_t){a} * (uint64_t){b})'
            else:
                prod = f'(uint64_t)((int64_t)(int32_t){a} * (int64_t)(int32_t){b})'
            st = [f'uint64_t t_w = {prod};']
            if base.endswith('lal'):
                st.append(f't_w += ((uint64_t){hi} << 32) | {lo};')
            st += [f'{lo} = (uint32_t)t_w;', f'{hi} = (uint32_t)(t_w >> 32);']
            if sets:
                st += [f'c->nf = {hi} >> 31;', f'c->zf = (t_w == 0);']
            return st

        if base in ('ldrd', 'strd'):
            r0 = reg_index(self.md, ops[0].reg)
            r1 = reg_index(self.md, ops[1].reg)
            addr, wb = self.mem_addr(ins, ops[2])
            st = [f'uint32_t t_ea = {addr};']
            if base == 'ldrd':
                st += [f'c->r[{r0}] = albw_r32(t_ea);',
                       f'c->r[{r1}] = albw_r32(t_ea + 4);']
            else:
                st += [f'albw_w32(t_ea, c->r[{r0}]);',
                       f'albw_w32(t_ea + 4, c->r[{r1}]);']
            if wb:
                st.append(wb)
            return st

        if base.startswith('it'):
            # IT only sets the condition for the instructions that follow, and capstone
            # already reports that condition on each of them.
            return ['(void)0;']

        if base in ('cbz', 'cbnz'):
            r = self.reg(ins, ops[0].reg)
            t = ops[1].imm & 0xFFFFFFFF
            if t not in labels:
                raise Unsupported('cbz target outside function')
            op = '==' if base == 'cbz' else '!='
            return [f'if ({r} {op} 0) goto L_{t:08x};']

        if base == 'movw':
            d = self.reg(ins, ops[0].reg, dest=True)
            return [f'{d} = {self.src(ins, ops[1])};']

        if base == 'movt':
            d = self.reg(ins, ops[0].reg, dest=True)
            return [f'{d} = ({d} & 0xFFFFu) | (({self.src(ins, ops[1])}) << 16);']

        if base == 'neg':
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            st = [f'uint32_t t_a = {a};', f'{d} = 0u - t_a;']
            if sets:
                st.append(f'albw_sub_flags(c, 0u, t_a, {d});')
            return st

        if base in ('rev', 'rev16', 'revsh'):
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            if base == 'rev':
                return [f'{d} = __builtin_bswap32({a});']
            if base == 'rev16':
                return [f'uint32_t t_a = {a};',
                        f'{d} = ((t_a & 0x00FF00FFu) << 8) | ((t_a >> 8) & 0x00FF00FFu);']
            return [f'{d} = (uint32_t)(int32_t)(int16_t)__builtin_bswap16('
                    f'(uint16_t){a});']

        if base == 'uqsub8':
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            b = self.reg(ins, ops[2].reg)
            return [f'{d} = albw_uqsub8({a}, {b});']

        if base in ('pld', 'pldw', 'clrex', 'dmb', 'dsb', 'isb', 'yield'):
            return ['(void)0;']   # hints and barriers: nothing to model on one core

        if base == 'clz':
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            # __builtin_clz is undefined at zero, where ARM defines the answer as 32.
            return [f'uint32_t t_a = {a};',
                    f'{d} = t_a ? (uint32_t)__builtin_clz(t_a) : 32u;']

        if base in ('sxtah', 'sxtab', 'uxtah', 'uxtab'):
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            b = self.src(ins, ops[2])
            cast = {'sxtah': '(uint32_t)(int32_t)(int16_t)', 'sxtab': '(uint32_t)(int32_t)(int8_t)',
                    'uxtah': '(uint32_t)(uint16_t)', 'uxtab': '(uint32_t)(uint8_t)'}[base]
            return [f'{d} = {a} + {cast}({b});']

        if base[:4] in ('smul', 'smla') and len(base) == 6 and base[4] in 'btw':
            # The halfword multiplies. SMUL<x><y> multiplies the signed halves the suffixes
            # name, SMLA<x><y> adds an accumulator, and the W forms multiply a whole word by a
            # halfword and keep the top 32 bits of the 48-bit product. Q, which SMLA sets on
            # overflow, lives in APSR and nothing in this binary reads it.
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            b = self.reg(ins, ops[2].reg)
            wide = base[4] == 'w'
            half = {'b': 'albw_sbot', 't': 'albw_stop'}[base[5]]
            if wide:
                product = f'(int32_t)(((int64_t)(int32_t){a} * {half}({b})) >> 16)'
            else:
                first = {'b': 'albw_sbot', 't': 'albw_stop'}[base[4]]
                product = f'({first}({a}) * {half}({b}))'
            if base[:4] == 'smla':
                acc = self.reg(ins, ops[3].reg)
                return [f'{d} = (uint32_t)({acc} + (uint32_t){product});']
            return [f'{d} = (uint32_t){product};']

        if base in ('uadd16', 'sadd16', 'usub16', 'ssub16',
                    'uadd8', 'sadd8', 'usub8', 'ssub8'):
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            b = self.src(ins, ops[2])
            fn = 'albw_' + ('add' if 'add' in base else 'sub') + ('8' if base.endswith('8') else '16')
            return [f'{d} = {fn}({a}, {b});']

        if base == 'mls':
            # Multiply and subtract from the accumulator, the other way round from mla.
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            b = self.reg(ins, ops[2].reg)
            acc = self.reg(ins, ops[3].reg)
            return [f'{d} = {acc} - {a} * {b};']

        if base in ('usat', 'ssat', 'usat16', 'ssat16'):
            d = self.reg(ins, ops[0].reg, dest=True)
            bits = ops[1].imm
            v = self.src(ins, ops[2])
            fn = 'albw_' + base
            cast = '' if base.endswith('16') else '(int32_t)'
            return [f'{d} = {fn}({cast}({v}), {bits});']

        if base == 'adr':
            # A pc-relative address, so a constant at translate time. The operand is the offset
            # rather than the target: in Thumb the base is the pc *aligned down to four*, which
            # differs from the instruction address whenever it is not itself aligned, and in ARM
            # it is the instruction address plus eight. Capstone gives a negative offset for the
            # subtracting form, so this adds signed.
            d = self.reg(ins, ops[0].reg, dest=True)
            off = ops[1].imm
            if self.thumb:
                pc = ((ins.address + 4) & ~3)
            else:
                pc = ins.address + 8
            return [f'{d} = 0x{(pc + off) & 0xFFFFFFFF:08x}u;']

        if base in ('pkhbt', 'pkhtb'):
            # One halfword from each operand. The shift belongs to the second operand and is
            # part of the instruction rather than a separate shifter: PKHBT shifts left, PKHTB
            # arithmetically right, and either may be absent.
            d = self.reg(ins, ops[0].reg, dest=True)
            a = self.reg(ins, ops[1].reg)
            b = self.reg(ins, ops[2].reg)
            amount = ops[2].shift.value if ops[2].shift.type != 0 else 0
            if base == 'pkhbt':
                hi = f'({b} << {amount})' if amount else b
                return [f'{d} = ({a} & 0xFFFFu) | ({hi} & 0xFFFF0000u);']
            lo = f'((uint32_t)((int32_t){b} >> {amount}))' if amount else b
            return [f'{d} = ({a} & 0xFFFF0000u) | ({lo} & 0xFFFFu);']

        if base in ('sxtb', 'sxth', 'uxtb', 'uxth'):
            d = self.reg(ins, ops[0].reg, dest=True)
            s = self.src(ins, ops[1])
            cast = {'sxtb': '(uint32_t)(int32_t)(int8_t)', 'sxth': '(uint32_t)(int32_t)(int16_t)',
                    'uxtb': '(uint32_t)(uint8_t)', 'uxth': '(uint32_t)(uint16_t)'}[base]
            return [f'{d} = {cast}({s});']

        if base == 'rrx':
            d = self.reg(ins, ops[0].reg, dest=True)
            s = self.reg(ins, ops[1].reg)
            st = [f'uint32_t t_a = {s};', f'{d} = (t_a >> 1) | (c->cf << 31);']
            if sets:
                st += ['c->cf = t_a & 1u;', f'albw_nz(c, {d});']
            return st

        if base == 'ldrexd':
            r0 = reg_index(self.md, ops[0].reg)
            r1 = reg_index(self.md, ops[1].reg)
            addr, wb = self.mem_addr(ins, ops[-1])
            if wb:
                raise Unsupported('ldrexd with writeback')
            return [f'uint32_t t_ea = {addr};',
                    f'c->r[{r0}] = albw_r32(t_ea);',
                    f'c->r[{r1}] = albw_r32(t_ea + 4);']

        if base == 'strexd':
            d = self.reg(ins, ops[0].reg, dest=True)
            r0 = reg_index(self.md, ops[1].reg)
            r1 = reg_index(self.md, ops[2].reg)
            addr, wb = self.mem_addr(ins, ops[-1])
            if wb:
                raise Unsupported('strexd with writeback')
            return [f'uint32_t t_ea = {addr};',
                    f'albw_w32(t_ea, c->r[{r0}]);',
                    f'albw_w32(t_ea + 4, c->r[{r1}]);',
                    f'{d} = 0u;']

        if base.startswith('ldrex'):
            # Single core and no preemption inside a translated function, so the
            # exclusive monitor has nothing to observe: a plain load is equivalent.
            d = self.reg(ins, ops[0].reg, dest=True)
            addr, wb = self.mem_addr(ins, ops[-1])
            if wb:
                raise Unsupported('ldrex with writeback')
            rd = {'ldrex': 'albw_r32(t_ea)', 'ldrexb': '(uint32_t)albw_r8(t_ea)',
                  'ldrexh': '(uint32_t)albw_r16(t_ea)'}.get(base)
            if rd is None:
                raise Unsupported(base)
            return [f'uint32_t t_ea = {addr};', f'{d} = {rd};']

        if base.startswith('strex'):
            # Status register always reports success, for the same reason.
            d = self.reg(ins, ops[0].reg, dest=True)
            s = self.reg(ins, ops[1].reg)
            addr, wb = self.mem_addr(ins, ops[-1])
            if wb:
                raise Unsupported('strex with writeback')
            wr = {'strex': f'albw_w32(t_ea, {s});',
                  'strexb': f'albw_w8(t_ea, (uint8_t){s});',
                  'strexh': f'albw_w16(t_ea, (uint16_t){s});'}.get(base)
            if wr is None:
                raise Unsupported(base)
            return [f'uint32_t t_ea = {addr};', wr, f'{d} = 0u;']

        # Unprivileged accesses behave identically without an MMU.
        if base in ('ldrt', 'ldrbt', 'ldrht', 'ldrsbt', 'ldrsht', 'strt', 'strbt', 'strht'):
            return self.translate(ins, base[:-1], sets, fn, labels)

        if base.startswith('v') or base in ('fmrx', 'fmxr'):
            return self.vfp(ins, base, ops)

        raise Unsupported(base)

    # ---- VFP -------------------------------------------------------------

    def vfp(self, ins, base, ops):
        # The ARM11's VFPv2 has no half-precision forms. Capstone decodes them anyway, and where it
        # does, the bytes are data the function database took for code (floats and a string, at
        # 0x001887ec); translated, the store would write somewhere quietly.
        if '.16' in ins.mnemonic or '.f16' in ins.mnemonic:
            raise Unsupported(f'half precision {ins.mnemonic}')
        if base in ('vldr', 'vstr'):
            dst, w = self.vreg(ops[0].reg)
            addr, wb = self.mem_addr(ins, ops[1])
            ty = 'float' if w == 's' else 'double'
            st = [f'uint32_t t_ea = {addr};']
            if base == 'vldr':
                st.append(f'{{ {ty} t_f; memcpy(&t_f, albw_mem + t_ea, sizeof t_f);'
                          f' {dst} = t_f; }}')
            else:
                # Through the store helper, so the write record and the not-a-number watch see it.
                st.append(f'albw_wf{"32" if w == "s" else "64"}(t_ea, {dst});')
            if wb:
                st.append(wb)
            return st

        if base in ('vadd', 'vsub', 'vmul', 'vdiv'):
            d, w = self.vreg(ops[0].reg)
            a, _ = self.vreg(ops[1].reg)
            b, _ = self.vreg(ops[2].reg)
            oper = {'vadd': '+', 'vsub': '-', 'vmul': '*', 'vdiv': '/'}[base]
            return [f'{d} = {a} {oper} {b};']

        if base == 'vnmul':
            d, w = self.vreg(ops[0].reg)
            a, _ = self.vreg(ops[1].reg)
            b, _ = self.vreg(ops[2].reg)
            return [f'{d} = -({a} * {b});']

        if base in ('vmla', 'vmls', 'vnmla', 'vnmls'):
            # Written out per operation rather than derived from the name. The N does not simply
            # negate the result of the unprefixed form: VNMLA negates the accumulate, VNMLS
            # negates only the accumulator, so deriving one from the other swaps the two -- which
            # is what this did before. And the negations come before the add, as the instruction
            # does them: -(x + y) is -0 where x + y cancels, -x - y is +0 (tools/insn_check.py).
            d, w = self.vreg(ops[0].reg)
            a, _ = self.vreg(ops[1].reg)
            b, _ = self.vreg(ops[2].reg)
            expr = {'vmla':  f'{d} + {a} * {b}',
                    'vmls':  f'{d} - {a} * {b}',
                    'vnmla': f'-{d} - {a} * {b}',
                    'vnmls': f'-{d} + {a} * {b}'}[base]
            return [f'{d} = {expr};']

        if base in ('vneg', 'vabs', 'vsqrt', 'vmov'):
            d, w = self.vreg(ops[0].reg) if self._is_v(ops[0]) else (None, None)
            # vmov also moves between core and VFP registers in both directions.
            if base == 'vmov':
                return self.vmov(ins, ops)
            a, _ = self.vreg(ops[1].reg)
            fn = {'vneg': f'-{a}', 'vabs': f'__builtin_fabs{"f" if w == "s" else ""}({a})',
                  'vsqrt': f'__builtin_sqrt{"f" if w == "s" else ""}({a})'}[base]
            return [f'{d} = {fn};']

        if base in ('vcmp', 'vcmpe'):
            a, _ = self.vreg(ops[0].reg)
            b = self.vreg(ops[1].reg)[0] if self._is_v(ops[1]) else '0.0'
            # A VFP compare writes FPSCR, not CPSR; only a later vmrs moves those bits
            # into NZCV. Short-circuiting that would also make this diverge from the
            # reference run for the instructions in between.
            return [f'double t_x = {a}, t_y = {b};',
                    'uint32_t t_nzcv;',
                    'if (t_x != t_x || t_y != t_y) t_nzcv = 0x3u;   /* unordered: C,V */',
                    'else if (t_x == t_y)          t_nzcv = 0x6u;   /* Z,C */',
                    'else if (t_x < t_y)           t_nzcv = 0x8u;   /* N */',
                    'else                          t_nzcv = 0x2u;   /* C */',
                    'c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (t_nzcv << 28);']

        if base in ('vmrs', 'fmrx'):
            if 'apsr' in ins.op_str.lower():
                return ['c->nf = (c->fpscr >> 31) & 1u;',
                        'c->zf = (c->fpscr >> 30) & 1u;',
                        'c->cf = (c->fpscr >> 29) & 1u;',
                        'c->vf = (c->fpscr >> 28) & 1u;']
            d = self.reg(ins, ops[0].reg, dest=True)
            return [f'{d} = c->fpscr;']

        if base in ('vmsr', 'fmxr'):
            # Sets the host's floating-point mode too: flush to zero, rounding (albw_cpu.h).
            return [f'albw_set_fpscr(c, {self.reg(ins, ops[-1].reg)});']

        if base == 'vcvt':
            return self.vcvt(ins, ops)

        if base in ('vpush', 'vpop', 'vstmia', 'vldmia', 'vstm', 'vldm'):
            return self.vfp_block(ins, base, ops)

        raise Unsupported(base)

    def _is_v(self, op):
        if op.type != ARM_OP_REG:
            return False
        n = self.md.reg_name(op.reg)
        return bool(n) and n[0] in 'sd' and n[1:].isdigit()

    def vmov(self, ins, ops):
        a_is_v, b_is_v = self._is_v(ops[0]), self._is_v(ops[1])
        if a_is_v and b_is_v:
            d, w = self.vreg(ops[0].reg)
            s, _ = self.vreg(ops[1].reg)
            return [f'{d} = {s};']
        if a_is_v and not b_is_v:
            d, w = self.vreg(ops[0].reg)
            n = reg_index(self.md, ops[1].reg)
            if w != 's':
                raise Unsupported('vmov core->d')
            return [f'memcpy(&c->v.u32[{self._vidx(ops[0].reg)}], &c->r[{n}], 4);']
        if not a_is_v and b_is_v:
            n = reg_index(self.md, ops[0].reg)
            s, w = self.vreg(ops[1].reg)
            if w != 's':
                raise Unsupported('vmov d->core')
            return [f'memcpy(&c->r[{n}], &c->v.u32[{self._vidx(ops[1].reg)}], 4);']
        raise Unsupported('vmov form')

    def _vidx(self, rid):
        return int(self.md.reg_name(rid)[1:])

    def vcvt(self, ins, ops):
        m = ins.mnemonic
        d, dw = self.vreg(ops[0].reg)
        s, sw = self.vreg(ops[1].reg)
        # Capstone spells the types in the mnemonic, e.g. vcvt.s32.f32.
        parts = m.split('.')
        if len(parts) < 3:
            raise Unsupported(f'vcvt form {m}')
        to, frm = parts[1], parts[2]
        cast = {'s32': '(int32_t)', 'u32': '(uint32_t)', 'f32': '(float)',
                'f64': '(double)'}
        if to not in cast or frm not in cast:
            raise Unsupported(f'vcvt {to}<-{frm}')
        if to in ('s32', 'u32'):
            # Integer results live in the VFP register file as raw bits. Through the helpers, not
            # a cast: out of range the instruction saturates, and a C cast is undefined there.
            return [f'c->v.u32[{self._vidx(ops[0].reg)}] = albw_vcvt_{to}({s});']
        if frm in ('s32', 'u32'):
            ity = 'int32_t' if frm == 's32' else 'uint32_t'
            return [f'{{ {ity} t_i; memcpy(&t_i, &c->v.u32[{self._vidx(ops[1].reg)}], 4);'
                    f' {d} = ({"float" if to == "f32" else "double"})t_i; }}']
        return [f'{d} = {cast[to]}{s};']

    def vfp_block(self, ins, base, ops):
        vregs = [op.reg for op in ops if self._is_v(op)]
        if not vregs:
            raise Unsupported(f'{base} without vfp regs')
        w = self.md.reg_name(vregs[0])[0]
        step = 4 if w == 's' else 8
        if base in ('vpush', 'vpop'):
            sp = 'c->r[13]'
            st = []
            if base == 'vpush':
                st.append(f'{sp} -= {step * len(vregs)};')
                for k, r in enumerate(vregs):
                    e, _ = self.vreg(r)
                    st.append(f'albw_wf{"32" if w == "s" else "64"}'
                              f'({sp} + {step * k}, {e});')
            else:
                for k, r in enumerate(vregs):
                    e, _ = self.vreg(r)
                    st.append(f'{{ {"float" if w == "s" else "double"} t_f;'
                              f' memcpy(&t_f, albw_mem + {sp} + {step * k},'
                              f' sizeof t_f); {e} = t_f; }}')
                st.append(f'{sp} += {step * len(vregs)};')
            return st

        bi = None
        for op in ops:
            if op.type == ARM_OP_REG and not self._is_v(op):
                bi = reg_index(self.md, op.reg)
                break
        if bi is None:
            raise Unsupported(f'{base} without base register')
        st = [f'uint32_t t_ea = c->r[{bi}];']
        for k, r in enumerate(vregs):
            e, _ = self.vreg(r)
            ty = 'float' if w == 's' else 'double'
            if base.startswith('vst'):
                st.append(f'albw_wf{"32" if w == "s" else "64"}'
                          f'(t_ea + {step * k}, {e});')
            else:
                st.append(f'{{ {ty} t_f; memcpy(&t_f, albw_mem + t_ea + {step * k},'
                          f' sizeof t_f); {e} = t_f; }}')
        if ins.writeback:
            st.append(f'c->r[{bi}] += {step * len(vregs)};')
        return st

    def block_transfer(self, ins, base):
        regs = []
        for op in ins.operands:
            if op.type == ARM_OP_REG:
                regs.append(reg_index(self.md, op.reg))
        if base in ('push', 'pop'):
            bi, regs, load = 13, sorted(regs), base == 'pop'
            mode, writeback = ('db' if base == 'push' else 'ia'), True
        else:
            # First register operand is the base; the rest are the transfer list.
            bi, regs = regs[0], sorted(regs[1:])
            load = base[0] == 'l'
            suffix = base[3:5] if len(base) > 4 else 'ia'
            mode = {'ia': 'ia', 'ib': 'ib', 'da': 'da', 'db': 'db',
                    'fd': 'ia' if load else 'db'}.get(suffix, 'ia')
            writeback = ins.writeback

        n = len(regs)
        if n == 0:
            raise Unsupported(f'{base} with empty register list')
        bexp = f'c->r[{bi}]'

        # Each mode fixes where the lowest-addressed word sits relative to the base, and
        # how far the base moves if it writes back.
        if mode == 'ia':
            first, delta = '', 4 * n
        elif mode == 'ib':
            first, delta = ' + 4', 4 * n
        elif mode == 'db':
            first, delta = f' - {4 * n}', -4 * n
        else:
            first, delta = f' - {4 * (n - 1)}', -4 * n

        st = [f'uint32_t t_base = {bexp}{first};']
        popped_pc = load and 15 in regs
        for k, r in enumerate(regs):
            if load:
                if r == 15:
                    st.append(f'uint32_t t_pc = albw_r32(t_base + {4 * k}); (void)t_pc;')
                else:
                    st.append(f'c->r[{r}] = albw_r32(t_base + {4 * k});')
            else:
                st.append(f'albw_w32(t_base + {4 * k}, c->r[{r}]);')
        if writeback:
            st.append(f'{bexp} {"+" if delta >= 0 else "-"}= {abs(delta)};')
        if popped_pc:
            if bi == 13:
                # Loading pc off the stack is a function return, and the C call stack
                # already holds that continuation. Dispatching to the popped value would
                # instead re-enter the caller mid-body, where no function starts.
                st.append('return;')
            else:
                # From any other base this really is a computed jump.
                st += ['albw_dispatch(c, t_pc);', 'return;']
        return st

    # ---- function level --------------------------------------------------

    def disasm_span(self, lo, hi, md=None):
        """Every instruction in [lo, hi], stepping over words that do not decode.

        A literal pool inside a function body is such a word, and capstone stops at the first
        one. Taking that as the end of the range silently drops every instruction after the pool
        -- which cost branch targets their labels, and turned an ordinary conditional branch into
        "outside the function and not a known function".
        """
        md = md or self.md
        step = 2 if self.thumb else 4
        out, at = [], lo
        while at <= hi:
            progressed = False
            for ins in md.disasm(self.code[at - TEXT_VA:hi - TEXT_VA + 1], at):
                progressed = True
                out.append(ins)
                at = ins.address + ins.size
            if not progressed:
                at += step
        return out

    def synth_range(self, addr, thumb, limit=0x4000):
        """Bound a function starting at addr by linear disassembly.

        Used for entry points that only the running guest knows about. Ghidra refuses to
        create functions at most of them, and fighting its analysis rules is not worth it
        when the address is already known to be real: the guest branched there.

        The end is the first terminating instruction that lies beyond every forward branch
        target seen so far, which is the usual way to stop without control flow recovery.
        """
        md = self.md_thumb if thumb else self.md_arm
        prev_md, prev_thumb = self.md, self.thumb
        self.md, self.thumb = md, thumb
        try:
            off = addr - TEXT_VA
            if off < 0 or off + 8 > len(self.code):
                return None
            # Walked one stretch at a time, stopping at the first terminator that lies beyond
            # every forward branch seen so far. Two details each cost a real fragment:
            #
            # Disassembly stops at the first word that does not decode, which in the middle of a
            # body is a literal pool, not the end of the code. Stopping there loses every
            # terminator past the pool; stepping over the word and carrying on is what a pool
            # asks for, within reason: a long run of them is data, not a body with pools in it.
            #
            # And a forward branch says the body reaches at least that far, while a branch to
            # somewhere far away says the opposite -- it leaves the body -- so only targets
            # inside the window move the bound. Letting one distant branch move it means no
            # terminator is ever far enough and the scan gives up on the whole fragment.
            step = 2 if thumb else 4
            stop = addr + limit
            furthest, at, gaps, seen = addr, addr, 0, 0
            while at < stop and gaps <= 64:
                progressed = False
                for ins in md.disasm(self.code[at - TEXT_VA:stop - TEXT_VA], at):
                    progressed = True
                    seen += 1
                    at = ins.address + ins.size
                    if base_mnemonic(ins) == 'b':
                        for op in ins.operands:
                            if op.type == ARM_OP_IMM:
                                t = op.imm & 0xFFFFFFFF
                                if furthest < t < stop:
                                    furthest = t
                    if seen >= 2 and at > furthest and self.terminates(ins):
                        return (addr, at - 1)
                if not progressed:
                    at += step
                    gaps += 1
            return None
        finally:
            self.md, self.thumb = prev_md, prev_thumb

    def inline_arg_helper(self, addr):
        """True for a function that takes the word after its call site as an argument.

        RVCT's floating-point library has one, at 0x00114d80, for the special cases of its
        conversions (NaN, infinity, out of range). It starts `ldr r4, [lr], #4`: the word after
        the `bl` is a table of nine three-bit actions, one per kind of operand. It then pops the
        caller's frame and either returns for the caller (actions 4 to 7) or jumps to one of four
        continuations after the word (0 to 3), whichever the operand asks for. So the caller never
        resumes after the call: the word is data, and the continuations are entry points of their
        own, reached through dispatch (tools/find_functions.py lists them).
        """
        cache = self.__dict__.setdefault('_inline_arg', {})
        if addr not in cache:
            off = addr - TEXT_VA
            ins = None
            if 0 <= off <= len(self.code) - 4 and not addr & 3:
                ins = next(self.md_arm.disasm(self.code[off:off + 4], addr, 1), None)
            cache[addr] = bool(ins) and ins.mnemonic == 'ldr' and \
                ins.op_str.replace(' ', '').endswith(',[lr],#4')
        return cache[addr]

    def find_veneer(self, ins):
        """Recognise a long-branch veneer: `ldr pc, [pc, #off]` with the target in a literal.

        The linker emits these where a bl cannot reach, as an eight-byte stub: the load into pc
        followed by the absolute destination. Both halves are constants in the image, so the
        jump is exact at translate time -- unlike a computed jump, there is nothing to resolve
        at runtime. Returns the destination address.
        """
        if base_mnemonic(ins) != 'ldr' or self.thumb:
            return None
        ops = ins.operands
        if len(ops) < 2 or ops[1].type != ARM_OP_MEM:
            return None
        if reg_index(self.md, ops[0].reg) != 15:
            return None
        m = ops[1].mem
        if not m.base or self.md.reg_name(m.base) != 'pc' or m.index:
            return None
        at = (ins.address + 8 + m.disp) & 0xFFFFFFFF
        off = at - TEXT_VA
        if off < 0 or off + 4 > len(self.code):
            return None
        return struct.unpack_from('<I', self.code, off)[0]

    def find_jump_table(self, insns, i, fn):
        """Recognise an ARM switch: `ldr pc, [pc, rX, lsl #2]` with the table inline.

        The compiler emits a bounds check, then loads the destination straight into pc from
        a table placed immediately after the instruction - so the table base is the
        instruction address plus 8, and the entry count is the immediate of the preceding
        compare. Targets land inside the same function, which is why this can become a
        computed goto instead of needing every basic block to be separately addressable.
        """
        ins = insns[i]
        if base_mnemonic(ins) != 'ldr':
            return None
        ops = ins.operands
        if len(ops) < 2 or ops[1].type != ARM_OP_MEM:
            return None
        if reg_index(self.md, ops[0].reg) != 15:
            return None
        m = ops[1].mem
        if not m.base or self.md.reg_name(m.base) != 'pc' or not m.index:
            return None
        if ops[1].shift.type != ARM_SFT_LSL or ops[1].shift.value != 2:
            return None
        index_reg = reg_index(self.md, m.index)

        # The compare need not be adjacent: the compiler hoists it above whatever register
        # setup the switch needs, so searching only a handful of instructions back misses
        # tables whose bound sits eight or more instructions away. Requiring the compare to be
        # against the index register is what keeps a wider window from matching an unrelated
        # one -- a mismatch gives up rather than guessing a count.
        count = None
        for j in range(i - 1, max(-1, i - 32), -1):
            prev = insns[j]
            if base_mnemonic(prev) != 'cmp':
                continue
            pops = prev.operands
            if (len(pops) >= 2 and pops[0].type == ARM_OP_REG
                    and reg_index(self.md, pops[0].reg) == index_reg
                    and pops[1].type == ARM_OP_IMM):
                count = pops[1].imm
                # The load's own condition says how the bound was meant: `lo` admits indices
                # below the immediate, `ls` admits the immediate itself as well.
                if ins.cc == ARM_CC_LS:
                    count += 1
            break
        if count is None or count <= 0 or count > 256:
            return None

        base = ins.address + 8
        targets = []
        for k in range(count):
            off = base + 4 * k - TEXT_VA
            if off < 0 or off + 4 > len(self.code):
                return None
            targets.append(struct.unpack_from('<I', self.code, off)[0])
        return base, index_reg, targets

    def find_branch_table(self, ins, by_addr):
        """Recognise the other ARM switch: `add pc, pc, rX, lsl #2` over a run of branches.

        Reading pc gives the instruction's address plus 8, so case k lands on the k-th of the
        branches that start two instructions on (the one between is the default for a conditional
        add, padding for an unconditional one). The run of unconditional branches there is the
        table. Returns the index register and the address of each case.
        """
        if self.thumb or base_mnemonic(ins) != 'add':
            return None
        ops = ins.operands
        if len(ops) != 3 or any(op.type != ARM_OP_REG for op in ops):
            return None
        if reg_index(self.md, ops[0].reg) != 15 or reg_index(self.md, ops[1].reg) != 15:
            return None
        if ops[2].shift.type != ARM_SFT_LSL or ops[2].shift.value != 2:
            return None
        treg = reg_index(self.md, ops[2].reg)
        targets = []
        at = ins.address + 8
        while at in by_addr and len(targets) < 256:
            b = by_addr[at]
            if base_mnemonic(b) != 'b' or b.cc not in (ARM_CC_AL, ARM_CC_INVALID):
                break
            targets.append(at)
            at += 4
        if treg == 15 or not targets:
            return None
        # The case after the last branch lands on whatever follows the table, which is where the
        # hardware goes for that index too; the floating-point helper's last case is exactly that.
        if at in by_addr:
            targets.append(at)
        return treg, targets

    def terminates(self, ins):
        """True if this instruction always transfers control, so nothing follows it."""
        if ins.cc not in (ARM_CC_AL, ARM_CC_INVALID):
            return False
        b = base_mnemonic(ins)
        if b in ('b', 'bx', 'bxj'):
            return True
        if b == 'bl' and ins.operands and ins.operands[0].type == ARM_OP_IMM \
                and not self.thumb and self.inline_arg_helper(ins.operands[0].imm & 0xFFFFFFFF):
            return True
        if b in ('pop', 'ldm', 'ldmia', 'ldmib', 'ldmda', 'ldmdb', 'ldmfd'):
            return any(op.type == ARM_OP_REG and self.md.reg_name(op.reg) == 'pc'
                       for op in ins.operands)
        # Any other write to pc: a return off the stack (`ldr pc, [sp], #4`), a veneer, a jump
        # table, a computed jump. Not one of them falls through, and taking one for an ordinary
        # instruction made the bytes after it -- data, as often as not -- into a continuation.
        ops = ins.operands
        return (b in ('ldr', 'mov', 'add', 'sub') and bool(ops) and ops[0].type == ARM_OP_REG
                and self.md.reg_name(ops[0].reg) == 'pc')

    def function(self, fn):
        self.thumb = bool(fn.thumb)
        self.md = self.md_thumb if self.thumb else self.md_arm
        per_range = []
        for lo, hi in fn.ranges:
            per_range.append(((lo, hi), self.disasm_span(lo, hi)))
        insns = [i for _, ri in per_range for i in ri]
        if not insns:
            raise Unsupported('no instructions')

        self.jump_tables = {}
        self.veneers = {}
        self.branch_tables = {}
        by_addr = {ins.address: ins for ins in insns}
        for i in range(len(insns)):
            found = self.find_jump_table(insns, i, fn)
            if found:
                self.jump_tables[insns[i].address] = found
                continue
            bt = self.find_branch_table(insns[i], by_addr)
            if bt:
                self.branch_tables[insns[i].address] = bt
                continue
            v = self.find_veneer(insns[i])
            if v is not None:
                self.veneers[insns[i].address] = v

        # A label is only usable where an instruction actually starts; targets inside the
        # body that land in a literal pool would emit a goto to a missing label.
        insn_addrs = {ins.address for ins in insns}
        labels = set()
        for ins in insns:
            if base_mnemonic(ins) != 'b':
                continue
            for op in ins.operands:
                if op.type == ARM_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if fn.contains(t) and t in insn_addrs:
                        labels.add(t)

        for site, (_base, _reg, targets) in self.jump_tables.items():
            for t in targets:
                if fn.contains(t) and t in insn_addrs:
                    labels.add(t)
        for site, (_reg, targets) in self.branch_tables.items():
            labels.update(t for t in targets if t in insn_addrs)

        # Where a range ends on an instruction that does not transfer control, the guest
        # keeps going at the next address. Letting the C function simply end there instead
        # would silently return to the caller.
        fallthrough = {}
        for (lo, hi), ri in per_range:
            if ri and not self.terminates(ri[-1]):
                t = hi + 1
                fallthrough[ri[-1].address] = t
                if fn.contains(t) and t in insn_addrs:
                    labels.add(t)

        out = [f'void {fn.sym}(albw_cpu *c) {{',
               '    uint32_t co = 0; (void)co;']

        # Ranges come in address order, but the entry point is not always the lowest
        # address: Ghidra attributes shared code such as an ARM/Thumb veneer to whichever
        # function claimed it, as a fragment that can sit far below the entry. Emitting
        # ranges in order then starts the C function at that fragment instead, so every
        # call runs the wrong code. Jump to the entry instead of relying on layout.
        if insns[0].address != fn.addr:
            if fn.addr not in insn_addrs:
                raise Unsupported('entry point is not an instruction boundary')
            labels.add(fn.addr)
            out.append(f'    goto L_{fn.addr:08x};')
        unsupported = []
        for ins in insns:
            if ins.address in labels:
                out.append(f'L_{ins.address:08x}:;')
            try:
                self.emit(ins, fn, labels, out)
            except Unsupported as e:
                self.gaps[str(e)] += 1
                unsupported.append((ins.address, f'{ins.mnemonic} {ins.op_str}', str(e)))
                out.append(f'    /* {ins.address:08x}  {ins.mnemonic} {ins.op_str} */')
                out.append('    albw_unimplemented(c, 0x%08xu, "%s");'
                           % (ins.address, f'{ins.mnemonic}: {e}'.replace('"', "'")))

            t = fallthrough.get(ins.address)
            if t is not None:
                out.append(f'    /* falls through to {t:08x} */')
                if fn.contains(t) and t in insn_addrs:
                    out.append(f'    goto L_{t:08x};')
                elif t in self.funcs:
                    if t not in self.slice_addrs:
                        self.external.add(t)
                    out.append(f'    {self.funcs[t].sym}(c);')
                    out.append('    return;')
                else:
                    self.gaps['fallthrough to unknown address'] += 1
                    unsupported.append((ins.address, 'fallthrough',
                                        f'to 0x{t:08x}, not a function'))
                    # The target in the form an entry list takes it: Thumb with its low bit set.
                    out.append('    albw_missing_function(c, 0x%08xu, 0x%08xu);'
                               % (ins.address, t | 1 if self.thumb else t))
        out.append('}')
        return '\n'.join(out), unsupported


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tsv', default='rom-extract/functions.tsv',
                    help='the function database: tools/find_functions.py writes it from the card '
                         'image; a Ghidra export in the same format works too')
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--out', default='src/generated')
    ap.add_argument('--max-depth', type=int, default=10 ** 9)
    ap.add_argument('--all', action='store_true',
                    help='translate every function, not just the boot closure. Needed '
                         'because C++ static initialisers dispatch indirectly across '
                         'the whole binary, so no static closure can bound them.')
    ap.add_argument('--split', type=int, default=1,
                    help='spread the generated functions over this many .c files')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--extra-entries', action='append', default=[],
                    help='file of hex addresses to translate as functions even though the '
                         'database has none there. Bit 0 set means Thumb. For a database that '
                         'does not already hold the addresses the running guest branched to, '
                         'as tools/find_functions.py\'s does. May be given more than once.')
    args = ap.parse_args()
    if not os.path.exists(args.tsv):
        sys.exit(f'recompile: no function database at {args.tsv}; '
                 f'run tools/find_functions.py first')

    code = open(args.bin, 'rb').read()
    funcs = load_functions(args.tsv)
    if args.all:
        slice_fns = list(funcs.values())
    else:
        slice_fns = [f for f in funcs.values() if 0 <= f.depth <= args.max_depth]
    slice_fns.sort(key=lambda f: f.addr)

    tr = Translator(code, funcs)

    # Ghidra exports three functions one byte long, which no instruction fits in: FUN_00544e54 is
    # one, and the game reaches it -- the function before it is two instructions and falls through,
    # so the body that was never translated is the rest of that function. Trusting the range gave a
    # body of "no instructions" and stopped the run. A range too short to hold an instruction says
    # nothing about where the code ends, so bound it the way a discovered entry is bounded.
    for fn in funcs.values():
        if sum(hi - lo + 1 for lo, hi in fn.ranges) >= (2 if fn.thumb else 4):
            continue
        rng = tr.synth_range(fn.addr, fn.thumb)
        if rng is not None:
            fn.ranges = [rng]
            fn.size = rng[1] - rng[0] + 1

    if args.extra_entries:
        added = skipped = 0
        lines = []
        for path in args.extra_entries:
            try:
                lines.extend(open(path))
            except FileNotFoundError:
                print(f'extra entries         : {path} is not there; skipped')
        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            raw = int(line, 16)
            addr, thumb = raw & ~1, raw & 1
            if addr in funcs:
                continue
            rng = tr.synth_range(addr, thumb)
            if rng is None:
                skipped += 1        # does not decode as code; discard rather than guess
                continue
            fn = Fn(addr, rng[1] - rng[0] + 1, f'discovered_{addr:08x}',
                    thumb, 0, [rng])
            funcs[addr] = fn
            slice_fns.append(fn)
            added += 1
        slice_fns.sort(key=lambda f: f.addr)
        print(f'extra entries         : {added} added, {skipped} did not decode')

    # Two kinds of edge name their target outright, so the translation can close them by
    # itself -- no run needed, unlike the indirect targets above. A direct bl names its callee;
    # a body that runs off the end of a range continues at the next address. New bodies bring
    # new edges, so this repeats until it stops finding any.
    #
    # Fall-through targets are kept out of the dispatch table. Where a body's bounds are simply
    # cut short, the next address is code and this is right; where the bytes after it are data,
    # synthesising a body there would turn a loud "no function at this address" into silently
    # running that data, and only an indirect branch can arrive at it that way. Reaching it by
    # falling through stays exact either way, because that edge is the one we followed.
    called, fell = 0, 0
    fallthrough_only = set()
    while True:
        # Continuations carry the instruction set of the body they continue; a call or a
        # tail branch that leaves a Thumb body may well be ARM, so those stay ARM until
        # something says otherwise.
        want_call, want_fall, want_veneer = set(), {}, {}
        for fn in slice_fns:
            md = tr.md_thumb if fn.thumb else tr.md_arm
            for start, end in fn.ranges:
                off = start - TEXT_VA
                if off < 0 or off >= len(code):
                    continue
                tr.md, tr.thumb = md, bool(fn.thumb)
                insns = tr.disasm_span(start, end, md)
                for ins in insns:
                    b = base_mnemonic(ins)
                    if b not in ('bl', 'b') or not ins.operands:
                        continue
                    op = ins.operands[0]
                    if op.type != ARM_OP_IMM:
                        continue
                    t = op.imm & 0xFFFFFFFF
                    if t in funcs or not (TEXT_VA <= t < TEXT_VA + len(code)):
                        continue
                    if b == 'bl':
                        want_call.add(t)
                    elif not fn.contains(t):
                        # A plain branch that leaves the body is a tail call, and lands on an
                        # entry point by the same argument as a call -- except that it may land
                        # inside another function's body, where starting a fresh one is right
                        # for this edge but wrong to offer indirect branches. A branch cannot
                        # change instruction set, so the target is in this body's.
                        want_fall[t] = fn.thumb
                for ins in insns:
                    v = tr.find_veneer(ins)
                    if v is None:
                        continue
                    # A veneer's literal carries the Thumb bit, which is the one place an
                    # entry point's instruction set is stated outright rather than inferred.
                    t, th = v & ~1, v & 1
                    if t not in funcs and TEXT_VA <= t < TEXT_VA + len(code):
                        want_veneer[t] = th
                if insns and not tr.terminates(insns[-1]):
                    t = end + 1
                    if t not in funcs and not fn.contains(t) \
                            and TEXT_VA <= t < TEXT_VA + len(code):
                        want_fall[t] = fn.thumb
        for t in list(want_call):
            want_fall.pop(t, None)
        for t in list(want_veneer):
            want_call.discard(t)
            want_fall.pop(t, None)
        if not want_call and not want_fall and not want_veneer:
            break
        added = 0
        plan = [(t, 'called', 0) for t in sorted(want_call)] \
             + [(t, 'tail', want_fall[t]) for t in sorted(want_fall)] \
             + [(t, 'veneer', want_veneer[t]) for t in sorted(want_veneer)]
        for t, kind, thumb in plan:
            if t in funcs:
                continue
            rng = tr.synth_range(t, thumb)
            if rng is None:
                continue                # does not decode as code; leave the gap reported
            fn = Fn(t, rng[1] - rng[0] + 1, f'{kind}_{t:08x}', thumb, 0, [rng])
            funcs[t] = fn
            slice_fns.append(fn)
            if kind == 'tail':
                fallthrough_only.add(t)
                fell += 1
            else:
                called += 1                     # veneer targets are real entry points
            added += 1
        if not added:
            break
        slice_fns.sort(key=lambda f: f.addr)
    if called or fell:
        print(f'static edges closed   : {called} call target(s), '
              f'{fell} continuation(s) reached by falling through or tail branch '
              f'(not in the dispatch table)')

    tr.slice_addrs = {f.addr for f in slice_fns}
    os.makedirs(args.out, exist_ok=True)

    units = []                                  # (address, symbol, body)
    clean = 0
    per_fn_gaps = []
    for fn in slice_fns:
        try:
            src, unsup = tr.function(fn)
        except Unsupported as e:
            # A body is still required: the function has a declaration and a dispatch
            # table entry, so skipping it silently would fail to link.
            tr.gaps[f'whole function: {e}'] += 1
            units.append((fn.addr, fn.sym,
                f'void {fn.sym}(albw_cpu *c) {{\n'
                f'    albw_unimplemented(c, 0x{fn.addr:08x}u,'
                f' "untranslatable function: {e}");\n}}'))
            continue
        units.append((fn.addr, fn.sym, src))
        if unsup:
            per_fn_gaps.append((fn, len(unsup)))
        else:
            clean += 1

    for addr in sorted(tr.external):
        fn = funcs[addr]
        units.append((addr, fn.sym,
            f'void {fn.sym}(albw_cpu *c) {{\n'
            f'    albw_unimplemented(c, 0x{addr:08x}u,'
            f' "call leaves the slice: {fn.name}");\n}}'))

    # Only these are reachable indirectly; see the closure above for why the rest are not.
    table_fns = [f for f in slice_fns if f.addr not in fallthrough_only]

    # The build should only redo what changed. Three things used to defeat that, so that adding one
    # function to the discovery list rebuilt all sixteen files -- four minutes a round, nearly all of
    # it compiling code that had not changed:
    #
    #   - every file included one header declaring every function and the table's length, and both
    #     change whenever a function is added;
    #   - the files were cut by count, so one function more moved every boundary after it;
    #   - every file was rewritten whether or not its contents changed, which alone is enough.
    #
    # So each file now declares only what it refers to, a function's file is chosen by its address
    # and stays put, the length lives in a header only the dispatcher and the table include, and a
    # file whose contents are unchanged is left alone.
    def write_if_changed(path, text):
        try:
            with open(path) as fh:
                if fh.read() == text:
                    return False
        except OSError:
            pass
        with open(path, 'w') as fh:
            fh.write(text)
        return True

    header = ('#ifndef ALBW_GENERATED_H\n#define ALBW_GENERATED_H\n'
              '#include "albw_cpu.h"\n\n'
              '// The dispatch table. Its length is in albw_fn_count.h, which only the dispatcher and\n'
              '// the table include: it changes with every function added.\n'
              'extern const uint32_t albw_fn_addrs[];\n'
              'extern const albw_fn albw_fn_ptrs[];\n'
              '\n#endif\n')
    written = int(write_if_changed(f'{args.out}/albw_generated.h', header))
    written += write_if_changed(f'{args.out}/albw_fn_count.h',
                                f'#define ALBW_FN_COUNT {len(table_fns)}\n')

    n_files = max(1, args.split)
    buckets = [[] for _ in range(n_files)]
    for addr, sym, src in units:
        # Bits 8 to 11 of the address and up: stable, and even across the files, since
        # functions are a few hundred bytes and spread over the whole of .text.
        buckets[(addr >> 8) % n_files].append((addr, sym, src))
    sym_ref = re.compile(r'\bf_[0-9a-f]{8}\b')
    for k, chunk in enumerate(buckets):
        chunk.sort()
        refs = set()
        for _, sym, src in chunk:
            refs.add(sym)
            refs.update(sym_ref.findall(src))
        text = ('/* Generated by tools/recompile.py - do not edit. */\n'
                '#include "albw_generated.h"\n\n'
                + ''.join(f'void {r}(albw_cpu *c);\n' for r in sorted(refs))
                + '\n' + '\n\n'.join(src for _, _, src in chunk) + '\n')
        written += write_if_changed(f'{args.out}/albw_generated_{k}.c', text)

    table = ('/* Generated by tools/recompile.py - do not edit. */\n'
             '#include "albw_generated.h"\n#include "albw_fn_count.h"\n\n'
             + ''.join(f'void {f.sym}(albw_cpu *c);\n' for f in table_fns)
             + '\nconst uint32_t albw_fn_addrs[ALBW_FN_COUNT] = {\n'
             + ''.join(f'    0x{f.addr:08x}u,\n' for f in table_fns)
             + '};\n\nconst albw_fn albw_fn_ptrs[ALBW_FN_COUNT] = {\n'
             + ''.join(f'    {f.sym},\n' for f in table_fns)
             + '};\n')
    written += write_if_changed(f'{args.out}/albw_tables.c', table)

    # Which code this is a translation of, for the runtime to check at start: the code it loads
    # must be the code translated here, and the card it reads must be the same release.
    h = 0xCBF29CE484222325
    for b in code:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    try:
        product = open(os.path.join(os.path.dirname(args.bin), 'product_code.txt')).read().strip()
    except FileNotFoundError:
        product = ''
    ident = ('/* Generated by tools/recompile.py - do not edit. */\n#include <stdint.h>\n\n'
             f'const char albw_build_product[] = "{product}";\n'
             f'const uint64_t albw_build_code_hash = 0x{h:016x}ull;   /* FNV-1a of code.bin */\n'
             f'const uint32_t albw_build_code_size = {len(code)}u;\n')
    written += write_if_changed(f'{args.out}/albw_code_id.c', ident)
    print(f'files written          : {written} of {n_files + 4} (the rest were unchanged)')
    print(f'generated             : {n_files} source file(s) plus the dispatch tables')

    print(f'functions in slice     : {len(slice_fns)}')
    print(f'fully translated       : {clean} ({100.0*clean/max(len(slice_fns),1):.1f}%)')
    print(f'with unimplemented ops : {len(per_fn_gaps)}')
    print(f'unsupported sites      : {sum(tr.gaps.values())}')
    print(f'external stubs emitted : {len(tr.external)}')
    if args.report:
        print('\ntop gaps:')
        for k, v in tr.gaps.most_common(25):
            print(f'  {v:>5}  {k}')


if __name__ == '__main__':
    main()

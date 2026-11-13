#!/usr/bin/env python3
"""insn_check.py -- every kind of instruction the game uses, translated alone and checked against Unicorn.

The reference run (tools/oracle.py) compares the whole boot path instruction by instruction, which
is exact but cannot reach the game proper: a Python hook on every instruction does not get through
millions of frames. Most mistranslations so far were in the semantics of one instruction -- a flag,
a shift, a VFP operation, a quirk of how capstone reports an operand -- and those can be found
without running the game: take each distinct form of instruction that occurs in the translated
code, translate a few real instances of it on their own, and run each from random register, flag,
VFP and memory states, once as the translation and once under Unicorn, comparing everything after.

Branches and system instructions are left out: they need a body around them. Thumb code is in,
as forms of its own.

    python3 tools/insn_check.py [--per-form 3] [--trials 40] [--seed 1]

Needs python-unicorn and capstone, and rom-extract/code.bin with the function database the
recompiler uses (tools/find_functions.py). Prints the forms that disagree, with the instruction
and both results.
"""
import argparse
import collections
import ctypes
import mmap
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recompile as R  # noqa: E402

from capstone.arm import (ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG, ARM_REG_PC,  # noqa: E402
                          ARM_REG_SP)
from unicorn import UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL, Uc, UcError  # noqa: E402
from unicorn import arm_const as A  # noqa: E402

TEXT_VA = R.TEXT_VA
MEM_SIZE = 0x20000000
DATA = 0x08000000                       # where every memory operand points
DATA_SIZE = 0x20000
GPR = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]

SKIP = {
    'b', 'bl', 'blx', 'bx', 'bxj', 'svc', 'swi', 'mrc', 'mcr', 'mrrc', 'mcrr', 'mrc2', 'mcr2',
    'ldrex', 'ldrexb', 'ldrexh', 'ldrexd', 'strex', 'strexb', 'strexh', 'strexd', 'clrex',
    'dmb', 'dsb', 'isb', 'wfi', 'wfe', 'sev', 'yield', 'pld', 'pldw', 'pli', 'bkpt', 'udf',
    'cps', 'cpsie', 'cpsid', 'setend', 'srs', 'rfe', 'smc', 'it', 'cbz', 'cbnz', 'tbb', 'tbh',
    'nop', 'vmrs', 'vmsr', 'mrs', 'msr', 'ldc', 'stc', 'cdp',
}


class Cpu(ctypes.Structure):
    """albw_cpu, as src/runtime/albw_cpu.h lays it out."""
    class _V(ctypes.Union):
        _fields_ = [('s', ctypes.c_float * 32), ('d', ctypes.c_double * 16),
                    ('u32', ctypes.c_uint32 * 32), ('u64', ctypes.c_uint64 * 16)]
    _fields_ = [('r', ctypes.c_uint32 * 16), ('nf', ctypes.c_uint32), ('zf', ctypes.c_uint32),
                ('cf', ctypes.c_uint32), ('vf', ctypes.c_uint32), ('v', _V),
                ('fpscr', ctypes.c_uint32), ('tls', ctypes.c_uint32), ('spins', ctypes.c_uint32)]


def form(ins):
    """What makes two instructions the same kind for this purpose."""
    kinds = []
    for op in ins.operands:
        if op.type == ARM_OP_REG:
            name = ins.reg_name(op.reg)
            k = name[0] if name[0] in 'sdq' and name[1:].isdigit() else 'r'
        elif op.type == ARM_OP_IMM:
            k = 'i'
        elif op.type == ARM_OP_MEM:
            k = 'm' + ('x' if op.mem.index else '') + ('p' if op.mem.base == ARM_REG_PC else '')
        else:
            k = '?'
        if op.shift.type:
            k += f's{op.shift.type}'
        if op.subtracted:
            k += '-'
        kinds.append(k)
    mnem = ins.mnemonic
    return (mnem, tuple(kinds), bool(ins.writeback))


def writes_pc(ins):
    for i, op in enumerate(ins.operands):
        if op.type == ARM_OP_REG and op.reg == ARM_REG_PC:
            base = R.base_mnemonic(ins)
            if base in ('pop', 'ldm', 'ldmia', 'ldmib', 'ldmda', 'ldmdb', 'ldmfd') or i == 0:
                return True
    return False


def collect(tr, funcs, per_form, rng):
    forms = collections.defaultdict(list)
    for fn in funcs.values():
        md = tr.md_thumb if fn.thumb else tr.md_arm
        tr.md, tr.thumb = md, bool(fn.thumb)
        for lo, hi in fn.ranges:
            for ins in tr.disasm_span(lo, hi, md):
                base = R.base_mnemonic(ins)
                if base in SKIP or writes_pc(ins):
                    continue
                # Thumb forms are kinds of their own: the same mnemonic is another encoding,
                # with its own flag rules (most Thumb data processing sets the flags).
                forms[(bool(fn.thumb),) + form(ins)].append(ins.address)
    chosen = []
    for key, addrs in forms.items():
        addrs = sorted(set(addrs))
        rng.shuffle(addrs)
        chosen.extend((key, a) for a in addrs[:per_form])
    return forms, chosen


def build_library(tr, code, chosen, workdir):
    lines = ['#include "albw_cpu.h"', 'uint8_t *albw_mem;',
             'static int unimplemented;',
             'void albw_dispatch(albw_cpu *c, uint32_t t) { (void)c; (void)t; }',
             'void albw_unimplemented(albw_cpu *c, uint32_t pc, const char *w) '
             '{ (void)c; (void)pc; (void)w; unimplemented = 1; }',
             'void albw_missing_function(albw_cpu *c, uint32_t pc, uint32_t t) '
             '{ (void)c; (void)pc; (void)t; }',
             'void albw_preempt(albw_cpu *c) { (void)c; }',
             'void albw_svc(albw_cpu *c, uint32_t imm) { (void)c; (void)imm; }',
             'void albw_trace(albw_cpu *c, uint32_t pc) { (void)c; (void)pc; }']
    tests = []
    tr.jump_tables, tr.veneers, tr.branch_tables = {}, {}, {}
    for key, addr in chosen:
        thumb = key[0]
        tr.thumb, tr.md = thumb, tr.md_thumb if thumb else tr.md_arm
        off = addr - TEXT_VA
        ins = next(tr.md.disasm(code[off:off + 4], addr), None)
        if ins is None:
            continue
        fn = R.Fn(addr, ins.size, 'test', int(thumb), 0, [(addr, addr + ins.size - 1)])
        out = []
        try:
            tr.emit(ins, fn, {}, out)
        except Exception:
            continue                    # not something the translator takes alone
        body = '\n'.join(out)
        if 'goto ' in body:
            continue
        idx = len(tests)
        lines.append(f'static void t_{idx}(albw_cpu *c) {{\n    uint32_t co = 0; (void)co;\n{body}\n}}')
        tests.append((key, addr, ins))
    lines.append('typedef void (*test_fn)(albw_cpu *);')
    lines.append('static const test_fn tests[] = { '
                 + ', '.join(f't_{i}' for i in range(len(tests))) + ' };')
    # Each test runs in the floating-point mode its FPSCR names, and Python's own arithmetic
    # gets its mode back afterwards.
    lines.append('int test_run(int i, albw_cpu *c) { unsigned saved = __builtin_ia32_stmxcsr();'
                 ' albw_set_fpscr(c, c->fpscr); unimplemented = 0; tests[i](c);'
                 ' __builtin_ia32_ldmxcsr(saved); return unimplemented; }')
    lines.append('void test_memory(uint8_t *m) { albw_mem = m; }')
    src = os.path.join(workdir, 'insn_tests.c')
    lib = os.path.join(workdir, 'insn_tests.so')
    with open(src, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    subprocess.check_call(['gcc', '-O1', '-shared', '-fPIC', '-fno-strict-aliasing', '-w', '-ffp-contract=off',
                           '-I', os.path.join(root, 'src/runtime'), src, '-o', lib, '-lm'])
    return lib, tests


SPECIAL_FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-40, -1e-40, 3.4e38, -3.4e38,
                  float('inf'), float('-inf'), float('nan'), 1e10, -7.25, 0.333333]


def random_float(rng):
    if rng.random() < 0.3:
        return rng.choice(SPECIAL_FLOATS)
    return rng.uniform(-1000.0, 1000.0) * (10 ** rng.randint(-6, 6))


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def same_float_bits(a, b, double):
    if a == b:
        return True
    if double:
        fa, fb = struct.unpack('<d', struct.pack('<Q', a))[0], struct.unpack('<d', struct.pack('<Q', b))[0]
    else:
        fa, fb = struct.unpack('<f', struct.pack('<I', a))[0], struct.unpack('<f', struct.pack('<I', b))[0]
    return fa != fa and fb != fb        # any NaN matches any NaN


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--tsv', default='rom-extract/functions.tsv')
    ap.add_argument('--per-form', type=int, default=3)
    ap.add_argument('--trials', type=int, default=40)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=60, help='disagreements to print')
    ap.add_argument('--fpscr', type=lambda x: int(x, 0), default=0x03000000,
                    help='floating-point mode for both sides; the game sets 0x03000000 in every '
                         'thread (flush to zero, default NaN, round to nearest)')
    a = ap.parse_args()
    rng = random.Random(a.seed)

    code = open(a.bin, 'rb').read()
    funcs = R.load_functions(a.tsv)
    tr = R.Translator(code, funcs)
    forms, chosen = collect(tr, funcs, a.per_form, rng)
    work = tempfile.mkdtemp(prefix='albw-insn-')
    lib_path, tests = build_library(tr, code, chosen, work)
    print(f'{len(forms)} instruction forms, {len(tests)} instances translated alone')

    lib = ctypes.CDLL(lib_path)
    lib.test_run.argtypes = [ctypes.c_int, ctypes.POINTER(Cpu)]
    lib.test_memory.argtypes = [ctypes.c_void_p]
    host = mmap.mmap(-1, MEM_SIZE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
    emu = mmap.mmap(-1, MEM_SIZE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
    host_ptr = ctypes.addressof(ctypes.c_char.from_buffer(host))
    emu_ptr = ctypes.addressof(ctypes.c_char.from_buffer(emu))
    lib.test_memory(host_ptr)
    host[TEXT_VA:TEXT_VA + len(code)] = code
    emu[TEXT_VA:TEXT_VA + len(code)] = code

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map_ptr(0, MEM_SIZE, UC_PROT_ALL, emu_ptr)
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)

    bad = collections.OrderedDict()
    ran = 0
    for ti, (key, addr, ins) in enumerate(tests):
        # The registers that address memory point into the data window, the rest are anything.
        addr_regs = set()
        for op in ins.operands:
            if op.type == ARM_OP_MEM:
                if op.mem.base and op.mem.base != ARM_REG_PC:
                    addr_regs.add(op.mem.base)
                if op.mem.index:
                    addr_regs.add(op.mem.index)
        base = R.base_mnemonic(ins)
        if base in ('push', 'pop', 'vpush', 'vpop'):
            addr_regs.add(ARM_REG_SP)
        elif base.startswith(('ldm', 'stm', 'vldm', 'vstm')):
            if ins.operands and ins.operands[0].type == ARM_OP_REG:
                addr_regs.add(ins.operands[0].reg)
        for trial in range(a.trials):
            regs = [rng.getrandbits(32) for _ in range(15)]
            if rng.random() < 0.3:
                for k in range(15):
                    regs[k] = rng.choice([0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 31, 32, 33,
                                          rng.getrandbits(8), rng.getrandbits(32)])
            for r in addr_regs:
                try:
                    k = R.reg_index(tr.md_arm, r) if r != ARM_REG_SP else 13
                except R.Unsupported:
                    continue
                if k is None or k > 14:
                    continue
                if r == next((op.mem.index for op in ins.operands
                              if op.type == ARM_OP_MEM and op.mem.index), None):
                    regs[k] = rng.randrange(0, 64) * 4
                else:
                    regs[k] = DATA + DATA_SIZE // 2 + rng.randrange(-256, 256) * 4
            flags = rng.getrandbits(4)
            vfp = [rng.getrandbits(32) for _ in range(32)]
            for k in range(32):
                if rng.random() < 0.8:
                    vfp[k] = f32_bits(random_float(rng))
            data = bytes(rng.getrandbits(8) for _ in range(DATA_SIZE))
            host[DATA:DATA + DATA_SIZE] = data
            emu[DATA:DATA + DATA_SIZE] = data

            thumb = key[0]
            cpu = Cpu()
            for k in range(15):
                cpu.r[k] = regs[k]
            cpu.r[15] = addr + (4 if thumb else 8)
            cpu.nf, cpu.zf, cpu.cf, cpu.vf = (flags >> 3) & 1, (flags >> 2) & 1, (flags >> 1) & 1, flags & 1
            for k in range(32):
                cpu.v.u32[k] = vfp[k]
            cpu.fpscr = a.fpscr
            if lib.test_run(ti, ctypes.byref(cpu)):
                break                   # the translator says it cannot do this one

            for k in range(15):
                uc.reg_write(GPR[k], regs[k])
            uc.reg_write(A.UC_ARM_REG_CPSR, (uc.reg_read(A.UC_ARM_REG_CPSR) & 0x0FFFFFFF & ~0x20)
                         | (flags << 28) | (0x20 if thumb else 0))
            for k in range(16):
                lo, hi = vfp[2 * k], vfp[2 * k + 1]
                uc.reg_write(getattr(A, f'UC_ARM_REG_D{k}'), lo | (hi << 32))
            uc.reg_write(A.UC_ARM_REG_FPSCR, a.fpscr)
            try:
                uc.emu_start(addr | 1 if thumb else addr, addr + ins.size, count=1)
            except UcError as e:
                bad.setdefault(key, (ins, f'unicorn: {e}'))
                break
            ran += 1

            diffs = []
            for k in range(15):
                u = uc.reg_read(GPR[k]) & 0xFFFFFFFF
                if u != cpu.r[k]:
                    diffs.append(f'r{k}: ours {cpu.r[k]:08x} unicorn {u:08x}')
            cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
            ours = (cpu.nf << 3) | (cpu.zf << 2) | (cpu.cf << 1) | cpu.vf
            if ours != (cpsr >> 28) & 0xF:
                diffs.append(f'NZCV: ours {ours:04b} unicorn {(cpsr >> 28) & 0xF:04b}')
            for k in range(16):
                u = uc.reg_read(getattr(A, f'UC_ARM_REG_D{k}'))
                o = cpu.v.u64[k]
                if u != o:
                    lo_ok = same_float_bits(u & 0xFFFFFFFF, o & 0xFFFFFFFF, False)
                    hi_ok = same_float_bits(u >> 32, o >> 32, False)
                    d_ok = same_float_bits(u, o, True)
                    if not ((lo_ok and hi_ok) or d_ok):
                        diffs.append(f'd{k}: ours {o:016x} unicorn {u:016x}')
            if host[DATA:DATA + DATA_SIZE] != emu[DATA:DATA + DATA_SIZE]:
                hd, ed = host[DATA:DATA + DATA_SIZE], emu[DATA:DATA + DATA_SIZE]
                first = next(i for i in range(DATA_SIZE) if hd[i] != ed[i])
                diffs.append(f'memory at {DATA + first:08x}: ours {hd[first]:02x} unicorn {ed[first]:02x}')
            if diffs:
                state = ' '.join(f'r{k}={regs[k]:08x}' for k in range(15)) + f' nzcv={flags:04b}'
                bad.setdefault(key, (ins, '; '.join(diffs[:4]) + f'\n        from {state}'))
                break

    print(f'{ran} runs compared; {len(bad)} instruction forms disagree')
    for i, (key, (ins, why)) in enumerate(bad.items()):
        if i >= a.show:
            print(f'... and {len(bad) - a.show} more')
            break
        isa = ' [Thumb]' if key[0] else ''
        print(f'  {ins.address:08x}  {ins.mnemonic} {ins.op_str}{isa}\n        {why}')


if __name__ == '__main__':
    main()

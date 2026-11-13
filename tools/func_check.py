#!/usr/bin/env python3
"""func_check.py -- one guest function, translated and under Unicorn, from the same states.

tools/insn_check.py checks instructions one at a time, which cannot see what lives between them:
a branch table, a call that does not return the usual way, a helper that reads the word after
its call. This runs a whole function -- with everything it calls, directly or through dispatch --
both ways and compares the result registers and the memory:

    python3 tools/func_check.py 0x006920b0 --floats=1.5,-3e9,1e30,inf,-inf,nan
    python3 tools/func_check.py 0x006920b0 --random 200

--floats passes float32 values in r0, one run each; --random fills r0 to r3. Each run starts with
the same registers, stack and image on both sides and compares r0, r1 and the stack at the end.
Needs python-unicorn and capstone, rom-extract/code.bin and the function database.
"""
import argparse
import ctypes
import math
import mmap
import os
import random
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recompile as R  # noqa: E402
import insn_check as I  # noqa: E402

from unicorn import UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL, Uc, UcError  # noqa: E402
from unicorn import arm_const as A  # noqa: E402

MEM_SIZE = 0x20000000
STACK_TOP = 0x0FF00000
RETURN = 0x0FFF0000         # where the function returns to; mapped, so Unicorn can stop there


def closure(tr, funcs, root):
    """The function and everything its translation names, translated: {address: C source}."""
    out, todo = {}, [root]
    while todo:
        a = todo.pop()
        if a in out:
            continue
        src, _ = tr.function(funcs[a])
        out[a] = src
        for m in re.finditer(r'\bf_([0-9a-f]{8})\(c\)', src):
            t = int(m.group(1), 16)
            if t not in out and t in funcs:
                todo.append(t)
        # Dispatch targets the translation states outright: continuations of the inline-argument
        # helper, veneers, jump tables. Anything else that is dispatched shows up as a miss.
        for m in re.finditer(r'albw_dispatch\(c, 0x([0-9a-f]+)u\)', src):
            t = int(m.group(1), 16) & ~1
            if t in funcs and t not in out:
                todo.append(t)
        # The inline-argument helper continues at the slots its argument word names.
        for m in re.finditer(r'/\* ([0-9a-f]{8})  bl #0x([0-9a-f]+) \*/', src):
            site, t = int(m.group(1), 16), int(m.group(2), 16)
            if tr.inline_arg_helper(t):
                arg = struct.unpack_from('<I', tr.code, site + 4 - R.TEXT_VA)[0]
                for k in range(9):
                    act = (arg >> (3 * k)) & 7
                    if act < 4 and site + 8 + 4 * act in funcs:
                        todo.append(site + 8 + 4 * act)
    return out


def build(units, funcs, workdir):
    lines = ['#include "albw_cpu.h"', '#include <stdio.h>', 'uint8_t *albw_mem;',
             'static int failed;', 'static unsigned missing_at;']
    for a in units:
        lines.append(f'void {funcs[a].sym}(albw_cpu *c);')
    lines += ['void albw_unimplemented(albw_cpu *c, uint32_t pc, const char *w) '
              '{ (void)c; fprintf(stderr, "unimplemented at %08x: %s\\n", pc, w); failed = 1; }',
              'void albw_missing_function(albw_cpu *c, uint32_t pc, uint32_t t) '
              '{ (void)c; (void)pc; missing_at = t; failed = 1; }',
              'void albw_preempt(albw_cpu *c) { (void)c; }',
              'void albw_svc(albw_cpu *c, uint32_t imm) { (void)c; (void)imm; failed = 1; }',
              'void albw_trace(albw_cpu *c, uint32_t pc) { (void)c; (void)pc; }',
              'void albw_dispatch(albw_cpu *c, uint32_t t) {',
              f'    if (t == 0x{RETURN:x}u) return;',
              '    switch (t & ~1u) {']
    for a in units:
        lines.append(f'    case 0x{a:x}u: {funcs[a].sym}(c); return;')
    lines += ['    }', '    missing_at = t; failed = 1;', '}']
    lines += [units[a] for a in units]
    lines += ['int test_run(albw_cpu *c, uint32_t entry) {',
              '    unsigned saved = __builtin_ia32_stmxcsr();',
              '    albw_set_fpscr(c, c->fpscr); failed = 0; missing_at = 0;',
              '    albw_dispatch(c, entry);',
              '    __builtin_ia32_ldmxcsr(saved);',
              '    return failed ? (missing_at ? (int)missing_at : 1) : 0; }',
              'void test_memory(uint8_t *m) { albw_mem = m; }']
    src = os.path.join(workdir, 'func.c')
    lib = os.path.join(workdir, 'func.so')
    open(src, 'w').write('\n'.join(lines) + '\n')
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    subprocess.check_call(['gcc', '-O1', '-shared', '-fPIC', '-fno-strict-aliasing', '-w',
                           '-ffp-contract=off', '-I', os.path.join(root, 'src/runtime'), src,
                           '-o', lib, '-lm'])
    return lib


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addr', type=lambda x: int(x, 0))
    ap.add_argument('--floats', default='',
                    help='float32 values to pass in r0, one run each, separated by commas: '
                         '--floats=1.5,-inf,nan,1e30')
    ap.add_argument('--random', type=int, default=0, help='also this many runs from random r0-r3')
    ap.add_argument('--fpscr', type=lambda x: int(x, 0), default=0x03000000)
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--tsv', default='rom-extract/functions.tsv')
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)

    code = open(a.bin, 'rb').read()
    funcs = R.load_functions(a.tsv)
    tr = R.Translator(code, funcs)
    tr.slice_addrs = set(funcs)
    units = closure(tr, funcs, a.addr)
    print(f'{len(units)} function(s) translated: ' +
          ' '.join(f'{x:08x}' for x in sorted(units)))
    lib = ctypes.CDLL(build(units, funcs, tempfile.mkdtemp(prefix='albw-func-')))
    lib.test_run.argtypes = [ctypes.POINTER(I.Cpu), ctypes.c_uint32]
    lib.test_memory.argtypes = [ctypes.c_void_p]

    host = mmap.mmap(-1, MEM_SIZE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
    emu = mmap.mmap(-1, MEM_SIZE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
    lib.test_memory(ctypes.addressof(ctypes.c_char.from_buffer(host)))
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map_ptr(0, MEM_SIZE, UC_PROT_ALL, ctypes.addressof(ctypes.c_char.from_buffer(emu)))
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)

    runs = []
    for v in filter(None, a.floats.split(',')):
        runs.append([f32(float(v))] + [0, 0, 0])
    for _ in range(a.random):
        runs.append([rng.choice([rng.getrandbits(32), f32(rng.uniform(-1e20, 1e20)),
                                 f32(rng.uniform(-100, 100))]) for _ in range(4)])
    bad = 0
    for regs in runs:
        for m in (host, emu):
            m[0x100000:0x100000 + len(code)] = code
            m[STACK_TOP - 0x10000:STACK_TOP] = bytes(0x10000)
        cpu = I.Cpu()
        for k in range(4):
            cpu.r[k] = regs[k]
        cpu.r[13], cpu.r[14] = STACK_TOP, RETURN
        cpu.fpscr = a.fpscr
        status = lib.test_run(ctypes.byref(cpu), a.addr)

        for k in range(13):
            uc.reg_write(I.GPR[k], regs[k] if k < 4 else 0)
        uc.reg_write(A.UC_ARM_REG_SP, STACK_TOP)
        uc.reg_write(A.UC_ARM_REG_LR, RETURN)
        uc.reg_write(A.UC_ARM_REG_CPSR, uc.reg_read(A.UC_ARM_REG_CPSR) & ~0x20)
        uc.reg_write(A.UC_ARM_REG_FPSCR, a.fpscr)
        try:
            uc.emu_start(a.addr, RETURN, count=1_000_000)
            ur = [uc.reg_read(I.GPR[k]) & 0xFFFFFFFF for k in range(4)]
            err = None
        except UcError as e:
            ur, err = None, str(e)
        ours = [cpu.r[k] for k in range(4)]
        shown = ' '.join(f'{x:08x}' for x in regs[:2])
        if status:
            what = f'missing function {status:#010x}' if status > 1 else 'gap reached'
            print(f'  r0 r1 = {shown}: ours stopped ({what})')
            bad += 1
        elif err or ours[:2] != ur[:2] or host[STACK_TOP - 0x10000:STACK_TOP] != \
                emu[STACK_TOP - 0x10000:STACK_TOP]:
            print(f'  r0 r1 = {shown}: ours {ours[0]:08x} {ours[1]:08x}, unicorn '
                  + (err if err else f'{ur[0]:08x} {ur[1]:08x}'))
            bad += 1
    print(f'{len(runs)} runs, {bad} disagree')


if __name__ == '__main__':
    main()

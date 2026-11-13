"""Reference execution of the boot path under Unicorn, for diffing against the recompiler.

The recompiled run and this one must agree instruction for instruction, so the first
differing trace record names the guest instruction that was translated wrongly.

Both sides share one Horizon implementation rather than having one each. The kernel is
loaded here as a shared library and driven over the same flat memory the emulator sees,
because a second implementation in Python would drift from the C one and the oracle would
then report its own drift as a translation bug - the very thing it exists to rule out. That
was not a hypothetical: once the kernel grew handles, config pages and RomFS, a stub that
only set r0 diverged from it at the first meaningful syscall.
"""
import argparse
import ctypes
import mmap
import pathlib
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_IMM

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_MODE_ARM, UC_PROT_ALL,
                     Uc, UcError)
from unicorn import arm_const as A
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_SP)

# r0-r14 spelled out. Unicorn's ARM constants are contiguous only for r0-r12: r13, r14 and
# r15 are 12, 10 and 11, so UC_ARM_REG_R0 + 13 silently reads an unrelated register.
GPR = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]

MEM_SIZE = 0x20000000
TEXT_BASE = 0x00100000
STACK_TOP = 0x10000000
TLS_BASE = 0x1FF82000
SENTINEL_LR = 0xDEAD0000


def preempt_spins(header='src/runtime/albw_cpu.h'):
    """The port's preemption threshold, read from the header rather than repeated here.

    Both runs have to give up the processor at the same instruction or the comparison reports
    a legitimate reordering as a translation bug, and two copies of a constant is exactly how
    that stops being true.
    """
    m = re.search(r'#define ALBW_PREEMPT_SPINS\s+(\d+)', pathlib.Path(header).read_text())
    if not m:
        raise RuntimeError(f'no ALBW_PREEMPT_SPINS in {header}')
    return int(m.group(1))


class AlbwCpu(ctypes.Structure):
    """Mirror of albw_cpu in src/runtime/albw_cpu.h."""
    class _Vfp(ctypes.Union):
        _fields_ = [('s', ctypes.c_float * 32), ('d', ctypes.c_double * 16),
                    ('u32', ctypes.c_uint32 * 32), ('u64', ctypes.c_uint64 * 16)]

    _fields_ = [('r', ctypes.c_uint32 * 16),
                ('nf', ctypes.c_uint32), ('zf', ctypes.c_uint32),
                ('cf', ctypes.c_uint32), ('vf', ctypes.c_uint32),
                ('v', _Vfp), ('fpscr', ctypes.c_uint32), ('tls', ctypes.c_uint32),
                ('spins', ctypes.c_uint32)]


# The scheduler calls this when a thread gives up the processor. Only the decision is shared
# with the port: it switches host stacks, while here the emulator's registers are swapped in
# the syscall hook once the kernel returns, so this only has to say who is next.
RESCHEDULE = ctypes.CFUNCTYPE(None)


class ThreadDriver(ctypes.Structure):
    _fields_ = [('reschedule', RESCHEDULE)]


class Oracle:
    def __init__(self, code, lib_path, rom_path, trace_path, cap, skip, verbose):
        self.cap, self.n, self.verbose = cap, 0, verbose
        # Record a window rather than the whole run, so a comparison deep in the boot path
        # does not cost a trace of everything before it.
        self.skip = skip
        self.recorded = 0
        self.stop_reason = 'cap reached'
        self.out = open(trace_path, 'wb')
        self.buf = bytearray()

        # One flat buffer, shared: the emulator maps it and the kernel writes into it, so
        # neither can observe memory the other cannot.
        self.mem = mmap.mmap(-1, MEM_SIZE)
        self.mem_ptr = ctypes.addressof(ctypes.c_char.from_buffer(self.mem))
        self.mem[TEXT_BASE:TEXT_BASE + len(code)] = code

        self.lib = ctypes.CDLL(lib_path)
        self.lib.albw_set_mem(ctypes.c_void_p(self.mem_ptr))
        # The kernel keeps the pointer, so the bytes have to outlive this call. Passing a
        # temporary left it dangling and RomFS silently failed to open - on one side only,
        # which is precisely the kind of asymmetry the shared kernel exists to prevent.
        self._rom_bytes = rom_path.encode() if rom_path else None
        self.lib.albw_kernel_set_rom(
            ctypes.c_char_p(self._rom_bytes) if self._rom_bytes else None)
        self.lib.albw_kernel_init()
        self.lib.albw_svc.argtypes = [ctypes.POINTER(AlbwCpu), ctypes.c_uint32]
        self.lib.albw_thread_cpu.restype = ctypes.POINTER(AlbwCpu)

        # Keep a reference: a garbage-collected callback would be a jump into freed memory.
        self._reschedule = RESCHEDULE(self.on_reschedule)
        self._driver = ThreadDriver(self._reschedule)
        self.lib.albw_thread_set_driver(ctypes.byref(self._driver))
        self.stuck = False

        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.mem_map_ptr(0, MEM_SIZE, UC_PROT_ALL, self.mem_ptr)
        self.enable_vfp()
        cpsr = self.uc.reg_read(UC_ARM_REG_CPSR)
        self.uc.reg_write(UC_ARM_REG_CPSR, cpsr & ~(0xF << 28))

        # Mirroring the port's preemption. It gives up the processor at a taken backward
        # branch, which is not something Unicorn reports, so the rule is reconstructed here
        # from the same question the recompiler asks: was the instruction we just left a direct
        # branch whose target is where we now are, and does it point backwards.
        self.lib.albw_preempt.argtypes = [ctypes.POINTER(AlbwCpu)]
        self.preempt_spins = preempt_spins()
        self._md = {0: Cs(CS_ARCH_ARM, CS_MODE_ARM), 1: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
        for md in self._md.values():
            md.detail = True
        self._branch = {}
        self._prev = None
        self.restart = False
        self.preempts = 0
        self._thumb = {}
        self._last_preempts = []
        self._resume = None
        self._tail = []

        self.uc.hook_add(UC_HOOK_CODE, self.on_code)
        self.uc.hook_add(UC_HOOK_INTR, self.on_intr)

    def branch_target(self, addr, thumb):
        """Where the direct branch at addr goes, or None if it is not one.

        Cached: the same loop edge is asked about thousands of times, and decoding is the
        expensive part of this hook.
        """
        key = (addr, thumb)
        if key not in self._branch:
            target = None
            try:
                data = bytes(self.uc.mem_read(addr, 4))
                for ins in self._md[thumb].disasm(data, addr):
                    mnem = ins.mnemonic.split('.')[0]
                    # Conditional forms keep the base 'b'; 'bl' and 'blx' are calls, and 'bx'
                    # has no immediate, so an immediate operand plus a 'b' base is the whole
                    # test -- the same one tools/recompile.py applies.
                    base = mnem
                    for suf in ('eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc',
                                'hi', 'ls', 'ge', 'lt', 'gt', 'le'):
                        if base.endswith(suf) and len(base) > len(suf):
                            base = base[:-len(suf)]
                            break
                    if base == 'b' and ins.operands and ins.operands[0].type == ARM_OP_IMM:
                        target = ins.operands[0].imm & 0xFFFFFFFF
                    break
            except UcError:
                pass
            self._branch[key] = target
        return self._branch[key]

    def start_main_thread(self, entry):
        """The entry point is a scheduled thread here too, so the two runs agree on who runs."""
        tid = self.lib.albw_thread_create(entry, 0, STACK_TOP, 0x30)
        if tid < 0:
            raise RuntimeError('could not create the main thread')
        self.lib.albw_thread_set_current(tid)
        self.load_thread(tid)

    def thread_cpu(self, tid):
        return self.lib.albw_thread_cpu(tid).contents

    def save_thread(self, tid):
        cpu = self.thread_cpu(tid)
        for i, r in enumerate(GPR):
            cpu.r[i] = self.uc.reg_read(r)
        cpsr = self.uc.reg_read(UC_ARM_REG_CPSR)
        cpu.nf = (cpsr >> 31) & 1
        cpu.zf = (cpsr >> 30) & 1
        cpu.cf = (cpsr >> 29) & 1
        cpu.vf = (cpsr >> 28) & 1
        # Which instruction set the thread was in. The port does not need this -- each
        # translated function knows its own -- so albw_cpu has no room for it, but restoring a
        # thread here means writing pc, and a Thumb address written without the T bit is
        # decoded as ARM and raises an invalid instruction at the first odd offset.
        self._thumb[tid] = (cpsr >> 5) & 1

    def load_thread(self, tid):
        cpu = self.thread_cpu(tid)
        for i, r in enumerate(GPR):
            self.uc.reg_write(r, cpu.r[i])
        cpsr = self.uc.reg_read(UC_ARM_REG_CPSR) & ~(0xF << 28) & ~(1 << 5)
        cpsr |= (cpu.nf << 31) | (cpu.zf << 30) | (cpu.cf << 29) | (cpu.vf << 28)
        cpsr |= self._thumb.get(tid, 0) << 5
        self.uc.reg_write(UC_ARM_REG_CPSR, cpsr)
        self.set_tls(cpu.tls)
        self.uc.reg_write(UC_ARM_REG_PC, self.lib.albw_thread_resume_pc(tid))

    def resume_thread(self, tid):
        """Put a thread back on the processor, in the instruction set it was in.

        Writing pc from a hook cannot change the instruction set -- only the address handed to
        emu_start can -- so every switch stops and restarts. Without this, a thread preempted in
        Thumb came back as ARM as soon as another thread had run, and died on the first
        two-byte instruction.
        """
        self.load_thread(tid)
        self._resume = self.lib.albw_thread_resume_pc(tid) | self._thumb.get(tid, 0)
        self._prev = None
        self.restart = True
        self.uc.emu_stop()

    def on_reschedule(self):
        nxt = self.lib.albw_thread_schedule()
        if nxt < 0:
            self.stuck = True
            return
        self.lib.albw_thread_set_current(nxt)

    def enable_vfp(self):
        """Unicorn starts with coprocessor access disabled, so VFP instructions raise
        UC_ERR_INSN_INVALID until cp10/cp11 are granted and FPEXC.EN is set."""
        from unicorn.arm_const import UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC
        cpacr = self.uc.reg_read(UC_ARM_REG_C1_C0_2)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, cpacr | (0xF << 20))
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    def set_tls(self, value):
        """TPIDRURO is what `mrc p15,0,Rd,c13,c0,3` reads."""
        try:
            from unicorn.arm_const import UC_ARM_REG_C13_C0_3
            self.uc.reg_write(UC_ARM_REG_C13_C0_3, value)
        except ImportError:
            print('warning: this unicorn build exposes no TPIDRURO register; '
                  'mrc reads will differ', file=sys.stderr)

    def on_code(self, uc, address, size, user):
        if address == SENTINEL_LR:
            self.stop_reason = 'returned to sentinel'
            uc.emu_stop()
            return

        self._tail.append((address, size, (uc.reg_read(UC_ARM_REG_CPSR) >> 5) & 1))
        del self._tail[:-8]
        prev, self._prev = self._prev, address
        if prev is not None and address <= prev:
            thumb = (uc.reg_read(UC_ARM_REG_CPSR) >> 5) & 1
            if self.branch_target(prev, thumb) == address:
                cur = self.lib.albw_thread_current()
                cpu = self.lib.albw_thread_cpu(cur)
                cpu.contents.spins += 1
                if cpu.contents.spins >= self.preempt_spins:
                    self.save_thread(cur)
                    self.lib.albw_thread_set_resume_pc(cur, address)
                    self.lib.albw_preempt(cpu)
                    if self.stuck:
                        self.stop_reason = 'scheduler has nothing runnable'
                        uc.emu_stop()
                        return
                    now = self.lib.albw_thread_current()
                    self.load_thread(now)
                    self._resume = (self.lib.albw_thread_resume_pc(now)
                                    | self._thumb.get(now, 0))
                    # Restarting rather than writing pc from this hook: stopping here is
                    # defined to leave the instruction unexecuted, so the resumed run picks it
                    # up exactly once.
                    self.preempts += 1
                    self._last_preempts.append((address, thumb, cur,
                                                self.lib.albw_thread_current()))
                    del self._last_preempts[:-5]
                    self._prev = None
                    self.restart = True
                    uc.emu_stop()
                    return

        rec = [address]
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        for r in GPR:
            rec.append(uc.reg_read(r))
        rec.append(((cpsr >> 31) & 1) << 3 | ((cpsr >> 30) & 1) << 2
                   | ((cpsr >> 29) & 1) << 1 | ((cpsr >> 28) & 1))
        self.n += 1
        if self.n > self.skip:
            self.buf += struct.pack('<17I', *rec)
            if len(self.buf) >= 1 << 20:
                self.out.write(self.buf)
                self.buf.clear()
            self.recorded += 1
            if self.recorded >= self.cap:
                uc.emu_stop()

    def on_intr(self, uc, intno, user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        imm = resume = None
        for cand in (pc, pc - 4):
            try:
                w = struct.unpack('<I', uc.mem_read(cand, 4))[0]
            except UcError:
                continue
            if (w & 0x0F000000) == 0x0F000000:      # SVC: cond + 1111 in bits 27..24
                imm, resume = w & 0xFFFFFF, cand + 4
                break
        if imm is None:
            self.stop_reason = f'interrupt {intno} at 0x{pc:08x} is not an svc'
            uc.emu_stop()
            return

        # The kernel is handed the thread's own state, not a copy, so that a switch it makes
        # is visible here afterwards.
        cur = self.lib.albw_thread_current()
        self.save_thread(cur)
        self.lib.albw_thread_set_resume_pc(cur, resume)

        self.lib.albw_svc(self.lib.albw_thread_cpu(cur), imm)

        if self.stuck:
            self.stop_reason = 'scheduler has nothing runnable'
            uc.emu_stop()
            return

        # resume_pc already says where to pick up, for this thread or whichever one the
        # scheduler chose, so both cases are the same.
        self.resume_thread(self.lib.albw_thread_current())

    def run(self):
        self.start_main_thread(TEXT_BASE)
        while True:
            self.restart = False
            # Unicorn takes the instruction set from the address emu_start is given: bit 0
            # set means Thumb, and writing the T bit in CPSR does not do it. The bit is carried
            # in self._resume rather than read back out of the emulator, because what a CPSR
            # read reports here is not something to rely on -- getting it wrong decoded Thumb as
            # ARM and died at the first odd offset.
            pc = self._resume if self._resume is not None else self.uc.reg_read(UC_ARM_REG_PC)
            self._resume = None
            try:
                self.uc.emu_start(pc, 0)
            except UcError as e:
                pc = self.uc.reg_read(UC_ARM_REG_PC)
                self.stop_reason = f'unicorn error at 0x{pc:08x}: {e}'
                break
            if not self.restart:
                break
        self.out.write(self.buf)
        self.out.close()
        print(f'oracle: {self.n} instructions, {self.recorded} recorded, '
              f'{self.preempts} preemption(s)')
        print(f'oracle: stopped because {self.stop_reason}')
        for a, sz, t in self._tail:
            print(f'oracle:   last 0x{a:08x} size {sz} {"thumb" if t else "arm"}')
        for a, t, was, now in self._last_preempts:
            print(f'oracle:   preempted at 0x{a:08x} '
                  f'({"thumb" if t else "arm"}), thread {was} -> {now}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='rom-extract/code.bin')
    ap.add_argument('--lib', default='build/libalbw-kernel.so')
    ap.add_argument('--rom', help='card image, for RomFS; also read from ALBW_ROM')
    ap.add_argument('--out', default='oracle-trace.bin')
    ap.add_argument('--cap', type=int, default=1000000)
    ap.add_argument('--skip', type=int, default=0,
                    help='instructions to execute before recording starts')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()

    import os
    rom = a.rom or os.environ.get('ALBW_ROM')
    Oracle(open(a.bin, 'rb').read(), a.lib, rom, a.out, a.cap, a.skip,
           a.verbose).run()


if __name__ == '__main__':
    main()

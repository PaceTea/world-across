// Backing for the kernel when it is loaded as a shared library by tools/oracle.py.
//
// The reference run must behave identically to the recompiled one, and the only way to
// guarantee that is to run the same code rather than a second implementation of it. A
// separate Python kernel diverges silently, and then the oracle reports its own drift as a
// translation bug - which is exactly the failure it exists to rule out.
//
// The executable provides albw_mem itself; this file supplies it for the library, where
// the caller points it at a buffer that is also mapped into the emulator.
#include "albw_cpu.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t *albw_mem;

void albw_set_mem(uint8_t *p) { albw_mem = p; }

// The reference run executes guest code inside the emulator, never through translated C
// functions, so nothing in this library should ever dispatch. It is referenced only because
// the thread trampoline enters a guest thread that way.
//
// Which is also the open problem: the scheduler's *state* is shareable between the two runs,
// but its context switch is not. The port swaps host stacks with ucontext; the reference run
// would have to save and restore the emulator's registers instead. Until that is separated
// out, the reference run cannot follow execution past the first guest thread that blocks -
// so say so loudly rather than corrupting a stack.
void albw_dispatch(albw_cpu *c, uint32_t target) {
    (void)c;
    fprintf(stderr, "[shim] the reference run cannot dispatch to 0x%08x: guest threads are "
                    "not mirrored yet\n", target);
    abort();
}

// The reference run counts instructions in its own driver, and writing that count into C on
// every instruction would cost more than the log line is worth, so its log positions read
// zero. Only the port's windows need aiming.
unsigned long albw_instruction_count(void) { return 0; }

void albw_missing_function(albw_cpu *c, uint32_t pc, uint32_t target) {
    (void)c;
    fprintf(stderr, "[shim] 0x%08x falls through to 0x%08x; the reference run drives "
                    "execution itself and cannot follow\n", pc, target);
    abort();
}

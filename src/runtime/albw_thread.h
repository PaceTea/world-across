#ifndef ALBW_THREAD_H
#define ALBW_THREAD_H

#include "albw_cpu.h"

// Cooperative guest threads.
//
// Switching happens only inside syscalls, never preemptively. That is a deliberate
// constraint rather than a simplification: the reference run compares a single stream of
// instructions, so both sides have to make the same scheduling decision at the same point.
// Preemption would make the interleaving depend on host timing and the comparison would
// report legitimate reordering as a translation bug.
//
// The scheduling *policy* is shared with the reference run; the *mechanism* cannot be. The
// port maps guest calls onto C calls, so a suspended thread's state lives partly in the C
// call stack and switching means swapping host stacks. The reference run executes inside an
// emulator, where everything is in guest memory and switching is a register swap. Hence the
// driver below: one decision, two ways of carrying it out. Without this split the reference
// run cannot follow execution past the first thread that blocks.
#define ALBW_MAX_THREADS 24

// The per-thread TLS block Horizon gives each thread; the IPC command buffer sits at +0x80.
#define ALBW_TLS_STRIDE 0x200u

typedef enum {
    ALBW_T_FREE = 0,
    ALBW_T_READY,
    ALBW_T_BLOCKED,
    ALBW_T_FINISHED,
} albw_thread_state;

// Why a blocked thread is blocked, so a wake can find the right sleepers.
typedef enum {
    ALBW_W_NONE = 0,
    ALBW_W_ADDRESS,         // svcArbitrateAddress on a guest address
    ALBW_W_SYNC,            // svcWaitSynchronization on a handle
    ALBW_W_SLEEP,           // svcSleepThread
} albw_wait_kind;

// Hands control to whichever thread should run next. The port's version does not return
// until the calling thread is scheduled again; the emulator's returns immediately, having
// only changed which thread is current, because the emulator itself continues from there.
typedef struct {
    void (*reschedule)(void);
} albw_thread_driver;

void albw_thread_set_driver(const albw_thread_driver *driver);

// Returns the new thread's id, or -1 when the table is full. The thread is runnable but
// does not start until the current one blocks or exits.
int albw_thread_create(uint32_t entry, uint32_t arg, uint32_t stack_top, int32_t priority);

albw_cpu *albw_thread_cpu(int id);
int albw_thread_current(void);
void albw_thread_set_current(int id);

// Where a thread picks up when it next runs. Only drivers that cannot keep a host call stack
// need this; the port's threads resume wherever their stack says.
uint32_t albw_thread_resume_pc(int id);
void albw_thread_set_resume_pc(int id, uint32_t pc);
uint32_t albw_thread_entry(int id);

// Chooses the next runnable thread, letting the kernel advance time if nothing can run.
// Returns -1 when the run is genuinely stuck.
int albw_thread_schedule(void);

// Gives up the processor. Returns once this thread is scheduled again.
void albw_thread_yield(void);

// Blocks until someone wakes this token. A blocked thread that nobody wakes is a deadlock,
// which the scheduler reports rather than hanging silently.
void albw_thread_block(albw_wait_kind kind, uint32_t token);

// Blocks until someone wakes any one of several tokens: WaitSynchronizationN waiting for any of its
// handles. The caller finds out which one fired by looking at the handles afterwards.
#define ALBW_WAIT_ANY_MAX 63
void albw_thread_block_any(albw_wait_kind kind, const uint32_t *tokens, int count);
int albw_thread_wake(albw_wait_kind kind, uint32_t token, int limit);

void albw_thread_exit(void);

// Wall time each thread has had the processor since the last call, in nanoseconds, its entry
// point and its priority; for ALBW_FRAME_TIMES. Clears the counts.
void albw_thread_take_times(uint64_t ns[ALBW_MAX_THREADS], uint32_t entry[ALBW_MAX_THREADS],
                            int32_t priority[ALBW_MAX_THREADS]);

// Horizon frees a thread when it has finished and no handle to it is left. Finished, the thread
// keeps its slot until albw_thread_release; detached, it gives the slot up as it finishes.
int albw_thread_finished(int id);
void albw_thread_release(int id);
void albw_thread_detach(int id);

// Prints what each thread is waiting for. The scheduler holds the wait tokens but only the
// kernel knows what they mean, so it formats the handles; this is the one view that says why
// a run is stuck rather than merely that it is.
void albw_thread_report_blocked(void);

// Creates the main thread at `entry` and runs it on host stacks until nothing is runnable.
// This is the port's driver; the reference run installs its own and drives the loop itself.
const char *albw_thread_run(uint32_t entry);

#endif

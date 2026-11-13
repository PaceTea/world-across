#include "albw_thread.h"
#include "albw_kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>

// Host stack per guest thread, for the ucontext driver. Generous because guest call depth
// becomes C call depth, and lazily committed, so the reservation costs nothing until a thread
// actually goes deep.
#define ALBW_HOST_STACK (8u * 1024u * 1024u)

typedef struct {
    albw_thread_state state;
    albw_wait_kind wait_kind;
    uint32_t wait_token;
    // Further tokens a wait for any of several handles also wakes on. Empty for an ordinary wait.
    uint32_t wait_more[ALBW_WAIT_ANY_MAX];
    int wait_more_count;
    int32_t priority;
    uint32_t entry;
    uint32_t resume_pc;
    albw_cpu cpu;
    ucontext_t ctx;         // only the ucontext driver uses these two
    void *stack;
    int prepared;
    int detached;           // no handle to it is left: its slot is free once it has finished
} albw_thread;

static albw_thread threads[ALBW_MAX_THREADS];
static int current = -1;
static const albw_thread_driver *driver;
static const char *stop_reason = "all threads finished";
static unsigned long switches;
static unsigned preempt_idle_checks;
static uint64_t ran_ns[ALBW_MAX_THREADS];      // wall time on the processor, for ALBW_FRAME_TIMES

static uint64_t clock_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void albw_thread_take_times(uint64_t ns[ALBW_MAX_THREADS], uint32_t entry[ALBW_MAX_THREADS],
                            int32_t priority[ALBW_MAX_THREADS]) {
    for (int i = 0; i < ALBW_MAX_THREADS; i++) {
        ns[i] = ran_ns[i];
        entry[i] = threads[i].entry;
        priority[i] = threads[i].priority;
        ran_ns[i] = 0;
    }
}

void albw_thread_set_driver(const albw_thread_driver *d) { driver = d; }

albw_cpu *albw_thread_cpu(int id) {
    return (id >= 0 && id < ALBW_MAX_THREADS) ? &threads[id].cpu : NULL;
}

int albw_thread_current(void) { return current; }
void albw_thread_set_current(int id) { current = id; }

uint32_t albw_thread_resume_pc(int id) {
    return (id >= 0 && id < ALBW_MAX_THREADS) ? threads[id].resume_pc : 0;
}
void albw_thread_set_resume_pc(int id, uint32_t pc) {
    if (id >= 0 && id < ALBW_MAX_THREADS) threads[id].resume_pc = pc;
}
uint32_t albw_thread_entry(int id) {
    return (id >= 0 && id < ALBW_MAX_THREADS) ? threads[id].entry : 0;
}

int albw_thread_create(uint32_t entry, uint32_t arg, uint32_t stack_top, int32_t priority) {
    for (int i = 0; i < ALBW_MAX_THREADS; i++) {
        if (threads[i].state != ALBW_T_FREE) continue;
        albw_thread *t = &threads[i];
        void *keep_stack = t->stack;                    // host stacks are reused
        memset(t, 0, sizeof *t);
        t->stack = keep_stack;
        t->state = ALBW_T_READY;
        t->priority = priority;
        t->entry = entry;
        t->resume_pc = entry;

        t->cpu.r[0] = arg;
        t->cpu.r[ALBW_SP] = stack_top;
        t->cpu.r[ALBW_LR] = 0;
        t->cpu.tls = ALBW_TLS_BASE + (uint32_t)i * ALBW_TLS_STRIDE;
        return i;
    }
    fprintf(stderr, "[thread] table full\n");
    return -1;
}

void albw_thread_yield(void) {
    if (current < 0 || !driver) return;
    switches++;
    driver->reschedule();
}

void albw_thread_block(albw_wait_kind kind, uint32_t token) {
    albw_thread_block_any(kind, &token, 1);
}

void albw_thread_block_any(albw_wait_kind kind, const uint32_t *tokens, int count) {
    if (current < 0 || !driver || count < 1) return;
    if (count > ALBW_WAIT_ANY_MAX + 1) count = ALBW_WAIT_ANY_MAX + 1;
    threads[current].state = ALBW_T_BLOCKED;
    threads[current].wait_kind = kind;
    threads[current].wait_token = tokens[0];
    threads[current].wait_more_count = count - 1;
    for (int i = 1; i < count; i++) threads[current].wait_more[i - 1] = tokens[i];
    switches++;
    driver->reschedule();
}

static int waits_on(const albw_thread *t, uint32_t token) {
    if (t->wait_token == token) return 1;
    for (int i = 0; i < t->wait_more_count; i++) {
        if (t->wait_more[i] == token) return 1;
    }
    return 0;
}

int albw_thread_wake(albw_wait_kind kind, uint32_t token, int limit) {
    int woken = 0;
    for (int i = 0; i < ALBW_MAX_THREADS && woken != limit; i++) {
        if (threads[i].state != ALBW_T_BLOCKED) continue;
        if (threads[i].wait_kind != kind || !waits_on(&threads[i], token)) continue;
        threads[i].state = ALBW_T_READY;
        threads[i].wait_kind = ALBW_W_NONE;
        threads[i].wait_more_count = 0;
        woken++;
    }
    return woken;
}

int albw_thread_finished(int id) {
    return id >= 0 && id < ALBW_MAX_THREADS && threads[id].state == ALBW_T_FINISHED;
}

void albw_thread_release(int id) {
    if (albw_thread_finished(id)) threads[id].state = ALBW_T_FREE;
}

void albw_thread_detach(int id) {
    if (id < 0 || id >= ALBW_MAX_THREADS || threads[id].state == ALBW_T_FREE) return;
    if (threads[id].state == ALBW_T_FINISHED) threads[id].state = ALBW_T_FREE;
    else threads[id].detached = 1;
}

void albw_thread_exit(void) {
    if (current < 0 || !driver) return;
    // A free slot is not run again; creating a thread in it later prepares a context afresh, on
    // the host stack this one leaves behind.
    threads[current].state = threads[current].detached ? ALBW_T_FREE : ALBW_T_FINISHED;
    switches++;
    driver->reschedule();
}

// Round robin from the thread after the current one, so a thread that yields does not
// immediately get the processor back. Priority is recorded but not honoured: Horizon's
// priorities matter for timing, and this scheduler has no time to allocate.
static int pick_next(void) {
    int from = current < 0 ? 0 : current;
    for (int n = 1; n <= ALBW_MAX_THREADS; n++) {
        int i = (from + n) % ALBW_MAX_THREADS;
        if (threads[i].state == ALBW_T_READY) return i;
    }
    return -1;
}

// Whether giving up the processor could achieve anything: another thread that is ready to run.
static int others_ready(void) {
    for (int i = 0; i < ALBW_MAX_THREADS; i++) {
        if (i != current && threads[i].state == ALBW_T_READY) return 1;
    }
    return 0;
}

void albw_preempt(albw_cpu *c) {
    c->spins = 0;
    if (others_ready()) {
        albw_thread_yield();
        return;
    }
    // Nothing else can run, so only a device can change this thread's mind. A thread spinning
    // on a flag no runnable thread will clear is idle in every sense that matters here, even
    // though the scheduler cannot see it that way -- so let time advance. Rationed, because a
    // long computation is also a lot of backward branches and should not count as frames.
    if (++preempt_idle_checks >= 32) {
        preempt_idle_checks = 0;
        if (albw_kernel_on_idle() && others_ready()) albw_thread_yield();
    }
}

int albw_thread_schedule(void) {
    int next = pick_next();
    if (next < 0 && albw_kernel_on_idle()) {
        // Nothing could run, so the kernel advanced time; somebody may be awake now.
        next = pick_next();
    }
    if (next < 0) {
        int blocked = 0, finished = 0;
        for (int i = 0; i < ALBW_MAX_THREADS; i++) {
            if (threads[i].state == ALBW_T_BLOCKED) blocked++;
            if (threads[i].state == ALBW_T_FINISHED) finished++;
        }
        if (blocked) {
            static char msg[320];
            const char *note = albw_kernel_stop_note();
            if (note) {
                snprintf(msg, sizeof msg, "%s. %d thread(s) waiting, after %lu switches",
                         note, blocked, switches);
            } else {
                snprintf(msg, sizeof msg,
                         "deadlock: %d thread(s) blocked with nothing runnable, "
                         "%d finished, after %lu switches", blocked, finished, switches);
            }
            stop_reason = msg;
        }
    }
    return next;
}

void albw_thread_report_blocked(void) {
    static const char *kinds[] = { "-", "address", "handle", "sleep" };
    for (int i = 0; i < ALBW_MAX_THREADS; i++) {
        if (threads[i].state == ALBW_T_FREE) continue;
        const char *st = threads[i].state == ALBW_T_READY ? "ready"
                       : threads[i].state == ALBW_T_BLOCKED ? "blocked" : "finished";
        printf("[thread] %d: %-8s entry=%08x", i, st, threads[i].entry);
        if (threads[i].state == ALBW_T_BLOCKED) {
            printf(" waiting on %s 0x%08x",
                   kinds[threads[i].wait_kind], threads[i].wait_token);
            if (threads[i].wait_kind == ALBW_W_SYNC) {
                char what[64];
                albw_kernel_describe_handle(threads[i].wait_token, what, sizeof what);
                printf(" (%s)", what);
            }
        }
        printf("\n");
    }
}

// ----------------------------------------- the port's driver: host stacks -----

static ucontext_t scheduler_ctx;
static int scheduler_ready;

// Guest threads return here when their entry function returns, which Horizon treats as
// svcExitThread.
static void thread_trampoline(int id) {
    albw_dispatch(&threads[id].cpu, threads[id].entry);
    // A thread whose function returns ends as one that calls ExitThread, so that the kernel can
    // signal its handle.
    albw_svc(&threads[id].cpu, 0x09);
    albw_thread_exit();
}

static void ucontext_prepare(int id) {
    albw_thread *t = &threads[id];
    if (!t->stack) {
        t->stack = mmap(NULL, ALBW_HOST_STACK, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (t->stack == MAP_FAILED) {
            perror("thread stack");
            t->stack = NULL;
            t->state = ALBW_T_FREE;
            return;
        }
    }
    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = ALBW_HOST_STACK;
    t->ctx.uc_link = &scheduler_ctx;
    makecontext(&t->ctx, (void (*)(void))thread_trampoline, 1, id);
    t->prepared = 1;
}

static void ucontext_reschedule(void) {
    if (!scheduler_ready) {
        fprintf(stderr, "[thread] a guest thread tried to switch with no scheduler "
                        "running; this build drives execution some other way\n");
        abort();
    }
    swapcontext(&threads[current].ctx, &scheduler_ctx);
}

static const albw_thread_driver ucontext_driver = { ucontext_reschedule };

const char *albw_thread_run(uint32_t entry) {
    albw_thread_set_driver(&ucontext_driver);

    int main_id = albw_thread_create(entry, 0, ALBW_MAIN_STACK_TOP, 0x30);
    if (main_id < 0) return "could not create the main thread";
    scheduler_ready = 1;

    for (;;) {
        int next = albw_thread_schedule();
        if (next < 0) return stop_reason;
        // Prepared on first run, not at creation: rewinding a context would throw away the
        // stack of a thread that is only suspended.
        if (!threads[next].prepared) {
            ucontext_prepare(next);
            if (threads[next].state == ALBW_T_FREE) return "could not start a thread";
        }
        current = next;
        uint64_t t0 = clock_ns();
        swapcontext(&scheduler_ctx, &threads[next].ctx);
        ran_ns[current] += clock_ns() - t0;
        if (threads[current].state == ALBW_T_READY && threads[current].ctx.uc_link == NULL) {
            threads[current].state = ALBW_T_FINISHED;
        }
    }
}

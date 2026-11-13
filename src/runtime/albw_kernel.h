#ifndef ALBW_KERNEL_H
#define ALBW_KERNEL_H

#include "albw_cpu.h"

// Horizon's per-thread TLS block. `mrc p15,0,Rd,c13,c0,3` reads its address, and every
// service call marshals through the command buffer at offset 0x80 - confirmed from the
// game's own SendSyncRequest wrapper, which does `str r2, [r4, #0x80]!`.
#define ALBW_TLS_BASE   0x1FF82000u

// Where the loader puts the main thread's stack.
#define ALBW_MAIN_STACK_TOP 0x10000000u

// Kernel-provided handles that refer to the caller itself.
#define ALBW_HANDLE_CURRENT_PROCESS 0xFFFF8000u
#define ALBW_HANDLE_CURRENT_THREAD  0xFFFF8001u

// Where svcControlMemory hands out pages when the guest does not name an address. Linear
// memory is a separate region because the guest asks for it with MEMOP_LINEAR_FLAG and
// expects contiguous addresses it can hand to the GPU.
#define ALBW_HEAP_BASE    0x08000000u
#define ALBW_HEAP_LIMIT   0x0C000000u
#define ALBW_LINEAR_BASE  0x14000000u
#define ALBW_LINEAR_LIMIT 0x18000000u

// Read-only pages the kernel publishes. The guest reads its total memory allowance from
// the kernel one, and reporting zero there made it compute a negative allocation size.
#define ALBW_KERNEL_CFG 0x1FF80000u
#define ALBW_SHARED_CFG 0x1FF81000u
#define ALBW_APP_MEMORY (64u * 1024u * 1024u)

// The calling thread's IPC command buffer. Per thread, not global: each thread has
// its own TLS block, so two threads can have requests in flight at once.
static inline uint32_t albw_cmdbuf(const albw_cpu *c) { return c->tls + 0x80u; }

// Guest addresses reaching the kernel come from the guest's own registers, so a
// mistranslation or a wrong ABI guess turns into a host segfault at an unhelpful place.
// Range-check them here; the generated code does not need this, because whether the guest
// computes sane addresses is what the reference run is for.
#define ALBW_GUEST_SIZE 0x20000000u

// Past the guest's memory the host keeps a second copy of the linear heap and VRAM: guest addresses
// 0x14000000 to 0x20000000, mirrored from 0x20000000 up. Frames drawn between the game's own go
// there, render targets, framebuffers and all, so they never touch anything the game can see; the
// bounds check above keeps the game out of it.
#define ALBW_SHADOW_FROM 0x14000000u
#define ALBW_SHADOW_BASE ALBW_GUEST_SIZE
#define ALBW_SHADOW_SIZE (ALBW_GUEST_SIZE - ALBW_SHADOW_FROM)
static inline int albw_shadowed(uint32_t addr) { return addr >= ALBW_SHADOW_FROM && addr < ALBW_GUEST_SIZE; }
static inline uint32_t albw_shadow(uint32_t addr) { return addr - ALBW_SHADOW_FROM + ALBW_SHADOW_BASE; }
static inline int albw_guest_ok(uint32_t addr, uint32_t len) {
    return len <= ALBW_GUEST_SIZE && addr <= ALBW_GUEST_SIZE - len;
}

// Called when no thread can run. Returns non-zero if it made something runnable.
//
// This is where time passes. The GPU raises an interrupt once per frame and the guest's GSP
// thread waits for it, so something has to play the part of that clock - and tying it to
// "nothing else can run" keeps the whole port deterministic, which is what lets the
// reference run compare against it at all. A wall clock would not.
int albw_kernel_on_idle(void);

// Names a handle for diagnostics: its kind, whatever label it was created with, and whether
// it is signalled. The scheduler holds the wait tokens but only the kernel knows what they
// mean, and "waiting on 0x100c" is not an answer to why a run is stuck.
void albw_kernel_describe_handle(uint32_t handle, char *out, unsigned long n);

void albw_kernel_init(void);
// The card image the guest's RomFS comes from; opened on first file access.
void albw_kernel_set_rom(const char *path);
unsigned long albw_svc_count(void);
unsigned long albw_frame_count(void);          // vblanks so far

// getenv, cached: the first answer for a name is kept for the rest of the run.
const char *albw_env(const char *name);

// The frame the guest is on, counted by the GPU interrupts it waits for.
unsigned long albw_kernel_frame(void);

// The controls as of this frame, for the HID service to hand the game. Copied.
struct albw_input;
void albw_kernel_set_input(const struct albw_input *in);

// Called once a frame with the screens ready to show; returning 0 ends the run.
void albw_kernel_set_frame_hook(int (*hook)(uint32_t shared_addr, uint32_t thread_index,
                                            unsigned long frame));

// Why the kernel stopped advancing time, or NULL. A guest that is running normally and simply
// has nothing new to do looks to the scheduler exactly like a deadlocked one, and calling it a
// deadlock in the last line of the run is misleading.
const char *albw_kernel_stop_note(void);

#endif

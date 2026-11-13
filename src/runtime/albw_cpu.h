#ifndef ALBW_CPU_H
#define ALBW_CPU_H

#include <stdint.h>
#include <string.h>

// Guest state for recompiled ARM11 code. Flags live in separate words rather than a
// packed CPSR because only 11% of instructions write them and 14% read them via
// predication, so the packing would cost more than it saves.
typedef struct {
    uint32_t r[16];
    uint32_t nf, zf, cf, vf;

    // VFPv2: s[2i] and s[2i+1] alias d[i]. Little-endian host and guest agree, so a
    // union is the whole of the aliasing rule.
    union {
        float s[32];
        double d[16];
        uint32_t u32[32];
        uint64_t u64[16];
    } v;
    uint32_t fpscr;

    // TPIDRURO, read by `mrc p15,0,Rd,c13,c0,3`. Points at this thread's TLS block,
    // whose 0x80 offset holds the IPC command buffer every service call goes through.
    uint32_t tls;

    // Backward branches taken since this thread last considered giving up the processor. See
    // albw_preempt below for why a count and not a clock.
    uint32_t spins;
} albw_cpu;

#define ALBW_SP 13
#define ALBW_LR 14
#define ALBW_PC 15

// Guest physical memory, indexed directly by guest address.
extern uint8_t *albw_mem;

static inline uint32_t albw_r32(uint32_t a) {
    uint32_t v;
    memcpy(&v, albw_mem + a, 4);
    return v;
}
static inline uint16_t albw_r16(uint32_t a) {
    uint16_t v;
    memcpy(&v, albw_mem + a, 2);
    return v;
}
static inline uint8_t albw_r8(uint32_t a) { return albw_mem[a]; }

// Where the guest has been writing, in a build configured for it.
//
// There to answer a question guessing could not: a buffer the GPU reads was written by the processor
// shortly before the draw that reads it, so the regions written since the last draw single it out.
// Every guest store goes through these three functions, so this is the one place it can be recorded.
// Compiled out otherwise -- a call per store is fine for a diagnostic and not for a run.
#ifdef ALBW_TRACK_WRITES
void albw_note_write(uint32_t addr, uint32_t len);
#define ALBW_NOTE_WRITE(a, n) albw_note_write((a), (n))
#else
#define ALBW_NOTE_WRITE(a, n) ((void)0)
#endif

// The value of a stored word, not only the fact of the store. Compiled in only for the
// albw-boot-nan target: the game uploads a projection matrix made of not-a-number, so the question
// is which of its own stores first produces one, and the generated functions are named after their
// guest addresses -- so a C backtrace at that store names the guest function outright.
//
// Narrower stores are watched too, for the address watch only: a struct the game fills with a byte
// copy would otherwise look as though nothing ever wrote it, which is exactly the claim being tested.
#ifdef ALBW_WATCH_NAN
void albw_note_stored_word(uint32_t addr, uint32_t value);
void albw_note_stored_narrow(uint32_t addr, uint32_t value, uint32_t len);
#define ALBW_NOTE_WORD(a, v) albw_note_stored_word((a), (v))
#define ALBW_NOTE_NARROW(a, v, n) albw_note_stored_narrow((a), (v), (n))
#else
#define ALBW_NOTE_WORD(a, v) ((void)0)
#define ALBW_NOTE_NARROW(a, v, n) ((void)0)
#endif

static inline void albw_w32(uint32_t a, uint32_t v) {
    ALBW_NOTE_WRITE(a, 4);
    ALBW_NOTE_WORD(a, v);
    memcpy(albw_mem + a, &v, 4);
}
// A float or a double store. These used to write guest memory with a bare memcpy, so the write record
// never saw them -- while its own comment above claimed every guest store went through it. Routing
// them here fixes the record and the claim at once, and is what let the not-a-number watch find
// anything at all.
static inline void albw_wf32(uint32_t a, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    albw_w32(a, bits);
}
static inline void albw_wf64(uint32_t a, double v) {
    uint32_t bits[2];
    memcpy(bits, &v, 8);
    albw_w32(a, bits[0]);
    albw_w32(a + 4, bits[1]);
}

static inline void albw_w16(uint32_t a, uint16_t v) {
    ALBW_NOTE_WRITE(a, 2);
    ALBW_NOTE_NARROW(a, v, 2);
    memcpy(albw_mem + a, &v, 2);
}
static inline void albw_w8(uint32_t a, uint8_t v) {
    ALBW_NOTE_WRITE(a, 1);
    ALBW_NOTE_NARROW(a, v, 1);
    albw_mem[a] = v;
}

// Condition codes. Each reads only the flags it needs.
#define ALBW_EQ(c) ((c)->zf)
#define ALBW_NE(c) (!(c)->zf)
#define ALBW_CS(c) ((c)->cf)
#define ALBW_CC(c) (!(c)->cf)
#define ALBW_MI(c) ((c)->nf)
#define ALBW_PL(c) (!(c)->nf)
#define ALBW_VS(c) ((c)->vf)
#define ALBW_VC(c) (!(c)->vf)
#define ALBW_HI(c) ((c)->cf && !(c)->zf)
#define ALBW_LS(c) (!(c)->cf || (c)->zf)
#define ALBW_GE(c) ((c)->nf == (c)->vf)
#define ALBW_LT(c) ((c)->nf != (c)->vf)
#define ALBW_GT(c) (!(c)->zf && (c)->nf == (c)->vf)
#define ALBW_LE(c) ((c)->zf || (c)->nf != (c)->vf)

// Flag updates. Carry and overflow differ per operation, so they are set by the
// emitted code; these only cover the common N/Z case.
static inline void albw_nz(albw_cpu *c, uint32_t res) {
    c->nf = res >> 31;
    c->zf = (res == 0);
}
static inline void albw_add_flags(albw_cpu *c, uint32_t a, uint32_t b, uint32_t res) {
    albw_nz(c, res);
    c->cf = (res < a);
    c->vf = (~(a ^ b) & (a ^ res)) >> 31;
}
static inline void albw_sub_flags(albw_cpu *c, uint32_t a, uint32_t b, uint32_t res) {
    albw_nz(c, res);
    c->cf = (a >= b);
    c->vf = ((a ^ b) & (a ^ res)) >> 31;
}

// Shifts return the shifter carry-out alongside the value, because ARM feeds it into the C
// flag for data-processing instructions with the S bit set.
//
// The carry-in is passed because a shift by zero leaves the carry *alone*, and that is not the
// same as leaving the out-parameter alone: the emitted code assigns it unconditionally, so an
// untouched variable hands over whatever the previous shift in that function left there.
// `tst r1, r3, lsl r2` with r2 zero set the carry from an unrelated instruction.
static inline uint32_t albw_lsl_c(uint32_t v, uint32_t n, uint32_t cin, uint32_t *co) {
    *co = cin;
    if (n == 0) return v;
    if (n < 32) { *co = (v >> (32 - n)) & 1; return v << n; }
    if (n == 32) { *co = v & 1; return 0; }
    *co = 0;
    return 0;
}
static inline uint32_t albw_lsr_c(uint32_t v, uint32_t n, uint32_t cin, uint32_t *co) {
    *co = cin;
    if (n == 0) return v;
    if (n < 32) { *co = (v >> (n - 1)) & 1; return v >> n; }
    if (n == 32) { *co = v >> 31; return 0; }
    *co = 0;
    return 0;
}
static inline uint32_t albw_asr_c(uint32_t v, uint32_t n, uint32_t cin, uint32_t *co) {
    *co = cin;
    if (n == 0) return v;
    if (n < 32) { *co = (v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
    *co = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t albw_ror_c(uint32_t v, uint32_t n, uint32_t cin, uint32_t *co) {
    *co = cin;
    if (n == 0) return v;
    uint32_t m = n & 31;
    if (m == 0) { *co = v >> 31; return v; }            // a rotation by a multiple of 32
    *co = (v >> (m - 1)) & 1;
    return (v >> m) | (v << (32 - m));
}

// The two signed halves of a word, for the halfword multiplies. Named after the suffixes the
// mnemonics use: B is bits 15:0, T is bits 31:16.
static inline int32_t albw_sbot(uint32_t v) { return (int16_t)(uint16_t)(v & 0xFFFFu); }
static inline int32_t albw_stop(uint32_t v) { return (int16_t)(uint16_t)(v >> 16); }

// Saturating a signed value into a range. The Q flag these set on saturation lives in APSR and
// nothing in this binary reads it.
static inline uint32_t albw_usat(int32_t v, uint32_t bits) {
    if (v < 0) return 0;
    if (bits >= 31) return (uint32_t)v;                 // the whole positive range fits
    int32_t hi = (int32_t)((1u << bits) - 1u);
    return (uint32_t)(v > hi ? hi : v);
}
static inline uint32_t albw_ssat(int32_t v, uint32_t bits) {
    if (bits >= 32) return (uint32_t)v;
    int32_t hi = (int32_t)((1u << (bits - 1)) - 1u);
    int32_t lo = -hi - 1;
    return (uint32_t)(v > hi ? hi : (v < lo ? lo : v));
}
// VMSR to FPSCR: the guest's floating-point mode becomes the host's. The game sets 0x03000000 at
// the start of every thread (0x00106314, called from the startup and from the thread entry at
// 0x0010994c): flush to zero, default NaN, round to nearest. Flush to zero is what matters -- a
// product too small for a normal float is 0 on the console and a denormal here otherwise, and a
// denormal operand counts as 0 there. SSE's FTZ and DAZ together are that mode, and the rounding
// field maps over directly. (Default NaN has no SSE equivalent: a NaN keeps its payload here, as
// the checker in tools/insn_check.py allows for.)
//
// The guest threads are fibers on one host thread, and glibc's swapcontext saves and restores
// MXCSR with the rest of a context, so each thread keeps the mode it set. The runtime's own code
// that runs inside a guest thread -- the services, the vertex shader -- runs in it as well; the
// rasteriser's workers are threads of their own and keep the host's default.
static inline void albw_set_fpscr(albw_cpu *c, uint32_t v) {
    c->fpscr = v;
#if defined(__SSE__)
    // ARM's rounding field is nearest, +inf, -inf, zero; SSE's (bits 14:13) nearest, -inf, +inf, zero.
    static const unsigned rc[4] = { 0x0000u, 0x4000u, 0x2000u, 0x6000u };
    unsigned csr = __builtin_ia32_stmxcsr() & ~0xE040u;  // FTZ (15), RC (14:13), DAZ (6)
    if (v & (1u << 24))
        csr |= 0x8040u;
    __builtin_ia32_ldmxcsr(csr | rc[(v >> 22) & 3u]);
#elif defined(__aarch64__)
    // FPCR keeps DN, FZ and the rounding field where FPSCR does.
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (fpcr & ~(uint64_t)0x03C00000u) | (v & 0x03C00000u);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

// VCVT from floating point to an integer truncates toward zero, like a C cast, but where the cast
// is undefined the instruction is not: a NaN gives 0 and a value past either end of the range gives
// that end. x86 answers 0x80000000 for all of those in the signed case, and wraps negative values
// in the unsigned one. A float widens to double exactly, so one pair serves both precisions. The
// invalid-operation flag the instruction sets in FPSCR is not kept; nothing in this binary reads it.
static inline uint32_t albw_vcvt_s32(double v) {
    if (v != v) return 0;
    if (v >= 2147483648.0) return 0x7FFFFFFFu;
    if (v <= -2147483649.0) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t albw_vcvt_u32(double v) {
    if (!(v > -1.0)) return 0;                          // a NaN, or anything truncating below zero
    if (v >= 4294967296.0) return 0xFFFFFFFFu;
    return (uint32_t)v;
}

// The 16-bit forms saturate each half independently.
static inline uint32_t albw_usat16(uint32_t v, uint32_t bits) {
    return albw_usat(albw_sbot(v), bits) | (albw_usat(albw_stop(v), bits) << 16);
}
static inline uint32_t albw_ssat16(uint32_t v, uint32_t bits) {
    return (albw_ssat(albw_sbot(v), bits) & 0xFFFFu)
         | ((albw_ssat(albw_stop(v), bits) & 0xFFFFu) << 16);
}

// Halfword and byte wise add and subtract, each lane independent and wrapping. The signed and
// unsigned forms produce the same bits and differ only in the GE flags, which live in APSR and
// nothing in this binary reads, so one helper serves both.
static inline uint32_t albw_add16(uint32_t a, uint32_t b) {
    return (((a & 0xFFFFu) + (b & 0xFFFFu)) & 0xFFFFu)
         | ((((a >> 16) + (b >> 16)) & 0xFFFFu) << 16);
}
static inline uint32_t albw_sub16(uint32_t a, uint32_t b) {
    return (((a & 0xFFFFu) - (b & 0xFFFFu)) & 0xFFFFu)
         | ((((a >> 16) - (b >> 16)) & 0xFFFFu) << 16);
}
static inline uint32_t albw_add8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        r |= (((a >> (i * 8)) + (b >> (i * 8))) & 0xFFu) << (i * 8);
    }
    return r;
}
static inline uint32_t albw_sub8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        r |= (((a >> (i * 8)) - (b >> (i * 8))) & 0xFFu) << (i * 8);
    }
    return r;
}

// Bytewise unsigned saturating subtract. Does not touch NZCV; the saturation shows up in
// the GE bits of APSR, which nothing in this binary reads.
static inline uint32_t albw_uqsub8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t x = (a >> (i * 8)) & 0xFF, y = (b >> (i * 8)) & 0xFF;
        r |= (x > y ? x - y : 0u) << (i * 8);
    }
    return r;
}

// Rotate right through carry by one: the incoming carry becomes bit 31, bit 0 becomes the new
// carry. The caller passes the carry in because the shifter reads it.
static inline uint32_t albw_rrx(uint32_t v, uint32_t carry_in, uint32_t *co) {
    *co = v & 1u;
    return (v >> 1) | (carry_in << 31);
}

// Every recompiled guest function has this shape; lr is set by the caller.
typedef void (*albw_fn)(albw_cpu *);

// Indirect calls and jumps resolve here. ~80% of ALBW's computed calls are virtual and
// cannot be resolved statically, so the table is the mechanism rather than a fallback.
void albw_dispatch(albw_cpu *c, uint32_t target);

// Raised by translated code for anything not yet implemented, so the first gap aborts
// loudly instead of silently computing the wrong thing.
void albw_unimplemented(albw_cpu *c, uint32_t pc, const char *what);

// A body that falls through into an address that is not the start of any translated function.
// Separate from albw_unimplemented because the target is a discoverable entry point rather
// than a hole in the instruction coverage.
void albw_missing_function(albw_cpu *c, uint32_t pc, uint32_t target);

// Called from a backward branch once `spins` reaches the threshold, which is the only place
// a thread can be taken off the processor without entering the kernel.
//
// It is needed because the guest busy-waits. It submits a command list and then spins on a
// flag its own interrupt handler clears (0x00504ff0), with nothing in the loop that enters the
// kernel; on hardware the scheduler preempts it, and a purely cooperative one never can.
//
// The threshold counts backward branches rather than elapsed time so that the decision is
// reproducible: the reference run has to reach the same conclusion at the same instruction, or
// it reports a legitimate reordering as a translation bug. Unicorn already hooks every
// instruction, so it can see the same branches; that mirror is not written yet, which is why a
// comparison has to stay inside a window with no preemption in it.
#define ALBW_PREEMPT_SPINS 2048u
void albw_preempt(albw_cpu *c);

#define ALBW_MAYBE_PREEMPT(c) \
    do { if (++(c)->spins >= ALBW_PREEMPT_SPINS) albw_preempt(c); } while (0)

void albw_svc(albw_cpu *c, uint32_t imm);

// Instructions executed so far, or 0 in a build without tracing. Lets a log line say *when*
// it happened, which is what makes a windowed comparison possible to aim.
unsigned long albw_instruction_count(void);

// Per-instruction trace, for diffing against the Unicorn reference run. Compiled out
// entirely unless ALBW_TRACE is defined, so the normal build carries no cost and both
// builds come from the same generated source.
#ifdef ALBW_TRACE
void albw_trace(albw_cpu *c, uint32_t pc);
#define ALBW_TRACE_PC(c, pc) albw_trace((c), (pc))
#else
#define ALBW_TRACE_PC(c, pc) ((void)0)
#endif

#endif

// The profiler below reads the interrupted instruction pointer out of the signal context and asks
// the dynamic linker where the executable was loaded; both are GNU extensions.
#define _GNU_SOURCE

#include "albw_cpu.h"
#include "albw_generated.h"
#include "albw_raster.h"
#ifdef ALBW_HAVE_WINDOW
#include "albw_window.h"
#include "albw_launcher.h"
#endif
// The dispatch table's length lives in its own header, so that adding a function changes only the
// dispatcher and the table and not the header every generated file includes. Guarded so that output
// from the generator before that change still builds.
#if !defined(ALBW_FN_COUNT) && __has_include("albw_fn_count.h")
#include "albw_fn_count.h"
#endif
#include "albw_dsp.h"
#include "albw_kernel.h"
#include "albw_savedata.h"
#include "albw_thread.h"

#include "albw_platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Guest address space. Sparse and lazily committed, so reserving the whole range costs
// nothing but makes every guest address a plain index.
#define ALBW_MEM_SIZE 0x20000000u

#define ALBW_TEXT_BASE 0x00100000u
// How far .text reaches, for telling a code pointer from an arbitrary word.
#define ALBW_TEXT_LIMIT 0x00700000u
#define ALBW_BSS_BASE  0x00728000u
#define ALBW_BSS_SIZE  0x00087A44u

// Returning here means the guest ran off the end of the boot path.
#define ALBW_SENTINEL_LR 0xDEAD0000u

uint8_t *albw_mem;

// Discovery mode. About 12% of .text is reachable only indirectly, and no static
// heuristic separates those entry points from data reliably: measured against a random
// control, prologue shape scores 7.9% versus 4.9%, which is noise. But an address the
// guest actually branches to is an entry point by construction.
//
// Only the FIRST miss of a run is recorded, and then the run ends. Skipping a call and
// carrying on collects more addresses per run, but everything after that point executed
// with a call silently omitted, so those addresses are whatever the derailed registers
// happened to hold - one such invention, 0x00100AB9, decoded as Thumb and would have
// become a fabricated function in a file that feeds every later build. Up to the first
// miss the run is faithful, so one trustworthy address per round it is.
static FILE *discover_fp;

static int plausible_text(uint32_t w) {
    uint32_t a = w & ~1u;
    return a >= ALBW_TEXT_BASE && a < ALBW_TEXT_BASE + ALBW_TEXT_LIMIT && (a & 1) == 0;
}

// Having found the target in a table, record the rest of that table too.
//
// This is not the pointer-run guess that scanning the image would be. The guest just
// dispatched through one entry of this run, which is what makes it a live function-pointer
// table rather than something that merely looks like one, and the neighbours of a function
// pointer in such a table are function pointers. It matters because the alternative is one
// address per round, and a round costs a full retranslation: the vtables that turn up here
// hold single-instruction bodies Ghidra never made functions of, eight at a time.
//
// Anything that does not decode as code is dropped later, when the recompiler tries to bound
// a body for it.
static unsigned record_table_siblings(uint32_t target) {
    unsigned written = 0, tables = 0;
    for (uint32_t a = 0x1000; a + 4 <= ALBW_MEM_SIZE && tables < 4; a += 4) {
        uint32_t v;
        memcpy(&v, albw_mem + a, 4);
        if (v != target) continue;

        uint32_t lo = a, hi = a;
        while (lo >= 0x1004) {
            uint32_t w;
            memcpy(&w, albw_mem + lo - 4, 4);
            if (!plausible_text(w)) break;
            lo -= 4;
        }
        while (hi + 8 <= ALBW_MEM_SIZE) {
            uint32_t w;
            memcpy(&w, albw_mem + hi + 4, 4);
            if (!plausible_text(w)) break;
            hi += 4;
        }
        if (hi - lo < 12) continue;                     // too short to be a table
        tables++;
        printf("[discover] 0x%08x sits in a %u-entry table at 0x%08x\n",
               target, (hi - lo) / 4 + 1, lo);
        for (uint32_t e = lo; e <= hi; e += 4) {
            uint32_t w;
            memcpy(&w, albw_mem + e, 4);
            if (w == target) continue;
            fprintf(discover_fp, "%08X\n", w);
            written++;
        }
    }
    return written;
}

// Records an entry point the translation is missing, for tools/discover_loop.sh. Only the first
// one the guest reached is kept, since the run is derailed from here on and anything after it
// is unreliable -- but the table that target came from is recorded with it.
static int record_missing(uint32_t target) {
    if (!discover_fp) return 0;
    fprintf(discover_fp, "%08X\n", target);
    unsigned extra = record_table_siblings(target);
    fclose(discover_fp);
    discover_fp = NULL;
    printf("[discover] first missing target 0x%08x after %lu syscalls"
           " (plus %u table sibling(s))\n", target, albw_svc_count(), extra);
    return 1;
}

// Where else the missing target sits in guest memory, and what surrounds it there. A target
// that is neither an absolute pointer nor a pc-relative offset in the image was built into a
// table at run time, and the neighbours of a function pointer in such a table are function
// pointers too. Diagnostic only: it says whether one run could yield a whole cluster instead
// of a single address, which is the difference between one round of the discovery loop and
// dozens.
static void report_neighbours(uint32_t target) {
    unsigned hits = 0;
    for (uint32_t a = 0x1000; a + 4 <= ALBW_MEM_SIZE && hits < 8; a += 4) {
        uint32_t v;
        memcpy(&v, albw_mem + a, 4);
        if (v != target) continue;
        hits++;
        printf("[neighbours] 0x%08x holds it; run:", a);
        for (int k = -6; k <= 6; k++) {
            long at = (long)a + k * 4;
            if (at < 0 || at + 4 > (long)ALBW_MEM_SIZE) continue;
            uint32_t w;
            memcpy(&w, albw_mem + at, 4);
            int text = (w & ~1u) >= ALBW_TEXT_BASE && (w & ~1u) < ALBW_TEXT_BASE + 0x700000u;
            printf(" %08x%s", w, text ? "*" : "");
        }
        printf("\n");
    }
    if (!hits) printf("[neighbours] 0x%08x appears nowhere in guest memory\n", target);
}

// A target nobody has translated, met in an ordinary run -- somebody playing, not the discovery
// loop. The run cannot go on, but it can leave behind what the next build needs: the target, and
// the rest of its table, appended to missing_entries.txt beside the saves, which
// tools/catch_up.sh folds into entries/<product code>.txt before it retranslates and rebuilds.
#include <errno.h>
#include <sys/stat.h>
static void make_dirs(const char *path) {
    char buf[600];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':') continue;                      // a Windows drive's root
        char sep = *p;
        *p = '\0';
        albw_mkdir(buf);
        *p = sep;
    }
    albw_mkdir(buf);
}

static void note_for_catch_up(uint32_t target) {
    if (discover_fp) return;                             // the discovery loop's run: its own file
    char path[600];
    make_dirs(albw_save_root());
    snprintf(path, sizeof path, "%s/missing_entries.txt", albw_save_root());
    discover_fp = fopen(path, "a");
    if (!discover_fp) return;
    record_missing(target);
    const char *msg =
        "\n[albw] The game has reached code this build has not translated yet (0x%08x).\n"
        "[albw] It is noted in %s.\n"
        "[albw] Run tools/catch_up.sh to translate it into the build, then start the game again;\n"
        "[albw] anything since your last save is lost.\n\n";
    printf(msg, target, path);
    fprintf(stderr, msg, target, path);
    fflush(stdout);
#ifdef ALBW_HAVE_WINDOW
    char text[900];
    snprintf(text, sizeof text,
             "The game has reached code this build has not translated yet (0x%08x).\n\n"
             "It is noted in %s.\n\nRun tools/catch_up.sh to translate it into the build, then "
             "start the game again; anything since your last save is lost.", target, path);
    albw_window_message("World Across: untranslated code", text);
#endif
}

void albw_dispatch(albw_cpu *c, uint32_t target) {
    uint32_t t = target & ~1u;
    size_t lo = 0, hi = ALBW_FN_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (albw_fn_addrs[mid] < t) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ALBW_FN_COUNT && albw_fn_addrs[lo] == t) {
        albw_fn_ptrs[lo](c);
        return;
    }
    if (t == ALBW_SENTINEL_LR) {
        printf("[dispatch] guest returned to sentinel: boot path finished\n");
        exit(0);
    }
    if (getenv("ALBW_NEIGHBOURS")) report_neighbours(target);
    if (record_missing(target)) exit(0);
    fprintf(stderr, "[dispatch] no translated function at 0x%08x "
                    "(indirect target outside the slice)\n", t);
    note_for_catch_up(t);
    exit(2);
}

// A body that runs off its end into an address no function starts at. The address is an entry
// point by exactly the same argument as an indirect target: the guest reaches it. So it is
// worth recording, or the discovery loop stops at the first one and has to be driven by hand.
void albw_missing_function(albw_cpu *c, uint32_t pc, uint32_t target) {
    (void)c;
    if (record_missing(target)) exit(0);
    fprintf(stderr, "[dispatch] 0x%08x falls through to 0x%08x, "
                    "where no function starts\n", pc, target);
    note_for_catch_up(target);
    exit(2);
}

#ifdef ALBW_TRACE
// One fixed-width record per instruction: pc, r0-r14, packed NZCV. r15 is omitted
// because the translation never materialises a guest pc.
#define ALBW_TRACE_WORDS 17
static FILE *trace_fp;
static unsigned long trace_n, trace_cap = 1000000, trace_skip, trace_total;

unsigned long albw_instruction_count(void) { return trace_total; }

void albw_trace(albw_cpu *c, uint32_t pc) {
    trace_total++;
    if (!trace_fp) return;
    // A window, not the whole run: comparing from instruction zero every time costs more the
    // further the boot path gets, and the interesting region is the newest one. Skipped
    // instructions still count, so both sides window the same range.
    if (trace_skip) {
        trace_skip--;
        return;
    }
    uint32_t rec[ALBW_TRACE_WORDS];
    rec[0] = pc;
    for (int i = 0; i < 15; i++) rec[1 + i] = c->r[i];
    rec[16] = (c->nf ? 8u : 0u) | (c->zf ? 4u : 0u)
            | (c->cf ? 2u : 0u) | (c->vf ? 1u : 0u);
    fwrite(rec, sizeof rec, 1, trace_fp);
    if (++trace_n >= trace_cap) {
        fclose(trace_fp);
        trace_fp = NULL;
        fprintf(stderr, "[trace] reached cap of %lu instructions\n", trace_cap);
        exit(0);
    }
}
#endif

#ifndef ALBW_TRACE
unsigned long albw_instruction_count(void) { return 0; }
#endif

void albw_unimplemented(albw_cpu *c, uint32_t pc, const char *what) {
    (void)c;
    fprintf(stderr, "[unimplemented] pc=0x%08x  %s\n", pc, what);
    fprintf(stderr, "                reached after %lu syscalls\n", albw_svc_count());
#ifdef ALBW_HAVE_WINDOW
    // Started from a desktop there is no terminal to read the line above in, and the window
    // would just vanish.
    char text[700];
    snprintf(text, sizeof text,
             "The game has reached an instruction this port cannot run yet, at 0x%08x:\n%s\n\n"
             "That is a gap in the port, not a fault in your copy of the game. Anything since "
             "your last save is lost. Please report the address and the line above.", pc, what);
    albw_window_message("World Across: instruction not supported", text);
#endif
    exit(3);
}

// ---------------------------------------------------------------------------------------------
// A sampling profiler, for ALBW_PROFILE=<file>: every millisecond of CPU time the interrupted
// instruction's address is counted, and at exit the most frequent ones are written out as offsets
// into the executable -- `addr2line -f -e build/albw-boot` turns them into functions and lines. There
// is no perf on every machine, and this needs nothing but the binary's own debug information.
// Linux only: it is built from signals, interval timers and the dynamic linker.
#ifdef _WIN32
static void profile_start(void) {
    if (getenv("ALBW_PROFILE")) printf("[profile] not on Windows: ALBW_PROFILE is ignored\n");
}
#else
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>

#define ALBW_PROFILE_SLOTS 65536
static uintptr_t profile_pc[ALBW_PROFILE_SLOTS];
static unsigned long profile_hits[ALBW_PROFILE_SLOTS];
static unsigned long profile_total, profile_lost;
static unsigned long profile_from;   // ALBW_PROFILE_FROM: the first frame counted

static void profile_tick(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    if (albw_kernel_frame() < profile_from) return;
    ucontext_t *uc = context;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    profile_total++;
    // Open addressing on the address itself; a full table just counts what it could not keep.
    uint32_t h = (uint32_t)((pc >> 4) * 2654435761u) % ALBW_PROFILE_SLOTS;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) % ALBW_PROFILE_SLOTS) {
        if (profile_pc[h] == pc) { profile_hits[h]++; return; }
        if (!profile_pc[h]) { profile_pc[h] = pc; profile_hits[h] = 1; return; }
    }
    profile_lost++;
}

static void profile_report(void) {
    const char *path = getenv("ALBW_PROFILE");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f) return;
    Dl_info self;
    uintptr_t base = 0;
    if (dladdr((void *)profile_report, &self)) base = (uintptr_t)self.dli_fbase;
    fprintf(f, "# %lu samples, %lu not kept; offset count\n", profile_total, profile_lost);
    for (int i = 0; i < ALBW_PROFILE_SLOTS; i++) {
        if (!profile_hits[i]) continue;
        // Outside the executable the offset means nothing to addr2line; name the library and the
        // nearest symbol instead, which is enough to tell a lock from a copy.
        Dl_info where;
        if (dladdr((void *)profile_pc[i], &where) && where.dli_fbase != self.dli_fbase) {
            const char *lib = strrchr(where.dli_fname, '/');
            fprintf(f, "lib:%s:%s %lu\n", lib ? lib + 1 : where.dli_fname,
                    where.dli_sname ? where.dli_sname : "?", profile_hits[i]);
            continue;
        }
        fprintf(f, "%lx %lu\n", (unsigned long)(profile_pc[i] - base), profile_hits[i]);
    }
    fclose(f);
}

static void profile_start(void) {
    if (!getenv("ALBW_PROFILE")) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = profile_tick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    atexit(profile_report);
    const char *from = getenv("ALBW_PROFILE_FROM");
    if (from) profile_from = strtoul(from, NULL, 0);
    // ALBW_PROFILE_WALL=1 samples only this thread, every millisecond of real time, waiting
    // included: the guest and the GPU front end both run here, so that is the frame's critical
    // path, while the CPU-time default adds up every thread.
    const char *wall = getenv("ALBW_PROFILE_WALL");
    if (wall && *wall && *wall != '0') {
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);   // sigev_notify_thread_id, where glibc names it
        timer_t timer;
        struct itimerspec every = { { 0, 1000000 }, { 0, 1000000 } };
        if (timer_create(CLOCK_MONOTONIC, &sev, &timer) == 0) {
            timer_settime(timer, 0, &every, NULL);
            return;
        }
    }
    struct itimerval every = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &every, NULL);
}
#endif

// ALBW_WAV=<file>: the DSP's output written to a WAV file as it is made, window or not -- the way to
// hear what a headless run played, and to look at it. The header's sizes are filled in at exit.
static FILE *wav_file;
static uint32_t wav_frames;
static void (*wav_also)(const int16_t *stereo, unsigned frames);

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4u, riff = 36u + data, fmt_size = 16u, rate = ALBW_DSP_SAMPLE_RATE;
    uint32_t byte_rate = rate * 4u;
    uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static void wav_write(const int16_t *stereo, unsigned frames) {
    if (wav_file) {
        fwrite(stereo, 4, frames, wav_file);
        wav_frames += frames;
    }
    if (wav_also) wav_also(stereo, frames);
}

static void wav_close(void) {
    if (!wav_file) return;
    fflush(wav_file);
    rewind(wav_file);
    wav_header(wav_file, wav_frames);
    fclose(wav_file);
    wav_file = NULL;
}

static void wav_start(void (*also)(const int16_t *stereo, unsigned frames)) {
    const char *path = getenv("ALBW_WAV");
    wav_also = also;
    if (!path || !*path) {
        albw_dsp_set_output(also);
        return;
    }
    wav_file = fopen(path, "wb");
    if (!wav_file) {
        printf("[dsp  ] cannot write %s; no recording\n", path);
        albw_dsp_set_output(also);
        return;
    }
    wav_header(wav_file, 0);
    atexit(wav_close);
    albw_dsp_set_output(wav_write);
}

#ifdef ALBW_HAVE_WINDOW
static const char *run_game(void) { return albw_thread_run(ALBW_TEXT_BASE); }

// The kernel's frame hook: show the frame, and pass the controls back for the HID service.
static int window_frame(uint32_t shared_addr, uint32_t thread_index, unsigned long frame) {
    int keep_going = albw_window_frame(shared_addr, thread_index, frame);
    albw_kernel_set_input(albw_window_input());
    return keep_going;
}
#endif

static void make_dirs(const char *path);

// Where things are, so that a player can start the port with nothing but the card image, once.
//
// The card image is remembered in $XDG_CONFIG_HOME/albw-port/card (else ~/.config/albw-port/card)
// whenever it is named on the command line, and used when nothing names one. The translated
// game's code, rom-extract/code.bin, is looked for where the tools put it: under the current
// directory, else beside the build directory this binary sits in.
static void config_file(char *out, size_t n, const char *name) {
    albw_config_path(out, n, name);
}

static const char *remembered_card(void) {
    static char card[1024];
    char path[1100];
    config_file(path, sizeof path, "card");
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (!fgets(card, sizeof card, f)) card[0] = '\0';
    fclose(f);
    card[strcspn(card, "\n")] = '\0';
    return card[0] ? card : NULL;
}

static void remember_card(const char *card) {
    char abs[1024], path[1100], dir[1100];
    if (albw_realpath(card, abs, sizeof abs) != 0) return;
    const char *old = remembered_card();
    if (old && !strcmp(old, abs)) return;
    config_file(dir, sizeof dir, "");
    make_dirs(dir);
    config_file(path, sizeof path, "card");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", abs);
    fclose(f);
    printf("[init] remembering the card image %s for next time (%s)\n", abs, path);
}

// A card image is a file of at least 16 MB; code.bin is a few.
static int looks_like_card(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size >= 16L * 1024 * 1024;
}

// Which code the build is a translation of, and which release (src/generated/albw_code_id.c).
// Weak, so that a translation made before the recompiler wrote them still links, unchecked.
extern const char albw_build_product[] __attribute__((weak));
extern const uint64_t albw_build_code_hash __attribute__((weak));
extern const uint32_t albw_build_code_size __attribute__((weak));

static void refuse(const char *title, const char *text) {
    fprintf(stderr, "[init] %s\n", text);
#ifdef ALBW_HAVE_WINDOW
    if (!getenv("ALBW_HEADLESS")) albw_window_alert(title, text);
#else
    (void)title;
#endif
    exit(1);
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// The product code in the card's first NCCH header, or "" if the file is not a card.
static void card_product(const char *card, char out[17]) {
    out[0] = '\0';
    FILE *f = fopen(card, "rb");
    if (!f) return;
    uint8_t b[4];
    if (fseek(f, 0x120, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4) {
        long off = (long)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24) * 0x200L;
        if (fseek(f, off + 0x150, SEEK_SET) != 0 || fread(out, 1, 16, f) != 16) out[0] = '\0';
        out[16] = '\0';
    }
    fclose(f);
}

static const char *default_code(void) {
    static char path[1100];
    struct stat st;
    if (stat("rom-extract/code.bin", &st) == 0) return "rom-extract/code.bin";
    char exe[1024];
    albw_exe_dir(exe, sizeof exe);
    if (exe[0]) {
        snprintf(path, sizeof path, "%s/../rom-extract/code.bin", exe);
        if (stat(path, &st) == 0) return path;
        // A Windows build sits in build/ itself, or anywhere with the folder beside it.
        snprintf(path, sizeof path, "%s/rom-extract/code.bin", exe);
        if (stat(path, &st) == 0) return path;
    }
    return "rom-extract/code.bin";
}

#ifdef ALBW_HAVE_WINDOW
// The launcher's file dialog picked a card (albw_launcher_set_card_chooser): checked as one named
// on the command line is, remembered, and given to the kernel for the game to read when it starts.
static int choose_card(const char *path, char product[17], char *why, size_t n) {
    static char chosen[1024];
    if (!looks_like_card(path)) {
        snprintf(why, n, "A card image is a file of 16 MB and more.");
        return 0;
    }
    char prod[17];
    card_product(path, prod);
    if (!prod[0]) {
        snprintf(why, n, "The file has no 3DS card header (a .3ds or .cci dump is what is needed).");
        return 0;
    }
    if (&albw_build_product && albw_build_product[0] && strcmp(prod, albw_build_product)) {
        snprintf(why, n, "That card is %s; this build is a translation of %s.", prod, albw_build_product);
        return 0;
    }
    snprintf(chosen, sizeof chosen, "%s", path);
    remember_card(chosen);
    albw_kernel_set_rom(chosen);
    memcpy(product, prod, 17);
    return 1;
}
#endif

int main(int argc, char **argv) {
    albw_platform_init();
    // albw-boot [card.3ds], albw-boot code.bin [card.3ds], or ALBW_ROM for the card.
    const char *path = NULL, *card = getenv("ALBW_ROM");
    int card_named = 0;
    if (argc > 1 && looks_like_card(argv[1])) {
        card = argv[1];
        card_named = 1;
    } else if (argc > 1) {
        path = argv[1];
        if (argc > 2) {
            card = argv[2];
            card_named = 1;
        }
    }
    if (!path) path = default_code();
    if (!card) card = remembered_card();
    if (card_named) remember_card(card);
    if (!card) {
        fprintf(stderr, "[init] no card image: start it once as  albw-boot /path/to/your.3ds  -- "
                        "the path is remembered after that\n");
    }

    // Line buffered even when redirected, so a run killed by a timeout still shows how
    // far it got instead of losing the whole log.
    setvbuf(stdout, NULL, _IOLBF, 0);

    // The guest's memory and, after it, the shadow (albw_kernel.h); untouched pages cost nothing.
    albw_mem = albw_reserve((size_t)ALBW_MEM_SIZE + ALBW_SHADOW_SIZE);
    if (!albw_mem) {
        fprintf(stderr, "[init] cannot reserve the guest's memory\n");
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fread(albw_mem + ALBW_TEXT_BASE, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "short read of %s\n", path);
        return 1;
    }
    fclose(f);
    printf("[init] loaded %ld bytes at 0x%08x, %d translated functions\n",
           size, ALBW_TEXT_BASE, ALBW_FN_COUNT);

    // The translation is of one code.bin, and of one release. Run against another, it would go
    // wrong somewhere unpredictable instead of saying so here.
    char mismatch[1400];
    if (&albw_build_code_hash && ((uint32_t)size != albw_build_code_size ||
            fnv1a(albw_mem + ALBW_TEXT_BASE, (size_t)size) != albw_build_code_hash)) {
        snprintf(mismatch, sizeof mismatch,
                 "%s is not the code this build was translated from. After extracting another "
                 "card, translate it again: tools/find_functions.py, then tools/recompile.py "
                 "--all --split 16, then ninja -C build.", path);
        refuse("World Across: the build does not match its code", mismatch);
    }
    if (card && albw_build_product && albw_build_product[0]) {
        char prod[17];
        card_product(card, prod);
        if (prod[0] && strcmp(prod, albw_build_product)) {
            snprintf(mismatch, sizeof mismatch,
                     "This build is a translation of %s, and the card image %s is %s. To play "
                     "that card, extract and translate it as the README describes under Building.",
                     albw_build_product, card, prod);
            refuse("World Across: another release", mismatch);
        }
    }

    const char *dp = getenv("ALBW_DISCOVER");
    if (dp) {
        discover_fp = fopen(dp, "w");
        if (!discover_fp) {
            perror("discover file");
            return 1;
        }
        printf("[discover] recording missing dispatch targets to %s\n", dp);
    }

#ifdef ALBW_TRACE
    const char *tp = getenv("ALBW_TRACE_FILE");
    const char *tc = getenv("ALBW_TRACE_CAP");
    if (tc) trace_cap = strtoul(tc, NULL, 10);
    const char *ts = getenv("ALBW_TRACE_SKIP");
    if (ts) trace_skip = strtoul(ts, NULL, 10);
    trace_fp = fopen(tp ? tp : "albw-trace.bin", "wb");
    if (!trace_fp) {
        perror("trace file");
        return 1;
    }
    printf("[trace] writing to %s, skipping %lu then recording %lu\n",
           tp ? tp : "albw-trace.bin", trace_skip, trace_cap);
#endif

    // The card: named on the command line, or ALBW_ROM so tools that drive the binary do not each
    // have to thread the path through, or the one remembered from last time.
    profile_start();
    albw_kernel_set_rom(card);
    albw_kernel_init();
#ifdef ALBW_HAVE_WINDOW
    // A window if there is a display and nobody asked for none. With one open the rasteriser draws
    // every frame, because somebody is looking.
    void (*speakers)(const int16_t *, unsigned) = NULL;
    if (card) {
        char prod[17];
        card_product(card, prod);
        albw_launcher_set_card(prod);
    }
    albw_launcher_set_card_chooser(choose_card);
    if (albw_window_open()) {
        if (!albw_window_launch()) {
            albw_window_close();
            return 0;
        }
        albw_kernel_set_frame_hook(window_frame);
        albw_raster_render_always();
        speakers = albw_window_audio;
    }
    wav_start(speakers);
#else
    wav_start(NULL);
#endif

    // The entry point runs as a scheduled thread like any other, so it can block and let
    // the threads it creates run. With a window, all of that goes on a thread of its own, and
    // this one is the window's.
    const char *why;
#ifdef ALBW_HAVE_WINDOW
    if (speakers) why = albw_window_play(run_game);
    else
#endif
    why = albw_thread_run(ALBW_TEXT_BASE);
    printf("[exit] scheduler stopped: %s (after %lu syscalls)\n", why, albw_svc_count());
#ifdef ALBW_HAVE_WINDOW
    albw_window_close();
#endif
    return 0;
}

#ifdef ALBW_WATCH_NAN
#include <execinfo.h>

// The first stores of a not-a-number bit pattern, with the C stack at each. The generated functions
// are named f_<guest address>, so the stack is the guest's call chain, which is what makes this worth
// more than a count.
//
// A quiet NaN is any value whose exponent is all ones and whose mantissa is not zero. The game's
// projection matrix arrives as 0x7FC00000 and 0xFFC00000, so the arithmetic that made it is what
// this is looking for -- almost certainly a division whose divisor this port supplies as zero.
// The watch window: ALBW_WATCH_ADDR, and ALBW_WATCH_LEN bytes from there (four by default).
static uint32_t watch_from, watch_len;
static int watch_ready;

static void watch_setup(void) {
    if (watch_ready) return;
    watch_ready = 1;
    const char *s = getenv("ALBW_WATCH_ADDR");
    watch_from = s ? (uint32_t)strtoul(s, NULL, 0) : 0;
    const char *n = getenv("ALBW_WATCH_LEN");
    watch_len = n ? (uint32_t)strtoul(n, NULL, 0) : 4u;
}

static void report_store(uint32_t addr, uint32_t value, uint32_t len) {
    static unsigned seen;
    const char *max_s = getenv("ALBW_WATCH_MAX");
    unsigned max = max_s ? (unsigned)strtoul(max_s, NULL, 0) : 6u;
    if (seen >= max) return;
    seen++;

    void *frames[12];
    int n = backtrace(frames, 12);
    char **names = backtrace_symbols(frames, n);
    // With ALBW_WATCH_LINE, one line per store: the guest functions only, innermost first. A window
    // of memory gets a store per word from the startup's clearing alone, and a stack per store
    // buries the one store that matters.
    if (getenv("ALBW_WATCH_LINE")) {
        printf("[watch] %08x <- %0*x:", addr, (int)len * 2, value);
        for (int i = 2; i < n; i++) {
            const char *f = names ? strstr(names[i], "(f_") : NULL;
            if (f) printf(" %.10s", f + 1);
        }
        printf("\n");
        free(names);
        return;
    }
    printf("[watch] %08x <- %0*x (%u byte%s), from:\n", addr, (int)len * 2, value, len,
           len == 1 ? "" : "s");
    // Two frames are this function and the hook that called it.
    for (int i = 2; i < n && i < 10; i++) {
        printf("[watch]   %s\n", names ? names[i] : "?");
    }
    free(names);
}

// Either the first stores of a not-a-number, or -- with ALBW_WATCH_ADDR -- every store into a window
// of memory. A bad floating-point value is usually made some distance from where it is noticed, so
// once the watch has named one store, following the value back a step at a time is the work.
void albw_note_stored_word(uint32_t addr, uint32_t value) {
    watch_setup();
    if (watch_from) {
        if (addr + 4u <= watch_from || addr >= watch_from + watch_len) return;
    } else {
        // The *canonical* quiet not-a-number and nothing else. Any value with an all-ones exponent
        // and a non-zero mantissa is a NaN, but 0xFFFFFFFF is also minus one and is stored
        // constantly, so a wider test drowns in integers. What an invalid floating-point operation
        // produces is 0x7FC00000, which is what the game's matrix is made of.
        if ((value & 0x7FFFFFFFu) != 0x7FC00000u) return;
    }
    report_store(addr, value, 4);
}

void albw_note_stored_narrow(uint32_t addr, uint32_t value, uint32_t len) {
    watch_setup();
    if (!watch_from) return;
    if (addr + len <= watch_from || addr >= watch_from + watch_len) return;
    report_store(addr, value, len);
}
#endif

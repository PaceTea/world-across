// ALBW_WATCH_VRAM reads the faulting instruction's address out of the signal context, a GNU
// extension.
#define _GNU_SOURCE

// Horizon kernel and service layer, as far as the boot path needs it.
//
// The syscall ABI here is read off the game's own wrappers rather than assumed. Every
// handle-returning call follows one shape:
//
//     str r0, [sp, #-4]!     ; save the caller's out pointer
//     svc 0x??               ; result in r0, handle in r1
//     ldr r2, [sp]
//     str r1, [r2]           ; the *stub* stores the handle, not the kernel
//
// So the kernel returns the handle in r1 and the result code in r0, and r0 on entry is
// the out pointer, which it ignores. Getting that backwards is what made the first
// SendSyncRequest receive the address of the "srv:" string as its session handle.
#include "albw_kernel.h"
#include "albw_romfs.h"
#include "albw_savedata.h"
#include "albw_pica.h"
#include "albw_config.h"
#include "albw_input.h"
#include "albw_dsp.h"
#include "albw_raster.h"
#include "albw_screen.h"
#ifdef ALBW_HAVE_WINDOW
#include "albw_window.h"
#endif

#include "albw_platform.h"
#include "albw_thread.h"
#ifndef _WIN32
#include <ucontext.h>                                    // ALBW_WATCH_VRAM's fault address
#endif

#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ALBW_MAX_HANDLES 512

typedef enum {
    ALBW_H_FREE = 0,
    ALBW_H_SESSION,
    ALBW_H_EVENT,
    ALBW_H_MUTEX,
    ALBW_H_THREAD,
    ALBW_H_MEMBLOCK,
    ALBW_H_ARBITER,
    ALBW_H_RESLIMIT,
    ALBW_H_FILE,
    ALBW_H_ARCHIVE,
} albw_handle_kind;

static const char *kind_name(albw_handle_kind k) {
    switch (k) {
        case ALBW_H_SESSION:  return "session";
        case ALBW_H_EVENT:    return "event";
        case ALBW_H_MUTEX:    return "mutex";
        case ALBW_H_THREAD:   return "thread";
        case ALBW_H_MEMBLOCK: return "memblock";
        case ALBW_H_ARBITER:  return "arbiter";
        case ALBW_H_RESLIMIT: return "reslimit";
        case ALBW_H_FILE:     return "file";
        case ALBW_H_ARCHIVE:  return "archive";
        default:              return "free";
    }
}

typedef struct {
    albw_handle_kind kind;
    char name[16];          // service or port name, for sessions
    uint32_t refs;
    uint64_t base, size;    // for files: the window this handle exposes
    uint32_t signalled;     // for events and mutexes
    uint32_t reset_type;    // 0 oneshot, 1 sticky, 2 pulse
    uint32_t archive_id;    // for archives
    int fd;                 // host-backed files: save data and NAND archives
} albw_handle;

// APT lifecycle. The Home Menu is the sender an application expects its wakeup from.
#define APT_APPID_HOMEMENU 0x0101u
#define APT_COMMAND_NONE   0u
#define APT_COMMAND_WAKEUP 1u
static int apt_wakeup_sent;
// APT hands these out once and the guest keeps them, so they live as long as the process.
static uint32_t apt_lock, apt_notify_event, apt_resume_event;
// HID hands out one shared memory block and five events, once.
static uint32_t hid_shared, hid_events[5], hid_shared_addr;
// srv: hands out one notification semaphore, which a listener thread waits on.
static uint32_t srv_notification;
// gsp::Gpu. The application supplies the event it wants interrupts on; the shared memory
// block carries the queue of interrupt ids it then reads.
static uint32_t gsp_event, gsp_shared, gsp_shared_addr;
static uint32_t gsp_thread_index;
static unsigned long gsp_frames;
static unsigned long ft_reads, ft_read_bytes;           // file reads this frame, for ALBW_FRAME_TIMES

// The system counter runs at 268,111,856 Hz and the LCDs at a little under 60 Hz: 4,481,136 ticks a
// frame.
#define ALBW_TICKS_PER_FRAME 4481136ull

// Called once a frame with the screens ready to be shown, by whoever is showing them. Returns 0 to
// end the run -- the window's close button. The reference run installs none, so it sees exactly what
// a headless run sees.
static int (*frame_hook)(uint32_t shared_addr, uint32_t thread_index, unsigned long frame);

void albw_kernel_set_frame_hook(int (*hook)(uint32_t, uint32_t, unsigned long)) {
    frame_hook = hook;
}

// Which frame the guest is on. The rasteriser needs it: rendering every frame of a run that lasts
// twenty thousand of them is more pixels than any purpose here requires.
unsigned long albw_kernel_frame(void) { return gsp_frames; }

// getenv, read once per name. The diagnostic switches are asked about per draw, per transfer and
// per register write, and libc's getenv walks the whole environment each time: a quarter of a
// rendering run's time went there. The environment never changes during a run, so the first answer
// is the answer. Keyed by the name's address first -- callers pass literals -- and by its text after.
const char *albw_env(const char *name) {
    static struct { const char *key; const char *value; } cache[96];
    static int used;
    for (int i = 0; i < used; i++) {
        if (cache[i].key == name) return cache[i].value;
    }
    for (int i = 0; i < used; i++) {
        if (!strcmp(cache[i].key, name)) return cache[i].value;
    }
    const char *value = getenv(name);
    if (used < (int)(sizeof cache / sizeof cache[0])) {
        cache[used].key = name;
        cache[used].value = value;
        used++;
    }
    return value;
}
// Set when the kernel stops advancing time on purpose; see albw_kernel_on_idle.
static const char *kernel_stop_note;
// dsp::DSP signals a semaphore once per audio frame, and a guest thread waits on it. Same
// shape as the GPU's interrupt event, and driven by the same idle clock so time stays tied to
// "nothing else can run" rather than to a wall clock.
static uint32_t dsp_event;

// The DSP is a separate Teak processor running its own firmware, which we do not have and
// cannot ship. What the game actually needs from it is a documented conversation, and the
// game's own code spells the whole of it out: it writes a 4-byte command into pipe 2, waits on
// an event it registered for that pipe, then reads a u16 count followed by that many halfwords
// of addresses (0x003df02c). Each address is run through ConvertProcessAddressFromDspDram
// twice, once per memory bank (0x003df1b8), and the results become the base pointers it mixes
// audio through. So the handshake is answerable without the firmware; only the mixing is not.
#define ALBW_DSP_PIPES      8
#define ALBW_DSP_PIPE_CAP   0x100
static uint8_t dsp_pipe[ALBW_DSP_PIPES][ALBW_DSP_PIPE_CAP];
static uint32_t dsp_pipe_len[ALBW_DSP_PIPES], dsp_pipe_pos[ALBW_DSP_PIPES];
// One event per pipe, handed to us by RegisterInterruptEvents. Indexed by channel because that
// is what the game registers per pipe; the interrupt number it passes alongside is the same.
static uint32_t dsp_pipe_event[ALBW_DSP_PIPES];
static uint32_t dsp_semaphore_mask;

// Opens a file in a host-backed archive, returning a handle or 0. The guest's path is a counted
// string in a static buffer and is not necessarily terminated, so it is copied out first.
static uint32_t fs_open_host_file(const albw_cpu *c, uint32_t archive_id, uint32_t path_type,
                                  uint32_t ptr, uint32_t len, int create);

// Horizon's path kinds. The order matters and is easy to get backwards: ASCII is 3 and UTF-16
// is 4, and reading /gamecoin.dat as ASCII yields "/" because every second byte is zero.
#define ALBW_PATH_EMPTY  1u
#define ALBW_PATH_BINARY 2u
#define ALBW_PATH_ASCII  3u
#define ALBW_PATH_UTF16  4u

// The only archive this port can serve, and the result code for one it cannot. Both were
// spelled out at each use before; naming them keeps the two paths that answer OpenArchive and
// OpenFileDirectly from drifting apart.
#define ALBW_ARCHIVE_ROMFS    3u
#define ALBW_ARCHIVE_SAVEDATA 4u
#define ALBW_FS_NOT_FOUND     0xC8804478u
// What an archive that was never formatted answers. The SDK's first-boot path keys on it: mount,
// see this, format, mount again.
#define ALBW_FS_NOT_FORMATTED 0xC8A04554u

// Defined with the rest of the GPU machinery, below the IPC dispatch that needs them.
static int gsp_run_command_queue(void);
static int gsp_deliver_interrupts(void);

// DSP RAM: 512 KiB in two banks, addressed by the DSP in halfwords. Bit 16 of a DSP DRAM
// address selects the bank, which is how one address yields the two buffers the game
// double-buffers between.
#define ALBW_DSP_RAM_BASE  0x1FF00000u
#define ALBW_DSP_BANK_SIZE 0x40000u

static uint32_t dsp_dram_to_guest(uint32_t addr) {
    return ALBW_DSP_RAM_BASE + ((addr >> 16) & 1u) * ALBW_DSP_BANK_SIZE
         + (addr & 0xFFFFu) * 2u;
}

// The fifteen region addresses the firmware reports, as halfword addresses in bank 0: frame counter,
// source configurations, source statuses, ADPCM coefficients, DSP configuration, DSP status, final
// samples, intermediate mix samples, compressor, debug, and five more. They are the real firmware's,
// laid out as its shared region is -- 0x8000 bytes, the frame counter in its last halfword -- because
// albw_dsp.c reads and writes that layout: the region starts at 0x8000, and its bank-1 twin at
// 0x18000. The game's own strides agree (0xC0 per source configuration, 0x0C per status, 0x20 per
// coefficient set, 24 sources).
#define ALBW_DSP_REGION_BASE 0x8000u
#define ALBW_DSP_SOURCES     24u

static const uint16_t dsp_region_addr[15] = {
    0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
    0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C,
};

static albw_handle handles[ALBW_MAX_HANDLES];
static uint32_t heap_next = ALBW_HEAP_BASE;
static uint32_t linear_next = ALBW_LINEAR_BASE;
static unsigned long svc_count;
static const char *rom_path;

// Readable in logs and far away from the pseudo-handles.
static uint32_t handle_value(int index) { return 0x1000u + (uint32_t)index * 4u; }
static int handle_index(uint32_t h) {
    if (h < 0x1000u || (h - 0x1000u) % 4u) return -1;
    int i = (int)((h - 0x1000u) / 4u);
    return (i < ALBW_MAX_HANDLES && handles[i].kind != ALBW_H_FREE) ? i : -1;
}

static uint32_t handle_create(albw_handle_kind kind, const char *name) {
    for (int i = 0; i < ALBW_MAX_HANDLES; i++) {
        if (handles[i].kind != ALBW_H_FREE) continue;
        handles[i].kind = kind;
        handles[i].refs = 1;
        handles[i].name[0] = '\0';
        if (name) {
            strncpy(handles[i].name, name, sizeof handles[i].name - 1);
            handles[i].name[sizeof handles[i].name - 1] = '\0';
        }
        return handle_value(i);
    }
    fprintf(stderr, "[kernel] handle table exhausted\n");
    return 0;
}

// Makes a pipe readable and wakes whoever registered an event for it. The event is sticky, so
// signalling before the guest reaches its wait is not a lost wakeup: the wait returns at once
// and the guest clears the event itself.
static void dsp_pipe_fill(uint32_t channel, const uint8_t *data, uint32_t len) {
    if (channel >= ALBW_DSP_PIPES || len > ALBW_DSP_PIPE_CAP) return;
    memcpy(dsp_pipe[channel], data, len);
    dsp_pipe_len[channel] = len;
    dsp_pipe_pos[channel] = 0;

    uint32_t ev = dsp_pipe_event[channel];
    if (!ev) return;
    int i = handle_index(ev);
    if (i >= 0) handles[i].signalled = 1;
    albw_thread_wake(ALBW_W_SYNC, ev, -1);
}

// One audio frame: the firmware plays it (albw_dsp.c) and raises the pipe 2 interrupt, which is what
// the game's audio thread waits for before it writes the next frame's settings. The frame counters
// are the game's to advance; the firmware only reads them to see which region is newer.
//
// Without the interrupt the audio thread waits forever after the Initialize handshake, and twelve
// threads end up blocked with nothing runnable.
static int dsp_frame_tick(void) {
    if (!dsp_pipe_event[2] || !albw_dsp_running()) return 0;
    albw_dsp_tick();
    int i = handle_index(dsp_pipe_event[2]);
    if (i >= 0) handles[i].signalled = 1;
    return albw_thread_wake(ALBW_W_SYNC, dsp_pipe_event[2], -1);
}

// Time, in ARM11 cycles, as far as the kernel has advanced it: to each vblank and each audio frame
// in turn. Nothing else moves it, which keeps the run deterministic.
static uint64_t kernel_time;
static uint64_t audio_next;                              // when the next audio frame is due

// What HID does every frame: put a fresh sample in its shared memory and raise the pad events.
//
// The layout is the one the game's own readers take apart, not a document's: the pad reader at
// 0x003E5F54 and the touch reader at 0x003E6100. Both read a header -- the tick of the newest ring
// reset, the tick of the one before, the index of the newest entry -- and then walk back through an
// eight-entry ring. The pad's entries start at 0x28 and are sixteen bytes: held, pressed, released,
// then the circle pad's x and y as halfwords. The touch section follows the pad's ring at 0xA8 with the
// same header, and eight-byte entries at 0x20 into it: x, y, and a byte saying the screen is touched.
//
// The ticks move only when the ring wraps to entry zero. The readers compare them with the ones they
// saw last to work out how many entries are new, so a tick that changes every frame looks like the
// ring having gone round more than once -- and then every entry counts as new, and a button pressed
// once is pressed again on each of the next seven reads.
#define ALBW_HID_ENTRIES      8u
#define ALBW_HID_PAD_CURRENT  0x1Cu
#define ALBW_HID_PAD_RING     0x28u
#define ALBW_HID_PAD_ENTRY    0x10u
#define ALBW_HID_TOUCH        0xA8u
#define ALBW_HID_TOUCH_RAW    0x18u
#define ALBW_HID_TOUCH_RING   0x20u
#define ALBW_HID_TOUCH_ENTRY  0x08u

static struct albw_input hid_input;
static uint32_t hid_held_before;

void albw_kernel_set_input(const struct albw_input *in) {
    if (in) hid_input = *in;
}

// The tick a sample is stamped with: one frame's worth of the system counter per frame.
static uint64_t hid_tick(unsigned long frame) { return (uint64_t)frame * ALBW_TICKS_PER_FRAME; }

// The header both sections share: on a wrap to entry zero the newest reset tick becomes the previous
// one and the current tick becomes the newest.
static void hid_header(uint32_t at, uint32_t index, unsigned long frame) {
    if (index == 0) {
        albw_w32(at + 8, albw_r32(at + 0));
        albw_w32(at + 12, albw_r32(at + 4));
        uint64_t tick = hid_tick(frame);
        albw_w32(at + 0, (uint32_t)tick);
        albw_w32(at + 4, (uint32_t)(tick >> 32));
    }
    albw_w32(at + 0x10, index);
}

// Buttons pressed by script rather than by hand: ALBW_PRESS=1500:A,1508:-,2000:A+START is A from
// frame 1500, nothing from 1508, A and Start from 2000. T<x>.<y> touches the bottom screen there
// (320 by 240, from its top left) for as long as the entry holds: 3000:T300.220,3010:-. Frames are
// the instruction stream's, so a
// scripted run is as reproducible as one with no input at all -- which is what lets the discovery
// loop and the reference run get past a screen that waits for a button.
#define ALBW_PRESS_MAX 4096
static struct {
    unsigned long frame;
    uint32_t buttons;
    int touching;
    uint16_t touch_x, touch_y;
} press_script[ALBW_PRESS_MAX];
static int press_count = -1;

static uint32_t press_button(const char *name, size_t len) {
    static const struct { const char *name; uint32_t bit; } names[] = {
        { "A", ALBW_PAD_A }, { "B", ALBW_PAD_B }, { "X", ALBW_PAD_X }, { "Y", ALBW_PAD_Y },
        { "L", ALBW_PAD_L }, { "R", ALBW_PAD_R }, { "START", ALBW_PAD_START },
        { "SELECT", ALBW_PAD_SELECT }, { "UP", ALBW_PAD_UP }, { "DOWN", ALBW_PAD_DOWN },
        { "LEFT", ALBW_PAD_LEFT }, { "RIGHT", ALBW_PAD_RIGHT },
        // The circle pad, pushed all the way: what walking takes, since the D-pad does not.
        { "CUP", ALBW_PAD_CIRCLE_UP }, { "CDOWN", ALBW_PAD_CIRCLE_DOWN },
        { "CLEFT", ALBW_PAD_CIRCLE_LEFT }, { "CRIGHT", ALBW_PAD_CIRCLE_RIGHT },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strlen(names[i].name) == len && !strncasecmp(names[i].name, name, len)) {
            return names[i].bit;
        }
    }
    printf("[hid  ] ALBW_PRESS names a button it does not know: %.*s\n", (int)len, name);
    return 0;
}

static uint32_t scripted_buttons(unsigned long frame) {
    if (press_count < 0) {
        press_count = 0;
        const char *s = albw_env("ALBW_PRESS");
        while (s && *s && press_count < ALBW_PRESS_MAX) {
            char *rest;
            unsigned long at = strtoul(s, &rest, 0);
            if (rest == s || *rest != ':') break;
            s = rest + 1;
            uint32_t buttons = 0;
            int touching = 0;
            unsigned long tx = 0, ty = 0;
            while (*s && *s != ',') {
                size_t n = strcspn(s, "+,");
                if ((*s == 'T' || *s == 't') && n > 1 && s[1] >= '0' && s[1] <= '9') {
                    char *dot;
                    tx = strtoul(s + 1, &dot, 10);
                    ty = *dot == '.' ? strtoul(dot + 1, NULL, 10) : 0;
                    touching = tx < 320 && ty < 240;
                    if (!touching) printf("[hid  ] ALBW_PRESS touches off the screen: %.*s\n", (int)n, s);
                } else if (!(n == 1 && *s == '-')) {
                    buttons |= press_button(s, n);
                }
                s += n;
                if (*s == '+') s++;
            }
            if (*s == ',') s++;
            press_script[press_count].frame = at;
            press_script[press_count].buttons = buttons;
            press_script[press_count].touching = touching;
            press_script[press_count].touch_x = (uint16_t)tx;
            press_script[press_count].touch_y = (uint16_t)ty;
            press_count++;
        }
        if (press_count) printf("[hid  ] %d scripted press(es)\n", press_count);
        // Said aloud: a script cut short drops its last presses, and its releases with them, which
        // looks exactly like a game that stopped listening -- an hour went into one such "bug".
        if (s && *s) {
            printf("[hid  ] ALBW_PRESS has more than %d entries; the rest are ignored\n",
                   ALBW_PRESS_MAX);
        }
    }
    uint32_t buttons = 0;
    for (int i = 0; i < press_count; i++) {
        if (press_script[i].frame <= frame) buttons = press_script[i].buttons;
    }
    return buttons;
}

// The scripted touch at this frame, if the entry that holds now has one.
static int scripted_touch(unsigned long frame, uint16_t *x, uint16_t *y) {
    int at = -1;
    for (int i = 0; i < press_count; i++) {
        if (press_script[i].frame <= frame) at = i;
    }
    if (at < 0 || !press_script[at].touching) return 0;
    *x = press_script[at].touch_x;
    *y = press_script[at].touch_y;
    return 1;
}

static void hid_frame_tick(unsigned long frame) {
    if (!hid_shared_addr) return;
    uint32_t index = frame % ALBW_HID_ENTRIES;
    uint32_t scripted = scripted_buttons(frame);
    uint32_t held = hid_input.buttons | scripted;
    // A scripted push of the circle pad goes all the way, and no further on a diagonal than straight
    // ahead, as the pad's round gate has it.
    int16_t circle_x = hid_input.circle_x, circle_y = hid_input.circle_y;
    if (scripted & (ALBW_PAD_CIRCLE_RIGHT | ALBW_PAD_CIRCLE_LEFT | ALBW_PAD_CIRCLE_UP
                    | ALBW_PAD_CIRCLE_DOWN)) {
        int x = (scripted & ALBW_PAD_CIRCLE_RIGHT) ? 1 : (scripted & ALBW_PAD_CIRCLE_LEFT) ? -1 : 0;
        int y = (scripted & ALBW_PAD_CIRCLE_UP) ? 1 : (scripted & ALBW_PAD_CIRCLE_DOWN) ? -1 : 0;
        int reach = (x && y) ? ALBW_CIRCLE_MAX * 707 / 1000 : ALBW_CIRCLE_MAX;
        circle_x = (int16_t)(x * reach);
        circle_y = (int16_t)(y * reach);
    }

    uint32_t pad = hid_shared_addr;
    uint32_t entry = pad + ALBW_HID_PAD_RING + index * ALBW_HID_PAD_ENTRY;
    albw_w32(entry + 0, held);
    albw_w32(entry + 4, held & ~hid_held_before);       // pressed since the last sample
    albw_w32(entry + 8, hid_held_before & ~held);       // released since the last sample
    albw_w16(entry + 12, (uint16_t)circle_x);
    albw_w16(entry + 14, (uint16_t)circle_y);
    albw_w32(pad + ALBW_HID_PAD_CURRENT, held);
    hid_header(pad, index, frame);
    hid_held_before = held;

    uint32_t touch = hid_shared_addr + ALBW_HID_TOUCH;
    uint32_t tentry = touch + ALBW_HID_TOUCH_RING + index * ALBW_HID_TOUCH_ENTRY;
    int touching = hid_input.touching;
    uint16_t tx = touching ? hid_input.touch_x : 0;
    uint16_t ty = touching ? hid_input.touch_y : 0;
    if (!touching) touching = scripted_touch(frame, &tx, &ty);
    for (uint32_t at = tentry, n = 0; n < 2; n++, at = touch + ALBW_HID_TOUCH_RAW) {
        albw_w16(at + 0, tx);
        albw_w16(at + 2, ty);
        albw_w32(at + 4, touching ? 1u : 0u);
    }
    hid_header(touch, index, frame);

    for (int i = 0; i < 2; i++) {
        if (!hid_events[i]) continue;
        int h = handle_index(hid_events[i]);
        if (h >= 0) handles[h].signalled = 1;
        albw_thread_wake(ALBW_W_SYNC, hid_events[i], -1);
    }
}

// Pipe 2 carries the audio commands. Initialize is the one that matters: the firmware answers
// it with the region addresses, and until it does, the audio thread never gets past its wait.
static void dsp_pipe_write(uint32_t channel, uint32_t addr, uint32_t size) {
    if (channel != 2 || size < 2 || !albw_guest_ok(addr, size)) {
        printf("[dsp  ] ignoring a %u-byte write to pipe %u\n", size, channel);
        return;
    }
    uint16_t command = albw_r16(addr);
    static const char *names[] = { "Initialize", "Shutdown", "Wakeup", "Sleep" };
    printf("[dsp  ] pipe 2 command %u (%s)\n", command,
           command < 4 ? names[command] : "unknown");

    if (command == 0 || command == 2) {                 // Initialize, Wakeup
        uint8_t reply[2 + sizeof dsp_region_addr];
        uint16_t count = (uint16_t)(sizeof dsp_region_addr / 2);
        memcpy(reply, &count, 2);
        memcpy(reply + 2, dsp_region_addr, sizeof dsp_region_addr);
        // The firmware starts from cleared memory; Wakeup keeps what it had, which here is the
        // same as never having slept.
        if (command == 0) {
            memset(albw_mem + dsp_dram_to_guest(ALBW_DSP_REGION_BASE), 0, 0x8000);
            memset(albw_mem + dsp_dram_to_guest(ALBW_DSP_REGION_BASE | 0x10000u), 0, 0x8000);
        }
        albw_dsp_start(dsp_dram_to_guest(ALBW_DSP_REGION_BASE),
                       dsp_dram_to_guest(ALBW_DSP_REGION_BASE | 0x10000u));
        dsp_pipe_fill(2, reply, sizeof reply);
        printf("[dsp  ] answered with %u region address(es)\n", count);
    } else if (command == 1 || command == 3) {          // Shutdown, Sleep
        albw_dsp_stop();
    }
}

// Reads from a pipe into the static buffer the guest registered at TLS+0x180, which is where
// the wrapper at 0x003d2e40 puts it. Returns the number of bytes actually read.
static uint32_t dsp_pipe_read(const albw_cpu *c, uint32_t channel, uint32_t size) {
    if (channel >= ALBW_DSP_PIPES) return 0;
    uint32_t available = dsp_pipe_len[channel] - dsp_pipe_pos[channel];
    uint32_t n = size < available ? size : available;
    uint32_t dest = albw_r32(c->tls + 0x184);
    if (!n || !albw_guest_ok(dest, n)) return 0;
    memcpy(albw_mem + dest, dsp_pipe[channel] + dsp_pipe_pos[channel], n);
    dsp_pipe_pos[channel] += n;
    return n;
}

// Reads a NUL-terminated guest string, bounded so a bad pointer cannot run away.
static void guest_string(uint32_t addr, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        uint8_t c = albw_r8(addr + (uint32_t)i);
        if (!c) break;
        out[i] = (char)c;
    }
    out[i] = '\0';
}

unsigned long albw_svc_count(void) { return svc_count; }
unsigned long albw_frame_count(void) { return gsp_frames; }

// ------------------------------------------------------------- logging -----
//
// Rate limited, because the guest polls services in tight loops: an unbounded log of this
// reached 14.8 GB and filled the filesystem before the run even finished. The first few of
// each distinct event are what carry information; the rest is a count, which the summary
// prints at exit.

#define ALBW_LOG_KINDS 1024

typedef struct {
    char key[32];
    unsigned long count;
} albw_log_entry;

static albw_log_entry log_entries[ALBW_LOG_KINDS];
static int log_kinds;

// The caps keep a runaway log from filling the disk -- two earlier runs wrote 14.8 GB and
// 8.4 GB. They are settable because chasing a specific event means seeing every occurrence of
// it, and three is not enough to tell a sensible read pattern from a nonsensical one.
static unsigned long log_first = 3, log_total = 4000;

static void log_limits_from_env(void) {
    const char *f = albw_env("ALBW_LOG_FIRST"), *t = albw_env("ALBW_LOG_TOTAL");
    if (f) log_first = strtoul(f, NULL, 0);
    if (t) log_total = strtoul(t, NULL, 0);
}

#define ALBW_LOG_FIRST log_first
#define ALBW_LOG_TOTAL log_total

// Returns how many times this key has been seen, including now.
static unsigned long log_bump(const char *key) {
    for (int i = 0; i < log_kinds; i++) {
        if (!strcmp(log_entries[i].key, key)) return ++log_entries[i].count;
    }
    if (log_kinds >= ALBW_LOG_KINDS) {
        // Saying this once matters more than the events themselves: silently dropping every
        // *new* kind of event turns "it never happened" and "the table was full" into the same
        // observation, and the second one is not evidence of anything.
        static int warned;
        if (!warned) {
            warned = 1;
            printf("[log  ] %d kinds of event recorded; new kinds from here on are neither "
                   "logged nor counted\n", ALBW_LOG_KINDS);
        }
        return ALBW_LOG_FIRST + 1;
    }
    snprintf(log_entries[log_kinds].key, sizeof log_entries[0].key, "%s", key);
    log_entries[log_kinds].count = 1;
    log_kinds++;
    return 1;
}

// A global ceiling as well as a per-event one. The per-event limit alone was not enough:
// one unbounded line inside the srv: handler still produced an 8.4 GB log, because the
// guest polls in a loop and every iteration asked for a fresh service handle.
static unsigned long log_lines;

static int log_wanted(const char *key) {
    unsigned long n = log_bump(key);
    if (n > ALBW_LOG_FIRST) return 0;
    if (log_lines == ALBW_LOG_TOTAL) {
        printf("[log  ] %lu lines reached; only the summary follows\n", (unsigned long)ALBW_LOG_TOTAL);
        log_lines++;
        return 0;
    }
    if (log_lines > ALBW_LOG_TOTAL) return 0;
    log_lines++;
    if (n == ALBW_LOG_FIRST) {
        printf("[log  ] %s: further occurrences suppressed, counted in the summary\n", key);
    }
    return 1;
}

static void kernel_summary(void) {
    printf("\n[sum  ] %lu syscalls; events by frequency:\n", svc_count);
    for (;;) {
        int best = -1;
        for (int i = 0; i < log_kinds; i++) {
            if (log_entries[i].count == 0) continue;
            if (best < 0 || log_entries[i].count > log_entries[best].count) best = i;
        }
        if (best < 0) break;
        printf("[sum  ] %10lu  %s\n", log_entries[best].count, log_entries[best].key);
        log_entries[best].count = 0;
    }
}

// ALBW_DUMP_MEM=<frame>,<frame>,... writes the guest's writable memory at those frames into
// ALBW_DUMP_MEM_DIR (default "."): mem-<frame>-data.bin from 0x006FF000 (.data and .bss),
// mem-<frame>-heap.bin from 0x08000000 and mem-<frame>-linear.bin from 0x14000000, each as far as
// it is in use. For finding where the game keeps a value -- a position, a counter -- by comparing
// frames whose difference is known.
static void dump_memory_if_asked(void) {
    static const char *list;
    static int checked;
    if (!checked) {
        checked = 1;
        list = albw_env("ALBW_DUMP_MEM");
    }
    if (!list) return;
    int wanted = 0;
    for (const char *p = list; *p;) {
        char *end;
        unsigned long f = strtoul(p, &end, 0);
        if (end == p) break;
        if (f == gsp_frames) wanted = 1;
        p = *end == ',' ? end + 1 : end;
    }
    if (!wanted) return;
    const char *dir = albw_env("ALBW_DUMP_MEM_DIR");
    if (!dir) dir = ".";
    struct { const char *name; uint32_t from, to; } part[3] = {
        { "data", 0x006FF000u, 0x007B0000u },
        // The game commits its small heap at a fixed address, which heap_next does not follow.
        { "heap", ALBW_HEAP_BASE, heap_next > ALBW_HEAP_BASE + 0x100000u ? heap_next
                                                                        : ALBW_HEAP_BASE + 0x100000u },
        { "linear", ALBW_LINEAR_BASE, linear_next },
    };
    for (int i = 0; i < 3; i++) {
        char path[600];
        snprintf(path, sizeof path, "%s/mem-%06lu-%s.bin", dir, gsp_frames, part[i].name);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        fwrite(albw_mem + part[i].from, 1, part[i].to - part[i].from, f);
        fclose(f);
    }
    printf("[mem  ] frame %lu: memory written to %s\n", gsp_frames, dir);
}

// The ways a game can stop that a player would otherwise only see as a window that freezes or
// vanishes: said in the window as well as on the terminal, which a game started from a desktop
// does not have.
static void tell_player(const char *title, const char *text) {
#ifdef ALBW_HAVE_WINDOW
    albw_window_message(title, text);
#else
    (void)title;
    (void)text;
#endif
}

// A run that hangs is the normal case right now, so it gets killed by a timeout - and
// SIGTERM would skip atexit and take the summary with it, which is the only part of the
// output that says what the guest was waiting for.
static void on_signal(int sig) {
    printf("\n[sum  ] terminated by signal %d\n", sig);
    exit(0);
}

// Populates the kernel configuration page. Only the fields the guest actually reads are
// set, found by following its pc-relative loads of the page base:
//   +0x14  byte, tested against 1 - the update flag, which must stay clear
//   +0x40  word, returned verbatim by the guest's own accessor at 0x00102290 - APPMEMALLOC
static void config_pages_init(void) {
    memset(albw_mem + ALBW_KERNEL_CFG, 0, 0x1000);
    memset(albw_mem + ALBW_SHARED_CFG, 0, 0x1000);
    albw_w8(ALBW_KERNEL_CFG + 0x14, 0);
    albw_w32(ALBW_KERNEL_CFG + 0x40, ALBW_APP_MEMORY);
}

void albw_kernel_set_rom(const char *path) { rom_path = path; }

void albw_kernel_init(void) {
    log_limits_from_env();
    memset(handles, 0, sizeof handles);
    config_pages_init();
    atexit(kernel_summary);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
}

// Resource limits describe the process's share of the 64 MB an application gets in the
// O3DS production memory mode the exheader asks for. The names are Horizon's
// ResourceLimitType; only the ones the guest asks for need to be right, so unknown names
// report zero and say so in the log rather than inventing a number.
static uint64_t albw_resource_limit(uint32_t name, int want_maximum) {
    switch (name) {
        case 0:                                         // commit, in bytes
            return want_maximum ? ALBW_APP_MEMORY
                                : (uint64_t)(heap_next - ALBW_HEAP_BASE);
        case 1: return want_maximum ? 32 : 2;           // threads
        case 2: return want_maximum ? 8 : 0;            // events
        case 3: return want_maximum ? 8 : 0;            // mutexes
        case 4: return want_maximum ? 8 : 0;            // semaphores
        case 5: return want_maximum ? 8 : 0;            // timers
        case 6: return want_maximum ? 16 : 0;           // shared memory blocks
        case 7: return want_maximum ? 16 : 1;           // address arbiters
        case 8: return want_maximum ? 0x1000 : 0;       // cpu time
        default:
            printf("[svc ] ResourceLimit: unknown name %u, reporting 0\n", name);
            return 0;
    }
}

// ---------------------------------------------------------------- IPC ------

static void ipc_reply(const albw_cpu *c, uint32_t command_id, uint32_t result) {
    uint32_t cb = albw_cmdbuf(c);
    albw_w32(cb, (command_id << 16) | (1u << 6));
    albw_w32(cb + 4, result);
}

static uint32_t fs_open_host_file(const albw_cpu *c, uint32_t archive_id, uint32_t path_type,
                                  uint32_t ptr, uint32_t len, int create) {
    (void)c;
    char path[256];
    uint32_t n = len < sizeof path - 1 ? len : (uint32_t)sizeof path - 1;
    if (!albw_guest_ok(ptr, n)) return 0;
    uint32_t w = 0;
    if (path_type == ALBW_PATH_UTF16) {
        // Only the Basic Latin range appears in these names; anything else becomes a
        // placeholder rather than half a character.
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            uint16_t unit = albw_r16(ptr + i);
            if (!unit) break;
            path[w++] = unit < 0x80 ? (char)unit : '_';
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint8_t ch = albw_r8(ptr + i);
            if (!ch) break;                             // counted, but often terminated too
            path[w++] = (char)ch;
        }
    }
    path[w] = '\0';

    if (log_wanted("fs host path")) {
        printf("[fs   ] path for archive %u: type %u, %u byte(s) at %08x =",
               archive_id, path_type, len, ptr);
        for (uint32_t i = 0; i < n && i < 32; i++) printf(" %02x", albw_r8(ptr + i));
        printf("  \"%s\"\n", path);
    }

    int fd = albw_save_open(archive_id, path, create);
    if (fd < 0) return 0;

    uint32_t fh = handle_create(ALBW_H_FILE, "hostfile");
    int fi = handle_index(fh);
    if (fi < 0) {
        close(fd);
        return 0;
    }
    handles[fi].fd = fd;
    handles[fi].base = 0;
    handles[fi].size = albw_save_size(fd);
    return fh;
}

// ---------------------------------------------------------------------------------------------
// The parameter an applet sends back, held until the game receives it.
#define APT_APPID_SWKBD          0x401u
#define APT_COMMAND_WAKEUP_EXIT  10u
#define APT_PARAM_MAX            0x1000u

#define APT_COMMAND_REQUEST      2u
#define APT_COMMAND_RESPONSE     3u

static int apt_param_pending;
static uint32_t apt_param_sender, apt_param_signal, apt_param_size, apt_param_handle;
static uint8_t apt_param_data[APT_PARAM_MAX];

// The reply to ReceiveParameter or GlanceParameter with a parameter in hand: sender, signal, size,
// no handle, and the bytes copied into the static buffer the receiving thread registered at
// TLS+0x180. Read off libctru's wrapper, which talks to the real service.
static void apt_deliver_parameter(const albw_cpu *c, uint32_t command) {
    const uint32_t ALBW_CMDBUF = albw_cmdbuf(c);
    uint32_t want = albw_r32(ALBW_CMDBUF + 8);
    uint32_t dest = albw_r32(c->tls + 0x184);
    uint32_t n = apt_param_size < want ? apt_param_size : want;
    if (dest && albw_guest_ok(dest, n)) {
        for (uint32_t i = 0; i < n; i++) albw_w8(dest + i, apt_param_data[i]);
    }
    albw_w32(ALBW_CMDBUF, (command << 16) | (4u << 6) | 4u);
    albw_w32(ALBW_CMDBUF + 4, 0);
    albw_w32(ALBW_CMDBUF + 8, apt_param_sender);
    albw_w32(ALBW_CMDBUF + 12, apt_param_signal);
    albw_w32(ALBW_CMDBUF + 16, n);
    albw_w32(ALBW_CMDBUF + 20, apt_param_handle ? 0x10u : 0u);   // handle descriptor: move one
    albw_w32(ALBW_CMDBUF + 24, apt_param_handle);
    albw_w32(ALBW_CMDBUF + 28, (n << 14) | 2u);         // the static buffer, handed back
    albw_w32(ALBW_CMDBUF + 32, dest);
    if (log_wanted("apt parameter")) {
        printf("[apt  ] parameter from %03x, signal %u, %u bytes\n", apt_param_sender,
               apt_param_signal, n);
    }
}

// The software keyboard's parameter block, laid out as libctru's SwkbdState -- which talks to the
// real applet and so has to match it -- with ARM's four-byte size_t.
#define SWKBD_BUTTONS_M1     0x004u                     // the number of buttons, less one
#define SWKBD_MAX_TEXT_LEN   0x020u
#define SWKBD_INITIAL_TEXT   0x120u                     // offset into the block, or -1
#define SWKBD_SHM_SIZE       0x130u
#define SWKBD_RESULT         0x138u
#define SWKBD_TEXT_OFFSET    0x144u
#define SWKBD_TEXT_LENGTH    0x148u

// The software keyboard, without a keyboard: it answers as if the player had confirmed the text the
// game offered, or the console's user name when it offered none. Typing a name for real belongs to
// the window, later; this is what lets the game get past its name entry at all.
static void apt_swkbd(uint32_t config, uint32_t size, uint32_t block) {
    int bi = handle_index(block);
    uint32_t shm = bi >= 0 ? (uint32_t)handles[bi].base : 0;
    uint32_t shm_size = bi >= 0 ? (uint32_t)handles[bi].size : 0;
    if (!albw_guest_ok(config, size) || size < 0x14C || !shm) {
        printf("[apt  ] software keyboard: no usable parameter block or text buffer\n");
        return;
    }
    uint32_t max_len = albw_r16(config + SWKBD_MAX_TEXT_LEN);
    if (!max_len || max_len > 64) max_len = 64;

    uint16_t text[65];
    uint32_t n = 0;
    int32_t initial = (int32_t)albw_r32(config + SWKBD_INITIAL_TEXT);
    if (initial >= 0 && (uint32_t)initial + 2u <= shm_size) {
        for (; n < max_len && (uint32_t)initial + n * 2u + 2u <= shm_size; n++) {
            uint16_t ch = albw_r16(shm + (uint32_t)initial + n * 2u);
            if (!ch) break;
            text[n] = ch;
        }
    }
    if (!n) {
        char name[11];
        albw_config_username(name);
        for (; name[n] && n < max_len; n++) text[n] = (uint8_t)name[n];
    }
    // Into the shared block at offset 0, terminated, as the applet writes it.
    for (uint32_t i = 0; i < n; i++) albw_w16(shm + i * 2u, text[i]);
    albw_w16(shm + n * 2u, 0);

    // The confirming button is the rightmost one, whose code depends on how many there are.
    static const int32_t confirm[3] = { 0, 2, 5 };
    uint32_t buttons = albw_r32(config + SWKBD_BUTTONS_M1);
    albw_w32(config + SWKBD_RESULT, (uint32_t)confirm[buttons < 3 ? buttons : 2]);
    albw_w32(config + SWKBD_TEXT_OFFSET, 0);
    albw_w16(config + SWKBD_TEXT_LENGTH, (uint16_t)n);

    char shown[65];
    for (uint32_t i = 0; i < n; i++) shown[i] = text[i] < 0x80 ? (char)text[i] : '?';
    shown[n] = '\0';
    printf("[apt  ] software keyboard: confirming \"%s\"\n", shown);
}

// Queues a parameter for the game and wakes whoever waits on APT's parameter event.
static void apt_queue_parameter(uint32_t sender, uint32_t signal, const uint8_t *data, uint32_t size,
                                uint32_t handle) {
    apt_param_sender = sender;
    apt_param_signal = signal;
    apt_param_size = size < APT_PARAM_MAX ? size : APT_PARAM_MAX;
    if (data) memcpy(apt_param_data, data, apt_param_size);
    apt_param_handle = handle;
    apt_param_pending = 1;
    // The second of the events APT handed out at Initialize is the one for parameters. The first is
    // for notifications -- the HOME button and the like -- and signalling it for a parameter makes
    // the game's applet thread ask InquireNotification what happened and fail its own assertion.
    if (apt_resume_event) {
        int h = handle_index(apt_resume_event);
        if (h >= 0) handles[h].signalled = 1;
        albw_thread_wake(ALBW_W_SYNC, apt_resume_event, -1);
    }
}

// Runs a library applet to completion at once and queues the parameter it sends back.
static void apt_run_applet(uint32_t appid, uint32_t config, uint32_t size, uint32_t block) {
    printf("[apt  ] frame %lu: starting library applet %03x with a %u-byte parameter\n", gsp_frames,
           appid, size);
    if (appid == APT_APPID_SWKBD) apt_swkbd(config, size, block);

    static uint8_t answer[APT_PARAM_MAX];
    uint32_t n = size < APT_PARAM_MAX ? size : APT_PARAM_MAX;
    for (uint32_t i = 0; i < n; i++) answer[i] = albw_guest_ok(config + i, 1) ? albw_r8(config + i) : 0;
    // The applet is done the moment it starts; its parameter block, with the answer filled in, is
    // what it sends back.
    apt_queue_parameter(appid, APT_COMMAND_WAKEUP_EXIT, answer, n, 0);
}

static void ipc_dispatch(albw_cpu *c, uint32_t session) {
    const uint32_t ALBW_CMDBUF = albw_cmdbuf(c);
    int idx = handle_index(session);
    const char *service = idx >= 0 ? handles[idx].name : "<bad handle>";

    uint32_t hdr = albw_r32(ALBW_CMDBUF);
    uint32_t command_id = hdr >> 16;
    uint32_t normal = (hdr >> 6) & 0x3Fu;
    uint32_t translate = hdr & 0x3Fu;

    char key[32];
    snprintf(key, sizeof key, "ipc %s 0x%04x", service, command_id);
    if (log_wanted(key)) {
        printf("[ipc ] @%-10lu %-10s cmd=0x%04x normal=%u translate=%u",
               albw_instruction_count(), service, command_id, normal, translate);
        for (uint32_t i = 1; i <= normal && i <= 6; i++) {
            printf(" p%u=%08x", i, albw_r32(ALBW_CMDBUF + 4 * i));
        }
        printf("\n");
    }

    // srv:EnableNotification returns the semaphore a listener thread parks on. The wrapper
    // at 0x001029dc takes it from cmdbuf[3]; the caller at 0x00101528 stores it in a global
    // and then creates the thread that waits on it. An empty reply left that global at zero,
    // so the thread waited on handle 0 and the guest failed its own assertion.
    //
    // It starts unsignalled on purpose: nothing has notified the application yet, so that
    // thread blocking is correct. If it ends up the only thread left, the scheduler reports
    // a deadlock, which is a true statement about this kernel rather than a spin.
    if (!strcmp(service, "srv:") && command_id == 0x0002) {
        if (!srv_notification) {
            srv_notification = handle_create(ALBW_H_EVENT, "srv-notify");
            int i = handle_index(srv_notification);
            if (i >= 0) handles[i].reset_type = 0;      // consumed by the wait it releases
        }
        albw_w32(ALBW_CMDBUF, (0x0002u << 16) | (1u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);                   // result
        albw_w32(ALBW_CMDBUF + 8, 0);                   // handle descriptor
        albw_w32(ALBW_CMDBUF + 12, srv_notification);
        printf("[ipc ] srv:EnableNotification -> semaphore 0x%08x\n", srv_notification);
        c->r[0] = 0;
        return;
    }

    // srv: hands out the sessions for every other service, so it has to work before any
    // of them can. GetServiceHandle is the one command that returns a handle.
    if (!strcmp(service, "srv:") && command_id == 0x0005) {
        char name[9];
        for (int i = 0; i < 8; i++) {
            name[i] = (char)albw_r8(ALBW_CMDBUF + 4 + (uint32_t)i);
        }
        name[8] = '\0';
        uint32_t h = handle_create(ALBW_H_SESSION, name);
        char gkey[48];
        snprintf(gkey, sizeof gkey, "srv:GetServiceHandle %s", name);
        if (log_wanted(gkey)) {
            printf("[ipc ] srv:GetServiceHandle(\"%s\") -> handle 0x%08x\n", name, h);
        }
        albw_w32(ALBW_CMDBUF, (0x0005u << 16) | (1u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, 0);     // translate descriptor: move one handle
        albw_w32(ALBW_CMDBUF + 12, h);
        c->r[0] = 0;
        return;
    }

    // fs:USER. The game's data lives in the RomFS of its own card image, so file access is
    // where the port stops being a CPU exercise. Request layout read off the wrapper at
    // 0x0010f7ec, which fills cmdbuf[2..3] with the archive id, [6] and [7] with the file
    // path type and length, and [12] with the path pointer.
    // fs:USER OpenArchive(archiveId, path type, path size, path). The guest opens the RomFS
    // both ways: directly as a file, and as an archive it then opens files inside. Answering
    // the second with an empty reply left it reading the archive handle out of whatever the
    // command buffer happened to hold -- it went on to call Read, GetSize and Close on the
    // value 0x0000000c, and a later seek against the size that came back failed with a fatal
    // error 250,000 instructions away from the cause.
    if (!strcmp(service, "fs:USER") && command_id == 0x080c) {
        uint32_t archive_id = albw_r32(ALBW_CMDBUF + 4);
        if (archive_id == ALBW_ARCHIVE_SAVEDATA && !albw_save_format_info(archive_id, NULL)) {
            // The cartridge's save, never formatted. Opening it anyway -- which this did, always --
            // contradicted GetFormatInfo's answer, and the game took the contradiction as leave to
            // read whatever files were lying in the directory instead of formatting.
            ipc_reply(c, command_id, ALBW_FS_NOT_FORMATTED);
            c->r[0] = 0;
            return;
        }
        if (archive_id != ALBW_ARCHIVE_ROMFS) {
            // Anything that is not the card image is backed by a directory on the host. The
            // guest treats a missing archive as fatal -- refusing shared extdata 0xF000000B,
            // the console's word filter, ended the boot -- while an archive that is present
            // and empty is the situation on a console that has never run the game.
            uint32_t ah = handle_create(ALBW_H_ARCHIVE, "host-arch");
            int ai = handle_index(ah);
            if (ai >= 0) handles[ai].archive_id = archive_id;
            albw_w32(ALBW_CMDBUF, (0x080cu << 16) | (3u << 6));
            albw_w32(ALBW_CMDBUF + 4, 0);
            albw_w32(ALBW_CMDBUF + 8, ah);
            albw_w32(ALBW_CMDBUF + 12, 0);
            printf("[fs   ] OpenArchive %u -> host archive 0x%08x\n", archive_id, ah);
            c->r[0] = 0;
            return;
        }
        if (!albw_romfs_open(rom_path)) {
            ipc_reply(c, command_id, ALBW_FS_NOT_FOUND);
            c->r[0] = 0;
            return;
        }
        uint32_t ah = handle_create(ALBW_H_ARCHIVE, "romfs-arch");
        int ai = handle_index(ah);
        if (ai >= 0) handles[ai].archive_id = archive_id;
        // The archive handle is a u64 in cmdbuf[2..3], not a translated handle.
        albw_w32(ALBW_CMDBUF, (0x080cu << 16) | (3u << 6));
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, ah);
        albw_w32(ALBW_CMDBUF + 12, 0);
        printf("[fs   ] OpenArchive %u -> archive 0x%08x\n", archive_id, ah);
        c->r[0] = 0;
        return;
    }

    // fs:USER CreateFile(transaction, archive u64, path type, path size, attributes,
    // size:u64, path). Only host-backed archives can have files made in them; the card image
    // cannot, and saying so is better than reporting a success that wrote nothing.
    if (!strcmp(service, "fs:USER") && command_id == 0x0808) {
        uint32_t ah = albw_r32(ALBW_CMDBUF + 8);
        uint32_t path_size = albw_r32(ALBW_CMDBUF + 20);
        int ai = handle_index(ah);
        if (ai < 0 || handles[ai].kind != ALBW_H_ARCHIVE
                || handles[ai].archive_id == ALBW_ARCHIVE_ROMFS) {
            ipc_reply(c, command_id, ALBW_FS_NOT_FOUND);
            c->r[0] = 0;
            return;
        }
        uint32_t fh = fs_open_host_file(c, handles[ai].archive_id,
                                        albw_r32(ALBW_CMDBUF + 16),
                                        albw_r32(ALBW_CMDBUF + 40), path_size, 1);
        if (fh) {
            int fi = handle_index(fh);
            uint64_t want = (uint64_t)albw_r32(ALBW_CMDBUF + 28)
                          | ((uint64_t)albw_r32(ALBW_CMDBUF + 32) << 32);
            if (fi >= 0 && want && albw_ftruncate(handles[fi].fd, want) == 0) {
                handles[fi].size = want;
            }
            // CreateFile hands back no handle, so the file it just made is closed again.
            if (fi >= 0) {
                albw_save_closed(handles[fi].fd);
                close(handles[fi].fd);
                handles[fi].fd = 0;
                handles[fi].kind = ALBW_H_FREE;
            }
        }
        ipc_reply(c, command_id, fh ? 0 : ALBW_FS_NOT_FOUND);
        c->r[0] = 0;
        return;
    }

    // fs:USER ControlArchive(archive u64, action, input size, output size, buffers). Action 0
    // commits a save archive: what the game wrote since its last commit becomes the save, all of
    // it at once (albw_savedata.c). The game commits after every save, and after formatting.
    if (!strcmp(service, "fs:USER") && command_id == 0x080d) {
        uint32_t ah = albw_r32(ALBW_CMDBUF + 4), action = albw_r32(ALBW_CMDBUF + 12);
        int ai = handle_index(ah);
        int ok = 1;
        if (ai >= 0 && handles[ai].kind == ALBW_H_ARCHIVE && action == 0) {
            ok = albw_save_commit(handles[ai].archive_id);
        }
        ipc_reply(c, command_id, ok ? 0 : ALBW_FS_NOT_FOUND);
        c->r[0] = 0;
        return;
    }

    // fs:USER CloseArchive(archive handle u64).
    if (!strcmp(service, "fs:USER") && command_id == 0x080e) {
        uint32_t ah = albw_r32(ALBW_CMDBUF + 4);
        int ai = handle_index(ah);
        if (ai >= 0 && handles[ai].kind == ALBW_H_ARCHIVE) handles[ai].kind = ALBW_H_FREE;
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // fs:USER OpenFile(transaction, archive u64, path type, path size, flags, attributes).
    // Inside the RomFS archive there is one thing to open: the image itself, selected by the
    // twelve-byte binary path, which is the same request OpenFileDirectly makes in one step.
    if (!strcmp(service, "fs:USER") && command_id == 0x0802) {
        uint32_t ah = albw_r32(ALBW_CMDBUF + 8);
        uint32_t path_type = albw_r32(ALBW_CMDBUF + 16);
        uint32_t path_size = albw_r32(ALBW_CMDBUF + 20);
        int ai = handle_index(ah);
        if (ai < 0 || handles[ai].kind != ALBW_H_ARCHIVE) {
            printf("[fs   ] OpenFile against 0x%08x, which is not an archive handle\n", ah);
            ipc_reply(c, command_id, ALBW_FS_NOT_FOUND);
            c->r[0] = 0;
            return;
        }
        if (handles[ai].archive_id != ALBW_ARCHIVE_ROMFS) {
            // FS_OPEN_CREATE in the open flags means the guest is willing to have it made.
            uint32_t flags = albw_r32(ALBW_CMDBUF + 24);
            uint32_t fh2 = fs_open_host_file(c, handles[ai].archive_id, path_type,
                                             albw_r32(ALBW_CMDBUF + 36), path_size,
                                             (flags & 4u) != 0);
            if (!fh2) {
                ipc_reply(c, command_id, ALBW_FS_NOT_FOUND);
                c->r[0] = 0;
                return;
            }
            albw_w32(ALBW_CMDBUF, (0x0802u << 16) | (1u << 6) | 2u);
            albw_w32(ALBW_CMDBUF + 4, 0);
            albw_w32(ALBW_CMDBUF + 8, 0);
            albw_w32(ALBW_CMDBUF + 12, fh2);
            c->r[0] = 0;
            return;
        }
        uint32_t fh = handle_create(ALBW_H_FILE, "romfs");
        int fi = handle_index(fh);
        if (fi >= 0) {
            handles[fi].base = 0;
            handles[fi].size = albw_romfs_size();
        }
        albw_w32(ALBW_CMDBUF, (0x0802u << 16) | (1u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, 0);                   // handle descriptor
        albw_w32(ALBW_CMDBUF + 12, fh);
        printf("[fs   ] OpenFile in archive 0x%08x (path type %u, %u bytes) -> file 0x%08x\n",
               ah, path_type, path_size, fh);
        c->r[0] = 0;
        return;
    }

    if (!strcmp(service, "fs:USER") && command_id == 0x0803) {
        // Field order settled by the static-buffer descriptors, whose sizes have to match
        // the declared path lengths: the archive id is a u32 here, not the u64 that later
        // SDK versions use, which is why there are eight normal parameters and not nine.
        uint32_t archive_id = albw_r32(ALBW_CMDBUF + 8);
        uint32_t arch_path_type = albw_r32(ALBW_CMDBUF + 12);
        uint32_t file_path_type = albw_r32(ALBW_CMDBUF + 20);
        uint32_t file_path_size = albw_r32(ALBW_CMDBUF + 24);
        uint32_t open_flags = albw_r32(ALBW_CMDBUF + 28);

        if (log_wanted("fs OpenFileDirectly")) {
            printf("[fs   ] OpenFileDirectly archive=%u archpath=%u filepath=%u/%u "
                   "flags=0x%x\n", archive_id, arch_path_type, file_path_type,
                   file_path_size, open_flags);
        }
        // Archive 3 with a 12-byte zero path is the standard "my own RomFS" request.
        if (archive_id != 3) {
            printf("[fs   ] archive %u is not implemented\n", archive_id);
            ipc_reply(c, command_id, 0xC8804478u);         // not found
            c->r[0] = 0;
            return;
        }
        if (!albw_romfs_open(rom_path)) {
            ipc_reply(c, command_id, 0xC8804478u);
            c->r[0] = 0;
            return;
        }

        uint32_t fh = handle_create(ALBW_H_FILE, "romfs");
        int fi = handle_index(fh);
        if (fi >= 0) {
            handles[fi].base = 0;
            handles[fi].size = albw_romfs_size();
        }
        printf("[fs   ] opened RomFS as handle 0x%08x, %llu MiB\n", fh,
               (unsigned long long)(albw_romfs_size() >> 20));

        // The caller at 0x0010f7ec takes the handle from cmdbuf[3] and the result from
        // cmdbuf[1].
        albw_w32(ALBW_CMDBUF, (0x0803u << 16) | (1u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, 0);                   // move one handle
        albw_w32(ALBW_CMDBUF + 12, fh);
        c->r[0] = 0;
        return;
    }

    // Commands sent to an open file.
    if (idx >= 0 && handles[idx].kind == ALBW_H_FILE) {
        char fkey[48];
        snprintf(fkey, sizeof fkey, "file cmd 0x%04x", command_id);
        if (log_wanted(fkey)) {
            printf("[file ] cmd=0x%04x normal=%u translate=%u", command_id, normal,
                   translate);
            for (uint32_t i = 1; i <= normal + translate && i <= 10; i++) {
                printf(" p%u=%08x", i, albw_r32(ALBW_CMDBUF + 4 * i));
            }
            printf("\n");
        }

        switch (command_id) {
            case 0x0802: {                              // Read(offset:u64, size, buffer)
                uint64_t offset = (uint64_t)albw_r32(ALBW_CMDBUF + 4)
                                | ((uint64_t)albw_r32(ALBW_CMDBUF + 8) << 32);
                uint32_t want = albw_r32(ALBW_CMDBUF + 12);
                uint32_t desc = albw_r32(ALBW_CMDBUF + 16);
                uint32_t dest = albw_r32(ALBW_CMDBUF + 20);

                uint64_t size = handles[idx].size;
                uint32_t got = 0;
                if (!albw_guest_ok(dest, want)) {
                    printf("[file ] read into %08x+%u is outside guest memory\n", dest, want);
                    ipc_reply(c, command_id, 0xD8E007F7u);
                    c->r[0] = 0;
                    return;
                }
                if (handles[idx].fd > 0) {
                    albw_ssize r = albw_pread(handles[idx].fd, albw_mem + dest, want, offset);
                    got = r > 0 ? (uint32_t)r : 0;
                } else if (offset < size) {
                    uint64_t left = size - offset;
                    got = want < left ? want : (uint32_t)left;
                    memcpy(albw_mem + dest, albw_romfs_data() + handles[idx].base + offset,
                           got);
                }
                ft_reads++;
                ft_read_bytes += got;
                // A file read fills guest memory without going through a guest store, so the
                // record of where things were written would not know about it -- and data the GPU
                // reads arrives this way rather than being built a word at a time.
                if (got) ALBW_NOTE_WRITE(dest, got);
                const char *pd = albw_env("ALBW_PROBE_DMA");
                uint32_t pwant = pd ? (uint32_t)strtoul(pd, NULL, 0) : 0;
                if (pwant && got && pwant >= dest && pwant < dest + got) {
                    printf("[file ] frame %lu: a read of %u bytes at %llu fills %08x..%08x\n",
                           gsp_frames, got, (unsigned long long)offset, dest, dest + got);
                }
                if (log_wanted("file read")) {
                    printf("[file ] read %u of %u bytes at %llu -> %08x\n", got, want,
                           (unsigned long long)offset, dest);
                }
                albw_w32(ALBW_CMDBUF, (0x0802u << 16) | (2u << 6) | 2u);
                albw_w32(ALBW_CMDBUF + 4, 0);
                albw_w32(ALBW_CMDBUF + 8, got);
                albw_w32(ALBW_CMDBUF + 12, desc);       // the buffer is handed straight back
                albw_w32(ALBW_CMDBUF + 16, dest);
                c->r[0] = 0;
                return;
            }

            case 0x0804: {                              // GetSize
                if (handles[idx].fd > 0) handles[idx].size = albw_save_size(handles[idx].fd);
                albw_w32(ALBW_CMDBUF, (0x0804u << 16) | (3u << 6));
                albw_w32(ALBW_CMDBUF + 4, 0);
                albw_w32(ALBW_CMDBUF + 8, (uint32_t)handles[idx].size);
                albw_w32(ALBW_CMDBUF + 12, (uint32_t)(handles[idx].size >> 32));
                c->r[0] = 0;
                return;
            }

            case 0x0803: {                              // Write(offset:u64, size, buffer)
                uint64_t offset = (uint64_t)albw_r32(ALBW_CMDBUF + 4)
                                | ((uint64_t)albw_r32(ALBW_CMDBUF + 8) << 32);
                uint32_t want = albw_r32(ALBW_CMDBUF + 12);
                uint32_t src = albw_r32(ALBW_CMDBUF + 24);
                uint32_t wrote = 0;
                if (handles[idx].fd > 0 && albw_guest_ok(src, want)) {
                    albw_ssize r = albw_save_before_write(handles[idx].fd)
                              ? albw_pwrite(handles[idx].fd, albw_mem + src, want, offset) : -1;
                    if (r > 0) wrote = (uint32_t)r;
                } else if (handles[idx].fd <= 0) {
                    // The card image is read-only, and pretending otherwise would lose data
                    // silently rather than loudly.
                    printf("[file ] refusing a %u-byte write to a read-only file\n", want);
                    ipc_reply(c, command_id, ALBW_FS_NOT_FOUND);
                    c->r[0] = 0;
                    return;
                }
                albw_w32(ALBW_CMDBUF, (0x0803u << 16) | (2u << 6));
                albw_w32(ALBW_CMDBUF + 4, 0);
                albw_w32(ALBW_CMDBUF + 8, wrote);
                c->r[0] = 0;
                return;
            }

            case 0x0805: {                              // SetSize(size:u64)
                uint64_t size = (uint64_t)albw_r32(ALBW_CMDBUF + 4)
                              | ((uint64_t)albw_r32(ALBW_CMDBUF + 8) << 32);
                if (handles[idx].fd > 0 && albw_save_before_write(handles[idx].fd)
                        && albw_ftruncate(handles[idx].fd, size) == 0) {
                    handles[idx].size = size;
                }
                ipc_reply(c, command_id, 0);
                c->r[0] = 0;
                return;
            }

            case 0x080C: {                              // OpenLinkFile
                // A second handle onto the same file. The game asks for one when it hands a file
                // to its streaming thread, and the generic reply gave it no handle at all: it
                // kept the zero it found in the reply, and its own check at 0x00561354 --
                // "a file's handle is never zero" -- threw a fatal error. That was the whole of
                // the black screen after the loading fairy.
                uint32_t link = handle_create(ALBW_H_FILE, handles[idx].name);
                int li = handle_index(link);
                if (li < 0) {
                    ipc_reply(c, command_id, 0xD8600403u);   // out of resources, as CreateThread says it
                    c->r[0] = 0;
                    return;
                }
                handles[li].base = handles[idx].base;
                handles[li].size = handles[idx].size;
                handles[li].archive_id = handles[idx].archive_id;
                // A host file gets its own descriptor, so closing one handle leaves the other.
                handles[li].fd = handles[idx].fd > 0 ? dup(handles[idx].fd) : 0;
                albw_w32(ALBW_CMDBUF, (0x080Cu << 16) | (1u << 6) | 2u);
                albw_w32(ALBW_CMDBUF + 4, 0);
                albw_w32(ALBW_CMDBUF + 8, 0x10u);       // move one handle
                albw_w32(ALBW_CMDBUF + 12, link);
                if (log_wanted("file link")) {
                    printf("[file ] OpenLinkFile 0x%08x -> 0x%08x\n", session, link);
                }
                c->r[0] = 0;
                return;
            }

            case 0x0808:                                // Close
                if (handles[idx].fd > 0) {
                    albw_save_closed(handles[idx].fd);
                    close(handles[idx].fd);
                }
                handles[idx].fd = 0;
                handles[idx].kind = ALBW_H_FREE;
                ipc_reply(c, command_id, 0);
                c->r[0] = 0;
                return;

            default:
                ipc_reply(c, command_id, 0);
                c->r[0] = 0;
                return;
        }
    }

    // err:f is how the guest reports a fatal error of its own. Its ThrowFatalError request
    // carries the whole diagnosis, so print all of it: reaching this means something the
    // service layer returned was wrong, and the result code and faulting address say what.
    if (!strcmp(service, "err:f") && command_id == 0x0001) {
        uint32_t w1 = albw_r32(ALBW_CMDBUF + 4);
        printf("\n[err:f] the guest threw a fatal error\n");
        printf("[err:f]   type=%u rev=%u.%u\n", w1 & 0xFFu, (w1 >> 8) & 0xFFu, w1 >> 16);
        printf("[err:f]   result code = 0x%08x\n", albw_r32(ALBW_CMDBUF + 8));
        printf("[err:f]   raised at     0x%08x\n", albw_r32(ALBW_CMDBUF + 12));
        printf("[err:f]   process id  = 0x%08x\n", albw_r32(ALBW_CMDBUF + 16));
        printf("[err:f]   raw buffer:\n");
        for (uint32_t i = 0; i < 32; i += 4) {
            printf("[err:f]     +%02x  %08x %08x %08x %08x\n", i * 4,
                   albw_r32(ALBW_CMDBUF + 4 + i * 4), albw_r32(ALBW_CMDBUF + 8 + i * 4),
                   albw_r32(ALBW_CMDBUF + 12 + i * 4), albw_r32(ALBW_CMDBUF + 16 + i * 4));
        }
        // A failure error carries a message instead of register state.
        char msg[0x61];
        for (int i = 0; i < 0x60; i++) {
            uint8_t ch = albw_r8(ALBW_CMDBUF + 0x24 + (uint32_t)i);
            msg[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
        }
        msg[0x60] = '\0';
        printf("[err:f]   text at +20: %s\n\n", msg);
        char text[400];
        snprintf(text, sizeof text,
                 "The game stopped with a fatal error of its own: result code 0x%08x, raised at "
                 "0x%08x.\n\nThat usually means the port answered one of its requests wrongly. "
                 "Anything since your last save is lost; please report the code and the address.",
                 albw_r32(ALBW_CMDBUF + 8), albw_r32(ALBW_CMDBUF + 12));
        tell_player("World Across: the game stopped", text);
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // dsp::DSP RegisterInterruptEvents. The wrapper at 0x003d2b18 passes (event, interrupt,
    // channel) and the marshalling puts the handle in the translate descriptor's slot; the
    // audio thread then waits on that very event, so this is the one binding that decides
    // whether it ever wakes.
    if (!strcmp(service, "dsp::DSP") && command_id == 0x0015) {
        uint32_t interrupt = albw_r32(ALBW_CMDBUF + 4);
        uint32_t channel = albw_r32(ALBW_CMDBUF + 8);
        uint32_t ev = albw_r32(ALBW_CMDBUF + 16);
        if (channel < ALBW_DSP_PIPES) dsp_pipe_event[channel] = ev;
        printf("[dsp  ] interrupt %u, pipe %u -> event 0x%08x\n", interrupt, channel, ev);
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // dsp::DSP WriteProcessPipe(channel, size, static buffer). Layout from the marshalling at
    // 0x003d2df8: cmdbuf[1] channel, cmdbuf[2] size, cmdbuf[3] a static buffer descriptor,
    // cmdbuf[4] the pointer.
    if (!strcmp(service, "dsp::DSP") && command_id == 0x000d) {
        dsp_pipe_write(albw_r32(ALBW_CMDBUF + 4), albw_r32(ALBW_CMDBUF + 16),
                       albw_r32(ALBW_CMDBUF + 8));
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // dsp::DSP ReadPipe / ReadPipeIfPossible. Both answer the same way here, because a pipe
    // that has nothing to give is not something this kernel can wait for: the data only ever
    // arrives from a write we already answered. cmdbuf[3]'s low halfword is the size
    // (0x003d2e40), and the reply reports bytes read in cmdbuf[2].
    if (!strcmp(service, "dsp::DSP") && (command_id == 0x000e || command_id == 0x0010)) {
        uint32_t channel = albw_r32(ALBW_CMDBUF + 4);
        uint32_t size = albw_r32(ALBW_CMDBUF + 12) & 0xFFFFu;
        uint32_t got = dsp_pipe_read(c, channel, size);
        albw_w32(ALBW_CMDBUF, (command_id << 16) | (2u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);                   // result
        albw_w32(ALBW_CMDBUF + 8, got);                 // bytes read
        albw_w32(ALBW_CMDBUF + 12, (got << 14) | 2u);   // the static buffer we filled
        albw_w32(ALBW_CMDBUF + 16, albw_r32(c->tls + 0x184));
        printf("[dsp  ] read %u of %u byte(s) from pipe %u\n", got, size, channel);
        c->r[0] = 0;
        return;
    }

    // dsp::DSP GetPipeReadableSize.
    if (!strcmp(service, "dsp::DSP") && command_id == 0x000f) {
        uint32_t channel = albw_r32(ALBW_CMDBUF + 4);
        uint32_t available = channel < ALBW_DSP_PIPES
                           ? dsp_pipe_len[channel] - dsp_pipe_pos[channel] : 0;
        albw_w32(ALBW_CMDBUF, (0x000fu << 16) | (2u << 6));
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, available);
        c->r[0] = 0;
        return;
    }

    // dsp::DSP ConvertProcessAddressFromDspDram. Called twice per region address, once per
    // bank, and the results are the pointers the game writes its audio through, so they have
    // to land in real memory rather than merely be plausible.
    if (!strcmp(service, "dsp::DSP") && command_id == 0x000c) {
        uint32_t addr = albw_r32(ALBW_CMDBUF + 4);
        albw_w32(ALBW_CMDBUF, (0x000cu << 16) | (2u << 6));
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, dsp_dram_to_guest(addr));
        c->r[0] = 0;
        return;
    }

    // dsp::DSP SetSemaphoreMask. Recorded rather than acted on: the mask selects which DSP
    // interrupts reach the semaphore event, and we only ever raise the one.
    if (!strcmp(service, "dsp::DSP") && command_id == 0x0017) {
        dsp_semaphore_mask = albw_r32(ALBW_CMDBUF + 4);
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // dsp::DSP GetSemaphoreEventHandle. The wrapper at 0x003d2ea8 takes the handle from
    // cmdbuf[3]; an empty reply gave the audio thread a zero to wait on.
    if (!strcmp(service, "dsp::DSP") && command_id == 0x0016) {
        if (!dsp_event) dsp_event = handle_create(ALBW_H_EVENT, "dsp-semaphore");
        albw_w32(ALBW_CMDBUF, (0x0016u << 16) | (1u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);                   // result
        albw_w32(ALBW_CMDBUF + 8, 0);                   // handle descriptor
        albw_w32(ALBW_CMDBUF + 12, dsp_event);
        printf("[dsp  ] semaphore event 0x%08x\n", dsp_event);
        c->r[0] = 0;
        return;
    }

    // gsp::Gpu FlushDataCache / InvalidateDataCache. Nothing to do: the guest and the device
    // share one flat memory here, so a cache maintenance call has nothing to maintain. It is
    // answered explicitly rather than through the generic reply because the guest makes 167 of
    // them before the first frame, and a service call that is silently generic is one nobody
    // has looked at.
    if (!strcmp(service, "gsp::Gpu") && (command_id == 0x0008 || command_id == 0x0009)) {
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // gsp::Gpu TriggerCmdReqQueue. The guest submits a command list and then spins on a flag
    // that its own interrupt handler clears (the submission is at 0x006013f0, the spin at
    // 0x00504ff0). Nothing in that loop enters the kernel, so deferring the completion to the
    // next idle frame deadlocks: the spinning thread stays runnable and idle never comes.
    // Carrying the queue out here, at the moment of submission, is also the truthful model --
    // there is no GPU pipeline to be asynchronous about.
    if (!strcmp(service, "gsp::Gpu") && command_id == 0x000c) {
        int commands = gsp_run_command_queue();
        int woken = gsp_deliver_interrupts();
        if (log_wanted("gx trigger")) {
            printf("[gsp  ] command queue triggered: %d command(s), woke %d thread(s)\n",
                   commands, woken);
        }
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // gsp::Gpu RegisterInterruptRelayQueue. The wrapper at 0x003e3eb0 passes the app's own
    // event handle in cmdbuf[3] and reads the thread index from cmdbuf[2] and the shared
    // memory handle from cmdbuf[4].
    if (!strcmp(service, "gsp::Gpu") && command_id == 0x0013) {
        gsp_event = albw_r32(ALBW_CMDBUF + 12);
        if (!gsp_shared) {
            gsp_shared = handle_create(ALBW_H_MEMBLOCK, "gsp-shared");
        }
        albw_w32(ALBW_CMDBUF, (0x0013u << 16) | (2u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);                   // result
        albw_w32(ALBW_CMDBUF + 8, gsp_thread_index);    // our slot in the interrupt queue
        albw_w32(ALBW_CMDBUF + 12, 0);                  // handle descriptor
        albw_w32(ALBW_CMDBUF + 16, gsp_shared);
        printf("[gsp  ] interrupt event 0x%08x, shared block 0x%08x, thread slot %u\n",
               gsp_event, gsp_shared, gsp_thread_index);
        c->r[0] = 0;
        return;
    }

    // hid:USER GetIpcHandles. The wrapper at 0x00104710 reads six handles from cmdbuf[3]
    // onwards: the shared memory block the pad state lives in, then five events. Answering
    // with an empty reply gave the guest zeros, which it then tried to wait on.
    if (!strcmp(service, "hid:USER") && command_id == 0x000A) {
        if (!hid_shared) {
            hid_shared = handle_create(ALBW_H_MEMBLOCK, "hid-shared");
            for (int i = 0; i < 5; i++) {
                hid_events[i] = handle_create(ALBW_H_EVENT, "hid-event");
                int j = handle_index(hid_events[i]);
                if (j >= 0) {
                    // Sticky and signalled: input state is always available to read, so a
                    // wait on these should proceed rather than park forever.
                    handles[j].signalled = 1;
                    handles[j].reset_type = 1;
                }
            }
        }
        albw_w32(ALBW_CMDBUF, (0x000Au << 16) | (1u << 6) | 14u);
        albw_w32(ALBW_CMDBUF + 4, 0);                   // result
        albw_w32(ALBW_CMDBUF + 8, 0);                   // handle descriptor, six handles
        albw_w32(ALBW_CMDBUF + 12, hid_shared);
        for (int i = 0; i < 5; i++) {
            albw_w32(ALBW_CMDBUF + 16 + 4u * (uint32_t)i, hid_events[i]);
        }
        printf("[ipc ] hid:USER GetIpcHandles -> shared 0x%08x + 5 events\n", hid_shared);
        c->r[0] = 0;
        return;
    }

    // APT's two handle-returning commands. Reply layouts read off the guest's wrappers:
    // GetLockHandle at 0x00103004 takes the lock from cmdbuf[5], and Initialize at
    // 0x00102fb4 takes two event handles from cmdbuf[3] and cmdbuf[4]. Returning an empty
    // reply left the guest waiting on handle 0 and failing its own assertion.
    if (!strcmp(service, "APT:U") && command_id == 0x0001) {
        if (!apt_lock) {
            apt_lock = handle_create(ALBW_H_MUTEX, "apt-lock");
            int i = handle_index(apt_lock);
            if (i >= 0) handles[i].signalled = 1;       // free to take
        }
        albw_w32(ALBW_CMDBUF, (0x0001u << 16) | (3u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);                   // result
        albw_w32(ALBW_CMDBUF + 8, 0);                   // applet attributes
        albw_w32(ALBW_CMDBUF + 12, 0);                  // apt state
        albw_w32(ALBW_CMDBUF + 16, 0);                  // handle descriptor
        albw_w32(ALBW_CMDBUF + 20, apt_lock);
        printf("[ipc ] APT:U GetLockHandle -> mutex 0x%08x\n", apt_lock);
        c->r[0] = 0;
        return;
    }
    if (!strcmp(service, "APT:U") && command_id == 0x0002) {
        if (!apt_notify_event) {
            apt_notify_event = handle_create(ALBW_H_EVENT, "apt-notify");
            apt_resume_event = handle_create(ALBW_H_EVENT, "apt-resume");
            // On hardware the Home Menu signals resume to let the application run; this is
            // the same permission the wakeup parameter grants.
            int i = handle_index(apt_resume_event);
            if (i >= 0) {
                handles[i].signalled = 1;
                handles[i].reset_type = 1;              // sticky
            }
        }
        albw_w32(ALBW_CMDBUF, (0x0002u << 16) | (1u << 6) | 4u);
        albw_w32(ALBW_CMDBUF + 4, 0);                   // result
        albw_w32(ALBW_CMDBUF + 8, 0);                   // handle descriptor, two handles
        albw_w32(ALBW_CMDBUF + 12, apt_notify_event);
        albw_w32(ALBW_CMDBUF + 16, apt_resume_event);
        printf("[ipc ] APT:U Initialize -> notify 0x%08x, resume 0x%08x\n",
               apt_notify_event, apt_resume_event);
        c->r[0] = 0;
        return;
    }

    // Library applets: the system's software keyboard, its error display, and the rest, each a
    // separate program on a console. The game prepares one, polls IsRegistered until it is ready,
    // starts it with a parameter block, and waits for it to send a parameter back -- the generic
    // reply said "not registered" for ever, and the game sat at its name entry polling.
    if (!strcmp(service, "APT:U") && command_id == 0x0009) {         // IsRegistered(appid)
        albw_w32(ALBW_CMDBUF, (0x0009u << 16) | (2u << 6));
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, 1);                 // every applet the game asks for is there
        c->r[0] = 0;
        return;
    }
    // SendParameter(source, destination, signal, size, handle, buffer). Before starting the keyboard
    // the game sends it a request carrying the size of the buffer it wants for a capture of the
    // screens, and the applet answers with a memory block of that size; the game then maps it and
    // copies the screens into it, for the keyboard to draw over. The generic reply sent nothing back,
    // and the game waited for the answer for good.
    if (!strcmp(service, "APT:U") && command_id == 0x000C) {
        uint32_t dest = albw_r32(ALBW_CMDBUF + 8);
        uint32_t signal = albw_r32(ALBW_CMDBUF + 12);
        uint32_t size = albw_r32(ALBW_CMDBUF + 16);
        uint32_t buffer = albw_r32(ALBW_CMDBUF + 32);
        if (log_wanted("apt send")) {
            printf("[apt  ] the game sends applet %03x signal %u with %u bytes\n", dest, signal, size);
        }
        if (dest == APT_APPID_SWKBD && signal == APT_COMMAND_REQUEST) {
            uint32_t want = size >= 4 && albw_guest_ok(buffer, 4) ? albw_r32(buffer) : 0;
            uint32_t block = handle_create(ALBW_H_MEMBLOCK, "applet-capture");
            int bi = handle_index(block);
            if (bi >= 0) handles[bi].size = want;
            apt_queue_parameter(dest, APT_COMMAND_RESPONSE, NULL, 0, block);
        }
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }
    if (!strcmp(service, "APT:U") && command_id == 0x0018) {         // PrepareToStartLibraryApplet
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }
    if (!strcmp(service, "APT:U") && command_id == 0x001E) {         // StartLibraryApplet
        uint32_t appid = albw_r32(ALBW_CMDBUF + 4);
        uint32_t size = albw_r32(ALBW_CMDBUF + 8);
        uint32_t block = albw_r32(ALBW_CMDBUF + 16);
        uint32_t config = albw_r32(ALBW_CMDBUF + 24);
        apt_run_applet(appid, config, size, block);
        ipc_reply(c, command_id, 0);
        c->r[0] = 0;
        return;
    }

    // APT is the application lifecycle service. A launching app polls ReceiveParameter
    // until the Home Menu sends it a wakeup, so with an empty reply it spins forever -
    // 201 million of those calls in one minute before this existed.
    if (!strcmp(service, "APT:U") && command_id == 0x000D) {
        // Response layout read off the guest's own wrapper at 0x0010b1b4, which stores
        // cmdbuf[2] as the sender app id, [3] as the command, [4] as the size and [6] as
        // a handle, then returns cmdbuf[1] as the result.
        if (apt_param_pending) {
            apt_deliver_parameter(c, 0x000Du);
            apt_param_pending = 0;
            c->r[0] = 0;
            return;
        }
        uint32_t signal = apt_wakeup_sent ? APT_COMMAND_NONE : APT_COMMAND_WAKEUP;
        apt_wakeup_sent = 1;
        albw_w32(ALBW_CMDBUF, (0x000Du << 16) | (4u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);              // result
        albw_w32(ALBW_CMDBUF + 8, APT_APPID_HOMEMENU);
        albw_w32(ALBW_CMDBUF + 12, signal);
        albw_w32(ALBW_CMDBUF + 16, 0);             // parameter size
        albw_w32(ALBW_CMDBUF + 20, 0);             // handle descriptor
        albw_w32(ALBW_CMDBUF + 24, 0);             // no parameter handle
        if (log_wanted("apt ReceiveParameter")) {
            printf("[ipc ] APT:U ReceiveParameter -> signal %u from %04x\n",
                   signal, APT_APPID_HOMEMENU);
        }
        c->r[0] = 0;
        return;
    }

    // fs:USER GetFormatInfo and FormatSaveData. The game asks what its save archive was formatted
    // with and formats it when the answer is not what it wants; the generic reply said zeros, so it
    // formatted on every boot, and only got away with it because formatting did nothing. Both now
    // go through the save module, which keeps the format beside the archive and moves old files
    // aside instead of erasing them.
    if (!strcmp(service, "fs:USER") && command_id == 0x0845) {
        uint32_t archive = albw_r32(ALBW_CMDBUF + 4);
        struct albw_save_format f;
        if (!albw_save_format_info(archive, &f)) {
            // Never formatted: the answer a console gives a cartridge it has not seen before.
            ipc_reply(c, command_id, ALBW_FS_NOT_FORMATTED);
            c->r[0] = 0;
            return;
        }
        albw_w32(ALBW_CMDBUF, (0x0845u << 16) | (5u << 6));
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, f.total_size);
        albw_w32(ALBW_CMDBUF + 12, f.directories);
        albw_w32(ALBW_CMDBUF + 16, f.files);
        albw_w32(ALBW_CMDBUF + 20, f.duplicate_data);
        c->r[0] = 0;
        return;
    }
    if (!strcmp(service, "fs:USER") && command_id == 0x084C) {
        struct albw_save_format f = {
            .total_size = albw_r32(ALBW_CMDBUF + 16) * 512u,   // the block count, in 512-byte blocks
            .directories = albw_r32(ALBW_CMDBUF + 20),
            .files = albw_r32(ALBW_CMDBUF + 24),
            .duplicate_data = albw_r32(ALBW_CMDBUF + 36) & 1u,
        };
        uint32_t archive = albw_r32(ALBW_CMDBUF + 4);
        ipc_reply(c, command_id, albw_save_format(archive, &f) ? 0 : ALBW_FS_NOT_FORMATTED);
        c->r[0] = 0;
        return;
    }

    // dsp::DSP LoadComponent: the size, two masks and the firmware as a mapped buffer, and the reply
    // says whether it loaded. The generic reply left the program mask in that word, which happened to
    // be non-zero.
    if (!strcmp(service, "dsp::DSP") && command_id == 0x0011) {
        uint32_t desc = albw_r32(ALBW_CMDBUF + 16), buffer = albw_r32(ALBW_CMDBUF + 20);
        albw_w32(ALBW_CMDBUF, (0x0011u << 16) | (2u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, 1);                   // loaded
        albw_w32(ALBW_CMDBUF + 12, desc);
        albw_w32(ALBW_CMDBUF + 16, buffer);
        c->r[0] = 0;
        return;
    }

    // APT:U GlanceParameter: what ReceiveParameter would return, without taking it. The generic
    // reply handed back whatever the request had left in the command word.
    if (!strcmp(service, "APT:U") && command_id == 0x000E) {
        if (apt_param_pending) {
            apt_deliver_parameter(c, 0x000Eu);
            c->r[0] = 0;
            return;
        }
        uint32_t signal = apt_wakeup_sent ? APT_COMMAND_NONE : APT_COMMAND_WAKEUP;
        albw_w32(ALBW_CMDBUF, (0x000Eu << 16) | (4u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, APT_APPID_HOMEMENU);
        albw_w32(ALBW_CMDBUF + 12, signal);
        albw_w32(ALBW_CMDBUF + 16, 0);
        albw_w32(ALBW_CMDBUF + 20, 0);
        albw_w32(ALBW_CMDBUF + 24, 0);
        c->r[0] = 0;
        return;
    }

    // cfg:u GetConfigInfoBlk2: size, block id, and the buffer as a mapped-write descriptor. The
    // generic reply below returned success and wrote nothing, which left the stereo camera block
    // zero -- and the camera divides by its screen width.
    if (!strcmp(service, "cfg:u") && command_id == 0x0001) {
        // The region the settings must agree with is read off the card, and the game asks for its
        // settings before it opens its RomFS -- so the image is opened here if nothing has yet.
        albw_romfs_open(rom_path);
        uint32_t size = albw_r32(ALBW_CMDBUF + 4);
        uint32_t block = albw_r32(ALBW_CMDBUF + 8);
        uint32_t dest = albw_r32(ALBW_CMDBUF + 16);
        uint32_t result = albw_config_block(block, size, dest);
        if (log_wanted("cfg block")) {
            printf("[cfg  ] block %08x, %u byte(s) at %08x\n", block, size, dest);
        }
        albw_w32(ALBW_CMDBUF, (0x0001u << 16) | (1u << 6) | 2u);
        albw_w32(ALBW_CMDBUF + 4, result);
        albw_w32(ALBW_CMDBUF + 8, (size << 4) | 0xCu);  // the descriptor, handed back
        albw_w32(ALBW_CMDBUF + 12, dest);
        c->r[0] = 0;
        return;
    }

    // cfg:u SecureInfoGetRegion. The generic reply left the region word as whatever the request had
    // there.
    if (!strcmp(service, "cfg:u") && command_id == 0x0002) {
        albw_romfs_open(rom_path);
        albw_w32(ALBW_CMDBUF, (0x0002u << 16) | (2u << 6));
        albw_w32(ALBW_CMDBUF + 4, 0);
        albw_w32(ALBW_CMDBUF + 8, albw_config_region());
        c->r[0] = 0;
        return;
    }

    // ptm:u GetTotalStepCount and GetStepHistory: the pedometer. A port has none, so no steps --
    // said outright, where the generic reply left the count word holding whatever the previous
    // request had put there. The history's hourly counts go to the buffer the request maps.
    if (!strcmp(service, "ptm:u") && (command_id == 0x000C || command_id == 0x000B)) {
        if (command_id == 0x000B) {
            // Three words of request (hours, and the start as a 64-bit time), then the buffer's
            // mapping descriptor and address.
            uint32_t hours = albw_r32(ALBW_CMDBUF + 4);
            uint32_t buf = albw_r32(ALBW_CMDBUF + 20);
            for (uint32_t i = 0; i < hours && i < 24 * 31; i++) albw_w16(buf + 2 * i, 0);
            albw_w32(ALBW_CMDBUF, (0x000Bu << 16) | (1u << 6) | 2u);
            albw_w32(ALBW_CMDBUF + 4, 0);
            albw_w32(ALBW_CMDBUF + 8, (hours * 2u << 4) | 0xCu);
            albw_w32(ALBW_CMDBUF + 12, buf);
        } else {
            albw_w32(ALBW_CMDBUF, (0x000Cu << 16) | (2u << 6));
            albw_w32(ALBW_CMDBUF + 4, 0);
            albw_w32(ALBW_CMDBUF + 8, 0);
        }
        c->r[0] = 0;
        return;
    }

    // Everything else reports success with an empty reply, so the boot path keeps moving
    // and the log shows which commands actually have to be implemented: each is counted under
    // its own key, so the summary at the end lists them apart from the ones answered properly.
    char generic[48];
    snprintf(generic, sizeof generic, "generic reply %s 0x%04x", service, command_id);
    if (log_bump(generic) == 1) {
        printf("[ipc ] %s 0x%04x has no implementation; answered with success and nothing else\n",
               service, command_id);
    }
    ipc_reply(c, command_id, 0);
    c->r[0] = 0;
}

// GSP interrupt ids, as the guest's queue encodes them.
#define GSP_IRQ_PSC0    0u
#define GSP_IRQ_PSC1    1u
#define GSP_IRQ_VBLANK0 2u
#define GSP_IRQ_VBLANK1 3u
#define GSP_IRQ_PPF     4u
#define GSP_IRQ_P3D     5u
#define GSP_IRQ_DMA     6u

// Each thread's queue is 0x40 bytes: offset, count, missed, error, two missed counters,
// then the list of pending interrupt ids.
static void gsp_push_interrupt(uint32_t slot, uint8_t id) {
    uint32_t q = gsp_shared_addr + slot * 0x40u;
    uint8_t offset = albw_r8(q + 0);
    uint8_t count = albw_r8(q + 1);
    if (count >= 0x34u) {
        albw_w8(q + 2, (uint8_t)(albw_r8(q + 2) + 1));  // missed
        return;
    }
    albw_w8(q + 0x0C + (uint32_t)((offset + count) % 0x34), id);
    albw_w8(q + 1, (uint8_t)(count + 1));
}

void albw_kernel_describe_handle(uint32_t handle, char *out, unsigned long n) {
    int i = handle_index(handle);
    if (i < 0) {
        snprintf(out, n, "not a handle");
        return;
    }
    snprintf(out, n, "%s%s%s, %s", kind_name(handles[i].kind),
             handles[i].name[0] ? " " : "", handles[i].name,
             handles[i].signalled ? "signalled" : "unsignalled");
}

// The GX command queue, at +0x800 of the shared block: a 0x20-byte header followed by
// 0x20-byte entries. The header's first byte is the index of the next command and the second
// is how many are pending; an entry's low byte is its type, then an address and a size.
// That layout is read off a queue the guest actually filled, not assumed - it had queued a
// FlushCacheRegions and a ProcessCommandList and was waiting for them.
//
// Only the commands with observable memory effects are carried out. ProcessCommandList needs
// a GPU, so it is acknowledged; acknowledging the *others* would leave the guest reading
// buffers that were never filled, which is worse than not running at all.
// Walks a PICA200 command list and reports what it writes.
//
// The format is a sequence of entries: a data word, then a header whose low 16 bits are the
// register index, bits 16-19 a byte write mask, bits 20-27 a count of further data words, and
// bit 31 a flag saying whether those go to consecutive registers or all to the same one. Each
// entry is padded to eight bytes.
//
// Nothing here draws anything. It exists because the next thing this port needs is a renderer,
// and the first question a renderer has to answer is what the game actually programs -- which
// is not something to guess at when the command stream says it. Off unless ALBW_DUMP_CMDLIST is
// set, since one list runs to thousands of writes.
static void gsp_dump_command_list(uint32_t addr, uint32_t size) {
    if (!albw_env("ALBW_DUMP_CMDLIST")) return;
    // The boot submits thousands of these. The interesting question is how the first few differ
    // from each other, not what the four thousandth looks like.
    static unsigned long shown;
    if (shown++ >= 4) return;
    if (!albw_guest_ok(addr, size)) {
        printf("[pica ] command list at %08x+%u is outside guest memory\n", addr, size);
        return;
    }

    // Counted per register rather than summarised by block. The block boundaries would have
    // to be named to be useful, and the first list this reports makes clear why guessing them is
    // a bad idea: nearly four thousand writes land in forty-eight registers, which is a bulk
    // upload port and not whatever that range is nominally called. What a renderer needs first
    // is which registers the game actually drives, and that is a measurement.
    static unsigned long per_reg[0x300];
    memset(per_reg, 0, sizeof per_reg);

    unsigned long writes = 0, entries = 0;
    uint32_t lowest = 0xFFFFFFFFu, highest = 0;
    uint32_t at = addr, end = addr + (size & ~7u);
    printf("[pica ] command list at %08x, %u bytes:\n", addr, size);
    while (at + 8 <= end) {
        uint32_t data = albw_r32(at);
        uint32_t header = albw_r32(at + 4);
        uint32_t reg = header & 0xFFFFu;
        uint32_t mask = (header >> 16) & 0xFu;
        uint32_t extra = (header >> 20) & 0xFFu;
        int consecutive = (header >> 31) & 1;

        entries++;
        writes += 1 + extra;
        if (reg < lowest) lowest = reg;
        if (reg + (consecutive ? extra : 0) > highest) highest = reg + (consecutive ? extra : 0);
        for (uint32_t k = 0; k <= (consecutive ? extra : 0); k++) {
            uint32_t r = reg + k;
            if (r < 0x300) per_reg[r] += consecutive ? 1 : 1 + extra;
        }

        if (entries <= 24) {
            printf("[pica ]   reg 0x%03x mask %x = %08x", reg, mask, data);
            if (extra) printf("  (+%u %s)", extra, consecutive ? "consecutive" : "to the same");
            printf("\n");
        }

        // The entry is the two words plus the extra data, rounded up to eight bytes.
        uint32_t step = 8u + extra * 4u;
        at += (step + 7u) & ~7u;
    }
    printf("[pica ] %lu entries, %lu register write(s), registers 0x%03x..0x%03x\n",
           entries, writes, lowest, highest);
    // The ten busiest, which is where a renderer has to start.
    for (int shown = 0; shown < 10; shown++) {
        uint32_t best = 0;
        for (uint32_t r = 1; r < 0x300; r++) {
            if (per_reg[r] > per_reg[best]) best = r;
        }
        if (!per_reg[best]) break;
        printf("[pica ]   reg 0x%03x: %lu write(s)\n", best, per_reg[best]);
        per_reg[best] = 0;
    }
}

// One display transfer: a render target becomes what an LCD scans out.
//
// The dimension words are (height << 16) | width, where width is the 240-pixel direction, because
// both surfaces are stored a column at a time -- which is why the top screen's transfer says 400
// by 240 and the bottom's destination says 320. Bit 2 of the flags crops instead of scaling, and
// that is exactly what the bottom screen needs: its source is the 400-wide surface and only 320
// of it is taken. The formats are in bits 8-10 and 12-14.
//
// Both sides are read and written linearly. That is right for a linear source and cannot be wrong
// for the constant one the guest currently supplies, since a surface filled with one value looks
// the same under any layout. It is *not* right for a render target the GPU has drawn into, which
// is tiled in eight by eight blocks -- so this reports the first time it sees a source that is not
// uniform, which is the moment the layout starts to matter and the frame dumps become the way to
// check it.
// GX TextureCopy: bytes as they are, from lines of one width and gap to lines of another. The game
// takes a picture of its top screen this way when it opens a menu over it -- 192,000 bytes read in
// one run, written in lines of 3,840 bytes 256 apart, which is the render target's 240 by 400 RGB565
// tiles laid into a texture 256 wide -- and draws the menu over that picture. Widths and gaps are
// in units of 16 bytes, the size is counted down to a multiple of 16, and a gap of zero means the
// side is one run whatever its width says.
static void gsp_texture_copy(uint32_t src, uint32_t dst, uint32_t size, uint32_t in_dim,
                             uint32_t out_dim) {
    uint32_t remaining = size & ~15u;
    uint32_t in_gap = (in_dim >> 16) * 16u, out_gap = (out_dim >> 16) * 16u;
    uint32_t in_width = in_gap ? (in_dim & 0xFFFFu) * 16u : remaining;
    uint32_t out_width = out_gap ? (out_dim & 0xFFFFu) * 16u : remaining;
    if (!remaining || !in_width || !out_width) return;
    // Where each side ends, gaps included, so the whole copy can be checked before any of it.
    uint64_t in_span = (uint64_t)remaining / in_width * (in_width + in_gap) + remaining % in_width;
    uint64_t out_span = (uint64_t)remaining / out_width * (out_width + out_gap) + remaining % out_width;
    if (in_span > 0x10000000u || out_span > 0x10000000u
            || !albw_guest_ok(src, (uint32_t)in_span) || !albw_guest_ok(dst, (uint32_t)out_span)) {
        printf("[gsp  ] refusing a texture copy of %u bytes from %08x to %08x: outside guest memory\n",
               size, src, dst);
        return;
    }
    uint32_t in_left = in_width, out_left = out_width, from = src, to = dst;
    while (remaining) {
        uint32_t n = in_left < out_left ? in_left : out_left;
        if (n > remaining) n = remaining;
        memmove(albw_mem + to, albw_mem + from, n);
        ALBW_NOTE_WRITE(to, n);
        from += n;
        to += n;
        in_left -= n;
        out_left -= n;
        remaining -= n;
        if (!in_left) {
            in_left = in_width;
            from += in_gap;
        }
        if (!out_left) {
            out_left = out_width;
            to += out_gap;
        }
    }
}

static void gsp_display_transfer(uint32_t src, uint32_t dst, uint32_t src_dim, uint32_t dst_dim,
                                 uint32_t flags) {
    uint32_t src_w = src_dim & 0xFFFFu, src_h = src_dim >> 16;
    uint32_t dst_w = dst_dim & 0xFFFFu, dst_h = dst_dim >> 16;
    uint32_t in_format = (flags >> 8) & 7u, out_format = (flags >> 12) & 7u;
    uint32_t in_bpp = albw_pixel_bytes(in_format), out_bpp = albw_pixel_bytes(out_format);

    uint32_t w = src_w < dst_w ? src_w : dst_w;
    uint32_t h = src_h < dst_h ? src_h : dst_h;
    if (!w || !h) return;
    if (!albw_guest_ok(src, src_w * src_h * in_bpp)
            || !albw_guest_ok(dst, dst_w * dst_h * out_bpp)) {
        printf("[gsp  ] refusing a transfer %08x -> %08x: outside guest memory\n", src, dst);
        return;
    }

    if (albw_env("ALBW_PROBE")) {
        uint32_t middle = src + albw_tiled_offset(src_w / 2u, src_h / 2u, src_w, in_bpp);
        printf("[gsp  ] transfer from %08x: its middle pixel is %04x\n", src, albw_r16(middle));
    }
    static unsigned logged;
    if (logged < 6 && albw_env("ALBW_PROBE")) {
        logged++;
        printf("[gsp  ] transfer %08x %ux%u -> %08x %ux%u flags %08x (in fmt %u, out fmt %u)\n",
               src, src_w, src_h, dst, dst_w, dst_h, flags, in_format, out_format);
    }

    static int warned_tiled;
    if (!warned_tiled) {
        // Two pixels far apart in the source. While they agree, the layout cannot show through.
        uint32_t a = albw_r32(src);
        uint32_t b = albw_r32(src + (src_w * src_h - 2) * in_bpp);
        if (a != b) {
            warned_tiled = 1;
            printf("[gsp  ] the transfer source at %08x is no longer uniform, so its tiling now "
                   "matters and this copies it linearly. Compare a frame dump against what the "
                   "game should be drawing before trusting the picture.\n", src);
        }
    }

    // The copy itself, tiles and all, is albw_transfer_copy's (albw_screen.c), which frames drawn
    // in between use as well.
    albw_transfer_copy(src, dst, src_dim, dst_dim, flags);
    for (uint32_t y = 0; y < h; y++) ALBW_NOTE_WRITE(dst + y * dst_w * out_bpp, w * out_bpp);

    // Each transfer's result as a picture, turned the way the screen shows it, when
    // ALBW_DUMP_TRANSFERS names a directory: what reaches a framebuffer is the one thing the frame
    // dumps and the render target dumps both stand between, and a fault that appears in one and not
    // the other is in the transfer or in which framebuffer is shown.
    const char *tdir = albw_env("ALBW_DUMP_TRANSFERS");
    static unsigned tdumped;
    const char *tfrom = albw_env("ALBW_DUMP_FROM");
    if (tdir && tdumped < 24 && gsp_frames >= (tfrom ? strtoul(tfrom, NULL, 0) : 0)) {
        tdumped++;
        char path[512];
        snprintf(path, sizeof path, "%s/transfer-%02u-frame%06lu-%08x.ppm", tdir, tdumped,
                 gsp_frames, dst);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", h, w);
            for (uint32_t row = 0; row < w; row++) {
                for (uint32_t col = 0; col < h; col++) {
                    uint8_t rgb[3];
                    albw_pixel_read(dst + (col * dst_w + (w - 1u - row)) * out_bpp, out_format, rgb);
                    fwrite(rgb, 1, 3, f);
                }
            }
            fclose(f);
        }
    }
}

// ALBW_FRAME_TIMES=<file>: each frame's wall time and what went into it -- syscalls, file reads
// and the bytes they read -- one line a frame, for finding where a hitch comes from.
static void frame_time_log(void) {
    static FILE *f;
    static int checked;
    static struct timespec last;
    static unsigned long last_svc;
    if (!checked) {
        checked = 1;
        const char *path = albw_env("ALBW_FRAME_TIMES");
        if (path) f = fopen(path, "w");
        clock_gettime(CLOCK_MONOTONIC, &last);
    }
    if (!f) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double ms = (double)(now.tv_sec - last.tv_sec) * 1e3 + (double)(now.tv_nsec - last.tv_nsec) * 1e-6;
    unsigned long svc = albw_svc_count();
    static double last_raster;
    double raster = albw_raster_busy();
    fprintf(f, "%lu %.3f svc %lu reads %lu bytes %lu raster %.3f", gsp_frames, ms, svc - last_svc,
            ft_reads, ft_read_bytes, (raster - last_raster) * 1e3);
    last_raster = raster;
    // Which threads had the processor, and for how long: a thread the game gave a low priority
    // takes the main thread's time here, since priorities are not honoured.
    uint64_t ns[ALBW_MAX_THREADS];
    uint32_t entry[ALBW_MAX_THREADS];
    int32_t priority[ALBW_MAX_THREADS];
    albw_thread_take_times(ns, entry, priority);
    for (int i = 0; i < ALBW_MAX_THREADS; i++) {
        if (ns[i] >= 200000u) {
            fprintf(f, " t%d/%08x/p%d:%.2f", i, entry[i], priority[i], (double)ns[i] * 1e-6);
        }
    }
    fputc('\n', f);
    fflush(f);
    ft_reads = ft_read_bytes = 0;
    last_svc = svc;
    last = now;
}

static int gsp_run_command_queue(void) {
    uint32_t gx = gsp_shared_addr + 0x800u;
    uint8_t index = albw_r8(gx);
    uint8_t count = albw_r8(gx + 1);
    if (albw_env("ALBW_TRACE_QUEUE") && gsp_frames >= strtoul(albw_env("ALBW_TRACE_QUEUE"), 0, 0)) {
        printf(" |%u@%u/%02x%02x", count, index, albw_r8(gx + 2), albw_r8(gx + 3));
    }
    if (!count) return 0;

    int done = 0;
    while (count) {
        uint32_t e = gx + 0x20u + (uint32_t)index * 0x20u;
        uint32_t head = albw_r32(e);
        uint32_t type = head & 0xFFu;
        // The order the queue is worked through, for a few frames. A picture that a fill wipes out
        // and a picture that is never drawn look the same from the framebuffer; this tells them
        // apart.
        // A transfer says where from and where to, and a list which colour target it left set, so
        // that what reaches a screen can be followed back to what drew it.
        static unsigned long ordered_frame = ~0ul;
        static unsigned ordered;
        int tracing = albw_env("ALBW_TRACE_QUEUE")
                   && gsp_frames >= strtoul(albw_env("ALBW_TRACE_QUEUE"), 0, 0) && ordered < 400;
        if (tracing) {
            if (gsp_frames != ordered_frame) {
                ordered_frame = gsp_frames;
                printf("\n[gsp  ] frame %lu queue:", gsp_frames);
            }
            if (type) {
                ordered++;
                if (type == 3) {
                    printf(" transfer(%08x->%08x)", albw_r32(e + 4), albw_r32(e + 8));
                } else if (type == 2) {
                    printf(" fill(%08x)", albw_r32(e + 4));
                } else if (type != 1) {
                    printf(" %02x", type);
                }
            }
        }

        switch (type) {
            case 0x02: {                                // MemoryFill, two regions
                albw_raster_before_memory();
                uint32_t control = albw_r32(e + 0x1C);
                for (int r = 0; r < 2; r++) {
                    uint32_t start = albw_r32(e + 4 + (uint32_t)r * 12);
                    uint32_t value = albw_r32(e + 8 + (uint32_t)r * 12);
                    uint32_t end = albw_r32(e + 12 + (uint32_t)r * 12);
                    if (!start || end <= start) continue;
                    if (!albw_guest_ok(start, end - start)) continue;
                    // The fill pattern is 16, 24 or 32 bits wide, chosen per region by the
                    // control word -- the low half for the first region, the high half for the
                    // second. memset would fill with the value's lowest byte, which happens to
                    // be right for white and wrong for every other colour.
                    // A fill names a surface the GPU draws into, which is how the register that
                    // holds it can be pointed out without a register map.
                    albw_pica_note_surface(start, r == 0 ? "cleared first (colour?)"
                                                        : "cleared second (depth?)");
                    uint32_t width = (control >> ((uint32_t)r * 16 + 8)) & 3u;
                    uint32_t step = width == 2 ? 4u : (width == 1 ? 3u : 2u);
                    // Straight into host memory a pattern's width at a time, and noted once: a byte
                    // at a time through the store helper cost six per cent of a rendering run.
                    uint8_t pattern[4];
                    for (uint32_t b = 0; b < 4; b++) pattern[b] = (uint8_t)(value >> (b * 8));
                    uint8_t *mem = albw_mem + start;
                    uint32_t len = (end - start) / step * step;
                    if (step == 4) {
                        uint32_t v = value;
                        for (uint32_t at = 0; at < len; at += 4) memcpy(mem + at, &v, 4);
                    } else if (step == 2) {
                        uint16_t v = (uint16_t)value;
                        for (uint32_t at = 0; at < len; at += 2) memcpy(mem + at, &v, 2);
                    } else {
                        for (uint32_t at = 0; at < len; at += 3) memcpy(mem + at, pattern, 3);
                    }
                    ALBW_NOTE_WRITE(start, len);
                    albw_raster_note_fill(start, end, value, step);
                }
                gsp_push_interrupt(gsp_thread_index, GSP_IRQ_PSC0);
                gsp_push_interrupt(gsp_thread_index, GSP_IRQ_PSC1);
                break;
            }
            case 0x05:                                  // FlushCacheRegions: nothing to do
                break;
            case 0x01:                                  // ProcessCommandList
                gsp_dump_command_list(albw_r32(e + 4), albw_r32(e + 8));
                albw_pica_execute(albw_r32(e + 4), albw_r32(e + 8));
                albw_raster_list_done();                // drawn now, or held for a second eye
                if (tracing) printf(" list(->%08x)", albw_pica_reg(0x11Du) << 3);
                gsp_push_interrupt(gsp_thread_index, GSP_IRQ_P3D);
                break;
            case 0x03:                                  // DisplayTransfer
                // A whole top screen is 240 by 400 as the framebuffer stores it.
                albw_raster_before_transfer(albw_r32(e + 16) == 0x019000F0u);
                albw_pica_note_surface(albw_r32(e + 4), "a display transfer read it");
                gsp_display_transfer(albw_r32(e + 4), albw_r32(e + 8), albw_r32(e + 12),
                                     albw_r32(e + 16), albw_r32(e + 20));
                albw_raster_note_transfer(albw_r32(e + 4), albw_r32(e + 8), albw_r32(e + 12),
                                          albw_r32(e + 16), albw_r32(e + 20));
                gsp_push_interrupt(gsp_thread_index, GSP_IRQ_PPF);
                break;
            case 0x04:                                  // TextureCopy
                albw_raster_before_memory();
                gsp_texture_copy(albw_r32(e + 4), albw_r32(e + 8), albw_r32(e + 12),
                                 albw_r32(e + 16), albw_r32(e + 20));
                albw_raster_note_texture_copy(albw_r32(e + 4), albw_r32(e + 8), albw_r32(e + 12),
                                              albw_r32(e + 16), albw_r32(e + 20));
                gsp_push_interrupt(gsp_thread_index, GSP_IRQ_PPF);
                break;
            case 0x00: {                                // RequestDma(src, dst, size)
                albw_raster_before_memory();
                // A DMA is a copy, so this one can be carried out exactly. The game uses it to
                // move data into VRAM at 0x1F000000, which is inside the guest map like
                // everything else, so there is nothing special to arrange.
                uint32_t src = albw_r32(e + 4);
                uint32_t dst = albw_r32(e + 8);
                uint32_t len = albw_r32(e + 12);
                if (len && albw_guest_ok(src, len) && albw_guest_ok(dst, len)) {
                    memmove(albw_mem + dst, albw_mem + src, len);
                    ALBW_NOTE_WRITE(dst, len);
                    // DMAs whose ends are not where guest-visible VRAM or the linear heap are: an
                    // address in another form would land the copy somewhere nothing reads.
                    if (albw_env("ALBW_PROBE_DMA_ODD")) {
                        int dst_ok = (dst >= 0x1F000000u && dst < 0x1F600000u)
                                  || (dst >= 0x14000000u && dst < 0x1C000000u);
                        int src_ok = (src >= 0x1F000000u && src < 0x1F600000u)
                                  || (src >= 0x14000000u && src < 0x1C000000u);
                        static unsigned odd_said;
                        if ((!dst_ok || !src_ok) && odd_said++ < 20) {
                            printf("[gsp  ] frame %lu: an unusual DMA %08x -> %08x, %u bytes\n",
                                   gsp_frames, src, dst, len);
                        }
                    }
                    // Which DMA fills a given address, for ALBW_PROBE_DMA=<address>: a vertex or
                    // texture buffer in VRAM that reads as zeros was either never sent or sent
                    // from memory that was still empty, and this tells the two apart.
                    const char *pd = albw_env("ALBW_PROBE_DMA");
                    uint32_t want = pd ? (uint32_t)strtoul(pd, NULL, 0) : 0;
                    const char *near = albw_env("ALBW_PROBE_DMA_NEAR");
                    uint32_t span = near ? (uint32_t)strtoul(near, NULL, 0) : 0;
                    if (want && span && dst + len + span > want && dst < want + span) {
                        static unsigned near_said;
                        if (near_said++ < 20) {
                            printf("[gsp  ] frame %lu: DMA %08x -> %08x..%08x (%u bytes), near %08x\n",
                                   gsp_frames, src, dst, dst + len, len, want);
                        }
                    }
                    if (want && want >= dst && want < dst + len) {
                        printf("[gsp  ] frame %lu: DMA %08x -> %08x, %u bytes; the word there is "
                               "now %08x\n", gsp_frames, src, dst, len, albw_r32(want & ~3u));
                    }
                } else if (len) {
                    printf("[gsp  ] refusing a DMA of %u bytes from %08x to %08x: outside "
                           "guest memory\n", len, src, dst);
                }
                gsp_push_interrupt(gsp_thread_index, GSP_IRQ_DMA);
                break;
            }
            default:
                printf("[gsp  ] unknown gx command type 0x%02x (header %08x)\n", type, head);
                break;
        }
        // Keyed by command rather than counting them all together, so the exit summary says what
        // the guest asks the GPU for over a whole run. Sharing one key answered "what does it
        // submit" with the first few it happened to submit, which is not the same question.
        static const char *const gx_names[] = {
            "RequestDma", "ProcessCommandList", "MemoryFill", "DisplayTransfer",
            "TextureCopy", "FlushCacheRegions",
        };
        char gx_key[32];
        snprintf(gx_key, sizeof gx_key, "gx %s",
                 type < sizeof gx_names / sizeof gx_names[0] ? gx_names[type] : "unknown");
        if (log_wanted(gx_key)) {
            // The fields differ per command, and printing them as "addr" and "size" for all of
            // them made a perfectly good DMA into VRAM look like a 520 MB transfer.
            uint32_t w1 = albw_r32(e + 4), w2 = albw_r32(e + 8), w3 = albw_r32(e + 12);
            switch (type) {
                case 0x00:
                    printf("[gsp  ] gx RequestDma %u bytes from %08x to %08x\n", w3, w1, w2);
                    break;
                case 0x01:
                    printf("[gsp  ] gx ProcessCommandList at %08x, %u bytes\n", w1, w2);
                    break;
                case 0x02:
                    printf("[gsp  ] gx MemoryFill %08x..%08x = %08x and %08x..%08x = %08x "
                           "(control %08x)\n", w1, w3, w2, albw_r32(e + 16),
                           albw_r32(e + 24), albw_r32(e + 20), albw_r32(e + 0x1C));
                    break;
                case 0x03:
                case 0x04:
                    printf("[gsp  ] gx %s %08x -> %08x, dims %08x -> %08x, flags %08x\n",
                           type == 0x03 ? "DisplayTransfer" : "TextureCopy", w1, w2,
                           albw_r32(e + 12), albw_r32(e + 16), albw_r32(e + 20));
                    break;
                case 0x05:
                    printf("[gsp  ] gx FlushCacheRegions at %08x, %u bytes\n", w1, w2);
                    break;
                default:
                    printf("[gsp  ] gx command type 0x%02x  %08x %08x %08x\n",
                           type, w1, w2, w3);
                    break;
            }
        }

        // Fifteen entries, not sixteen: the game wraps from 14 to 0. Wrapping at sixteen read a slot
        // past the queue, left the command the game had put in slot 0 for the game to overwrite
        // with its next one, and so lost one command in fifteen -- a transfer, and the top screen
        // showed a stale picture, or a list, and a pass went undrawn.
        index = (uint8_t)((index + 1) % 15u);
        count--;
        done++;
    }
    albw_w8(gx, index);
    albw_w8(gx + 1, 0);
    return done;
}

// Raises the interrupt event the application registered and wakes whoever waits on it. Split
// out because interrupts are not only a frame boundary: the guest submits a command list and
// then spins on a flag that its own interrupt handler clears, so if the completion waits for
// the next idle it never arrives -- the spinning thread is runnable, and idle never comes.
static int gsp_deliver_interrupts(void) {
    int i = handle_index(gsp_event);
    if (i >= 0) handles[i].signalled = 1;
    int woken = albw_thread_wake(ALBW_W_SYNC, gsp_event, -1);
    if (woken && i >= 0 && handles[i].reset_type == 0) handles[i].signalled = 0;
    return woken;
}

int albw_kernel_on_idle(void) {
    if (!gsp_event || !gsp_shared_addr) return 0;

    // Frame count alone says nothing: a healthy game loop also runs frames forever. What
    // distinguishes running from spinning is whether anything *new* happens, so measure that
    // instead - the number of distinct logged events. Counting frames had me report a working
    // main loop as a deadlock.
    #define ALBW_QUIET_FRAMES 20000
    static int kinds_at_last_change;
    static unsigned long quiet_since;
    if (log_kinds != kinds_at_last_change) {
        kinds_at_last_change = log_kinds;
        quiet_since = gsp_frames;
    } else if (!frame_hook && gsp_frames - quiet_since >= ALBW_QUIET_FRAMES) {
        // Only without a window. With one, a quiet stretch is somebody sitting on the title screen,
        // and ending the run under them would be absurd; they close the window when they are done.
        // Not a deadlock, and the difference matters: the guest is running, submitting work and
        // waiting for the next frame, exactly as a game does when it is showing something and
        // nobody is looking at it. What stops here is this check, on the grounds that nothing
        // *new* has happened for twenty thousand frames -- which is a reason to come back with a
        // renderer, not evidence that anything is stuck.
        static char note[200];
        snprintf(note, sizeof note,
                 "the guest ran %lu frames without a new kind of activity: it is idling, not "
                 "stuck. Nothing here can see what it is drawing", gsp_frames);
        kernel_stop_note = note;
        printf("[gsp  ] %lu frames with no new kind of activity; the loop turns but the run "
               "is not advancing. Thread states:\n", gsp_frames - quiet_since);
        albw_thread_report_blocked();
        albw_raster_report();
        albw_dsp_report();
        // The GX command queue sits at +0x800 of the shared block. If the guest has queued
        // work there that nothing consumes, that is what it is waiting for - and unlike the
        // earlier guess, this is evidence.
        uint32_t gx = gsp_shared_addr + 0x800u;
        // Where the screens are. The game never calls SetBufferSwap; it writes its framebuffer
        // addresses into the shared block and the GSP module is expected to pick them up at
        // vblank. Each screen has a header with the entry index and an update flag, then two
        // entries: the active buffer, its left and right addresses, stride, format, and which
        // one is being shown.
        for (uint32_t screen = 0; screen < 2; screen++) {
            uint32_t fb = gsp_shared_addr + 0x200u + gsp_thread_index * 0x80u + screen * 0x40u;
            printf("[gsp  ] %s screen info at %08x: index=%u flag=%u\n",
                   screen ? "bottom" : "top", fb, albw_r8(fb), albw_r8(fb + 1));
            for (uint32_t e = 0; e < 2; e++) {
                uint32_t entry = fb + 4u + e * 0x1Cu;
                printf("[gsp  ]   entry %u: active=%08x left=%08x right=%08x stride=%u "
                       "format=%08x shown=%08x\n", e,
                       albw_r32(entry), albw_r32(entry + 4), albw_r32(entry + 8),
                       albw_r32(entry + 12), albw_r32(entry + 16), albw_r32(entry + 20));
            }
        }

        printf("[gsp  ] gx queue at %08x:", gx);
        for (uint32_t x = 0; x < 16; x++) printf(" %02x", albw_r8(gx + x));
        printf("\n[gsp  ] first entries:\n");
        for (uint32_t e = 0; e < 3; e++) {
            printf("[gsp  ]   +%02x:", 0x20u + e * 0x20u);
            for (uint32_t x = 0; x < 32; x += 4) {
                printf(" %08x", albw_r32(gx + 0x20u + e * 0x20u + x));
            }
            printf("\n");
        }
        return 0;
    }

    // Whether the guest consumes what is written here is the question: if the queue layout
    // is wrong it wakes, finds nothing and sleeps again, which looks identical from outside.
    if (gsp_frames < 4) {
        uint32_t q = gsp_shared_addr + gsp_thread_index * 0x40u;
        printf("[gsp  ] queue before frame %lu: offset=%u count=%u missed=%u  list:",
               gsp_frames + 1, albw_r8(q), albw_r8(q + 1), albw_r8(q + 2));
        for (uint32_t x = 0; x < 8; x++) printf(" %02x", albw_r8(q + 0x0C + x));
        printf("\n");
    }

    // Audio frames due before the next vblank come first, each on an idle turn of its own, so that
    // the audio thread runs between them as it would between the firmware's interrupts -- three or
    // four times a video frame.
    uint64_t vblank_at = (uint64_t)(gsp_frames + 1) * ALBW_TICKS_PER_FRAME;
    if (!albw_dsp_running() || !dsp_pipe_event[2]) {
        audio_next = (kernel_time > vblank_at ? kernel_time : vblank_at) + ALBW_DSP_FRAME_TICKS;
    }
    while (albw_dsp_running() && dsp_pipe_event[2] && audio_next <= vblank_at) {
        kernel_time = audio_next;
        audio_next += ALBW_DSP_FRAME_TICKS;
        if (dsp_frame_tick() > 0) return 1;
    }

    // Queued GPU work is drained before the frame it belongs to ends.
    int commands = gsp_run_command_queue();

    gsp_frames++;
    if (kernel_time < vblank_at) kernel_time = vblank_at;
    frame_time_log();
    dump_memory_if_asked();
    // ALBW_STOP_AT=<frame> ends the run there: a benchmark wants the same frames every time, and
    // without it a run lasts until the guest has been quiet long enough to call it stuck.
    static long stop_at = -1;
    if (stop_at < 0) {
        const char *s = albw_env("ALBW_STOP_AT");
        stop_at = s ? (long)strtoul(s, NULL, 0) : 0;
    }
    if (stop_at && gsp_frames >= (unsigned long)stop_at) {
        printf("[gsp  ] frame %lu: stopping, as ALBW_STOP_AT asks\n", gsp_frames);
        if (albw_env("ALBW_THREADS")) albw_thread_report_blocked();
        albw_raster_report();
        albw_dsp_report();
        fflush(stdout);
        exit(0);
    }
    gsp_push_interrupt(gsp_thread_index, GSP_IRQ_VBLANK0);
    gsp_push_interrupt(gsp_thread_index, GSP_IRQ_VBLANK1);

    int woken = gsp_deliver_interrupts();

    // A frame is also how long a sleep lasts: there is no finer clock, and waking sleepers
    // here is what makes svcSleepThread yield the processor for real.
    albw_thread_wake(ALBW_W_SLEEP, 0, -1);

    // Audio advances on the same clock. Nothing consumes the samples yet, but the thread that
    // produces them has to get its turn or the run stalls behind it.
    albw_raster_frame_end(gsp_shared_addr, gsp_thread_index, gsp_frames);
    albw_screen_dump(gsp_shared_addr, gsp_thread_index, gsp_frames);
    if (frame_hook && !frame_hook(gsp_shared_addr, gsp_thread_index, gsp_frames)) {
        kernel_stop_note = "the window was closed";
        return 0;
    }
    hid_frame_tick(gsp_frames);

    if (dsp_event) {
        int di = handle_index(dsp_event);
        if (di >= 0) handles[di].signalled = 1;
        int dw = albw_thread_wake(ALBW_W_SYNC, dsp_event, -1);
        if (dw && di >= 0 && handles[di].reset_type == 0) handles[di].signalled = 0;
        woken += dw;
    }

    if (log_wanted("gsp vblank")) {
        printf("[gsp  ] frame %lu: %d gx command(s), vblank delivered, woke %d thread(s)\n",
               gsp_frames, commands, woken);
    }
    return woken > 0;
}

// Waits on one handle. Returns a result code; 0 means the wait was satisfied.
static uint32_t albw_wait_one(albw_cpu *c, uint32_t handle) {
    int i = handle_index(handle);
    if (i < 0) {
        // lr names the guest code that asked, which is what makes a bad handle traceable
        // back to the call that failed to produce one.
        printf("[svc ] thread %d waits on unknown handle 0x%08x (called from 0x%08x)\n",
               albw_thread_current(), handle, c->r[14]);
        printf("[svc ]   r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x\n",
               c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5]);
        // r4 usually still points at whatever struct the caller was working from, so the
        // words around it show which field was supposed to hold a handle.
        if (albw_guest_ok(c->r[4], 32)) {
            for (uint32_t k2 = 0; k2 < 8; k2++) {
                printf("[svc ]   [r4+%02x] = %08x\n", k2 * 4, albw_r32(c->r[4] + k2 * 4));
            }
        }
        return 0xD8E007F7u;                             // invalid handle
    }
    if (handles[i].kind == ALBW_H_THREAD) {
        // Waiting for a thread means waiting for it to finish -- which it may already have done:
        // its handle is signalled then, and stays so.
        if (handles[i].signalled) return 0;
        albw_thread_block(ALBW_W_SYNC, handle);
        return 0;
    }
    if (handles[i].signalled) {
        if (handles[i].reset_type == 0) handles[i].signalled = 0;
        return 0;
    }
    albw_thread_block(ALBW_W_SYNC, handle);
    return 0;
}

// ------------------------------------------------------------ syscalls -----

static const char *svc_name(uint32_t imm) {
    switch (imm) {
        case 0x01: return "ControlMemory";
        case 0x02: return "QueryMemory";
        case 0x03: return "ExitProcess";
        case 0x08: return "CreateThread";
        case 0x09: return "ExitThread";
        case 0x0A: return "SleepThread";
        case 0x0B: return "GetThreadPriority";
        case 0x13: return "CreateMutex";
        case 0x14: return "ReleaseMutex";
        case 0x17: return "CreateEvent";
        case 0x18: return "SignalEvent";
        case 0x19: return "ClearEvent";
        case 0x1E: return "CreateMemoryBlock";
        case 0x1F: return "MapMemoryBlock";
        case 0x20: return "UnmapMemoryBlock";
        case 0x21: return "CreateAddressArbiter";
        case 0x22: return "ArbitrateAddress";
        case 0x23: return "CloseHandle";
        case 0x24: return "WaitSynchronization1";
        case 0x25: return "WaitSynchronizationN";
        case 0x27: return "DuplicateHandle";
        case 0x28: return "GetSystemTick";
        case 0x2D: return "ConnectToPort";
        case 0x32: return "SendSyncRequest";
        case 0x35: return "GetProcessId";
        case 0x37: return "GetThreadId";
        case 0x38: return "GetResourceLimit";
        case 0x3A: return "GetResourceLimitCurrentValues";
        case 0x3C: return "Break";
        case 0x3D: return "OutputDebugString";
        default:   return "?";
    }
}

// ALBW_WATCH_VRAM=1, a diagnostic: VRAM is made inaccessible whenever guest code runs, and each
// place that touches it anyway is reported once -- the question being whether the game ever reads
// what the GPU drew with the processor, which decides whether a renderer may leave VRAM undrawn.
// Kernel code (the svc handlers) has it open; a fault outside a handler opens it until the next.
// Linux only: it is made of mprotect and a SIGSEGV handler.
#ifndef _WIN32
#include <sys/mman.h>
#define VRAM_FROM 0x1F000000u
#define VRAM_SIZE 0x00600000u
static int vram_watch = -1;
static uintptr_t vram_seen[256];
static int vram_seen_count;

static void vram_fault(int sig, siginfo_t *info, void *context) {
    uint8_t *at = info->si_addr;
    if (at < albw_mem + VRAM_FROM || at >= albw_mem + VRAM_FROM + VRAM_SIZE) {
        signal(sig, SIG_DFL);
        return;                                          // faults again, with the default action
    }
    ucontext_t *uc = context;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    int known = 0;
    for (int i = 0; i < vram_seen_count; i++) known |= vram_seen[i] == pc;
    if (!known && vram_seen_count < 256) {
        vram_seen[vram_seen_count++] = pc;
        char line[160];
        int n = snprintf(line, sizeof line, "[vram ] frame %lu: pc %p touches %08x\n", gsp_frames,
                         (void *)pc, (unsigned)(at - albw_mem));
        if (n > 0) (void)!write(1, line, (size_t)n);
    }
    mprotect(albw_mem + VRAM_FROM, VRAM_SIZE, PROT_READ | PROT_WRITE);
}

static void vram_close(void) {
    if (mprotect(albw_mem + VRAM_FROM, VRAM_SIZE, PROT_NONE) != 0) perror("[vram ] mprotect");
}

static void vram_open(void) {
    mprotect(albw_mem + VRAM_FROM, VRAM_SIZE, PROT_READ | PROT_WRITE);
}

#endif

static void svc_body(albw_cpu *c, uint32_t imm);

void albw_svc(albw_cpu *c, uint32_t imm) {
#ifdef _WIN32
    svc_body(c, imm);
#else
    if (vram_watch < 0) {
        const char *e = albw_env("ALBW_WATCH_VRAM");
        vram_watch = e && *e && *e != '0';
        if (vram_watch) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = vram_fault;
            sa.sa_flags = SA_SIGINFO | SA_NODEFER;
            sigaction(SIGSEGV, &sa, NULL);
            printf("[vram ] watching guest access to VRAM\n");
        }
    }
    if (vram_watch) vram_open();
    svc_body(c, imm);
    if (vram_watch) vram_close();
#endif
}

static void svc_body(albw_cpu *c, uint32_t imm) {
    svc_count++;

    switch (imm) {
        case 0x2D: {                                    // ConnectToPort
            char port[16];
            guest_string(c->r[1], port, sizeof port);
            uint32_t h = handle_create(ALBW_H_SESSION, port);
            if (log_wanted("svc ConnectToPort")) printf("[svc ] ConnectToPort(\"%s\") -> handle 0x%08x\n", port, h);
            c->r[1] = h;
            c->r[0] = 0;
            return;
        }

        case 0x32: {                                    // SendSyncRequest
            ipc_dispatch(c, c->r[0]);
            // Every reply puts its result in cmdbuf[1], so one check here catches a failing
            // service call whichever handler produced it -- and a failure the guest then turns
            // into a fatal error is otherwise invisible among the rate-limited logs.
            uint32_t result = albw_r32(albw_cmdbuf(c) + 4);
            if (result & 0x80000000u) {
                printf("[ipc ] reply to %s is an error: 0x%08x "
                       "(module %u, description %u), asked from 0x%08x\n",
                       handle_index(c->r[0]) >= 0
                           ? handles[handle_index(c->r[0])].name : "<bad handle>", result,
                       (result >> 10) & 0xFFu, result & 0x3FFu, c->r[14]);
            }
            return;
        }

        case 0x17: {                                    // CreateEvent(reset type in r1)
            uint32_t h = handle_create(ALBW_H_EVENT, NULL);
            int i = handle_index(h);
            if (i >= 0) handles[i].reset_type = c->r[1];
            // lr names the guest code that made it, which is how an event nobody signals gets
            // traced back to whatever was supposed to signal it.
            printf("[svc ] CreateEvent -> 0x%08x (reset=%u) from 0x%08x, thread %d\n",
                   h, c->r[1], c->r[14], albw_thread_current());
            c->r[1] = h;
            c->r[0] = 0;
            return;
        }

        case 0x18: {                                    // SignalEvent
            int i = handle_index(c->r[0]);
            if (i >= 0) {
                handles[i].signalled = 1;
                int woken = albw_thread_wake(ALBW_W_SYNC, c->r[0], -1);
                // A oneshot event is consumed by the wait it releases.
                if (woken && handles[i].reset_type == 0) handles[i].signalled = 0;
            }
            c->r[0] = 0;
            return;
        }

        case 0x19: {                                    // ClearEvent
            int i = handle_index(c->r[0]);
            if (i >= 0) handles[i].signalled = 0;
            c->r[0] = 0;
            return;
        }

        case 0x14: {                                    // ReleaseMutex
            int i = handle_index(c->r[0]);
            if (i >= 0) {
                handles[i].signalled = 1;
                if (albw_thread_wake(ALBW_W_SYNC, c->r[0], 1)) handles[i].signalled = 0;
            }
            c->r[0] = 0;
            return;
        }
        case 0x13: {                                    // CreateMutex(initial in r1)
            uint32_t h = handle_create(ALBW_H_MUTEX, NULL);
            c->r[1] = h;
            c->r[0] = 0;
            return;
        }
        case 0x21: {                                    // CreateAddressArbiter
            uint32_t h = handle_create(ALBW_H_ARBITER, NULL);
            c->r[1] = h;
            c->r[0] = 0;
            return;
        }
        case 0x1E: {                                    // CreateMemoryBlock
            // r1 is the block's address and r2 its size. Kept, because a library applet is handed
            // the block and writes into it -- the software keyboard puts its text there.
            uint32_t h = handle_create(ALBW_H_MEMBLOCK, NULL);
            int i = handle_index(h);
            if (i >= 0) {
                handles[i].base = c->r[1];
                handles[i].size = c->r[2];
            }
            c->r[1] = h;
            c->r[0] = 0;
            return;
        }

        case 0x27: {                                    // DuplicateHandle(original in r1)
            uint32_t src = c->r[1];
            if (src == ALBW_HANDLE_CURRENT_PROCESS || src == ALBW_HANDLE_CURRENT_THREAD) {
                c->r[1] = src;                          // duplicating yourself is itself
                c->r[0] = 0;
                return;
            }
            int i = handle_index(src);
            if (i < 0) {
                printf("[svc ] DuplicateHandle(0x%08x): unknown handle\n", src);
                c->r[0] = 0xD8E007F7u;                  // invalid handle
                return;
            }
            uint32_t dup = handle_create(handles[i].kind, handles[i].name);
            printf("[svc ] DuplicateHandle(0x%08x %s) -> 0x%08x\n",
                   src, kind_name(handles[i].kind), dup);
            c->r[1] = dup;
            c->r[0] = 0;
            return;
        }

        case 0x08: {                                    // CreateThread
            // r0=priority r1=entry r2=arg r3=stack_top r4=processor, per the wrapper at
            // 0x00103c80, which loads the last two from the caller's stack.
            int tid = albw_thread_create(c->r[1], c->r[2], c->r[3], (int32_t)c->r[0]);
            if (tid < 0) {
                c->r[0] = 0xD8600403u;                  // out of resources
                return;
            }
            uint32_t h = handle_create(ALBW_H_THREAD, NULL);
            int hi = handle_index(h);
            if (hi >= 0) {
                handles[hi].base = (uint64_t)tid;
                handles[hi].signalled = 0;
                handles[hi].reset_type = 1;             // a finished thread stays finished
            }
            printf("[svc ] CreateThread(entry=%08x arg=%08x stack=%08x prio=%u)"
                   " -> thread %d, handle 0x%08x\n",
                   c->r[1], c->r[2], c->r[3], c->r[0], tid, h);
            c->r[1] = h;
            c->r[0] = 0;
            return;
        }

        case 0x09: {                                    // ExitThread
            // Its handles are signalled, and whoever waits on them wakes. Before this a thread
            // that had ended left the one waiting for it waiting for good -- the main thread,
            // after quitting to the title screen, which then stayed black.
            int tid = albw_thread_current();
            int handled = 0;
            for (int k = 0; k < ALBW_MAX_HANDLES; k++) {
                if (handles[k].kind != ALBW_H_THREAD || handles[k].base != (uint64_t)tid) continue;
                handled = 1;
                handles[k].signalled = 1;
                albw_thread_wake(ALBW_W_SYNC, handle_value(k), -1);
            }
            if (!handled) albw_thread_detach(tid);      // nobody can wait for it or close it
            printf("[svc ] thread %d exited\n", tid);
            albw_thread_exit();
            return;                                     // not reached
        }

        case 0x0A:                                      // SleepThread
            // Blocking, not yielding. A yield leaves the thread runnable, so the scheduler
            // picks it straight back up when everyone else is waiting - and since time only
            // advances when nothing can run, it never advanced. The audio worker's poll loop
            // therefore span 314 million times while vblank never fired once.
            albw_thread_block(ALBW_W_SLEEP, 0);
            c->r[0] = 0;
            return;

        case 0x22: {                                    // ArbitrateAddress
            // r0=arbiter r1=address r2=type r3=value. Horizon's address arbiter is a futex:
            // waiters park on a guest address and a signal releases them.
            uint32_t addr = c->r[1], type = c->r[2];
            int32_t value = (int32_t)c->r[3];
            // The guest's own locks live at these addresses and it manipulates them with
            // ldrex/strex, which the kernel never sees - so the only way to follow a
            // contended lock is to record the value each time the arbiter is asked about it.
            static const char *types[] = { "signal", "wait<", "dec+wait<",
                                           "wait<+to", "dec+wait<+to" };
            char akey[48];
            // The address stays out of the key: one key per guest address filled
            // the table, and a full table hides every event kind that appears
            // after it -- which is how a failing service call went unlogged.
            snprintf(akey, sizeof akey, "arbiter type %u", type);
            if (log_wanted(akey)) {
                printf("[arb  ] thread %d: %s on %08x, *addr=%d, value=%d\n",
                       albw_thread_current(), type < 5 ? types[type] : "?", addr,
                       (int32_t)albw_r32(addr), value);
            }
            switch (type) {
                case 0:                                 // signal
                    albw_thread_wake(ALBW_W_ADDRESS, addr, value < 0 ? -1 : value);
                    break;
                case 2: case 4:                         // decrement, then wait if less
                    if ((int32_t)albw_r32(addr) < value) {
                        albw_w32(addr, albw_r32(addr) - 1u);
                        albw_thread_block(ALBW_W_ADDRESS, addr);
                    }
                    break;
                case 1: case 3:                         // wait if less than
                    if ((int32_t)albw_r32(addr) < value) {
                        albw_thread_block(ALBW_W_ADDRESS, addr);
                    }
                    break;
                default:
                    printf("[svc ] ArbitrateAddress: unknown type %u\n", type);
                    break;
            }
            c->r[0] = 0;
            return;
        }

        case 0x24: {                                    // WaitSynchronization1(handle)
            // Yielding and then reporting success was a livelock: two threads each woke
            // the other and neither made progress. Blocking until something signals is
            // both correct and diagnosable - a wait nobody can satisfy now surfaces as a
            // reported deadlock instead of a spin.
            c->r[0] = albw_wait_one(c, c->r[0]);
            return;
        }

        case 0x25: {                                    // WaitSynchronizationN
            // The wrapper at 0x0010639c settles the ABI: r0 is the timeout, r1 the handle
            // array, r2 the count, r3 waitAll - and the index of the handle that fired comes
            // back in r1, which the stub stores. Treating r0 as the out pointer wrote to a
            // timeout value as an address and segfaulted the host.
            uint32_t list = c->r[1];
            int32_t count = (int32_t)c->r[2];
            int wait_all = (int)c->r[3];
            if (count <= 0 || count > 64 || !albw_guest_ok(list, 4u * (uint32_t)count)) {
                printf("[svc ] WaitSynchronizationN: %d handles at %08x is not sane\n",
                       count, list);
                c->r[0] = 0xD8E007F7u;
                return;
            }
            if (wait_all) {
                for (int32_t i = 0; i < count; i++) {
                    uint32_t res = albw_wait_one(c, albw_r32(list + 4u * (uint32_t)i));
                    if (res) { c->r[0] = res; return; }
                }
                c->r[1] = 0;
                c->r[0] = 0;
                return;
            }
            for (int32_t i = 0; i < count; i++) {
                int hi2 = handle_index(albw_r32(list + 4u * (uint32_t)i));
                if (hi2 >= 0 && handles[hi2].signalled) {
                    if (handles[hi2].reset_type == 0) handles[hi2].signalled = 0;
                    c->r[1] = (uint32_t)i;
                    c->r[0] = 0;
                    return;
                }
            }
            // None ready: wait on all of them, and say afterwards which one fired. Parking on the
            // first alone, as this did, left the game's applet thread -- which waits on APT's
            // notification and parameter events together -- deaf to a parameter, and reported every
            // wake-up as a notification.
            uint32_t tokens[64];
            for (int32_t i = 0; i < count; i++) tokens[i] = albw_r32(list + 4u * (uint32_t)i);
            if (log_wanted("svc WaitSynchronizationN blocks")) {
                printf("[svc ] WaitSynchronizationN on %d handles, none ready; blocking on all\n",
                       count);
            }
            albw_thread_block_any(ALBW_W_SYNC, tokens, count);
            c->r[1] = 0;
            for (int32_t i = 0; i < count; i++) {
                int hi3 = handle_index(tokens[i]);
                if (hi3 >= 0 && handles[hi3].signalled) {
                    if (handles[hi3].reset_type == 0) handles[hi3].signalled = 0;
                    c->r[1] = (uint32_t)i;
                    break;
                }
            }
            c->r[0] = 0;
            return;
        }

        case 0x23: {                                    // CloseHandle
            uint32_t h = c->r[0];
            int i = handle_index(h);
            if (i >= 0 && handles[i].kind == ALBW_H_THREAD) {
                // The last handle to a thread lets its slot go, now if it has finished and as it
                // finishes otherwise: without that, every thread ever made kept one, and the
                // twenty-fifth could not be made.
                int tid = (int)handles[i].base, others = 0;
                for (int k = 0; k < ALBW_MAX_HANDLES; k++) {
                    if (k != i && handles[k].kind == ALBW_H_THREAD && handles[k].base == handles[i].base) {
                        others = 1;
                    }
                }
                if (!others) albw_thread_detach(tid);
            }
            if (i >= 0) {
                handles[i].kind = ALBW_H_FREE;
            } else if (h != ALBW_HANDLE_CURRENT_PROCESS && h != ALBW_HANDLE_CURRENT_THREAD) {
                printf("[svc ] CloseHandle(0x%08x): unknown handle\n", h);
            }
            c->r[0] = 0;
            return;
        }

        case 0x01: {                                    // ControlMemory
            // r0=operation r1=addr0 r2=addr1 r3=size, address back in r1. Guest memory is
            // one flat mapping, so this only has to hand out an address.
            uint32_t op = c->r[0], addr0 = c->r[1], size = c->r[3];
            uint32_t addr = addr0;
            if (!addr) {
                int linear = (op & 0x10000u) != 0;
                uint32_t *next = linear ? &linear_next : &heap_next;
                uint32_t limit = linear ? ALBW_LINEAR_LIMIT : ALBW_HEAP_LIMIT;
                uint32_t rounded = (size + 0xFFFu) & ~0xFFFu;
                // Compared against the space left rather than by adding first: a size of
                // 0xFFF9F000 wrapped the sum around and slipped past the check.
                if (rounded < size || rounded > limit - *next) {
                    fprintf(stderr, "[svc ] ControlMemory: cannot give %08x bytes of %s "
                                    "memory (%u MiB left)\n", size,
                            linear ? "linear" : "heap", (limit - *next) >> 20);
                    c->r[0] = 0xD86007F3u;              // out of memory
                    return;
                }
                addr = *next;
                *next += rounded;
            }
            if (log_wanted("svc ControlMemory")) printf("[svc ] ControlMemory(op=%08x addr=%08x size=%08x) -> %08x\n",
                   c->r[0], addr0, size, addr);
            c->r[1] = addr;
            c->r[0] = 0;
            return;
        }

        case 0x38: {                                    // GetResourceLimit
            uint32_t h = handle_create(ALBW_H_RESLIMIT, NULL);
            c->r[1] = h;
            c->r[0] = 0;
            return;
        }

        case 0x39:                                      // GetResourceLimitLimitValues
        case 0x3A: {                                    // ...CurrentValues
            // svc(values*, limitHandle, names*, count) with 64-bit values, read off the
            // caller at 0x001022a0: it asks for one name and returns the low half.
            uint32_t values = c->r[0], names = c->r[2];
            int32_t count = (int32_t)c->r[3];
            if (count < 0 || !albw_guest_ok(names, 4u * (uint32_t)count)
                    || !albw_guest_ok(values, 8u * (uint32_t)count)) {
                printf("[svc ] ResourceLimit: %d names at %08x is not sane\n", count, names);
                c->r[0] = 0xD8E007F7u;
                return;
            }
            for (int32_t i = 0; i < count && i < 8; i++) {
                uint32_t name = albw_r32(names + 4u * (uint32_t)i);
                uint64_t v = albw_resource_limit(name, imm == 0x39);
                albw_w32(values + 8u * (uint32_t)i, (uint32_t)v);
                albw_w32(values + 8u * (uint32_t)i + 4u, (uint32_t)(v >> 32));
                char key[32];
                snprintf(key, sizeof key, "reslimit %s %u",
                         imm == 0x39 ? "limit" : "current", name);
                if (log_wanted(key)) {
                    printf("[svc ] ResourceLimit %s name=%u -> %llu\n",
                           imm == 0x39 ? "limit" : "current", name,
                           (unsigned long long)v);
                }
            }
            c->r[0] = 0;
            return;
        }

        case 0x37:                                      // GetThreadId
            c->r[1] = 1;                                // one thread runs so far
            c->r[0] = 0;
            return;

        case 0x35:                                      // GetProcessId
            c->r[1] = 1;
            c->r[0] = 0;
            return;

        case 0x28: {                                    // GetSystemTick
            // The console's 268 MHz counter, as the guest sees it: at least a frame's worth of
            // ticks per frame, so that the game's clocks keep the pace of its frames, and at least
            // a little more on every call, so that a loop waiting for a deadline always gets there.
            // Both inputs are the instruction stream's, which keeps the reference run in step.
            //
            // It used to be the syscall count times a thousand, in the low word only: thirty times
            // a frame meant time ran at a hundred and fiftieth of its speed, and after 4.3 million
            // calls the high word that was never set made it run backwards.
            static uint64_t last_tick;
            uint64_t floor = (uint64_t)gsp_frames * ALBW_TICKS_PER_FRAME;
            if (kernel_time > floor) floor = kernel_time;
            last_tick = last_tick + 1000u > floor ? last_tick + 1000u : floor;
            c->r[0] = (uint32_t)last_tick;
            c->r[1] = (uint32_t)(last_tick >> 32);
            return;
        }

        case 0x03:
            printf("[svc ] ExitProcess after %lu syscalls\n", svc_count);
            exit(0);

        case 0x3C:
            fprintf(stderr, "[svc ] Break: the guest failed an assertion of its own\n");
            tell_player("World Across: the game stopped",
                        "The game failed one of its own checks and stopped (svcBreak).\n\n"
                        "That usually means the port answered one of its requests wrongly. "
                        "Anything since your last save is lost.");
            exit(1);

        case 0x1F: {                                    // MapMemoryBlock
            // r0=handle r1=address. Guest memory is one flat mapping, so mapping itself is
            // a no-op; what matters is remembering where the GSP block landed, because the
            // interrupt queue has to be written where the guest will read it.
            if (c->r[0] == gsp_shared && gsp_shared) {
                gsp_shared_addr = c->r[1];
                memset(albw_mem + gsp_shared_addr, 0, 0x1000);
                printf("[gsp  ] shared block mapped at %08x\n", gsp_shared_addr);
            }
            if (c->r[0] == hid_shared && hid_shared) {
                hid_shared_addr = c->r[1];
                memset(albw_mem + hid_shared_addr, 0, 0x1000);
                printf("[hid  ] shared block mapped at %08x\n", hid_shared_addr);
            }
            c->r[0] = 0;
            return;
        }

        default:
            // Reporting success keeps the boot path moving so the log shows what is
            // actually needed next, rather than stopping at the first gap.
            char stub_key[40];
            snprintf(stub_key, sizeof stub_key, "svc %s", svc_name(imm));
            if (log_wanted(stub_key)) {
                printf("[svc ] %-32s (0x%02x) r0=%08x r1=%08x r2=%08x  (stub)\n",
                       svc_name(imm), imm, c->r[0], c->r[1], c->r[2]);
            }
            c->r[0] = 0;
            return;
    }
}

const char *albw_kernel_stop_note(void) { return kernel_stop_note; }

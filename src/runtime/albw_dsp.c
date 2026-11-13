#include "albw_dsp.h"

#include "albw_cpu.h"
#include "albw_kernel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// The shared region.
//
// 0x8000 bytes, the same layout twice. Offsets are bytes from the region's start; the firmware's
// own addresses for them -- the ones the Initialize reply lists -- are 0x8000 plus half of these.
// Words the DSP itself treats as 32 bits are stored high half first.
#define R_DSP_STATUS     0x0800u
#define R_FINAL_SAMPLES  0x0A80u                         // s16 [160][2]
#define R_SOURCE_STATUS  0x0D00u                         // 24 of 12 bytes
#define R_DSP_CONFIG     0x2860u
#define R_INTERMEDIATE   0x2924u                         // s32 [2 buses][4 channels][160]
#define R_SOURCE_CONFIG  0x3D24u                         // 24 of 192 bytes
#define R_ADPCM_COEFFS   0x4F24u                         // s16 [24][16]
#define R_FRAME_COUNTER  0x7FFEu

#define SOURCES 24
#define FRAME   ALBW_DSP_SAMPLES_PER_FRAME

// A voice's configuration, by byte offset.
#define C_DIRTY          0
#define C_GAIN           4                               // float [3][4]
#define C_RATE           52
#define C_INTERPOLATION  56
#define C_FILTERS_ON     58
#define C_SIMPLE         60                              // b0, a1
#define C_BIQUAD         64                              // a2, a1, b2, b1, b0
#define C_BUFFERS_DIRTY  74
#define C_BUFFERS        76                              // 4 of 20 bytes
#define C_ENABLE         160
#define C_SYNC_COUNT     162
#define C_PLAY_POSITION  164
#define C_ADDRESS        172
#define C_LENGTH         176
#define C_FLAGS1         180                             // channels in bits 0-1, format in 2-3
#define C_ADPCM_PS       182
#define C_ADPCM_YN       184
#define C_FLAGS2         188                             // ADPCM history valid in bit 0, loop in 1
#define C_BUFFER_ID      190

// Which settings changed, in the configuration's first word.
#define D_FORMAT          (1u << 0)
#define D_CHANNELS        (1u << 1)
#define D_ADPCM_COEFFS    (1u << 2)
#define D_PARTIAL_EMBED   (1u << 3)
#define D_PARTIAL_RESET   (1u << 4)
#define D_ENABLE          (1u << 16)
#define D_INTERPOLATION   (1u << 17)
#define D_RATE            (1u << 18)
#define D_BUFFER_QUEUE    (1u << 19)
#define D_LOOP_RELATED    (1u << 20)
#define D_PLAY_POSITION   (1u << 21)
#define D_FILTERS_ON      (1u << 22)
#define D_SIMPLE          (1u << 23)
#define D_BIQUAD          (1u << 24)
#define D_GAIN0           (1u << 25)
#define D_SYNC_COUNT      (1u << 28)
#define D_RESET           (1u << 29)
#define D_EMBEDDED        (1u << 30)

enum { FORMAT_PCM8 = 0, FORMAT_PCM16 = 1, FORMAT_ADPCM = 2 };
enum { INTERP_POLYPHASE = 0, INTERP_LINEAR = 1, INTERP_NONE = 2 };

static uint32_t rd32(uint32_t a) { return albw_r32(a); }
static uint16_t rd16(uint32_t a) { return albw_r16(a); }
static uint32_t rd_dsp32(uint32_t a) { return ((uint32_t)albw_r16(a) << 16) | albw_r16(a + 2); }
static float rdf(uint32_t a) {
    uint32_t v = albw_r32(a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static void wr16(uint32_t a, uint16_t v) { albw_w16(a, v); }
static void wr_dsp32(uint32_t a, uint32_t v) {
    albw_w16(a, (uint16_t)(v >> 16));
    albw_w16(a + 2, (uint16_t)v);
}

// The physical addresses voices name, mapped to where the guest sees that memory.
static uint32_t phys_to_guest(uint32_t phys, uint32_t len) {
    uint32_t guest;
    if (phys >= 0x20000000u && phys < 0x28000000u) guest = phys - 0x20000000u + 0x14000000u;
    else if (phys >= 0x18000000u && phys < 0x18600000u) guest = phys - 0x18000000u + 0x1F000000u;
    else return 0;
    return albw_guest_ok(guest, len) ? guest : 0;
}

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

// ---------------------------------------------------------------------------------------------
// Voices.
struct buffer {
    uint32_t address, length;                            // physical, and in samples
    uint16_t id;
    int16_t yn1, yn2;
    int adpcm_dirty, looping, from_queue, has_played;
    uint32_t play_position;
    int format, stereo;
};

#define QUEUE_MAX 32

struct source {
    int enabled;
    uint16_t sync_count;
    float rate;
    int interpolation;
    float gain[3][4];
    int simple_on, biquad_on;
    int16_t simple_b0, simple_a1;
    int16_t biquad_a1, biquad_a2, biquad_b0, biquad_b1, biquad_b2;
    int32_t simple_y1[2], biquad_x1[2], biquad_x2[2], biquad_y1[2], biquad_y2[2];
    int format, stereo;
    int16_t coeff[16];
    int32_t yn1, yn2;                                    // the ADPCM decoder's history

    struct buffer queue[QUEUE_MAX];                      // waiting, lowest buffer id first
    int queued;

    int16_t *samples;                                    // the playing buffer, decoded to stereo
    uint32_t sample_count, sample_room, next;            // next: the first not yet resampled
    uint32_t position;                                   // samples consumed, for the status
    uint16_t current_id, last_id;
    int buffer_update;

    uint64_t fraction;                                   // the resampler's position, 24 bits
    int16_t xn1[2], xn2[2];                              // and the two samples before it

    int16_t frame[FRAME][2];
};

static struct source sources[SOURCES];
static uint32_t regions[2];
static int running;
static void (*output)(const int16_t *stereo, unsigned frames);

// The mixer's own state.
static float bus_volume[3] = { 1.0f, 0.0f, 0.0f };
static int aux_enabled[2];
static int output_format = 1;                            // 0 mono, 1 stereo, 2 surround

void albw_dsp_set_output(void (*out)(const int16_t *stereo, unsigned frames)) { output = out; }
int albw_dsp_running(void) { return running; }

// What was played, for the report at the end. ALBW_DSP_LOG=<frame> also prints, from that frame
// on, each buffer a voice starts, each voice the game switches on or off and each that runs dry.
static unsigned long stat_buffers[3][2], stat_cut_short, stat_ends;

static int logging(void) {
    static int checked;
    static unsigned long from = ~0ul;
    if (!checked) {
        checked = 1;
        const char *e = albw_env("ALBW_DSP_LOG");
        if (e) from = strtoul(e, NULL, 0);
    }
    return albw_kernel_frame() >= from;
}

static void source_reset(struct source *s) {
    free(s->samples);
    memset(s, 0, sizeof *s);
    s->rate = 1.0f;
    s->interpolation = INTERP_LINEAR;
    s->format = FORMAT_PCM16;
    s->stereo = 0;
}

void albw_dsp_start(uint32_t region0, uint32_t region1) {
    regions[0] = region0;
    regions[1] = region1;
    for (int i = 0; i < SOURCES; i++) source_reset(&sources[i]);
    bus_volume[0] = 1.0f;
    bus_volume[1] = bus_volume[2] = 0.0f;
    aux_enabled[0] = aux_enabled[1] = 0;
    output_format = 1;
    running = 1;
}

void albw_dsp_stop(void) { running = 0; }

static void enqueue(struct source *s, const struct buffer *b) {
    if (s->queued == QUEUE_MAX) {
        static unsigned said;
        if (said++ < 4) printf("[dsp  ] a voice's buffer queue is full; dropping buffer %u\n", b->id);
        return;
    }
    int at = s->queued;
    while (at > 0 && s->queue[at - 1].id > b->id) {
        s->queue[at] = s->queue[at - 1];
        at--;
    }
    s->queue[at] = *b;
    s->queued++;
}

// Reads a buffer description: the embedded one (base C_ADDRESS) or one of the four queued.
static void read_buffer(uint32_t at, struct buffer *b) {
    b->address = rd_dsp32(at + 0);
    b->length = rd_dsp32(at + 4);
    b->yn1 = (int16_t)rd16(at + 10);
    b->yn2 = (int16_t)rd16(at + 12);
    b->adpcm_dirty = albw_r8(at + 14) != 0;
    b->looping = albw_r8(at + 15) != 0;
    b->id = rd16(at + 16);
}

static void parse_config(struct source *s, uint32_t config, uint32_t coeffs) {
    uint32_t dirty = rd32(config + C_DIRTY);
    if (!dirty) return;

    if (dirty & D_RESET) {
        // The voice starts over; the decoded buffer goes with it.
        int16_t *keep = s->samples;
        uint32_t room = s->sample_room;
        s->samples = NULL;
        source_reset(s);
        s->samples = keep;
        s->sample_room = room;
    }
    if (dirty & D_PARTIAL_RESET) s->queued = 0;
    if (dirty & D_ENABLE) {
        s->enabled = albw_r8(config + C_ENABLE) != 0;
        if (logging()) {
            printf("[dsp  ] frame %lu voice %d: switched %s, sync %u, dirty %08x\n",
                   albw_kernel_frame(), (int)(s - sources), s->enabled ? "on" : "off",
                   rd16(config + C_SYNC_COUNT), dirty);
        }
    }
    if (dirty & D_SYNC_COUNT) s->sync_count = rd16(config + C_SYNC_COUNT);
    if (dirty & D_RATE) {
        float rate = rdf(config + C_RATE);
        s->rate = rate > 0.0f && rate < 64.0f ? rate : 1.0f;
    }
    if (dirty & D_ADPCM_COEFFS) {
        for (int i = 0; i < 16; i++) s->coeff[i] = (int16_t)rd16(coeffs + (uint32_t)i * 2u);
    }
    for (int bus = 0; bus < 3; bus++) {
        if (!(dirty & (D_GAIN0 << bus))) continue;
        for (int ch = 0; ch < 4; ch++) {
            s->gain[bus][ch] = rdf(config + C_GAIN + (uint32_t)(bus * 4 + ch) * 4u);
        }
    }
    if (dirty & D_FILTERS_ON) {
        uint16_t on = rd16(config + C_FILTERS_ON);
        s->simple_on = on & 1u;
        s->biquad_on = (on >> 1) & 1u;
    }
    if (dirty & D_SIMPLE) {
        s->simple_b0 = (int16_t)rd16(config + C_SIMPLE);
        s->simple_a1 = (int16_t)rd16(config + C_SIMPLE + 2);
    }
    if (dirty & D_BIQUAD) {
        s->biquad_a2 = (int16_t)rd16(config + C_BIQUAD);
        s->biquad_a1 = (int16_t)rd16(config + C_BIQUAD + 2);
        s->biquad_b2 = (int16_t)rd16(config + C_BIQUAD + 4);
        s->biquad_b1 = (int16_t)rd16(config + C_BIQUAD + 6);
        s->biquad_b0 = (int16_t)rd16(config + C_BIQUAD + 8);
    }
    if (dirty & D_INTERPOLATION) s->interpolation = albw_r8(config + C_INTERPOLATION);
    uint16_t flags1 = rd16(config + C_FLAGS1);
    if (dirty & (D_FORMAT | D_EMBEDDED)) s->format = (flags1 >> 2) & 3u;
    if (dirty & (D_CHANNELS | D_EMBEDDED)) s->stereo = (flags1 & 3u) == 2u;

    uint32_t play_position = 0;
    if (dirty & D_PLAY_POSITION) play_position = rd_dsp32(config + C_PLAY_POSITION);

    if (dirty & D_PARTIAL_EMBED) {
        // The game lengthened the buffer that is playing. Simplest faithful answer: forget what is
        // left of it and let the queue carry on; the case is rare and logged.
        static unsigned said;
        if (said++ < 4) printf("[dsp  ] a voice's playing buffer was extended; not followed\n");
    }

    if (dirty & D_EMBEDDED) {
        struct buffer b;
        memset(&b, 0, sizeof b);
        b.address = rd_dsp32(config + C_ADDRESS);
        b.length = rd_dsp32(config + C_LENGTH);
        b.yn1 = (int16_t)rd16(config + C_ADPCM_YN);
        b.yn2 = (int16_t)rd16(config + C_ADPCM_YN + 2);
        uint16_t flags2 = rd16(config + C_FLAGS2);
        b.adpcm_dirty = flags2 & 1u;
        b.looping = (flags2 >> 1) & 1u;
        b.id = rd16(config + C_BUFFER_ID);
        b.play_position = play_position;
        b.format = s->format;
        b.stereo = s->stereo;
        if ((int32_t)b.length >= 0) enqueue(s, &b);
    }

    if (dirty & D_BUFFER_QUEUE) {
        uint16_t which = rd16(config + C_BUFFERS_DIRTY);
        for (int i = 0; i < 4; i++) {
            if (!((which >> i) & 1u)) continue;
            struct buffer b;
            memset(&b, 0, sizeof b);
            read_buffer(config + C_BUFFERS + (uint32_t)i * 20u, &b);
            b.from_queue = 1;
            b.format = s->format;
            b.stereo = s->stereo;
            if ((int32_t)b.length >= 0) enqueue(s, &b);
        }
        wr16(config + C_BUFFERS_DIRTY, 0);
    }

    // Read and acted on: the firmware clears the word, and the game waits for it to see the
    // change taken.
    albw_w32(config + C_DIRTY, 0);
}

// ---------------------------------------------------------------------------------------------
// Decoding.
static int room_for(struct source *s, uint32_t frames) {
    if (frames <= s->sample_room) return 1;
    int16_t *bigger = realloc(s->samples, (size_t)frames * 2u * sizeof *bigger);
    if (!bigger) return 0;
    s->samples = bigger;
    s->sample_room = frames;
    return 1;
}

// Nintendo's ADPCM: frames of eight bytes, a header byte -- the coefficient pair in bits 4-6, the
// scale exponent in 0-3 -- and fourteen four-bit samples, high nibble first, each run through a
// second-order predictor with coefficients in 11-bit fixed point.
static void decode_adpcm(struct source *s, const uint8_t *data, uint32_t count) {
    static const int nibble[16] = { 0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1 };
    int32_t yn1 = s->yn1, yn2 = s->yn2;
    uint32_t out = 0;
    for (uint32_t f = 0; out < count; f++) {
        const uint8_t *frame = data + f * 8u;
        int scale = 1 << (frame[0] & 0xF);
        int pair = (frame[0] >> 4) & 7;
        int32_t c1 = s->coeff[pair * 2], c2 = s->coeff[pair * 2 + 1];
        for (int i = 0; i < 14 && out < count; i++) {
            uint8_t byte = frame[1 + i / 2];
            int n = nibble[(i & 1) ? (byte & 0xF) : (byte >> 4)];
            int32_t v = ((n * scale) << 11) + 0x400 + c1 * yn1 + c2 * yn2;
            int16_t sample = clamp16(v >> 11);
            yn2 = yn1;
            yn1 = sample;
            s->samples[out * 2] = s->samples[out * 2 + 1] = sample;
            out++;
        }
    }
    s->yn1 = yn1;
    s->yn2 = yn2;
}


static int dequeue(struct source *s) {
    if (!s->queued) return 0;
    struct buffer b = s->queue[0];
    if (b.format < 3) stat_buffers[b.format][b.stereo ? 1 : 0]++;
    if (logging()) {
        static const char *formats[] = { "PCM8", "PCM16", "ADPCM", "?" };
        printf("[dsp  ] frame %lu voice %d: buffer %u, %s %s, %u samples at %08x, rate %.3f, "
               "interpolation %d%s%s\n", albw_kernel_frame(), (int)(s - sources), b.id,
               formats[b.format & 3], b.stereo ? "stereo" : "mono", b.length, b.address,
               (double)s->rate, s->interpolation, b.looping ? ", looping" : "",
               b.from_queue ? ", queued" : ", embedded");
    }
    memmove(&s->queue[0], &s->queue[1], (size_t)(s->queued - 1) * sizeof s->queue[0]);
    s->queued--;

    if (b.adpcm_dirty) {
        s->yn1 = b.yn1;
        s->yn2 = b.yn2;
    }
    uint32_t channels = b.stereo ? 2u : 1u;
    uint32_t bytes = b.format == FORMAT_ADPCM ? (b.length + 13u) / 14u * 8u
                   : b.length * channels * (b.format == FORMAT_PCM8 ? 1u : 2u);
    uint32_t at = phys_to_guest(b.address & ~3u, bytes);
    s->sample_count = 0;
    s->next = 0;
    if (at && b.length && room_for(s, b.length)) {
        const uint8_t *data = albw_mem + at;
        switch (b.format) {
            case FORMAT_PCM8:
                for (uint32_t i = 0; i < b.length; i++) {
                    int16_t l = (int16_t)((int8_t)data[i * channels] * 256);
                    int16_t r = (int16_t)((int8_t)data[i * channels + channels - 1] * 256);
                    s->samples[i * 2] = l;
                    s->samples[i * 2 + 1] = r;
                }
                break;
            case FORMAT_PCM16:
                for (uint32_t i = 0; i < b.length; i++) {
                    int16_t l, r;
                    memcpy(&l, data + i * channels * 2u, 2);
                    memcpy(&r, data + (i * channels + channels - 1) * 2u, 2);
                    s->samples[i * 2] = l;
                    s->samples[i * 2 + 1] = r;
                }
                break;
            case FORMAT_ADPCM:
                decode_adpcm(s, data, b.length);
                break;
            default:
                break;
        }
        s->sample_count = b.length;
    } else if (b.length && !at) {
        static unsigned said;
        if (said++ < 8) {
            printf("[dsp  ] a voice names buffer %u at %08x, which is not memory here\n", b.id,
                   b.address);
        }
    }

    // A buffer starts at its play position the first time, from the beginning when it loops.
    s->position = b.has_played ? 0 : b.play_position;
    if (s->position > s->sample_count) s->position = 0;
    s->next = s->position;
    s->current_id = b.id;
    s->last_id = 0;
    s->buffer_update = b.from_queue && !b.has_played;
    if (b.looping) {
        b.has_played = 1;
        enqueue(s, &b);
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Resampling: step through the input at the voice's rate in 24-bit fixed point, two samples of
// history carried from one buffer to the next. Linear interpolation, or none; the firmware's
// polyphase filter is taken as linear.
static void resample(struct source *s, uint32_t *out_at) {
    const uint64_t step = (uint64_t)((double)s->rate * (double)(1u << 24));
    uint32_t available = s->sample_count - s->next;
    if (!available) return;
    // Input i is history for i < 2, the buffer's sample next + i - 2 after.
#define IN(i, ch) ((i) == 0 ? s->xn2[ch] : (i) == 1 ? s->xn1[ch] \
                   : s->samples[(s->next + (i) - 2u) * 2u + (ch)])
    uint32_t total = available + 2u;
    uint32_t i = 0;
    while (*out_at < FRAME) {
        i = (uint32_t)(s->fraction >> 24);
        if (i + 2u >= total) {
            i = total - 2u;
            break;
        }
        uint32_t f = (uint32_t)(s->fraction & 0xFFFFFFu);
        for (int ch = 0; ch < 2; ch++) {
            int32_t x0 = IN(i, ch), x1 = IN(i + 1u, ch);
            int32_t v = x0;
            if (s->interpolation != INTERP_NONE) {
                int32_t delta = x1 - x0;
                delta = delta < -32768 ? -32768 : (delta > 32767 ? 32767 : delta);
                v = x0 + (int32_t)(((int64_t)f * delta) >> 24);
            }
            s->frame[*out_at][ch] = clamp16(v);
        }
        (*out_at)++;
        s->fraction += step;
    }
    int16_t n2[2] = { IN(i, 0), IN(i, 1) }, n1[2] = { IN(i + 1u, 0), IN(i + 1u, 1) };
#undef IN
    memcpy(s->xn2, n2, sizeof n2);
    memcpy(s->xn1, n1, sizeof n1);
    s->fraction -= (uint64_t)i << 24;
    s->next += i;
    s->position += i;
}

static void filter_frame(struct source *s) {
    for (uint32_t n = 0; n < FRAME; n++) {
        for (int ch = 0; ch < 2; ch++) {
            int32_t x = s->frame[n][ch];
            if (s->simple_on) {
                int32_t y = (s->simple_b0 * x + s->simple_a1 * s->simple_y1[ch]) >> 15;
                x = clamp16(y);
                s->simple_y1[ch] = x;
            }
            if (s->biquad_on) {
                int32_t y = (s->biquad_b0 * x + s->biquad_b1 * s->biquad_x1[ch]
                             + s->biquad_b2 * s->biquad_x2[ch] + s->biquad_a1 * s->biquad_y1[ch]
                             + s->biquad_a2 * s->biquad_y2[ch]) >> 14;
                s->biquad_x2[ch] = s->biquad_x1[ch];
                s->biquad_x1[ch] = x;
                x = clamp16(y);
                s->biquad_y2[ch] = s->biquad_y1[ch];
                s->biquad_y1[ch] = x;
            }
            s->frame[n][ch] = (int16_t)x;
        }
    }
}

static void generate_frame(struct source *s) {
    memset(s->frame, 0, sizeof s->frame);
    if (s->next >= s->sample_count) {
        // Nothing playing. A waiting buffer starts on the next frame, as the firmware has it; with
        // none, the voice ends.
        if (dequeue(s)) return;
        stat_ends++;
        if (logging()) {
            printf("[dsp  ] frame %lu voice %d: ran dry after buffer %u\n", albw_kernel_frame(),
                   (int)(s - sources), s->current_id);
        }
        s->enabled = 0;
        s->buffer_update = 1;
        s->last_id = s->current_id;
        s->current_id = 0;
        return;
    }
    uint32_t out = 0;
    while (out < FRAME) {
        if (s->next >= s->sample_count && !dequeue(s)) {
            stat_cut_short++;                            // the voice ran out partway through
            break;
        }
        uint32_t before = out;
        resample(s, &out);
        if (out == before && s->next < s->sample_count) break;   // cannot happen; never spin
    }
    filter_frame(s);
}

static void write_status(const struct source *s, uint32_t at) {
    albw_w8(at + 0, (uint8_t)s->enabled);
    albw_w8(at + 1, (uint8_t)s->buffer_update);
    wr16(at + 2, s->sync_count);
    wr_dsp32(at + 4, s->position);
    wr16(at + 8, s->current_id);
    wr16(at + 10, s->last_id);
}

// ---------------------------------------------------------------------------------------------
// The frame.
static int read_index(void) {
    uint16_t c0 = albw_r16(regions[0] + R_FRAME_COUNTER), c1 = albw_r16(regions[1] + R_FRAME_COUNTER);
    // The newer counter, allowing for one having wrapped past the other.
    if (c0 == 0xFFFFu && c1 != 0xFFFEu) return 1;
    if (c1 == 0xFFFFu && c0 != 0xFFFEu) return 0;
    return c0 > c1 ? 0 : 1;
}

// The firmware's own effects, a delay and a reverb on each auxiliary bus, which this does not
// play. Said once each when the game first sets one up, with its words, so that an effect missing
// from the sound can be found in the log. The delay effects are 20 bytes from offset 44, the
// reverbs 52 bytes from 84.
//
// This game switches on a reverb on bus A and a delay on bus B at the title screen. The delay's
// words read: enable, outputs (4), a work buffer, 53 frames of delay (a quarter of a second), and
// three gains in 1.7 fixed point (0.59, 0.40, 0.59) -- 3dbrew describes a feedback delay with a gain
// on the feedback and a one-pole filter behind the delay, and 0.40 and 0.59 sum to one like such a
// filter's coefficients. The reverb has five work buffers of 1, 3, 19, 23 and 13 frames (each
// 1280 bytes a frame: four 16-bit channels) and seven gains (0.80, 0.90, 0.50, 0.58, 0.52, 0.25,
// 0.75); 3dbrew says two combs and an all-pass. Neither algorithm is known exactly.
static void note_effect(uint32_t config, uint32_t dirty) {
    static int said[4];
    for (int e = 0; e < 4; e++) {
        if (!(dirty & (1u << (10 + e))) || said[e]) continue;
        uint32_t at = config + (e < 2 ? 44u + 20u * (uint32_t)e : 84u + 52u * (uint32_t)(e - 2));
        uint32_t words = e < 2 ? 10u : 26u;
        if (!rd16(at + 2)) continue;                     // set up, but not switched on
        said[e] = 1;
        printf("[dsp  ] the game switches on the DSP's %s on auxiliary bus %d, which this does "
               "not play:", e < 2 ? "delay" : "reverb", e & 1);
        for (uint32_t i = 0; i < words; i++) printf(" %04x", rd16(at + 2u * i));
        printf("\n");
    }
}

static void parse_mixer_config(uint32_t config) {
    uint32_t dirty = rd32(config);
    if (!dirty) return;
    note_effect(config, dirty);
    if (dirty & (1u << 8)) aux_enabled[0] = rd16(config + 40) != 0;
    if (dirty & (1u << 9)) aux_enabled[1] = rd16(config + 42) != 0;
    if (dirty & (1u << 16)) bus_volume[0] = rdf(config + 4);
    if (dirty & (1u << 24)) bus_volume[1] = rdf(config + 8);
    if (dirty & (1u << 25)) bus_volume[2] = rdf(config + 12);
    if (dirty & (1u << 26)) output_format = rd16(config + 22);
    albw_w32(config, 0);
}

void albw_dsp_tick(void) {
    if (!running) return;
    int ri = read_index();
    uint32_t read = regions[ri], write = regions[ri ^ 1];

    static int32_t mix[3][FRAME][4];
    memset(mix, 0, sizeof mix);
    for (int v = 0; v < SOURCES; v++) {
        struct source *s = &sources[v];
        parse_config(s, read + R_SOURCE_CONFIG + (uint32_t)v * 192u,
                     read + R_ADPCM_COEFFS + (uint32_t)v * 32u);
        if (s->enabled) generate_frame(s);
        write_status(s, write + R_SOURCE_STATUS + (uint32_t)v * 12u);
        s->buffer_update = 0;
        if (!s->enabled) continue;
        for (int bus = 0; bus < 3; bus++) {
            const float *g = s->gain[bus];
            if (g[0] == 0.0f && g[1] == 0.0f && g[2] == 0.0f && g[3] == 0.0f) continue;
            for (uint32_t n = 0; n < FRAME; n++) {
                mix[bus][n][0] += (int32_t)(g[0] * s->frame[n][0]);
                mix[bus][n][1] += (int32_t)(g[1] * s->frame[n][1]);
                mix[bus][n][2] += (int32_t)(g[2] * s->frame[n][0]);
                mix[bus][n][3] += (int32_t)(g[3] * s->frame[n][1]);
            }
        }
    }

    parse_mixer_config(read + R_DSP_CONFIG);
    // The two auxiliary buses go out to the game for its effects and come back from it a frame
    // later, through the intermediate samples, when it has asked for that: 160 samples of four
    // 32-bit channels each (left A, right A, left B, right B), interleaved, per bus. This game
    // never asks; it leaves its auxiliary buses to the firmware's own reverb and delay (see
    // note_effect), and sends them little where it has been played so far -- a two-hundredth of
    // what the main bus carries outdoors.
    for (int aux = 0; aux < 2; aux++) {
        if (!aux_enabled[aux]) continue;
        uint32_t base_r = read + R_INTERMEDIATE + (uint32_t)aux * 2560u;
        uint32_t base_w = write + R_INTERMEDIATE + (uint32_t)aux * 2560u;
        int32_t back[FRAME][4];
        for (uint32_t n = 0; n < FRAME; n++) {
            for (int ch = 0; ch < 4; ch++) {
                uint32_t at = (n * 4u + (uint32_t)ch) * 4u;
                back[n][ch] = (int32_t)albw_r32(base_r + at);
                albw_w32(base_w + at, (uint32_t)mix[aux + 1][n][ch]);
            }
        }
        memcpy(mix[aux + 1], back, sizeof back);
    }

    int16_t out[FRAME][2];
    memset(out, 0, sizeof out);
    for (int bus = 0; bus < 3; bus++) {
        float gain = bus_volume[bus];
        if (gain == 0.0f) continue;
        for (uint32_t n = 0; n < FRAME; n++) {
            const int32_t *q = mix[bus][n];
            int32_t l, r;
            if (output_format == 0) {
                l = r = clamp16((int32_t)((gain * q[0] + gain * q[1] + gain * q[2] + gain * q[3]) / 2));
            } else {
                l = clamp16((int32_t)(gain * q[0] + gain * q[2]));
                r = clamp16((int32_t)(gain * q[1] + gain * q[3]));
            }
            out[n][0] = clamp16(out[n][0] + l);
            out[n][1] = clamp16(out[n][1] + r);
        }
    }
    for (uint32_t n = 0; n < FRAME; n++) {
        wr16(write + R_FINAL_SAMPLES + n * 4u, (uint16_t)out[n][0]);
        wr16(write + R_FINAL_SAMPLES + n * 4u + 2u, (uint16_t)out[n][1]);
    }
    // The status: nothing dropped.
    wr16(write + R_DSP_STATUS, 0);
    wr16(write + R_DSP_STATUS + 2, 0);

    if (output) output(&out[0][0], FRAME);
}

void albw_dsp_report(void) {
    printf("[dsp  ] buffers played: PCM8 %lu mono, %lu stereo; PCM16 %lu, %lu; ADPCM %lu, %lu; "
           "%lu frames a voice ran out in, %lu voices ended\n", stat_buffers[0][0],
           stat_buffers[0][1], stat_buffers[1][0], stat_buffers[1][1], stat_buffers[2][0],
           stat_buffers[2][1], stat_cut_short, stat_ends);
}

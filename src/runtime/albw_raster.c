#include "albw_raster.h"

#include "albw_cpu.h"
#include "albw_gpu.h"
#include "albw_kernel.h"
#include "albw_lighting.h"
#include "albw_pica.h"
#include "albw_screen.h"
#include "albw_shader.h"
#include "../shaders/pica_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Where things live.
//
// VRAM and the linear heap, in the two views that have to be reconciled: the hardware addresses
// memory physically, the guest sees it somewhere else, and the GPU's address registers hold the
// physical address divided by eight. All three facts were established from the fills that clear the
// screen, not assumed.
#define VRAM_PHYS   0x18000000u
#define VRAM_VIRT   0x1F000000u
#define VRAM_SIZE   0x00600000u
#define FCRAM_PHYS  0x20000000u
#define FCRAM_VIRT  0x14000000u
#define FCRAM_SIZE  0x08000000u

static uint32_t to_guest(uint32_t phys) {
    if (phys >= VRAM_PHYS && phys < VRAM_PHYS + VRAM_SIZE) return phys - VRAM_PHYS + VRAM_VIRT;
    if (phys >= FCRAM_PHYS && phys < FCRAM_PHYS + FCRAM_SIZE) return phys - FCRAM_PHYS + FCRAM_VIRT;
    return 0;
}

static uint32_t reg(uint32_t index) { return albw_pica_reg(index); }

// The GPU renderer shows the picture (albw_gpu.h): the game's frames are recorded and not drawn here.
static int gpu_display;

// Registers, named after what they were shown to hold.
#define R_CULL          0x040u
#define R_VIEWPORT_W    0x041u                           // half the width, as a 24-bit float
#define R_VIEWPORT_H    0x043u
#define R_DEPTH_RANGE   0x04Du
#define R_DEPTH_NEAR    0x04Eu
#define R_VS_OUT_TOTAL  0x04Fu
#define R_VS_OUT_MAP    0x050u                           // seven of them, one per output register
#define R_SCISSOR_MODE  0x065u
#define R_SCISSOR_POS   0x066u
#define R_SCISSOR_DIM   0x067u
#define R_VIEWPORT_XY   0x068u
#define R_DEPTHMAP      0x06Du
#define R_TEX_CONFIG    0x080u
#define R_TEXENV0       0x0C0u
#define R_TEXENV_BUFFER 0x0E0u
#define R_TEXENV_BUFCOL 0x0FDu
#define R_BLEND_OP      0x100u
#define R_BLEND_FUNC    0x101u
#define R_LOGIC_OP      0x102u
#define R_BLEND_COLOR   0x103u
#define R_ALPHA_TEST    0x104u
#define R_STENCIL_TEST  0x105u
#define R_STENCIL_OP    0x106u
#define R_DEPTH_TEST    0x107u
#define R_DEPTH_FORMAT  0x116u
#define R_COLOR_FORMAT  0x117u
#define R_DEPTH_ADDR    0x11Cu
#define R_COLOR_ADDR    0x11Du
#define R_FB_DIM        0x11Eu
#define R_ALLOW_COLOR_WRITE 0x113u
#define R_ALLOW_DEPTH_WRITE 0x115u                       // bit 1 depth, bit 0 stencil
#define R_COUNT         0x228u
#define R_PRIMITIVE     0x25Eu                           // the topology, in bits 8 and 9
#define R_VS_ENTRY      0x2BAu

// The six combiner stages are three groups of five registers plus two more groups further along --
// which is the hardware's layout and not a tidy one.
static const uint32_t texenv_base[6] = { 0x0C0u, 0x0C8u, 0x0D0u, 0x0D8u, 0x0F0u, 0x0F8u };
// Each texture unit's five registers: border colour, dimensions, configuration, address, format.
static const uint32_t tex_base[3] = { 0x081u, 0x091u, 0x099u };
static const uint32_t tex_addr_reg[3] = { 0x085u, 0x095u, 0x09Du };
static const uint32_t tex_format_reg[3] = { 0x08Eu, 0x096u, 0x09Eu };

static float f24(uint32_t raw) { return albw_pica_float24(raw); }

// ---------------------------------------------------------------------------------------------
// The tiled layout. The offset itself lives with the pixel formats in albw_screen.c, because the
// display transfer has to read through exactly the same layout the rasteriser writes through. Only
// the within-a-tile part is needed separately here, for the four-bit texture formats where two
// texels share a byte and the offset cannot be counted in whole bytes.
static uint32_t morton(uint32_t x, uint32_t y) {
    static const uint32_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
    static const uint32_t yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    return xl[x & 7u] + yl[y & 7u];
}

// albw_tiled_offset, here where every pixel and texel can inline it.
static inline uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t bpp) {
    return (y & ~7u) * width * bpp + (morton(x, y) + (x & ~7u) * 8u) * bpp;
}


// ---------------------------------------------------------------------------------------------
// One vertex, after the shader, sorted into what the semantics say each output means.
struct vertex {
    float pos[4];
    float color[4];
    float tc[3][2];
    float tc0_w;
    float quat[4];                                       // the normal, as a rotation
    float view[3];                                       // from the surface towards the eye
    // For a frame drawn for a temporal upscaler: the same vertex's clip-space position in the
    // frame before. Zero, and unused, everywhere else.
    float prev[4];
};

// The output map says, per output register and per component, which of these a value is. The table
// below is not guessed: this game writes 0x00010203 into the first map, 0x08090a0b into the second
// and 0x0c0d, 0x0e0f, 0x1617 into the next three, which is position, colour and three sets of
// texture coordinates in exactly the order a document would list them.
static void apply_semantics(const float out[7][4], struct vertex *v) {
    memset(v, 0, sizeof *v);
    v->pos[3] = 1.0f;
    v->color[3] = 1.0f;
    uint32_t total = (reg(R_VS_OUT_TOTAL) & 7u);
    if (total == 0 || total > 7) total = 7;
    for (uint32_t o = 0; o < total; o++) {
        uint32_t map = reg(R_VS_OUT_MAP + o);
        for (int c = 0; c < 4; c++) {
            uint32_t what = (map >> (c * 8)) & 0x1Fu;
            float value = out[o][c];
            switch (what) {
                case 0: case 1: case 2: case 3: v->pos[what] = value; break;
                case 8: case 9: case 10: case 11: v->color[what - 8] = value; break;
                case 12: v->tc[0][0] = value; break;
                case 13: v->tc[0][1] = value; break;
                case 14: v->tc[1][0] = value; break;
                case 15: v->tc[1][1] = value; break;
                case 16: v->tc0_w = value; break;
                case 22: v->tc[2][0] = value; break;
                case 23: v->tc[2][1] = value; break;
                case 4: case 5: case 6: case 7: v->quat[what - 4] = value; break;
                case 18: case 19: case 20: v->view[what - 18] = value; break;
                default: break;                          // unused slots
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Textures.
//
// The formats are listed in the order the format register numbers them. The four-bit ones pack two
// pixels per byte, low nibble first.
enum {
    TEX_RGBA8 = 0, TEX_RGB8, TEX_RGB5A1, TEX_RGB565, TEX_RGBA4, TEX_IA8, TEX_RG8, TEX_I8,
    TEX_A8, TEX_IA4, TEX_I4, TEX_A4, TEX_ETC1, TEX_ETC1A4
};

static uint32_t texel_bits(uint32_t format) {
    switch (format) {
        case TEX_RGBA8: return 32;
        case TEX_RGB8: return 24;
        case TEX_RGB5A1: case TEX_RGB565: case TEX_RGBA4: case TEX_IA8: case TEX_RG8: return 16;
        case TEX_I8: case TEX_A8: case TEX_IA4: return 8;
        case TEX_I4: case TEX_A4: return 4;
        case TEX_ETC1: return 4;
        default: return 8;                               // ETC1 with a four-bit alpha alongside
    }
}

static uint8_t expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t expand4(uint32_t v) { return (uint8_t)(v * 17u); }

// ETC1: a four-by-four block holds two base colours, a table index each and two bits per pixel that
// pick one of four brightness offsets. The tables are the format's, and the only part of it that has
// to be written out.
static const int etc1_table[8][4] = {
    {  2,   8,  -2,   -8 }, {  5,  17,  -5,  -17 }, {  9,  29,  -9,  -29 },
    { 13,  42, -13,  -42 }, { 18,  60, -18,  -60 }, { 24,  80, -24,  -80 },
    { 33, 106, -33, -106 }, { 47, 183, -47, -183 }
};

static uint32_t clamp8(int v) { return (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// A colour as the registers pack one: red in the low byte, alpha in the high one. Colours travel as
// one word and not as four bytes in memory: a word assembled from byte stores and read back whole
// stalls the processor's store forwarding, and the combiner did that at every stage of every pixel.
typedef uint32_t rgba32;
static inline uint32_t ch(rgba32 v, int i) { return (v >> (8 * i)) & 0xFFu; }
static inline rgba32 pack4(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}
#define OPAQUE_BLACK 0xFF000000u

// Four floats side by side, for the rasteriser's four pixels at a time.
typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));
static inline v4f splat(float x) { return (v4f){ x, x, x, x }; }
static inline v4f select4(v4i m, v4f a, v4f b) { return (v4f)((m & (v4i)a) | (~m & (v4i)b)); }

// Half the base-two logarithm, for a level of detail from a squared footprint: the exponent from
// the bits, and the mantissa's logarithm from a quadratic that is within 0.005 of it. Zero gives a
// large negative number, which is magnification.
static inline v4f half_log2(v4f x) {
    v4i bits = (v4i)x;
    v4f e = __builtin_convertvector((bits >> 23) - 127, v4f);
    v4f m = (v4f)((bits & 0x007FFFFF) | 0x3F800000);
    v4f lm = (splat(-0.34484843f) * m + splat(2.02466578f)) * m - splat(1.67487759f);
    return (e + lm) * splat(0.5f);
}

// One pixel of an ETC1 block, alpha left zero. The block is eight bytes: the low half holds two bits
// per pixel selecting one of four brightness offsets, the high half holds the two base colours,
// their table indices and two flags. Pixels run down the columns, not across the rows.
// A whole block, all sixteen pixels, at x * 4 + y as the selectors number them.
static void etc1_block(uint32_t at, rgba32 out[16]) {
    uint32_t bits = albw_r32(at);                        // the per-pixel selectors
    uint32_t ctrl = albw_r32(at + 4);                    // the colours and the flags
    int flip = (int)(ctrl & 1u);
    int differential = (int)((ctrl >> 1) & 1u);
    uint32_t table[2] = { (ctrl >> 5) & 7u, (ctrl >> 2) & 7u };

    // Each half's base colour: left and right, or top and bottom.
    int base[2][3];
    if (differential) {
        // One five-bit colour and a three-bit signed difference from it.
        int r = (int)((ctrl >> 27) & 0x1Fu), g = (int)((ctrl >> 19) & 0x1Fu);
        int b = (int)((ctrl >> 11) & 0x1Fu);
        int dr = (int)((ctrl >> 24) & 7u), dg = (int)((ctrl >> 16) & 7u);
        int db = (int)((ctrl >> 8) & 7u);
        if (dr > 3) dr -= 8;
        if (dg > 3) dg -= 8;
        if (db > 3) db -= 8;
        base[0][0] = expand5((uint32_t)r);
        base[0][1] = expand5((uint32_t)g);
        base[0][2] = expand5((uint32_t)b);
        base[1][0] = expand5((uint32_t)((r + dr) & 0x1F));
        base[1][1] = expand5((uint32_t)((g + dg) & 0x1F));
        base[1][2] = expand5((uint32_t)((b + db) & 0x1F));
    } else {
        for (int half = 0; half < 2; half++) {
            int shift = half ? 0 : 4;
            base[half][0] = expand4((ctrl >> (24 + shift)) & 0xFu);
            base[half][1] = expand4((ctrl >> (16 + shift)) & 0xFu);
            base[half][2] = expand4((ctrl >> (8 + shift)) & 0xFu);
        }
    }
    for (uint32_t x = 0; x < 4; x++) {
        for (uint32_t y = 0; y < 4; y++) {
            int half = flip ? (y >= 2) : (x >= 2);
            uint32_t j = x * 4u + y;
            uint32_t selector = ((bits >> (j + 16)) & 1u) * 2u + ((bits >> j) & 1u);
            int offset = etc1_table[table[half]][selector];
            out[j] = pack4(clamp8(base[half][0] + offset), clamp8(base[half][1] + offset),
                           clamp8(base[half][2] + offset), 0);
        }
    }
}

// Decoded blocks, per thread: a filtered sample reads four texels and its neighbours read the same
// ones, so a block is decoded once and not sixty-four times. The cache is good for one batch --
// memory may change between batches, never during one -- so a flush moves the epoch on.
struct etc1_cached { uint32_t at, epoch; rgba32 texel[16]; };
#define ETC1_CACHE 256u
static _Thread_local struct etc1_cached etc1_cache[ETC1_CACHE];
static uint32_t etc1_epoch = 1;

// One pixel of an ETC1 block, alpha left zero. The block is eight bytes: the low half holds two bits
// per pixel selecting one of four brightness offsets, the high half holds the two base colours,
// their table indices and two flags. Pixels run down the columns, not across the rows.
static inline rgba32 etc1_texel(uint32_t at, uint32_t x, uint32_t y) {
    // Blocks are eight bytes apart, sixteen with alpha alongside; folding in the higher bits keeps
    // the three units a draw may sample from landing on the same entries.
    struct etc1_cached *c = &etc1_cache[((at >> 3) ^ (at >> 4) ^ (at >> 12)) & (ETC1_CACHE - 1u)];
    if (c->at != at || c->epoch != etc1_epoch) {
        etc1_block(at, c->texel);
        c->at = at;
        c->epoch = etc1_epoch;
    }
    return c->texel[x * 4u + y];
}

// A texel. Textures are stored bottom row first, like the render targets, so the caller's y is
// flipped here rather than at every call.
static rgba32 sample_texel(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                           uint32_t x, uint32_t y) {
    if (y >= height || x >= width) return OPAQUE_BLACK;
    y = height - 1u - y;

    if (format == TEX_ETC1 || format == TEX_ETC1A4) {
        uint32_t block_bytes = format == TEX_ETC1 ? 8u : 16u;
        uint32_t tile = ((y / 8u) * (width / 8u) + x / 8u) * block_bytes * 4u;
        uint32_t sub = ((x / 4u) & 1u) + 2u * ((y / 4u) & 1u);
        uint32_t at = base + tile + sub * block_bytes;
        uint32_t alpha = 255;
        if (format == TEX_ETC1A4) {
            uint32_t j = (x % 4u) * 4u + (y % 4u);
            uint32_t word = albw_r32(at + (j >= 8u ? 4u : 0u));
            alpha = expand4((word >> ((j % 8u) * 4u)) & 0xFu);
            at += 8u;
        }
        return etc1_texel(at, x % 4u, y % 4u) | (alpha << 24);
    }

    uint32_t bits = texel_bits(format);
    uint32_t at;
    if (bits >= 8) {
        at = base + tiled_offset(x, y, width, bits / 8u);
    } else {
        // Four bits a texel: the tile offset is counted in texels and then halved, the low nibble
        // coming first.
        uint32_t texels = (y & ~7u) * width + morton(x, y) + (x & ~7u) * 8u;
        at = base + texels / 2u;
        uint32_t v = (albw_r8(at) >> ((texels & 1u) * 4u)) & 0xFu;
        if (format == TEX_I4) {
            uint32_t i = expand4(v);
            return pack4(i, i, i, 255);
        }
        return (uint32_t)expand4(v) << 24;
    }

    switch (format) {
        case TEX_RGBA8: {
            uint32_t v = albw_r32(at);                   // alpha, blue, green, red from the bottom
            return __builtin_bswap32(v);
        }
        case TEX_RGB8:
            return pack4(albw_r8(at + 2), albw_r8(at + 1), albw_r8(at + 0), 255);
        case TEX_RGB5A1: {
            uint32_t v = albw_r16(at);
            return pack4(expand5((v >> 11) & 0x1Fu), expand5((v >> 6) & 0x1Fu),
                         expand5((v >> 1) & 0x1Fu), (v & 1u) ? 255u : 0u);
        }
        case TEX_RGB565: {
            uint32_t v = albw_r16(at);
            return pack4(expand5((v >> 11) & 0x1Fu),
                         (((v >> 5) & 0x3Fu) << 2) | (((v >> 5) & 0x3Fu) >> 4),
                         expand5(v & 0x1Fu), 255);
        }
        case TEX_RGBA4: {
            uint32_t v = albw_r16(at);
            return pack4(expand4((v >> 12) & 0xFu), expand4((v >> 8) & 0xFu),
                         expand4((v >> 4) & 0xFu), expand4(v & 0xFu));
        }
        case TEX_IA8: {
            uint32_t i = albw_r8(at + 1);
            return pack4(i, i, i, albw_r8(at + 0));
        }
        case TEX_RG8:
            return pack4(albw_r8(at + 1), albw_r8(at + 0), 0, 255);
        case TEX_I8: {
            uint32_t i = albw_r8(at);
            return pack4(i, i, i, 255);
        }
        case TEX_A8:
            return (uint32_t)albw_r8(at) << 24;
        case TEX_IA4: {
            uint32_t v = albw_r8(at);
            uint32_t i = expand4(v >> 4);
            return pack4(i, i, i, expand4(v & 0xFu));
        }
        default:
            return OPAQUE_BLACK;
    }
}


// Per-format texel readers, chosen once a draw: the coordinates are inside the texture and
// counted from the top, as the samplers have them; each flips y to the stored bottom-up order and
// finds the texel in its tile. sample_texel above is the same in one function, for the rare
// callers that do not have a unit's state.
struct tex_state;
typedef rgba32 (*texel_reader)(const struct tex_state *t, uint32_t x, uint32_t y);

// The wrap modes, in the order the configuration register numbers them.
static inline int wrap_coord(int v, int size, uint32_t mode) {
    switch (mode & 3u) {
        case 2:                                          // repeat
            v %= size;
            return v < 0 ? v + size : v;
        case 3: {                                        // mirror at every other repetition
            int period = size * 2;
            v %= period;
            if (v < 0) v += period;
            return v < size ? v : period - 1 - v;
        }
        default:                                         // clamp, to the edge or to the border
            return v < 0 ? 0 : (v >= size ? size - 1 : v);
    }
}

// One texture unit, sampled at the given coordinates. Nearest only: the filter bits say linear for
// some units, and the difference is worth having once there is a picture to compare, not before.
static void sample_texture(int unit, float s, float t, uint8_t out[4]) {
    uint32_t dim = reg(tex_base[unit] + 1u);
    uint32_t config = reg(tex_base[unit] + 2u);
    uint32_t width = (dim >> 16) & 0x7FFu;
    uint32_t height = dim & 0x7FFu;
    uint32_t format = reg(tex_format_reg[unit]) & 0xFu;
    uint32_t base = to_guest(reg(tex_addr_reg[unit]) << 3);
    out[0] = out[1] = out[2] = 0;
    out[3] = 255;
    if (!base || !width || !height) return;

    int x = (int)floorf(s * (float)width);
    int y = (int)floorf(t * (float)height);
    uint32_t wrap_s = (config >> 12) & 0xFu, wrap_t = (config >> 8) & 0xFu;
    // Clamp to the border: outside the texture the sample is the border colour, not the nearest
    // edge texel. Treating it as clamp-to-edge smears the edge across whatever part of the quad lies
    // outside -- a faint rectangle around a sprite whose edge is not quite transparent.
    rgba32 v;
    if (((wrap_s & 3u) == 1u && (x < 0 || x >= (int)width))
            || ((wrap_t & 3u) == 1u && (y < 0 || y >= (int)height))) {
        v = reg(tex_base[unit]);
    } else {
        x = wrap_coord(x, (int)width, wrap_s);
        y = wrap_coord(y, (int)height, wrap_t);
        v = sample_texel(base, width, height, format, (uint32_t)x, (uint32_t)y);
    }
    for (int c = 0; c < 4; c++) out[c] = (uint8_t)ch(v, c);
}

// ---------------------------------------------------------------------------------------------
// The texture combiner: six stages, each choosing three sources, modifying them, and combining them
// into one colour. The sources, the modifiers and the operations are numbered by the registers; the
// names below are what each number does.
enum {
    SRC_PRIMARY = 0, SRC_PRIMARY_FRAGMENT = 1, SRC_SECONDARY_FRAGMENT = 2,
    SRC_TEX0 = 3, SRC_TEX1 = 4, SRC_TEX2 = 5, SRC_TEX3 = 6,
    SRC_PREVIOUS_BUFFER = 0xD, SRC_CONSTANT = 0xE, SRC_PREVIOUS = 0xF
};

// A colour modifier applies to all three channels, so it gives three.
static inline void modify_rgb(rgba32 v, uint32_t how, int out[3]) {
    int pick, invert = 0;
    switch (how) {
        case 1:  invert = 1; /* fall through */
        case 0:  out[0] = (int)ch(v, 0); out[1] = (int)ch(v, 1); out[2] = (int)ch(v, 2); goto done;
        case 3:  invert = 1; /* fall through */
        case 2:  pick = 3; break;
        case 5:  invert = 1; /* fall through */
        case 4:  pick = 0; break;
        case 9:  invert = 1; /* fall through */
        case 8:  pick = 1; break;
        case 13: invert = 1; /* fall through */
        case 12: pick = 2; break;
        default: out[0] = (int)ch(v, 0); out[1] = (int)ch(v, 1); out[2] = (int)ch(v, 2); return;
    }
    out[0] = out[1] = out[2] = (int)ch(v, pick);
done:
    if (invert) for (int i = 0; i < 3; i++) out[i] = 255 - out[i];
}

static inline int modify_alpha(rgba32 v, uint32_t how) {
    switch (how & 7u) {
        case 0: return (int)ch(v, 3);
        case 1: return 255 - (int)ch(v, 3);
        case 2: return (int)ch(v, 0);
        case 3: return 255 - (int)ch(v, 0);
        case 4: return (int)ch(v, 1);
        case 5: return 255 - (int)ch(v, 1);
        case 6: return (int)ch(v, 2);
        default: return 255 - (int)ch(v, 2);
    }
}

static inline int combine(uint32_t op, int a, int b, int c) {
    switch (op & 0xFu) {
        case 0: return a;                                            // replace
        case 1: return a * b / 255;                                  // modulate
        case 2: return a + b;                                        // add
        case 3: return a + b - 128;                                  // add, signed
        case 4: return (a * c + b * (255 - c)) / 255;                // interpolate
        case 5: return a - b;                                        // subtract
        case 8: return a * b / 255 + c;                              // multiply then add
        case 9: return (a + b) * c / 255;                            // add then multiply
        default: return a;
    }
}

// The dot-product operations work on all three channels at once, so they are handled apart from the
// per-channel ones above.
static inline int combine_dot3(const int a[3], const int b[3]) {
    int dot = 0;
    for (int i = 0; i < 3; i++) dot += (a[i] - 128) * (b[i] - 128);
    return dot * 4 / 255;
}

static inline uint8_t saturate(int v, uint32_t scale) {
    v <<= (scale & 3u);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// ---------------------------------------------------------------------------------------------
// The framebuffer.
static uint32_t fb_width(void)  { return reg(R_FB_DIM) & 0x7FFu; }
static uint32_t fb_height(void) { return ((reg(R_FB_DIM) >> 12) & 0x3FFu) + 1u; }

// The colour buffer's format register and the display transfer number their formats differently:
// the buffer has RGB5A1 as 2 and RGB565 as 3, the transfer and the framebuffers the other way round.
// The pixel helpers go by the transfer's numbering, so the buffer's has to be translated -- without
// it every pixel this game draws went in as RGB5A1 and came out of the transfer read as RGB565, which
// is a blue cast and a darkening over everything, while a dump of the buffer read back with the same
// mistake looked right.
static uint32_t color_format(void) {
    static const uint32_t to_framebuffer[8] = {
        ALBW_FB_RGBA8, ALBW_FB_RGB8, ALBW_FB_RGB5A1, ALBW_FB_RGB565, ALBW_FB_RGBA4,
        ALBW_FB_RGBA4, ALBW_FB_RGBA4, ALBW_FB_RGBA4,
    };
    return to_framebuffer[(reg(R_COLOR_FORMAT) >> 16) & 7u];
}

static uint32_t color_bpp(void) {
    switch ((reg(R_COLOR_FORMAT) >> 16) & 7u) {
        case 0: return 4;
        case 1: return 3;
        default: return 2;
    }
}

static uint32_t depth_bpp(void) {
    switch (reg(R_DEPTH_FORMAT) & 3u) {
        case 0: return 2;
        case 2: return 3;
        default: return 4;
    }
}

// The render target is stored bottom row first, like a texture. The height register holds one less
// than the height, which is the number to subtract.
static uint32_t color_at(uint32_t x, uint32_t y) {
    uint32_t base = to_guest(reg(R_COLOR_ADDR) << 3);
    if (!base) return 0;
    uint32_t flipped = ((reg(R_FB_DIM) >> 12) & 0x3FFu) - y;
    return base + albw_tiled_offset(x, flipped, fb_width(), color_bpp());
}

static inline int compare_func(uint32_t how, uint32_t a, uint32_t b) {
    switch (how & 7u) {
        case 0: return 0;                                // never
        case 1: return 1;                                // always
        case 2: return a == b;
        case 3: return a != b;
        case 4: return a < b;
        case 5: return a <= b;
        case 6: return a > b;
        default: return a >= b;
    }
}

static inline int blend_factor(uint32_t which, rgba32 src, rgba32 dst, rgba32 cst, int channel) {
    switch (which & 0xFu) {
        case 0: return 0;
        case 1: return 255;
        case 2: return (int)ch(src, channel);
        case 3: return 255 - (int)ch(src, channel);
        case 4: return (int)ch(dst, channel);
        case 5: return 255 - (int)ch(dst, channel);
        case 6: return (int)ch(src, 3);
        case 7: return 255 - (int)ch(src, 3);
        case 8: return (int)ch(dst, 3);
        case 9: return 255 - (int)ch(dst, 3);
        case 10: return (int)ch(cst, channel);
        case 11: return 255 - (int)ch(cst, channel);
        case 12: return (int)ch(cst, 3);
        case 13: return 255 - (int)ch(cst, 3);
        default: {
            int s = (int)ch(src, 3), d = 255 - (int)ch(dst, 3);
            return s < d ? s : d;
        }
    }
}

static inline int blend_equation(uint32_t how, int s, int d) {
    switch (how & 7u) {
        case 0: return s + d;
        case 1: return s - d;
        case 2: return d - s;
        case 3: return s < d ? s : d;
        default: return s > d ? s : d;
    }
}

// ---------------------------------------------------------------------------------------------
// One pixel.
static unsigned long pixels_drawn, triangles_drawn, triangles_skipped, draws_skipped;
static unsigned long triangles_clipped_away;
static unsigned long skipped_for_count;

// ---------------------------------------------------------------------------------------------
// The draw's state, decoded once.
//
// Everything a pixel needs from the registers is read here, when a draw starts, and not again per
// pixel: reading and decoding thirty-odd registers for every fragment was most of the time a frame
// took. Nothing in a draw can change them -- a command list writes registers between draws, never
// during one -- so this is the same answer, computed once.
struct tex_state {
    int enabled, usable, linear;
    int coords;                                          // which texture coordinates it samples
    // Mipmaps: smaller copies that follow the full-size level in memory, each half the size of the
    // one before. A minified pixel samples the level its footprint asks for, with the minification
    // filter, and between two levels when the mip filter is linear.
    int max_level, min_level, min_linear, mip_linear;
    float lod_bias, size_sq[2];                          // width and height, squared
    uint32_t base, width, height, format, wrap_s, wrap_t;
    rgba32 border;
    texel_reader read;                                   // for the format
    uint32_t row_bytes;                                  // one row of 8x8 tiles, for 8 bits and up
};

static inline uint32_t tex_at(const struct tex_state *t, uint32_t x, uint32_t y, uint32_t bpp) {
    uint32_t f = t->height - 1u - y;
    return t->base + (f >> 3) * t->row_bytes + (morton(x, f) + (x & ~7u) * 8u) * bpp;
}

static rgba32 read_rgba8(const struct tex_state *t, uint32_t x, uint32_t y) {
    return __builtin_bswap32(albw_r32(tex_at(t, x, y, 4)));
}
static rgba32 read_rgb8(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t at = tex_at(t, x, y, 3);
    return pack4(albw_r8(at + 2), albw_r8(at + 1), albw_r8(at + 0), 255);
}
static rgba32 read_rgb5a1(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t v = albw_r16(tex_at(t, x, y, 2));
    return pack4(expand5((v >> 11) & 0x1Fu), expand5((v >> 6) & 0x1Fu), expand5((v >> 1) & 0x1Fu),
                 (v & 1u) ? 255u : 0u);
}
static rgba32 read_rgb565(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t v = albw_r16(tex_at(t, x, y, 2));
    return pack4(expand5((v >> 11) & 0x1Fu), (((v >> 5) & 0x3Fu) << 2) | (((v >> 5) & 0x3Fu) >> 4),
                 expand5(v & 0x1Fu), 255);
}
static rgba32 read_rgba4(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t v = albw_r16(tex_at(t, x, y, 2));
    return pack4(expand4((v >> 12) & 0xFu), expand4((v >> 8) & 0xFu), expand4((v >> 4) & 0xFu),
                 expand4(v & 0xFu));
}
static rgba32 read_ia8(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t at = tex_at(t, x, y, 2), i = albw_r8(at + 1);
    return pack4(i, i, i, albw_r8(at));
}
static rgba32 read_rg8(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t at = tex_at(t, x, y, 2);
    return pack4(albw_r8(at + 1), albw_r8(at), 0, 255);
}
static rgba32 read_i8(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t i = albw_r8(tex_at(t, x, y, 1));
    return pack4(i, i, i, 255);
}
static rgba32 read_a8(const struct tex_state *t, uint32_t x, uint32_t y) {
    return (uint32_t)albw_r8(tex_at(t, x, y, 1)) << 24;
}
static rgba32 read_ia4(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t v = albw_r8(tex_at(t, x, y, 1)), i = expand4(v >> 4);
    return pack4(i, i, i, expand4(v & 0xFu));
}
static inline uint32_t nibble_at(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t f = t->height - 1u - y;
    uint32_t texels = (f & ~7u) * t->width + morton(x, f) + (x & ~7u) * 8u;
    return (albw_r8(t->base + texels / 2u) >> ((texels & 1u) * 4u)) & 0xFu;
}
static rgba32 read_i4(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t i = expand4(nibble_at(t, x, y));
    return pack4(i, i, i, 255);
}
static rgba32 read_a4(const struct tex_state *t, uint32_t x, uint32_t y) {
    return (uint32_t)expand4(nibble_at(t, x, y)) << 24;
}
static inline uint32_t etc1_block_at(const struct tex_state *t, uint32_t x, uint32_t f,
                                     uint32_t block_bytes) {
    uint32_t tile = ((f >> 3) * (t->width >> 3) + (x >> 3)) * block_bytes * 4u;
    uint32_t sub = ((x >> 2) & 1u) + 2u * ((f >> 2) & 1u);
    return t->base + tile + sub * block_bytes;
}
static rgba32 read_etc1(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t f = t->height - 1u - y;
    return etc1_texel(etc1_block_at(t, x, f, 8u), x & 3u, f & 3u) | 0xFF000000u;
}
static rgba32 read_etc1a4(const struct tex_state *t, uint32_t x, uint32_t y) {
    uint32_t f = t->height - 1u - y;
    uint32_t at = etc1_block_at(t, x, f, 16u);
    uint32_t j = (x & 3u) * 4u + (f & 3u);
    uint32_t word = albw_r32(at + (j >= 8u ? 4u : 0u));
    uint32_t alpha = expand4((word >> ((j & 7u) * 4u)) & 0xFu);
    return etc1_texel(at + 8u, x & 3u, f & 3u) | (alpha << 24);
}
static rgba32 read_none(const struct tex_state *t, uint32_t x, uint32_t y) {
    (void)t; (void)x; (void)y;
    return OPAQUE_BLACK;
}

struct stage_state {
    uint8_t src[3], src_a[3], mod_rgb[3], mod_a[3];
    uint8_t op_rgb, op_a, scale_rgb, scale_a;
    rgba32 constant;
    int passthrough;                                     // hands the previous output on unchanged
    // Both halves a plain replace or a plain multiply -- no modifier, no scale -- which is most of
    // what this game's stages are: then neither the modifiers nor the general operations are needed.
    uint8_t rgb_kind, a_kind;
};
enum { KIND_GENERAL = 0, KIND_REPLACE, KIND_MODULATE };

struct draw_state {
    struct tex_state tex[3];
    struct tex_state mip[3][7];                          // levels 1 to 7, as far as each unit has them
    struct stage_state stage[6];
    rgba32 buffer_color;
    uint32_t update;
    int lighting;                                        // on, and named by some stage
    struct albw_lighting_state light;
    int alpha_test;
    uint32_t alpha_func, alpha_ref;
    int depth_test, depth_write;
    uint32_t depth_func, color_mask;
    // The stencil, in the depth buffer's fourth byte when that is 24-bit depth with 8-bit stencil,
    // and the logic op that replaces blending when blending is off.
    int stencil;
    uint32_t stencil_func, stencil_ref, stencil_in_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t logic_op;
    int blend;
    uint32_t blend_func, blend_plain;
    rgba32 blend_color;
    uint32_t color_base, depth_base, width, height_m1, color_fmt, color_bytes, depth_bytes;
    uint32_t depth_max;
    // What setting a triangle up reads: the viewport, the depth mapping, culling and the scissor.
    // Kept with the rest, so a triangle can be set up again later -- for a frame in between --
    // from nothing but the draw it came from.
    float half_w, half_h, offset_x, offset_y, depth_range, depth_near;
    uint32_t cull;
    int scissor_x0, scissor_y0, scissor_x1, scissor_y1;
    int wbuffer;
};

// The draw being set up. Its triangles take a copy along into the batch.
static struct draw_state ds;

static int same_target(const struct draw_state *a, const struct draw_state *b) {
    return a->color_base == b->color_base && a->depth_base == b->depth_base
        && a->width == b->width && a->height_m1 == b->height_m1 && a->color_fmt == b->color_fmt
        && a->color_bytes == b->color_bytes && a->depth_bytes == b->depth_bytes;
}

// What the game asks of the GPU that this renderer does not draw yet. Each is said once, with the
// frame it first came up in, and counted for the report -- so that a scene that looks wrong can be
// matched to what it is missing, by a player reading the log as much as by anyone else.
enum { MISS_FOG, MISS_GAS, MISS_CUBE, MISS_SHADOW_TEX, MISS_PROJECTION_TEX, MISS_PROCTEX,
       MISS_SHADOW_OUT, MISS_COUNT };
static const char *const miss_name[MISS_COUNT] = {
    "fog", "gas rendering", "a cube map", "a shadow texture", "a projected texture",
    "the procedural texture unit", "shadow-map output",
};
static unsigned long miss_draws[MISS_COUNT];

static void note_missing(int what) {
    if (miss_draws[what]++ == 0) {
        printf("[pica ] the game asks for %s, which the renderer does not draw yet (frame %lu)\n",
               miss_name[what], albw_frame_count());
    }
}

static void audit_state(uint32_t units) {
    uint32_t fog = reg(R_TEXENV_BUFFER) & 7u;
    if (fog == 5u) note_missing(MISS_FOG);
    if (fog == 7u) note_missing(MISS_GAS);
    if ((reg(R_BLEND_OP) & 3u) == 3u) note_missing(MISS_SHADOW_OUT);
    if ((reg(R_BLEND_OP) & 3u) == 1u) note_missing(MISS_GAS);
    if ((units >> 10) & 1u) note_missing(MISS_PROCTEX);
    for (int u = 0; u < 3; u++) {
        if (!((units >> u) & 1u)) continue;
        // Only unit 0 has a type; the others are always plain 2D.
        if (u == 0) {
            uint32_t type = (reg(tex_base[0] + 2u) >> 28) & 7u;
            if (type == 1u) note_missing(MISS_CUBE);
            if (type == 2u || type == 4u) note_missing(MISS_SHADOW_TEX);
            if (type == 3u) note_missing(MISS_PROJECTION_TEX);
        }
    }
}

static void snapshot_state(void) {
    uint32_t units = reg(R_TEX_CONFIG);
    audit_state(units);
    for (int u = 0; u < 3; u++) {
        struct tex_state *t = &ds.tex[u];
        uint32_t dim = reg(tex_base[u] + 1u), config = reg(tex_base[u] + 2u);
        t->enabled = (int)((units >> u) & 1u);
        // Unit 2 can be told to sample the second set of coordinates instead of its own.
        t->coords = (u == 2 && ((units >> 13) & 1u)) ? 1 : u;
        t->width = (dim >> 16) & 0x7FFu;
        t->height = dim & 0x7FFu;
        t->format = reg(tex_format_reg[u]) & 0xFu;
        t->base = to_guest(reg(tex_addr_reg[u]) << 3);
        t->usable = t->base && t->width && t->height;
        static const texel_reader readers[16] = {
            read_rgba8, read_rgb8, read_rgb5a1, read_rgb565, read_rgba4, read_ia8, read_rg8,
            read_i8, read_a8, read_ia4, read_i4, read_a4, read_etc1, read_etc1a4, read_none,
            read_none,
        };
        t->read = readers[t->format & 15u];
        uint32_t bits = texel_bits(t->format);
        t->row_bytes = t->width * 8u * (bits >= 8u ? bits / 8u : 1u);
        t->wrap_s = (config >> 12) & 0xFu;
        t->wrap_t = (config >> 8) & 0xFu;
        // Bilinear when the magnification filter says so. The game sets the two filters alike --
        // linear for nearly every unit it uses -- so which one applies does not need working out.
        t->linear = (int)((config >> 1) & 1u);
        t->border = reg(tex_base[u]);

        // The level-of-detail register follows the configuration: a bias in 1.4.8 fixed point,
        // the highest and the lowest level to use. Levels are laid out one after another, and a
        // level narrower than a tile is not one the hardware can have.
        uint32_t lod = reg(tex_base[u] + 3u);
        int bias = (int)(lod & 0x1FFFu);
        if (bias & 0x1000) bias -= 0x2000;
        t->lod_bias = (float)bias / 256.0f;
        t->min_linear = (int)((config >> 2) & 1u);
        t->mip_linear = (int)((config >> 24) & 1u);
        t->size_sq[0] = (float)t->width * (float)t->width;
        t->size_sq[1] = (float)t->height * (float)t->height;
        int want = (int)((lod >> 16) & 0xFu), levels = 0;
        uint32_t at = t->base, w = t->width, h = t->height;
        for (int k = 1; k <= want && k <= 7 && t->usable; k++) {
            at += w * h * bits / 8u;
            w >>= 1;
            h >>= 1;
            if (w < 8u || h < 8u) break;
            struct tex_state *m = &ds.mip[u][k - 1];
            *m = *t;
            m->base = at;
            m->width = w;
            m->height = h;
            m->row_bytes = w * 8u * (bits >= 8u ? bits / 8u : 1u);
            m->max_level = 0;
            levels = k;
        }
        t->max_level = levels;
        t->min_level = (int)((lod >> 24) & 0xFu);
        if (t->min_level > levels) t->min_level = levels;
        // A unit whose texture, with its levels, would run past guest memory reads as an unusable
        // one: a unit left on with a stale address once read straight off the end of the map.
        if (t->usable) {
            uint64_t span = 0;
            for (int k = 0; k <= levels; k++) {
                span += (uint64_t)(t->width >> k) * (t->height >> k) * bits / 8u;
            }
            if (span > 0xFFFFFFFFu || !albw_guest_ok(t->base, (uint32_t)span)) {
                t->usable = 0;
                t->max_level = t->min_level = 0;
            }
        }
    }
    int names_lighting = 0;
    for (int st = 0; st < 6; st++) {
        uint32_t at = texenv_base[st];
        uint32_t source = reg(at), modifier = reg(at + 1u), op = reg(at + 2u);
        uint32_t scale = reg(at + 4u);
        struct stage_state *g = &ds.stage[st];
        for (int i = 0; i < 3; i++) {
            g->src[i] = (uint8_t)((source >> (i * 4)) & 0xFu);
            g->src_a[i] = (uint8_t)((source >> (16 + i * 4)) & 0xFu);
            g->mod_rgb[i] = (uint8_t)((modifier >> (i * 4)) & 0xFu);
            g->mod_a[i] = (uint8_t)((modifier >> (12 + i * 4)) & 7u);
            if (g->src[i] == SRC_PRIMARY_FRAGMENT || g->src[i] == SRC_SECONDARY_FRAGMENT
                    || g->src_a[i] == SRC_PRIMARY_FRAGMENT
                    || g->src_a[i] == SRC_SECONDARY_FRAGMENT) {
                names_lighting = 1;
            }
        }
        // A source the combiner does not know reads as the previous stage's output; mapping it
        // here lets a pixel index a table of sources instead of deciding per source.
        for (int i = 0; i < 3; i++) {
            if (g->src[i] > SRC_TEX3 && g->src[i] < SRC_PREVIOUS_BUFFER) g->src[i] = SRC_PREVIOUS;
            if (g->src_a[i] > SRC_TEX3 && g->src_a[i] < SRC_PREVIOUS_BUFFER) g->src_a[i] = SRC_PREVIOUS;
        }
        g->op_rgb = (uint8_t)(op & 0xFu);
        g->op_a = (uint8_t)((op >> 16) & 0xFu);
        g->scale_rgb = (uint8_t)(scale & 3u);
        g->scale_a = (uint8_t)((scale >> 16) & 3u);
        g->constant = reg(at + 3u);
        // "Previous, unmodified, replaced, unscaled" for both colour and alpha is a stage that does
        // nothing, and most draws leave four or five of the six like that.
        g->passthrough = g->src[0] == SRC_PREVIOUS && g->mod_rgb[0] == 0 && g->op_rgb == 0
                      && g->scale_rgb == 0 && g->src_a[0] == SRC_PREVIOUS && g->mod_a[0] == 0
                      && g->op_a == 0 && g->scale_a == 0;
        g->rgb_kind = KIND_GENERAL;
        if (g->scale_rgb == 0 && g->mod_rgb[0] == 0) {
            if (g->op_rgb == 0) g->rgb_kind = KIND_REPLACE;
            else if (g->op_rgb == 1 && g->mod_rgb[1] == 0) g->rgb_kind = KIND_MODULATE;
        }
        g->a_kind = KIND_GENERAL;
        if (g->scale_a == 0 && g->mod_a[0] == 0) {
            if (g->op_a == 0) g->a_kind = KIND_REPLACE;
            else if (g->op_a == 1 && g->mod_a[1] == 0) g->a_kind = KIND_MODULATE;
        }
    }
    ds.buffer_color = reg(R_TEXENV_BUFCOL);
    ds.update = reg(R_TEXENV_BUFFER);
    // Lighting is skipped altogether when no stage reads its colours: the result would be thrown
    // away, and it is the most expensive thing a fragment can ask for.
    ds.lighting = names_lighting && albw_lighting_enabled();
    if (ds.lighting) albw_lighting_snapshot(&ds.light);

    uint32_t alpha = reg(R_ALPHA_TEST);
    ds.alpha_test = (int)(alpha & 1u);
    ds.alpha_func = (alpha >> 4) & 7u;
    ds.alpha_ref = (alpha >> 8) & 0xFFu;
    uint32_t merger = reg(R_DEPTH_TEST);
    ds.depth_test = (int)(merger & 1u);
    ds.depth_func = (merger >> 4) & 7u;
    ds.depth_write = (int)((merger >> 12) & 1u);
    ds.color_mask = (merger >> 8) & 7u;
    ds.blend = (int)((reg(R_BLEND_OP) >> 8) & 1u);
    ds.logic_op = reg(R_LOGIC_OP) & 0xFu;
    uint32_t stencil = reg(R_STENCIL_TEST), ops = reg(R_STENCIL_OP);
    ds.stencil = (int)(stencil & 1u) && (reg(R_DEPTH_FORMAT) & 3u) == 3u;
    ds.stencil_func = (stencil >> 4) & 7u;
    ds.stencil_write_mask = (stencil >> 8) & 0xFFu;
    ds.stencil_ref = (stencil >> 16) & 0xFFu;
    ds.stencil_in_mask = (stencil >> 24) & 0xFFu;
    ds.stencil_fail = ops & 7u;
    ds.stencil_zfail = (ops >> 4) & 7u;
    ds.stencil_zpass = (ops >> 8) & 7u;
    ds.blend_func = reg(R_BLEND_FUNC);
    ds.blend_plain = (ds.blend_func & ~0x00FF0007u) | (6u << 16) | (7u << 20);
    ds.blend_color = reg(R_BLEND_COLOR);

    ds.color_base = to_guest(reg(R_COLOR_ADDR) << 3);
    ds.depth_base = to_guest(reg(R_DEPTH_ADDR) << 3);
    ds.width = fb_width();
    ds.height_m1 = (reg(R_FB_DIM) >> 12) & 0x3FFu;
    ds.color_fmt = color_format();
    ds.color_bytes = color_bpp();
    ds.depth_bytes = depth_bpp();
    ds.depth_max = ds.depth_bytes == 2 ? 0xFFFFu : 0xFFFFFFu;

    ds.half_w = f24(reg(R_VIEWPORT_W));
    ds.half_h = f24(reg(R_VIEWPORT_H));
    ds.offset_x = (float)(reg(R_VIEWPORT_XY) & 0x3FFu);
    ds.offset_y = (float)((reg(R_VIEWPORT_XY) >> 16) & 0x3FFu);
    ds.depth_range = f24(reg(R_DEPTH_RANGE));
    ds.depth_near = f24(reg(R_DEPTH_NEAR));
    ds.cull = reg(R_CULL) & 3u;
    // The scissor, which this game sets to the whole target but which is the only thing that keeps
    // a draw inside a smaller region when it does not.
    ds.scissor_x0 = 0;
    ds.scissor_y0 = 0;
    ds.scissor_x1 = (int)fb_width() - 1;
    ds.scissor_y1 = (int)fb_height() - 1;
    if ((reg(R_SCISSOR_MODE) & 3u) == 3u) {
        ds.scissor_x0 = (int)(reg(R_SCISSOR_POS) & 0x3FFu);
        ds.scissor_y0 = (int)((reg(R_SCISSOR_POS) >> 16) & 0x3FFu);
        ds.scissor_x1 = (int)(reg(R_SCISSOR_DIM) & 0x3FFu);
        ds.scissor_y1 = (int)((reg(R_SCISSOR_DIM) >> 16) & 0x3FFu);
    }
    ds.wbuffer = !(reg(R_DEPTHMAP) & 1u);
}

// One texel of a unit, wrapped, or the border colour outside a unit that clamps to it.
static inline rgba32 unit_texel(const struct tex_state *t, int x, int y) {
    if (((t->wrap_s & 3u) == 1u && (x < 0 || x >= (int)t->width))
            || ((t->wrap_t & 3u) == 1u && (y < 0 || y >= (int)t->height))) {
        return t->border;
    }
    x = wrap_coord(x, (int)t->width, t->wrap_s);
    y = wrap_coord(y, (int)t->height, t->wrap_t);
    return t->read(t, (uint32_t)x, (uint32_t)y);
}

// a and b mixed by w out of 256, all four channels at once: red and blue, then green and alpha,
// in sixteen-bit lanes that cannot carry into each other.
static inline rgba32 mix256(rgba32 a, rgba32 b, uint32_t w) {
    uint32_t iw = 256u - w;
    uint32_t rb = (((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    uint32_t ga = ((((a >> 8) & 0x00FF00FFu) * iw + ((b >> 8) & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    return rb | (ga << 8);
}

// One texture unit at (s, t), from the snapshot: the nearest texel, or the four around the point
// weighted by how near each is.
static rgba32 sample_level(const struct tex_state *t, int linear, float s, float tt) {
    if (!t->usable) return OPAQUE_BLACK;
    if (!linear) {
        int x = (int)floorf(s * (float)t->width), y = (int)floorf(tt * (float)t->height);
        if ((uint32_t)x < t->width && (uint32_t)y < t->height) return t->read(t, (uint32_t)x, (uint32_t)y);
        return unit_texel(t, x, y);
    }
    float fx = s * (float)t->width - 0.5f, fy = tt * (float)t->height - 0.5f;
    if (!(fx > -1e9f && fx < 1e9f && fy > -1e9f && fy < 1e9f)) return OPAQUE_BLACK;   // and NaN
    // Rounded down by hand: a conversion and a compare, where floorf without SSE4.1 is a call.
    int x0 = (int)fx, y0 = (int)fy;
    if ((float)x0 > fx) x0--;
    if ((float)y0 > fy) y0--;
    uint32_t wx = (uint32_t)((fx - (float)x0) * 256.0f), wy = (uint32_t)((fy - (float)y0) * 256.0f);
    if (wx > 256u) wx = 256u;
    if (wy > 256u) wy = 256u;
    rgba32 c00, c10, c01, c11;
    if ((uint32_t)x0 < t->width - 1u && (uint32_t)y0 < t->height - 1u) {
        // All four inside, which is nearly every sample: no wrapping to work out.
        uint32_t ux = (uint32_t)x0, uy = (uint32_t)y0;
        c00 = t->read(t, ux, uy);
        c10 = t->read(t, ux + 1u, uy);
        c01 = t->read(t, ux, uy + 1u);
        c11 = t->read(t, ux + 1u, uy + 1u);
    } else {
        c00 = unit_texel(t, x0, y0);
        c10 = unit_texel(t, x0 + 1, y0);
        c01 = unit_texel(t, x0, y0 + 1);
        c11 = unit_texel(t, x0 + 1, y0 + 1);
    }
    return mix256(mix256(c00, c10, wx), mix256(c01, c11, wx), wy);
}

// A unit's colour at (s, t), for a pixel whose footprint is 2^lod texels across. Magnified, or
// without mipmaps, that is the full-size level with the magnification filter; minified, the level
// the footprint asks for, within the unit's range, or the two around it mixed by how far between.
static rgba32 sample_unit(const struct draw_state *S, int u, float s, float tt, float lod) {
    const struct tex_state *t = &S->tex[u];
    if (!t->max_level) return sample_level(t, t->linear, s, tt);
    lod += t->lod_bias;
    if (lod < (float)t->min_level) lod = (float)t->min_level;
    if (lod > (float)t->max_level) lod = (float)t->max_level;
    if (!(lod > 0.0f)) return sample_level(t, t->linear, s, tt);    // and NaN
    if (!t->mip_linear) {
        int k = (int)(lod + 0.5f);
        return sample_level(k ? &S->mip[u][k - 1] : t, t->min_linear, s, tt);
    }
    int k = (int)lod;
    uint32_t frac = (uint32_t)((lod - (float)k) * 256.0f);
    rgba32 a = sample_level(k ? &S->mip[u][k - 1] : t, t->min_linear, s, tt);
    if (k >= t->max_level || frac == 0u) return a;
    return mix256(a, sample_level(&S->mip[u][k], t->min_linear, s, tt), frac);
}

// The target's colour at `at`, alpha opaque: what the blend sees as the destination.
static inline rgba32 read_target(const struct draw_state *S, uint32_t at) {
    uint32_t r, g, b;
    switch (S->color_fmt) {
        case ALBW_FB_RGB565: {
            uint32_t v = albw_r16(at);
            r = (v >> 11) & 0x1Fu; g = (v >> 5) & 0x3Fu; b = v & 0x1Fu;
            return pack4((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
        }
        case ALBW_FB_RGBA8: {
            uint32_t v = albw_r32(at);
            return pack4((v >> 24) & 0xFFu, (v >> 16) & 0xFFu, (v >> 8) & 0xFFu, 255);
        }
        case ALBW_FB_RGB8:                               // stored blue first
            return pack4(albw_r8(at + 2), albw_r8(at + 1), albw_r8(at + 0), 255);
        case ALBW_FB_RGB5A1: {
            uint32_t v = albw_r16(at);
            r = (v >> 11) & 0x1Fu; g = (v >> 6) & 0x1Fu; b = (v >> 1) & 0x1Fu;
            return pack4((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255);
        }
        default: {                                       // RGBA4
            uint32_t v = albw_r16(at);
            return pack4(((v >> 12) & 0xFu) * 17u, ((v >> 8) & 0xFu) * 17u, ((v >> 4) & 0xFu) * 17u,
                         255);
        }
    }
}

// Writes the colour's red, green and blue; the formats with alpha get it opaque, as
// albw_pixel_write does.
static inline void write_target(const struct draw_state *S, uint32_t at, rgba32 c) {
    uint32_t r = ch(c, 0), g = ch(c, 1), b = ch(c, 2);
    switch (S->color_fmt) {
        case ALBW_FB_RGB565: {
            uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            memcpy(albw_mem + at, &v, 2);
            ALBW_NOTE_WRITE(at, 2);
            return;
        }
        case ALBW_FB_RGBA8: {
            uint32_t v = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
            memcpy(albw_mem + at, &v, 4);
            ALBW_NOTE_WRITE(at, 4);
            return;
        }
        case ALBW_FB_RGB8:
            albw_mem[at + 0] = (uint8_t)b;
            albw_mem[at + 1] = (uint8_t)g;
            albw_mem[at + 2] = (uint8_t)r;
            ALBW_NOTE_WRITE(at, 3);
            return;
        case ALBW_FB_RGB5A1: {
            uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1u);
            memcpy(albw_mem + at, &v, 2);
            ALBW_NOTE_WRITE(at, 2);
            return;
        }
        default: {                                       // RGBA4
            uint16_t v = (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | 0xFu);
            memcpy(albw_mem + at, &v, 2);
            ALBW_NOTE_WRITE(at, 2);
            return;
        }
    }
}

// The stencil operations, in the order the register numbers them.
static inline uint32_t stencil_op(uint32_t op, uint32_t old, uint32_t ref) {
    switch (op & 7u) {
        case 0: return old;                              // keep
        case 1: return 0;                                // zero
        case 2: return ref;                              // replace
        case 3: return old < 255u ? old + 1u : 255u;     // increment, saturating
        case 4: return old ? old - 1u : 0u;              // decrement, saturating
        case 5: return ~old & 0xFFu;                     // invert
        case 6: return (old + 1u) & 0xFFu;               // increment, wrapping
        default: return (old - 1u) & 0xFFu;              // decrement, wrapping
    }
}

// Applies one of the three stencil operations at `dz`, through the write mask.
static inline void stencil_update(const struct draw_state *S, uint32_t dz, uint32_t op) {
    if (!(op & 7u)) return;
    uint32_t old = albw_mem[dz + 3u];
    uint32_t v = stencil_op(op, old, S->stencil_ref);
    albw_mem[dz + 3u] = (uint8_t)((old & ~S->stencil_write_mask) | (v & S->stencil_write_mask));
    ALBW_NOTE_WRITE(dz + 3u, 1);
}

// The logic ops, which take the blend's place when blending is off, on red, green and blue.
static inline uint32_t logic_op(uint32_t op, uint32_t s, uint32_t d) {
    switch (op & 0xFu) {
        case 0: return 0;
        case 1: return s & d;
        case 2: return s & ~d;
        case 3: return s;
        case 4: return ~0u;
        case 5: return ~s;
        case 6: return d;
        case 7: return ~d;
        case 8: return ~(s & d);
        case 9: return s | d;
        case 10: return ~(s | d);
        case 11: return s ^ d;
        case 12: return ~(s ^ d);
        case 13: return ~s & d;
        case 14: return s | ~d;
        default: return ~s | d;
    }
}

// Whether a fragment at depth `want` survives the depth test against the buffer at `dz`.
static inline int depth_passes(const struct draw_state *S, uint32_t dz, uint32_t want) {
    uint32_t have;
    if (S->depth_bytes == 2) {
        uint16_t h16;
        memcpy(&h16, albw_mem + dz, 2);
        have = h16;
    } else {
        uint32_t h32;
        memcpy(&h32, albw_mem + dz, 4);
        have = h32 & 0x00FFFFFFu;
    }
    return compare_func(S->depth_func, want, have);
}

// Everything after the depth test, which the caller has done: texturing, lighting, the combiner,
// the alpha test, blending and the writes. `flipped` is the row as memory counts it.
static int shade_pixel(const struct draw_state *S, uint32_t x, uint32_t flipped, uint32_t dz,
                       uint32_t want, rgba32 primary, const float tc[3][2], const float lod[3],
                       const float quat[4], const float view[3]) {
    // Every colour a stage can name, at the index its source number gives: the vertex colour, the
    // two lighting colours, four texture units, and the buffer, constant and previous output that
    // change from stage to stage. A disabled unit reads as nothing, which is what the hardware gives
    // a stage that names it anyway.
    rgba32 src_tab[16] = { 0 };
    src_tab[SRC_PRIMARY] = primary;
    for (int u = 0; u < 3; u++) {
        if (S->tex[u].enabled) {
            const float *c = tc[S->tex[u].coords];
            src_tab[SRC_TEX0 + u] = sample_unit(S, u, c[0], c[1], lod[u]);
        }
    }

    // The fragment colours the lighting unit produces: zero when it is off, as the hardware gives a
    // stage that names them anyway. Using the vertex colour in their place, as this did, painted
    // every lit surface in the title scene in flat colour.
    if (S->lighting) {
        albw_lighting_compute(&S->light, quat, view, &src_tab[SRC_TEX0], &src_tab[SRC_PRIMARY_FRAGMENT],
                              &src_tab[SRC_SECONDARY_FRAGMENT]);
    }

    // The buffer a stage can read lags two stages behind, not one: stage 0 sees nothing, stage 1 the
    // buffer colour register, and from stage 2 on the output of the last updating stage at least two
    // back. With a lag of one, as this had, the title scene's fourth stage read the dark specular
    // product of the third instead of the diffuse colour of the first, multiplied the texture by it,
    // and the whole lit scene came out black. And the "previous" output a first stage can name is
    // nothing, not the vertex colour.
    rgba32 next_buffer = S->buffer_color;
    rgba32 previous = 0;

    for (int stage = 0; stage < 6; stage++) {
        const struct stage_state *g = &S->stage[stage];
        rgba32 out;
        if (g->passthrough) {
            out = previous;
        } else {
            src_tab[SRC_PREVIOUS] = previous;
            src_tab[SRC_CONSTANT] = g->constant;

            // Colour and alpha are worked out apart, each the short way when it is a plain replace
            // or multiply. Only the dot products tie the two together.
            uint32_t rgb, a;
            int dot = 0, dotted = g->op_rgb == 6 || g->op_rgb == 7;
            if (g->rgb_kind == KIND_REPLACE) {
                rgb = src_tab[g->src[0]] & 0x00FFFFFFu;
            } else if (g->rgb_kind == KIND_MODULATE) {
                rgba32 s0 = src_tab[g->src[0]], s1 = src_tab[g->src[1]];
                rgb = pack4(ch(s0, 0) * ch(s1, 0) / 255u, ch(s0, 1) * ch(s1, 1) / 255u,
                            ch(s0, 2) * ch(s1, 2) / 255u, 0);
            } else {
                int m[3][3];
                for (int i = 0; i < 3; i++) modify_rgb(src_tab[g->src[i]], g->mod_rgb[i], m[i]);
                if (dotted) {
                    dot = combine_dot3(m[0], m[1]);
                    uint32_t d = saturate(dot, g->scale_rgb);
                    rgb = pack4(d, d, d, 0);
                } else {
                    rgb = pack4(saturate(combine(g->op_rgb, m[0][0], m[1][0], m[2][0]), g->scale_rgb),
                                saturate(combine(g->op_rgb, m[0][1], m[1][1], m[2][1]), g->scale_rgb),
                                saturate(combine(g->op_rgb, m[0][2], m[1][2], m[2][2]), g->scale_rgb),
                                0);
                }
            }
            // The alpha path reads its own three sources. Reading the colour ones, as this did
            // before, gave the logo's quad on the title screen a yellow, half-opaque backdrop.
            if (g->op_rgb == 7) {
                a = saturate(dot, g->scale_a);
            } else if (g->a_kind == KIND_REPLACE) {
                a = src_tab[g->src_a[0]] >> 24;
            } else if (g->a_kind == KIND_MODULATE) {
                a = (src_tab[g->src_a[0]] >> 24) * (src_tab[g->src_a[1]] >> 24) / 255u;
            } else {
                int alpha[3];
                for (int i = 0; i < 3; i++) alpha[i] = modify_alpha(src_tab[g->src_a[i]], g->mod_a[i]);
                a = saturate(combine(g->op_a, alpha[0], alpha[1], alpha[2]), g->scale_a);
            }
            out = rgb | (a << 24);
        }
        previous = out;

        // The first four stages can feed the buffer, which the stage after next is the first to see.
        src_tab[SRC_PREVIOUS_BUFFER] = next_buffer;
        if (stage < 4) {
            if ((S->update >> (8 + stage)) & 1u) next_buffer = (next_buffer & 0xFF000000u) | (out & 0x00FFFFFFu);
            if ((S->update >> (12 + stage)) & 1u) next_buffer = (next_buffer & 0x00FFFFFFu) | (out & 0xFF000000u);
        }
    }

    rgba32 src = previous;
    if (S->alpha_test && !compare_func(S->alpha_func, ch(src, 3), S->alpha_ref)) return 0;

    // With the stencil on, its test and the depth test come here, after the alpha test, because
    // each failure has its own operation on the stencil and a fragment the alpha test drops makes
    // none. Without it the caller has already done the depth test.
    if (S->stencil && dz) {
        uint32_t have = albw_mem[dz + 3u];
        if (!compare_func(S->stencil_func, S->stencil_ref & S->stencil_in_mask,
                          have & S->stencil_in_mask)) {
            stencil_update(S, dz, S->stencil_fail);
            return 0;
        }
        if (S->depth_test && !depth_passes(S, dz, want)) {
            stencil_update(S, dz, S->stencil_zfail);
            return 0;
        }
        stencil_update(S, dz, S->stencil_zpass);
    }

    // A logic op that keeps the destination writes no colour at all -- the stencil-only draws of
    // the game's shadow volumes, which drawn as colour were pale shapes standing in the scene.
    if (!S->blend && S->logic_op == 6u) {
        if (dz && S->depth_write) goto write_depth;
        return 1;
    }

    uint32_t at = S->color_base + tiled_offset(x, flipped, S->width, S->color_bytes);

    // The destination, needed only when the blend, the logic op or the write mask uses it.
    rgba32 dst = OPAQUE_BLACK;
    int keeps = S->blend || S->color_mask != 7u || S->logic_op != 3u;
    if (keeps) dst = read_target(S, at);

    // Only red, green and blue reach memory -- every target format is written opaque -- so the
    // blend's alpha is not worked out.
    rgba32 result = src;
    if (!S->blend && S->logic_op != 3u) {
        result = logic_op(S->logic_op, src, dst);
    } else if (S->blend && S->blend_func == S->blend_plain) {
        // The usual one: source alpha and one minus it, added.
        uint32_t a = ch(src, 3);
        result = 0;
        for (int i = 0; i < 3; i++) {
            uint32_t v = ch(src, i) * a / 255u + ch(dst, i) * (255u - a) / 255u;
            result |= (v > 255u ? 255u : v) << (8 * i);
        }
    } else if (S->blend) {
        uint32_t func = S->blend_func;
        result = 0;
        for (int i = 0; i < 3; i++) {
            uint32_t sf = i == 3 ? (func >> 24) & 0xFu : (func >> 16) & 0xFu;
            uint32_t df = i == 3 ? (func >> 28) & 0xFu : (func >> 20) & 0xFu;
            uint32_t eq = i == 3 ? (func >> 8) & 7u : func & 7u;
            int sv = (int)ch(src, i) * blend_factor(sf, src, dst, S->blend_color, i) / 255;
            int dv = (int)ch(dst, i) * blend_factor(df, src, dst, S->blend_color, i) / 255;
            int v = blend_equation(eq, sv, dv);
            result |= (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v)) << (8 * i);
        }
    }

    // Not yet gated on the framebuffer's permission registers 0x113 and 0x115: doing that the
    // obvious way made the title scene vanish, so their exact meaning is still open.
    uint32_t keep_new = (S->color_mask & 1u ? 0xFFu : 0u) | (S->color_mask & 2u ? 0xFF00u : 0u)
                      | (S->color_mask & 4u ? 0xFF0000u : 0u);
    write_target(S, at, (result & keep_new) | (dst & ~keep_new));
    if (dz && S->depth_write) {
write_depth:
        if (S->depth_bytes == 2) {
            uint16_t v16 = (uint16_t)want;
            memcpy(albw_mem + dz, &v16, 2);
        } else {
            for (int b = 0; b < 3; b++) albw_mem[dz + b] = (uint8_t)(want >> (b * 8));
        }
        ALBW_NOTE_WRITE(dz, S->depth_bytes == 2 ? 2u : 3u);
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------
// One triangle.
static inline float edge(const float a[2], const float b[2], float px, float py) {
    return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
}

// ---------------------------------------------------------------------------------------------
// Clipping.
//
// The hardware's clip volume is -w <= x <= w, -w <= y <= w, -w <= z <= 0 and w >= a small epsilon --
// z runs from the near plane at 0 to the far one at -w, which is why the depth range register holds
// minus one. A triangle partly outside is cut down to the part inside, which is a polygon of up to
// nine corners, and drawn as a fan. Throwing away any triangle with a corner behind the eye, which
// is what happened before, is right for nothing but flat work: the first wall the camera stands in
// front of would vanish.
#define CLIP_MAX 10
#define CLIP_EPSILON 1e-5f

static float clip_distance(const struct vertex *v, int plane) {
    const float *p = v->pos;
    switch (plane) {
        case 0: return p[3] - p[0];                      // x <= w
        case 1: return p[3] + p[0];                      // x >= -w
        case 2: return p[3] - p[1];                      // y <= w
        case 3: return p[3] + p[1];                      // y >= -w
        case 4: return -p[2];                            // z <= 0
        case 5: return p[3] + p[2];                      // z >= -w
        default: return p[3] - CLIP_EPSILON;             // w >= epsilon
    }
}

static void lerp_vertex(const struct vertex *a, const struct vertex *b, float t, struct vertex *out) {
    const float *fa = (const float *)a, *fb = (const float *)b;
    float *fo = (float *)out;
    for (size_t i = 0; i < sizeof *out / sizeof(float); i++) fo[i] = fa[i] + (fb[i] - fa[i]) * t;
}

struct prepared;
// A batch: its triangles, the states they point into, and, once it is being drawn, which bands
// each triangle touches. The game's draws go into one; frames drawn in between go into another, so
// the two never share a triangle or a state.
struct batch {
    struct prepared *prep;
    uint32_t prep_count, prep_room;
    struct draw_state *states;
    uint32_t state_count, state_room;
    int state_in_batch;                                  // the draw's state is the last entry
    // band b's triangles are bin[bin_start[b]] up to bin[bin_start[b + 1]], in drawing order
    uint32_t *bin, bin_room, *bin_start, bin_start_room, *fill, fill_room;
    int band_count;
    // Where a replay for an upscaler writes each pixel's motion: two floats a pixel in the target's
    // memory layout, the screen position the surface had a frame before less the one it has now,
    // in the target's pixels with the jitter taken out. NULL for every other batch.
    float *motion;
    float jitter[2];                                     // what the viewport was moved by
    // And how much of each pixel is something blended over what was there: one byte a pixel, in
    // the same layout, for the upscaler's reactive mask.
    uint8_t *reactive;
};
static struct batch game;
static uint32_t lut_epoch;                               // bumped whenever the tables are released

static void prepare_triangle(struct batch *b, const struct draw_state *S, const struct vertex *in);

static void draw_triangle(struct batch *b, const struct draw_state *S, const struct vertex *v) {
    // The common case first: everything inside, nothing to cut.
    int inside = 1;
    for (int plane = 0; plane < 7 && inside; plane++) {
        for (int i = 0; i < 3; i++) {
            if (clip_distance(&v[i], plane) < 0.0f) { inside = 0; break; }
        }
    }
    if (inside) {
        prepare_triangle(b, S, v);
        return;
    }

    struct vertex poly[2][CLIP_MAX];
    int count = 3, cur = 0;
    memcpy(poly[0], v, 3 * sizeof *v);
    for (int plane = 0; plane < 7 && count >= 3; plane++) {
        int out_count = 0;
        for (int i = 0; i < count; i++) {
            const struct vertex *a = &poly[cur][i], *b = &poly[cur][(i + 1) % count];
            float da = clip_distance(a, plane), db = clip_distance(b, plane);
            if (da >= 0.0f && out_count < CLIP_MAX) poly[!cur][out_count++] = *a;
            if ((da >= 0.0f) != (db >= 0.0f) && out_count < CLIP_MAX) {
                lerp_vertex(a, b, da / (da - db), &poly[!cur][out_count++]);
            }
        }
        cur = !cur;
        count = out_count;
    }
    if (count < 3) {
        if (b == &game) triangles_clipped_away++;
        return;
    }
    for (int i = 1; i + 1 < count; i++) {
        struct vertex tri[3] = { poly[cur][0], poly[cur][i], poly[cur][i + 1] };
        prepare_triangle(b, S, tri);
    }
}

// ---------------------------------------------------------------------------------------------
// Triangles, set up once and rasterised in parallel.
//
// Triangles are clipped and set up on the guest's thread into a batch that runs across draws: each
// one keeps a copy of its draw's state, so the registers can move on. The batch is drawn when it has
// to be -- the target changes, the lighting tables are written, a command list ends -- and that is a
// few times a frame instead of once a draw, each time waking threads that cost more than most draws.
//
// The target's rows are dealt out in bands of four, counted the way memory counts them (bottom up):
// in the tiled layout four rows of eight pixels share a cache line, so neighbouring rows drawn by
// different threads made every write fight over the same line. Each band is drawn by one thread,
// taking its triangles in their original order, so blending and the depth test come out exactly as
// they would one triangle at a time; which thread takes which band does not matter, so a thread
// that finishes early takes the next band left.
struct prepared {
    struct vertex v[3];
    float screen[3][2], inv_w[3], z[3], area;
    int x0, x1, y0, y1, wbuffer;
    uint32_t state;                                      // which of the batch's draw states
    int32_t fx[3], fy[3];                                // the corners in sixteenths of a pixel
    int sgn;                                             // the winding: 1 or -1
    int bias[3];                                         // 0 where an edge owns its pixels, -1 not
    float inv_area;                                      // one over the area, in sixteenths squared
    int narrow;                                          // the edges fit 32 bits anywhere on target
};
static const int edge_from[3] = { 1, 2, 0 }, edge_to[3] = { 2, 0, 1 };


static void prepare_triangle(struct batch *b, const struct draw_state *S, const struct vertex *in) {
    // Two quaternions for the same rotation can point opposite ways, and interpolating between them
    // goes the long way round through a normal that faces somewhere else entirely. Turning the
    // others into the first one's hemisphere is what keeps a smooth surface smooth.
    struct prepared t;
    t.v[0] = in[0];
    t.v[1] = in[1];
    t.v[2] = in[2];
    struct vertex *v = t.v;
    for (int k = 1; k < 3; k++) {
        float d = 0.0f;
        for (int c = 0; c < 4; c++) d += v[0].quat[c] * v[k].quat[c];
        if (d < 0.0f) for (int c = 0; c < 4; c++) v[k].quat[c] = -v[k].quat[c];
    }
    const float half_w = S->half_w, half_h = S->half_h, offset_x = S->offset_x;
    const float offset_y = S->offset_y, depth_range = S->depth_range, depth_near = S->depth_near;

    for (int i = 0; i < 3; i++) {
        if (!(v[i].pos[3] > 0.0f)) {
            // Clipping leaves w at least a small epsilon, so this is a shader this machine ran
            // wrongly. Say what it looked like the first few times rather than counting silently.
            if (b == &game) triangles_skipped++;
            static unsigned said;
            if (albw_env("ALBW_PROBE") && said < 8) {
                said++;
                printf("[raster] skipping a triangle from shader %04x:", albw_pica_reg(R_VS_ENTRY));
                for (int k = 0; k < 3; k++) {
                    printf("  (%g %g %g %g)", v[k].pos[0], v[k].pos[1], v[k].pos[2], v[k].pos[3]);
                }
                printf("\n");
            }
            return;
        }
        t.inv_w[i] = 1.0f / v[i].pos[3];
        t.screen[i][0] = (v[i].pos[0] * t.inv_w[i] + 1.0f) * half_w + offset_x;
        t.screen[i][1] = (v[i].pos[1] * t.inv_w[i] + 1.0f) * half_h + offset_y;
        t.z[i] = depth_range * v[i].pos[2] * t.inv_w[i] + depth_near;
    }

    // The corners snapped to sixteenths of a pixel, as the hardware's rasteriser has them, and the
    // edges worked out in integers from there. Two triangles that share an edge then compute
    // exactly opposite values along it, and the fill rule below gives each pixel on it to one of
    // the two. In floating point they computed slightly different values, a pixel on the edge could
    // belong to neither, and large quads showed thin diagonal seams.
    for (int i = 0; i < 3; i++) {
        float sx = t.screen[i][0] * 16.0f, sy = t.screen[i][1] * 16.0f;
        if (!(fabsf(sx) < 1048576.0f && fabsf(sy) < 1048576.0f)) {
            if (b == &game) triangles_skipped++;         // not anywhere a target can be, or NaN
            return;
        }
        t.fx[i] = (int32_t)lrintf(sx);
        t.fy[i] = (int32_t)lrintf(sy);
    }
    int64_t area = (int64_t)(t.fx[1] - t.fx[0]) * (t.fy[2] - t.fy[0])
                 - (int64_t)(t.fy[1] - t.fy[0]) * (t.fx[2] - t.fx[0]);
    if (area == 0) {
        if (b == &game) triangles_skipped++;
        return;
    }
    t.area = (float)area;
    t.sgn = area > 0 ? 1 : -1;
    // With every corner within 15000 sixteenths (some 900 pixels) of the origin, an edge's value at
    // any pixel of a target -- at most 1024 wide, plus the three pixels a group of four can run
    // past it -- is a difference of two products under 2^30 each, so it fits 32 bits.
    t.narrow = 1;
    for (int i = 0; i < 3; i++) {
        if (t.fx[i] < -15000 || t.fx[i] > 15000 || t.fy[i] < -15000 || t.fy[i] > 15000) t.narrow = 0;
    }
    t.inv_area = 1.0f / (float)(area > 0 ? area : -area);
    // The fill rule: a pixel exactly on an edge belongs to the triangle that lies to its right, or
    // above it where the edge is level. A shared edge has the two triangles on opposite sides, so
    // exactly one of them takes it.
    for (int k = 0; k < 3; k++) {
        int64_t dx = t.fx[edge_to[k]] - t.fx[edge_from[k]], dy = t.fy[edge_to[k]] - t.fy[edge_from[k]];
        int64_t nx = -t.sgn * dy, ny = t.sgn * dx;       // pointing into the triangle
        t.bias[k] = (nx > 0 || (nx == 0 && ny > 0)) ? 0 : -1;
    }
    // Culling, as the register numbers the choices: keep everything, keep one winding, keep the
    // other. The sign of the area is the winding.
    uint32_t cull = S->cull;
    if ((cull == 1 && t.area > 0.0f) || (cull == 2 && t.area < 0.0f)) return;

    uint32_t width = S->width, height = S->height_m1 + 1u;
    t.x0 = (int)floorf(fminf(fminf(t.screen[0][0], t.screen[1][0]), t.screen[2][0]));
    t.x1 = (int)ceilf(fmaxf(fmaxf(t.screen[0][0], t.screen[1][0]), t.screen[2][0]));
    t.y0 = (int)floorf(fminf(fminf(t.screen[0][1], t.screen[1][1]), t.screen[2][1]));
    t.y1 = (int)ceilf(fmaxf(fmaxf(t.screen[0][1], t.screen[1][1]), t.screen[2][1]));

    int sx0 = S->scissor_x0, sy0 = S->scissor_y0, sx1 = S->scissor_x1, sy1 = S->scissor_y1;
    if (t.x0 < sx0) t.x0 = sx0;
    if (t.y0 < sy0) t.y0 = sy0;
    if (t.x1 > sx1) t.x1 = sx1;
    if (t.y1 > sy1) t.y1 = sy1;
    if (t.x1 >= (int)width) t.x1 = (int)width - 1;
    if (t.y1 >= (int)height) t.y1 = (int)height - 1;
    if (t.x0 > t.x1 || t.y0 > t.y1) return;
    t.wbuffer = S->wbuffer;

    if (b->prep_count == b->prep_room) {
        uint32_t room = b->prep_room ? b->prep_room * 2u : 256u;
        struct prepared *bigger = realloc(b->prep, room * sizeof *bigger);
        if (!bigger) return;
        b->prep = bigger;
        b->prep_room = room;
    }
    if (!b->state_in_batch) {
        if (b->state_count == b->state_room) {
            uint32_t room = b->state_room ? b->state_room * 2u : 64u;
            struct draw_state *bigger = realloc(b->states, room * sizeof *bigger);
            if (!bigger) return;
            b->states = bigger;
            b->state_room = room;
        }
        b->states[b->state_count++] = *S;
        b->state_in_batch = 1;
    }
    t.state = b->state_count - 1u;
    b->prep[b->prep_count++] = t;
    if (b == &game) triangles_drawn++;
}

// One triangle's rows inside band `band`. Within a row only the span between the triangle's edges is
// visited. The span is widened by a pixel each side against rounding, and every pixel in it still
// takes the exact edge test, so which pixels are drawn is what testing the whole bounding box decides.
static unsigned long rasterise_band(const struct batch *b, const struct prepared *t, int band) {
    const struct draw_state *S = &b->states[t->state];
    const struct vertex *v = t->v;
    unsigned long drawn = 0;
    int need_tc[3] = { 0, 0, 0 };
    for (int u = 0; u < 3; u++) {
        if (S->tex[u].enabled) need_tc[S->tex[u].coords] = 1;
    }
    const int need_light = S->lighting;
    const int front = t->sgn > 0;
    const float sign = front ? 1.0f : -1.0f;
    float tc[3][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    float quat[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, view[3] = { 0.0f, 0.0f, 0.0f };
    float lod[3] = { 0.0f, 0.0f, 0.0f };
    int lod_unit[3], lod_coords[3] = { 0, 0, 0 }, need_lod = 0;
    for (int u = 0; u < 3; u++) {
        lod_unit[u] = S->tex[u].enabled && S->tex[u].max_level > 0;
        if (lod_unit[u]) lod_coords[S->tex[u].coords] = 1;
        need_lod |= lod_unit[u];
    }

    const int hm1 = (int)S->height_m1;
    int ya = hm1 - (band * 4 + 3), yb = hm1 - band * 4;
    if (ya < t->y0) ya = t->y0;
    if (yb > t->y1) yb = t->y1;
    for (int y = ya; y <= yb; y++) {
        const float py = (float)y + 0.5f;
        const uint32_t flipped = (uint32_t)(hm1 - y);

        // Each edge is c - d * (px - a.x) along the row; inside is where that has the winding's
        // sign, which bounds px from one side.
        int xa = t->x0, xb = t->x1;
        for (int k = 0; k < 3 && xa <= xb; k++) {
            const float *a = t->screen[edge_from[k]], *b = t->screen[edge_to[k]];
            float c = sign * ((b[0] - a[0]) * (py - a[1])), d = sign * (b[1] - a[1]);
            if (d == 0.0f) {
                if (c < 0.0f) xb = xa - 1;
                continue;
            }
            float limit = a[0] + c / d - 0.5f;
            if (d > 0.0f) {
                if (limit < (float)xa - 2.0f) xb = xa - 1;
                else if (limit < (float)xb) {
                    int m = (int)floorf(limit) + 1;
                    if (m < xb) xb = m;
                }
            } else {
                if (limit > (float)xb + 2.0f) xb = xa - 1;
                else if (limit > (float)xa) {
                    int m = (int)ceilf(limit) - 1;
                    if (m > xa) xa = m;
                }
            }
        }

        // Four pixels at a time. The edges are integers in sixteenths of a pixel, counted with the
        // winding's sign so that inside is where all three are at least their fill-rule bias; each
        // steps by a constant from one pixel to the next.
        int64_t E[3], step[3];
        const int64_t py16 = (int64_t)y * 16 + 8;
        for (int k = 0; k < 3; k++) {
            int a = edge_from[k], b = edge_to[k];
            int64_t dx = t->fx[b] - t->fx[a], dy = t->fy[b] - t->fy[a];
            E[k] = t->sgn * (dx * (py16 - t->fy[a]) - dy * ((int64_t)xa * 16 + 8 - t->fx[a]));
            step[k] = -t->sgn * dy * 16;
        }
        // For the level of detail, how fast each coordinate changes from pixel to pixel. Over w,
        // an attribute and the reciprocal of w are both linear in the pixel position, so the
        // derivative of their quotient is exact: d(N/D) = (dN - (N/D) dD) / D. These are dN and
        // dD, along x and y; the quotient and 1/D come per pixel anyway.
        float Dx = 0.0f, Dy = 0.0f, Gx[3][2] = { { 0.0f } }, Gy[3][2] = { { 0.0f } };
        if (need_lod) {
            for (int k = 0; k < 3; k++) {
                int64_t dx = t->fx[edge_to[k]] - t->fx[edge_from[k]];
                float bx = (float)step[k] * t->inv_area * t->inv_w[k];
                float by = (float)(t->sgn * dx * 16) * t->inv_area * t->inv_w[k];
                Dx += bx;
                Dy += by;
                for (int c = 0; c < 3; c++) {
                    if (!lod_coords[c]) continue;
                    for (int i = 0; i < 2; i++) {
                        Gx[c][i] += bx * v[k].tc[c][i];
                        Gy[c][i] += by * v[k].tc[c][i];
                    }
                }
            }
        }
        const v4f inv_area = splat(t->inv_area);
        const v4f iw0 = splat(t->inv_w[0]), iw1 = splat(t->inv_w[1]), iw2 = splat(t->inv_w[2]);
        const v4f z0 = splat(t->z[0]), z1 = splat(t->z[1]), z2 = splat(t->z[2]);
        const v4f zero = splat(0.0f), one = splat(1.0f), half = splat(0.5f);
        const v4f depth_scale = splat((float)S->depth_max);
        // The 32-bit form, four lanes at once, for the triangles that fit it; a lane is inside
        // when all three edges, less their bias, are not negative.
        const v4i lane = { 0, 1, 2, 3 };
        v4i Ev[3], step4[3], keep[3];
        if (t->narrow) {
            for (int k = 0; k < 3; k++) {
                Ev[k] = (v4i){ (int32_t)E[k], (int32_t)E[k], (int32_t)E[k], (int32_t)E[k] }
                      + lane * (int32_t)step[k];
                step4[k] = (v4i){ 4, 4, 4, 4 } * (int32_t)step[k];
                keep[k] = (v4i){ -1, -1, -1, -1 } - (v4i){ t->bias[k], t->bias[k], t->bias[k],
                                                           t->bias[k] };
            }
        }
        for (int x = xa; x <= xb; x += 4) {
            v4f b0, b1, b2;
            int live = 0;
            if (t->narrow) {
                // e + bias >= 0 is e > -1 - bias.
                v4i in = (Ev[0] > keep[0]) & (Ev[1] > keep[1]) & (Ev[2] > keep[2])
                       & (lane + x <= (v4i){ xb, xb, xb, xb });
                for (int l = 0; l < 4; l++) live |= (in[l] & 1) << l;
                b0 = __builtin_convertvector(Ev[0], v4f);
                b1 = __builtin_convertvector(Ev[1], v4f);
                b2 = __builtin_convertvector(Ev[2], v4f);
                for (int k = 0; k < 3; k++) Ev[k] += step4[k];
            } else {
                for (int l = 0; l < 4; l++) {
                    int64_t e0 = E[0] + l * step[0], e1 = E[1] + l * step[1];
                    int64_t e2 = E[2] + l * step[2];
                    if (x + l <= xb && e0 + t->bias[0] >= 0 && e1 + t->bias[1] >= 0
                            && e2 + t->bias[2] >= 0) {
                        live |= 1 << l;
                    }
                    b0[l] = (float)e0;
                    b1[l] = (float)e1;
                    b2[l] = (float)e2;
                }
                for (int k = 0; k < 3; k++) E[k] += 4 * step[k];
            }
            if (!live) continue;
            b0 = b0 * inv_area;
            b1 = b1 * inv_area;
            b2 = b2 * inv_area;

            // Perspective correction: an attribute is interpolated divided by w and then divided by
            // the interpolated reciprocal of w. With a constant w -- which is what the game's flat
            // work has -- this is the same as a plain interpolation, and it costs nothing to be
            // right about the other case.
            v4f sum = b0 * iw0 + b1 * iw1 + b2 * iw2;
            v4i sum_zero = sum == zero;
            v4f inv_sum = one / sum;

            // Depth interpolates in screen space, not perspective-corrected: it is already divided
            // through by w above. Register 0x06D picks between that, a Z buffer, and a W buffer,
            // which multiplies the same value back by the fragment's w; and either way the result
            // is clamped to the range a depth buffer holds. Without the clamp a value past one ran
            // over in the conversion to an integer and wrote a depth nearer than anything.
            v4f depth = b0 * z0 + b1 * z1 + b2 * z2;
            if (t->wbuffer) depth = depth * inv_sum;
            depth = select4(depth < zero, zero, select4(depth > one, one, depth));
            v4f want_f = depth * depth_scale + half;

            // The depth test before anything else: nothing below changes its answer (there is no
            // stencil yet, whose depth-fail operation would need the fragment), and most of what a
            // scene draws behind something nearer would otherwise be textured, lit and combined
            // only to be thrown away.
            uint32_t dz[4], want[4];
            for (int l = 0; l < 4; l++) {
                if (!(live >> l & 1)) continue;
                if (sum_zero[l]) { live &= ~(1 << l); continue; }
                dz[l] = S->depth_base ? S->depth_base + tiled_offset((uint32_t)(x + l), flipped,
                                                                     S->width, S->depth_bytes)
                                      : 0;
                want[l] = (uint32_t)want_f[l];
                if (dz[l] && S->depth_test && !S->stencil && !depth_passes(S, dz[l], want[l])) {
                    live &= ~(1 << l);
                }
            }
            if (!live || !S->color_base) continue;

            v4f w0 = b0 * iw0 * inv_sum, w1 = b1 * iw1 * inv_sum, w2 = b2 * iw2 * inv_sum;
            uint32_t primary[4] = { 0, 0, 0, 0 };
            for (int c = 0; c < 4; c++) {
                v4f value = zero + w0 * splat(v[0].color[c]);
                value = value + w1 * splat(v[1].color[c]);
                value = value + w2 * splat(v[2].color[c]);
                value = select4(value < zero, zero, select4(value > one, one, value));
                v4f scaled = value * splat(255.0f) + half;
                for (int l = 0; l < 4; l++) {
                    primary[l] |= (uint32_t)(uint8_t)scaled[l] << (8 * c);
                }
            }
            // Only what this draw reads: coordinates for the units that are on, and the normal and
            // view vector only when the lighting result is used.
            v4f tcv[3][2], quatv[4], viewv[3];
            for (int u = 0; u < 3; u++) {
                if (!need_tc[u]) continue;
                for (int c = 0; c < 2; c++) {
                    v4f value = zero + w0 * splat(v[0].tc[u][c]);
                    value = value + w1 * splat(v[1].tc[u][c]);
                    tcv[u][c] = value + w2 * splat(v[2].tc[u][c]);
                }
            }
            v4f lodv[3];
            for (int u = 0; u < 3; u++) {
                if (!lod_unit[u]) continue;
                int c = S->tex[u].coords;
                v4f s0 = tcv[c][0], s1 = tcv[c][1];
                v4f sx = (splat(Gx[c][0]) - s0 * splat(Dx)) * inv_sum;
                v4f tx = (splat(Gx[c][1]) - s1 * splat(Dx)) * inv_sum;
                v4f sy = (splat(Gy[c][0]) - s0 * splat(Dy)) * inv_sum;
                v4f ty = (splat(Gy[c][1]) - s1 * splat(Dy)) * inv_sum;
                v4f w2s = splat(S->tex[u].size_sq[0]), h2s = splat(S->tex[u].size_sq[1]);
                v4f rx = sx * sx * w2s + tx * tx * h2s, ry = sy * sy * w2s + ty * ty * h2s;
                lodv[u] = half_log2(select4(rx > ry, rx, ry));
            }
            if (need_light) {
                for (int c = 0; c < 4; c++) {
                    quatv[c] = w0 * splat(v[0].quat[c]) + w1 * splat(v[1].quat[c])
                             + w2 * splat(v[2].quat[c]);
                }
                for (int c = 0; c < 3; c++) {
                    viewv[c] = w0 * splat(v[0].view[c]) + w1 * splat(v[1].view[c])
                             + w2 * splat(v[2].view[c]);
                }
            }
            for (int l = 0; l < 4; l++) {
                if (!(live >> l & 1)) continue;
                for (int u = 0; u < 3; u++) {
                    if (need_tc[u]) {
                        tc[u][0] = tcv[u][0][l];
                        tc[u][1] = tcv[u][1][l];
                    }
                    if (lod_unit[u]) lod[u] = lodv[u][l];
                }
                if (need_light) {
                    for (int c = 0; c < 4; c++) quat[c] = quatv[c][l];
                    for (int c = 0; c < 3; c++) view[c] = viewv[c][l];
                }
                if (shade_pixel(S, (uint32_t)(x + l), flipped, dz[l], want[l], primary[l], tc, lod,
                                quat, view)) {
                    drawn++;
                    // Whatever was drawn last is what the pixel shows, and its motion is the
                    // pixel's -- the interface over a moving scene stands still. What is blended
                    // over what was there is marked for the upscaler as well: such a pixel is
                    // both, and it should trust the new frame there more than its history.
                    if (b->motion) {
                        size_t at = (size_t)flipped * S->width + (uint32_t)(x + l);
                        b->reactive[at] = S->blend && !S->depth_write ? 200 : 0;
                        float pc[4];
                        for (int c = 0; c < 4; c++) {
                            pc[c] = w0[l] * v[0].prev[c] + w1[l] * v[1].prev[c] + w2[l] * v[2].prev[c];
                        }
                        float *m = b->motion + at * 2u;
                        if (pc[3] > 1e-6f) {
                            float iw = 1.0f / pc[3];
                            float px = (pc[0] * iw + 1.0f) * S->half_w + S->offset_x - b->jitter[0];
                            float py = (pc[1] * iw + 1.0f) * S->half_h + S->offset_y - b->jitter[1];
                            m[0] = px - ((float)(x + l) + 0.5f - b->jitter[0]);
                            m[1] = py - ((float)y + 0.5f - b->jitter[1]);
                        } else {
                            m[0] = m[1] = 0.0f;
                        }
                    }
                }
            }
        }
    }
    return drawn;
}

// ---------------------------------------------------------------------------------------------
// The worker pool.
//
// A job is a number of items -- bands to draw, groups of vertices to shade -- that the threads take
// one at a time from a shared counter until none are left; the guest's thread takes them too. The
// counter carries the job's generation and size, so a thread that comes late to one job can never
// take an item that belongs to the next. A thread with nothing to do spins for a while before it
// sleeps: draws come in bursts, and waking a sleeping thread takes longer than most jobs.
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS 32
#define SPIN_NS 200000L                                  // how long an idle thread keeps looking
static int workers = -1;                                 // decided at the first job

// Two jobs can be out at once, in two slots: the game's thread's, and the window's -- the frames
// in between and the picture's scaling. A worker always takes the game's items first, and the
// window's only when the game has none, so the game waits for nothing but the item a worker has in
// hand -- a band of four rows -- while the window gets every hand the game leaves idle. Each slot
// has one thread that puts jobs in it and helps with them; nobody else touches its fields.
struct slot {
    _Atomic uint64_t cursor;                             // generation 24 bits, items 20, next 20
    _Atomic uint32_t done;                               // items finished
    void (*fn)(uint32_t item, int worker);
    void *arg;                                           // the job's own, as job_arg while it runs
    uint32_t generation;
};
static struct slot slots[2];
static _Atomic uint32_t job_wake;                        // bumped per job; sleepers wait on it
static _Atomic int sleepers;
static _Thread_local void *job_arg;

// Per-thread pixel counts, a cache line each.
static struct { unsigned long n; char pad[56]; } pixels_by_worker[MAX_WORKERS + 1];

static long futex(_Atomic uint32_t *word, int op, uint32_t value) {
    return syscall(SYS_futex, (uint32_t *)word, op, value, NULL, NULL, 0);
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// Takes one item of a slot's job, if there is one left. A look first, so that an exhausted cursor
// is not counted on past its items for every idle look at it.
static int take_one(struct slot *sl, int worker) {
    uint64_t c = atomic_load_explicit(&sl->cursor, memory_order_acquire);
    if ((uint32_t)c % (1u << 20) >= ((uint32_t)(c >> 20) & 0xFFFFFu)) return 0;
    c = atomic_fetch_add_explicit(&sl->cursor, 1, memory_order_acq_rel);
    uint32_t items = (uint32_t)(c >> 20) & 0xFFFFFu, next = (uint32_t)c & 0xFFFFFu;
    if (next >= items) return 0;
    job_arg = sl->arg;
    sl->fn(next, worker);
    atomic_fetch_add_explicit(&sl->done, 1, memory_order_release);
    return 1;
}

static void *worker_main(void *arg) {
    int id = (int)(intptr_t)arg;
    uint32_t seen = atomic_load_explicit(&job_wake, memory_order_acquire);
    for (;;) {
        // Wait for the next job: spin, then sleep.
        uint32_t now = atomic_load_explicit(&job_wake, memory_order_acquire);
        if (now == seen) {
            uint64_t until = now_ns() + SPIN_NS;
            unsigned spins = 0;
            while ((now = atomic_load_explicit(&job_wake, memory_order_acquire)) == seen) {
                if (++spins % 64u == 0 && now_ns() > until) {
                    atomic_fetch_add_explicit(&sleepers, 1, memory_order_seq_cst);
                    if (atomic_load_explicit(&job_wake, memory_order_seq_cst) == seen) {
                        futex(&job_wake, FUTEX_WAIT_PRIVATE, seen);
                    }
                    atomic_fetch_sub_explicit(&sleepers, 1, memory_order_seq_cst);
                    until = now_ns() + SPIN_NS;
                }
                __builtin_ia32_pause();
            }
        }
        seen = now;
        // The game's items while there are any, then one of the window's, then look again. A
        // quarter of the workers never take the window's: the game's jobs are many and short --
        // a draw's vertices are one -- and would otherwise start each time only as fast as a
        // worker finishes a band of a frame in between.
        for (;;) {
            if (take_one(&slots[0], id)) continue;
            if (id > workers / 4 && take_one(&slots[1], id)) continue;
            break;
        }
    }
    return NULL;
}

static void start_workers(void) {
    const char *s = albw_env("ALBW_RENDER_THREADS");
    long n = s ? atol(s) : sysconf(_SC_NPROCESSORS_ONLN) - 1;
#ifdef ALBW_TRACK_WRITES
    n = 1;                                               // the write record is not thread-safe
#endif
    if (n < 1) n = 1;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    workers = (int)n;
    for (int i = 1; i < workers; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, worker_main, (void *)(intptr_t)i) != 0) {
            // Fewer threads than asked for only means fewer hands: items go to whoever is there.
            printf("[raster] could only start %d of %d threads\n", i, workers);
            workers = i;
            break;
        }
        pthread_detach(th);
    }
    printf("[raster] drawing with %d thread(s)\n", workers);
}

// Which thread is the game's: until the window starts it on a thread of its own, every thread is.
static pthread_t game_thread;
static int game_thread_known;
static pthread_once_t workers_once = PTHREAD_ONCE_INIT;

static int on_game_thread(void) {
    return !game_thread_known || pthread_equal(pthread_self(), game_thread);
}

void albw_raster_game_thread(void) {
    game_thread = pthread_self();
    game_thread_known = 1;
}

// Runs `fn` on items 0 to items - 1, shared among the workers, and returns when all are done; `arg`
// is job_arg while it runs. The game's thread uses the first slot, the window's the second.
static void pool_run_arg(void (*fn)(uint32_t item, int worker), void *arg, uint32_t items) {
    pthread_once(&workers_once, start_workers);
    if (workers <= 1 || items <= 1) {
        job_arg = arg;
        for (uint32_t i = 0; i < items; i++) fn(i, 0);
        return;
    }
    if (items > 0xFFFF0u) items = 0xFFFF0u;              // the cursor's field; never reached
    struct slot *sl = &slots[on_game_thread() ? 0 : 1];
    sl->fn = fn;
    sl->arg = arg;
    atomic_store_explicit(&sl->done, 0, memory_order_relaxed);
    sl->generation = (sl->generation + 1u) & 0xFFFFFFu;
    atomic_store_explicit(&sl->cursor, ((uint64_t)sl->generation << 40) | ((uint64_t)items << 20),
                          memory_order_release);
    atomic_fetch_add_explicit(&job_wake, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&sleepers, memory_order_seq_cst) > 0) {
        futex(&job_wake, FUTEX_WAKE_PRIVATE, (uint32_t)workers);
    }
    while (take_one(sl, 0)) {}
    while (atomic_load_explicit(&sl->done, memory_order_acquire) < items) __builtin_ia32_pause();
}

static void pool_run(void (*fn)(uint32_t item, int worker), uint32_t items) {
    pool_run_arg(fn, NULL, items);
}

void albw_raster_parallel(void (*fn)(uint32_t item, int worker), uint32_t items) {
    pool_run(fn, items);
}

// Where rendering's time goes, for the report at the end: shading vertices, setting triangles up,
// and rasterising them, each split into the work shared out and the work done alone. A clock read
// costs nanoseconds and a draw tens of microseconds, so this is always on.
static double probe_now(void) {
    return (double)now_ns() * 1e-9;
}
static double t_shade_pool, t_shade_alone, t_setup, t_rast_pool, t_rast_alone;
static unsigned long vertices_named, vertices_shaded;
static unsigned long n_shade_pool, n_shade_alone, n_rast_pool, n_rast_alone, n_draws_timed;

// One band of the batch the job was given.
static void band_job(uint32_t band, int worker) {
    const struct batch *b = job_arg;
    unsigned long drawn = 0;
    for (uint32_t k = b->bin_start[band]; k < b->bin_start[band + 1u]; k++) {
        drawn += rasterise_band(b, &b->prep[b->bin[k]], (int)band);
    }
    if (b == &game) pixels_by_worker[worker].n += drawn;
}

// Draws the batch and empties it.
// The right eye.
//
// The game draws its top screen twice a frame, once per eye, into the same target, and copies each
// to its own framebuffer: left eye, right eye, then the bottom screen into the target's last 320
// columns. Only the left eye is ever shown here, and with the 3D slider at zero, which is what the
// system page reports, the two eyes are the same picture -- so the second pass is work whose result
// nobody sees and which leaves the target exactly as the first one did.
//
// So after a transfer of a whole top screen the draws are kept back instead of drawn at the end of
// each list. If the next transfer is a whole top screen again, they were the right eye and are let
// go; anything else -- another kind of transfer, a fill, a DMA, a change of target -- draws them
// first, late but in order. ALBW_RIGHT_EYE=1 draws both eyes regardless.
static int after_left_eye;
static unsigned long right_eyes_skipped;

static int right_eye_skipping(void) {
    static int checked, skip;
    if (!checked) {
        checked = 1;
        const char *e = albw_env("ALBW_RIGHT_EYE");
        skip = !(e && *e && *e != '0');
    }
    return skip;
}

static void rec_mark(void);
static void rec_back_to_mark(void);

static unsigned long flushes;
static void discard_batch(void) {
    etc1_epoch++;
    game.prep_count = 0;
    game.state_count = 0;
    game.state_in_batch = 0;
    albw_lighting_release();
    lut_epoch++;
}

void albw_raster_list_done(void) {
    if (after_left_eye) return;                          // held until the next transfer says
    albw_raster_flush();
}

void albw_raster_before_transfer(int whole_top_screen) {
    if (after_left_eye) {
        after_left_eye = 0;
        if (whole_top_screen) {
            if (game.prep_count) right_eyes_skipped++;
            discard_batch();
            rec_back_to_mark();                          // and the record forgets them too
            return;
        }
        albw_raster_flush();
        return;
    }
    albw_raster_flush();
    if (whole_top_screen && right_eye_skipping()) {
        after_left_eye = 1;
        rec_mark();
    }
}

void albw_raster_before_memory(void) {
    after_left_eye = 0;
    albw_raster_flush();
}

// Draws a batch into its target and empties it. Every draw in a batch has the same target (a
// change of target ends one), so its bands are the same for all of them.
static void draw_batch(struct batch *b) {
    if (!b->prep_count) return;
    int hm1 = (int)b->states[0].height_m1;
    b->band_count = (hm1 >> 2) + 1;
    uint32_t bands = (uint32_t)b->band_count;
    if (bands + 1u > b->bin_start_room) {
        uint32_t *bigger = realloc(b->bin_start, (bands + 1u) * sizeof *bigger);
        if (!bigger) return;
        b->bin_start = bigger;
        b->bin_start_room = bands + 1u;
    }
    memset(b->bin_start, 0, (bands + 1u) * sizeof *b->bin_start);
    uint64_t entries = 0, area = 0;
    for (uint32_t i = 0; i < b->prep_count; i++) {
        const struct prepared *t = &b->prep[i];
        int lo = (hm1 - t->y1) >> 2, hi = (hm1 - t->y0) >> 2;
        for (int k = lo; k <= hi; k++) b->bin_start[k + 1]++;
        entries += (uint64_t)(hi - lo + 1);
        area += (uint64_t)(t->x1 - t->x0 + 1) * (uint64_t)(t->y1 - t->y0 + 1);
    }
    if (entries > b->bin_room) {
        uint32_t *bigger = realloc(b->bin, entries * sizeof *bigger);
        if (!bigger) return;
        b->bin = bigger;
        b->bin_room = (uint32_t)entries;
    }
    for (uint32_t k = 0; k < bands; k++) b->bin_start[k + 1] += b->bin_start[k];
    // Filled through a moving copy of the starts, which leaves each band's list in drawing order.
    if (bands > b->fill_room) {
        uint32_t *bigger = realloc(b->fill, bands * sizeof *bigger);
        if (!bigger) return;
        b->fill = bigger;
        b->fill_room = bands;
    }
    memcpy(b->fill, b->bin_start, bands * sizeof *b->fill);
    for (uint32_t i = 0; i < b->prep_count; i++) {
        const struct prepared *t = &b->prep[i];
        int lo = (hm1 - t->y1) >> 2, hi = (hm1 - t->y0) >> 2;
        for (int k = lo; k <= hi; k++) b->bin[b->fill[k]++] = i;
    }

    // A batch of a few small sprites is not worth waking anybody for.
    double t0 = probe_now();
    int alone = area < 4096u;
    if (alone) {
        job_arg = b;
        for (uint32_t k = 0; k < bands; k++) band_job(k, 0);
    } else {
        pool_run_arg(band_job, b, bands);
    }
    if (b == &game) {
        double t1 = probe_now();
        if (alone) { t_rast_alone += t1 - t0; n_rast_alone++; }
        else { t_rast_pool += t1 - t0; n_rast_pool++; }
    }
}

void albw_raster_flush(void) {
    etc1_epoch++;                                        // memory may have changed since the last
    if (game.prep_count) {
        flushes++;
        draw_batch(&game);
    }
    game.prep_count = 0;
    game.state_count = 0;
    game.state_in_batch = 0;
    albw_lighting_release();
    lut_epoch++;
}

// ---------------------------------------------------------------------------------------------
// Frames in between.
//
// The game draws 60 frames a second, and a display can show more. The frames between are drawn
// here from the real geometry: every frame the game draws is recorded -- each draw's shaded
// vertices and state, each fill and display transfer, in order -- and a frame part of the way from
// one recorded frame to the next is the later frame drawn again with every vertex moved back
// towards where the earlier frame had it.
//
// Which draw of the earlier frame is the same draw is told by what it draws: its shader, its
// vertex buffers, its textures and its target, and its place among the draws that share all of
// those. A draw whose vertex count changed, or whose vertices jumped across the screen, is not
// moved -- that is an object that appeared, or a cut. A draw with no counterpart is drawn where it
// is.
//
// The replay draws into the shadow (albw_kernel.h), never into anything the game can see, and
// reads only what the record holds plus the textures as they are now; so it can run on another
// thread while the game goes on. Played back unmoved, it must give exactly the frame the game drew,
// which ALBW_REPLAY_CHECK=1 checks every frame.

enum { EV_DRAW, EV_FILL, EV_TRANSFER, EV_TEXCOPY };
struct rec_event {
    uint32_t kind, a, b, c, d, e;    // a draw: its index; a fill: start, end, value, step;
};                                   // a transfer: source, destination, their sizes, its flags

struct rec_draw {
    uint64_t key;                    // what it draws
    uint64_t loose;                  // the same without where its vertices are in memory
    float cx, cy;                    // where, roughly: its vertices' mean on screen, -1 to 1
    int placed;                      // cx and cy mean something: some vertex is in front of the eye
    int32_t link;                    // the same draw in the record before, or -1
    uint32_t state;
    uint32_t first_vertex, vertices; // its distinct shaded vertices
    uint32_t first_corner, corners;  // its triangles, three corners each, as indices into those
};

#define REC_LUTS ALBW_LIGHTING_LUTS
struct frame_rec {
    unsigned long frame;
    struct rec_event *ev;
    uint32_t ev_count, ev_room;
    struct rec_draw *draw;
    uint32_t draw_count, draw_room;
    struct draw_state *state;
    uint32_t state_count, state_room;
    struct vertex *vert;
    uint32_t vert_count, vert_room;
    uint32_t *corner;
    uint32_t corner_count, corner_room;
    // Copies of the lighting tables the states point at; each one 256 words, never moved.
    uint32_t **lut;
    uint32_t lut_count, lut_room;
    // Per table, the last one copied, what it was copied from and when, so that a table the
    // states keep pointing at is copied once and not once a draw.
    const uint32_t *lut_from[REC_LUTS];
    uint32_t *lut_last[REC_LUTS];
    uint32_t lut_when[REC_LUTS];
    unsigned long linked_to;         // the frame whose record `link` points into; 0 for none
    struct albw_screen_source screen[2];
    int screen_ok[2];                // the frame drew that screen, so a replay can show it
    struct { uint32_t ev, draw, state, vert, corner; } mark;
};

// Records go round: one being recorded, the last two finished, and those the window still draws
// from; when the window falls behind and none is free, a frame goes unrecorded.
#define RECS 6
static struct frame_rec recs[RECS];
static int rec_cur = -1, rec_latest = -1, rec_prev = -1;
static int rec_busy[RECS];                               // the window is drawing from it
static int recording = -1;                               // decided at the first frame
static volatile int recording_wanted;                    // the window's wish, taken at frame ends
static int replay_checking, replay_halving;
static pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
// Whoever replays -- the window, or the check on the game's thread -- holds this: there is one
// shadow and one batch to draw it with.
static pthread_mutex_t replay_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct batch replay_batch;
static int held_prev = -1, held_cur = -1;                // the pair the window draws from

static int grow(void **p, uint32_t *room, uint32_t need, size_t size) {
    if (need <= *room) return 1;
    uint32_t r = *room ? *room : 64u;
    while (r < need) r *= 2u;
    void *bigger = realloc(*p, (size_t)r * size);
    if (!bigger) return 0;
    *p = bigger;
    *room = r;
    return 1;
}
#define GROW(arr, room, need) grow((void **)&(arr), &(room), (need), sizeof *(arr))

static void rec_reset(struct frame_rec *r) {
    r->ev_count = r->draw_count = r->state_count = r->vert_count = r->corner_count = 0;
    r->lut_count = 0;                                    // the tables stay allocated, for reuse
    memset(r->lut_from, 0, sizeof r->lut_from);
    memset(r->lut_last, 0, sizeof r->lut_last);
    memset(&r->mark, 0, sizeof r->mark);
}

static void recording_decide(void) {
    if (recording >= 0) return;
    replay_checking = albw_env("ALBW_REPLAY_CHECK") != NULL;
    replay_halving = albw_env("ALBW_REPLAY_HALF") != NULL || albw_env("ALBW_FSR3_TEST") != NULL
                  || albw_env("ALBW_REC_DUMP") != NULL || albw_env("ALBW_GPU_TEST") != NULL
                  || albw_env("ALBW_GPU_FSR3_TEST") != NULL;
    recording = replay_checking || replay_halving || recording_wanted;
}

// A record nothing else holds, for the next frame. Under rec_mutex.
static int rec_free(void) {
    for (int i = 0; i < RECS; i++) {
        if (i != rec_latest && i != rec_prev && !rec_busy[i]) return i;
    }
    return -1;
}

static struct frame_rec *rec_now(void) {
    recording_decide();
    if (!recording || rec_cur < 0) return NULL;
    return &recs[rec_cur];
}

void albw_raster_set_recording(int on) { recording_wanted = on; }

// At a frame's end, before anything is recorded for the next: the window's wish taken, and a
// record found to hold the next frame. A record is only ever begun here, so one never holds
// part of a frame.
static void rec_next_frame(void) {
    recording_decide();
    int want = recording_wanted || replay_checking || replay_halving;
    pthread_mutex_lock(&rec_mutex);
    if (want != recording) {
        recording = want;
        rec_latest = rec_prev = -1;                      // what was recorded before is stale
    }
    rec_cur = recording ? rec_free() : -1;
    pthread_mutex_unlock(&rec_mutex);
    if (rec_cur >= 0) rec_reset(&recs[rec_cur]);
}

static void rec_event(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    struct frame_rec *r = rec_now();
    if (!r || !GROW(r->ev, r->ev_room, r->ev_count + 1u)) return;
    r->ev[r->ev_count++] = (struct rec_event){ kind, a, b, c, d, e };
}

void albw_raster_note_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t step) {
    rec_event(EV_FILL, start, end, value, step, 0);
}

void albw_raster_note_transfer(uint32_t src, uint32_t dst, uint32_t src_dim, uint32_t dst_dim,
                               uint32_t flags) {
    rec_event(EV_TRANSFER, src, dst, src_dim, dst_dim, flags);
}

void albw_raster_note_texture_copy(uint32_t src, uint32_t dst, uint32_t size, uint32_t in_dim,
                                   uint32_t out_dim) {
    rec_event(EV_TEXCOPY, src, dst, size, in_dim, out_dim);
}

static void rec_mark(void) {
    struct frame_rec *r = rec_now();
    if (!r) return;
    r->mark.ev = r->ev_count;
    r->mark.draw = r->draw_count;
    r->mark.state = r->state_count;
    r->mark.vert = r->vert_count;
    r->mark.corner = r->corner_count;
}

static void rec_back_to_mark(void) {
    struct frame_rec *r = rec_now();
    if (!r || r->mark.ev > r->ev_count) return;
    r->ev_count = r->mark.ev;
    r->draw_count = r->mark.draw;
    r->state_count = r->mark.state;
    r->vert_count = r->mark.vert;
    r->corner_count = r->mark.corner;
}

// A lighting table as the state points at it, as a copy the record owns.
static const uint32_t *rec_lut(struct frame_rec *r, int t, const uint32_t *table) {
    if (!table) return NULL;
    if (r->lut_from[t] == table && r->lut_when[t] == lut_epoch) return r->lut_last[t];
    // Released since, or another table: the same contents are still the same copy.
    if (r->lut_last[t] && !memcmp(r->lut_last[t], table, 256 * sizeof *table)) {
        r->lut_from[t] = table;
        r->lut_when[t] = lut_epoch;
        return r->lut_last[t];
    }
    if (r->lut_count == r->lut_room) {
        if (!GROW(r->lut, r->lut_room, r->lut_count + 1u)) return table;
        for (uint32_t i = r->lut_count; i < r->lut_room; i++) r->lut[i] = NULL;
    }
    uint32_t **slot = &r->lut[r->lut_count];
    if (!*slot) *slot = malloc(256 * sizeof **slot);
    if (!*slot) return table;
    r->lut_count++;
    memcpy(*slot, table, 256 * sizeof **slot);
    r->lut_from[t] = table;
    r->lut_when[t] = lut_epoch;
    r->lut_last[t] = *slot;
    return *slot;
}

static uint64_t fnv64(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

// One draw, after its vertices were shaded: `shaded` holds its distinct vertices, and its n-th
// vertex is shaded[slot_of[n]], or shaded[n] when slot_of is NULL.
static void rec_draw(const struct vertex *shaded, uint32_t distinct, const uint32_t *slot_of,
                     uint32_t count, uint32_t topology, uint32_t entry) {
    struct frame_rec *r = rec_now();
    if (!r) return;
    uint32_t triangles = count >= 3 ? (topology == 0 || topology == 3 ? count / 3u : count - 2u) : 0;
    if (!triangles) return;
    if (!GROW(r->draw, r->draw_room, r->draw_count + 1u)
            || !GROW(r->state, r->state_room, r->state_count + 1u)
            || !GROW(r->vert, r->vert_room, r->vert_count + distinct)
            || !GROW(r->corner, r->corner_room, r->corner_count + triangles * 3u)
            || !GROW(r->ev, r->ev_room, r->ev_count + 1u)) {
        return;
    }
    struct draw_state *S = &r->state[r->state_count];
    *S = ds;
    for (int t = 0; t < REC_LUTS; t++) {
        S->light.lut[t] = S->lighting ? rec_lut(r, t, ds.light.lut[t]) : NULL;
    }

    uint64_t loose = 0xCBF29CE484222325ull;
    loose = fnv64(loose, entry);
    loose = fnv64(loose, count);
    loose = fnv64(loose, topology);
    for (int u = 0; u < 3; u++) loose = fnv64(loose, ds.tex[u].enabled ? ds.tex[u].base : 0u);
    loose = fnv64(loose, ds.color_base);
    uint64_t key = loose;
    for (uint32_t i = 0x200u; i <= 0x227u; i++) key = fnv64(key, reg(i));   // the vertex arrays

    struct rec_draw *d = &r->draw[r->draw_count];
    d->key = key;
    d->loose = loose;
    d->link = -1;
    float sx = 0.0f, sy = 0.0f;
    uint32_t seen = 0;
    for (uint32_t k = 0; k < distinct; k++) {
        const float *pos = shaded[k].pos;
        if (!(pos[3] > 1e-6f)) continue;
        sx += pos[0] / pos[3];
        sy += pos[1] / pos[3];
        seen++;
    }
    d->placed = seen > 0;
    d->cx = seen ? sx / (float)seen : 0.0f;
    d->cy = seen ? sy / (float)seen : 0.0f;
    d->state = r->state_count;
    d->first_vertex = r->vert_count;
    d->vertices = distinct;
    d->first_corner = r->corner_count;
    memcpy(r->vert + r->vert_count, shaded, distinct * sizeof *shaded);
    uint32_t *c = r->corner + r->corner_count;
#define SLOT(n) (slot_of ? slot_of[n] : (n))
    // The corners as the draw walks them, as albw_raster_draw makes its triangles.
    if (topology == 1) {
        for (uint32_t n = 2; n < count; n++) {
            *c++ = SLOT(n - 2);
            *c++ = SLOT((n & 1u) ? n : n - 1);
            *c++ = SLOT((n & 1u) ? n - 1 : n);
        }
    } else if (topology == 2) {
        for (uint32_t n = 2; n < count; n++) {
            *c++ = SLOT(0);
            *c++ = SLOT(n - 1);
            *c++ = SLOT(n);
        }
    } else {
        for (uint32_t n = 0; n + 2 < count; n += 3) {
            *c++ = SLOT(n);
            *c++ = SLOT(n + 1);
            *c++ = SLOT(n + 2);
        }
    }
#undef SLOT
    d->corners = (uint32_t)(c - (r->corner + r->corner_count));
    r->corner_count += d->corners;
    r->vert_count += distinct;
    r->state_count++;
    r->ev[r->ev_count++] = (struct rec_event){ EV_DRAW, r->draw_count, 0, 0, 0, 0 };
    r->draw_count++;
}

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

// Which draw of the record before each draw is. Draws with the same key are the same mesh drawn
// with the same textures into the same target -- often many of them, a bush or a fence post again
// and again -- and which of those is which goes by where they are: each takes the nearest of the
// earlier ones not yet taken, within a third of the view. Going by their order instead went wrong
// as soon as one came into view or went out of it, and the rest all slipped by one.
//
// A draw whose vertices are written anew each frame -- into a buffer that is somewhere else each
// time -- has no counterpart by that key; a second pass looks for one by the loose key, which is
// the same without the vertex arrays' addresses.
static const struct frame_rec *link_from;
static int link_loose;
static uint64_t link_key(const struct rec_draw *d) { return link_loose ? d->loose : d->key; }
static int link_order(const void *a, const void *b) {
    uint64_t ka = link_key(&link_from->draw[*(const uint32_t *)a]);
    uint64_t kb = link_key(&link_from->draw[*(const uint32_t *)b]);
    if (ka != kb) return ka < kb ? -1 : 1;
    return *(const uint32_t *)a < *(const uint32_t *)b ? -1 : 1;
}

static void rec_link(const struct frame_rec *prev, struct frame_rec *cur) {
    cur->linked_to = 0;
    for (uint32_t i = 0; i < cur->draw_count; i++) cur->draw[i].link = -1;
    if (!prev || !prev->draw_count) return;
    static uint32_t *order, order_room;
    static uint8_t *taken;
    static uint32_t taken_room;
    if (!GROW(order, order_room, prev->draw_count) || !GROW(taken, taken_room, prev->draw_count)) return;
    memset(taken, 0, prev->draw_count);
    link_from = prev;
    for (link_loose = 0; link_loose < 2; link_loose++) {
    for (uint32_t i = 0; i < prev->draw_count; i++) order[i] = i;
    qsort(order, prev->draw_count, sizeof *order, link_order);
    for (uint32_t i = 0; i < cur->draw_count; i++) {
        struct rec_draw *d = &cur->draw[i];
        if (d->link >= 0) continue;
        // The first of the earlier draws with this key.
        uint64_t want = link_key(d);
        uint32_t lo = 0, hi = prev->draw_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2u;
            if (link_key(&prev->draw[order[mid]]) < want) lo = mid + 1u;
            else hi = mid;
        }
        int32_t best = -1;
        float best_d = 0.0f;
        for (uint32_t k = lo; k < prev->draw_count && link_key(&prev->draw[order[k]]) == want; k++) {
            uint32_t j = order[k];
            const struct rec_draw *p = &prev->draw[j];
            if (taken[j] || p->vertices != d->vertices) continue;
            float dist = 0.0f;
            if (p->placed && d->placed) {
                float dx = p->cx - d->cx, dy = p->cy - d->cy;
                dist = dx * dx + dy * dy;
                if (dist > 0.67f * 0.67f) continue;
            }
            if (best < 0 || dist < best_d) {
                best = (int32_t)j;
                best_d = dist;
            }
        }
        if (best >= 0) {
            taken[best] = 1;
            d->link = best;
        }
    }
    }
    cur->linked_to = prev->frame;
}

// The draw of `prev` that `d` of `cur` is, if `cur` was linked to `prev`.
static const struct rec_draw *rec_linked(const struct frame_rec *prev, const struct frame_rec *cur,
                                         const struct rec_draw *d) {
    if (!prev || !cur->linked_to || cur->linked_to != prev->frame || d->link < 0) return NULL;
    return &prev->draw[d->link];
}

// The shadow's version of a draw state: its targets moved into the shadow. Returns 0 for a target
// outside what the shadow mirrors, which a replay then leaves undrawn.
static int shadow_state(struct draw_state *S) {
    if (!albw_shadowed(S->color_base)) return 0;
    S->color_base = albw_shadow(S->color_base);
    if (S->depth_base) {
        if (albw_shadowed(S->depth_base)) S->depth_base = albw_shadow(S->depth_base);
        else S->depth_base = 0;
    }
    return 1;
}

// The texture cache is left as it is: the game's own flushes keep it fresh, and a frame in
// between that read a texture a moment stale would not show it. Flushing it here, as the game's
// batches do, threw the game's decoded textures away dozens of times a frame.
static double replay_raster_s, replay_setup_s;
static void replay_flush(void) {
    double t0 = probe_now();
    draw_batch(&replay_batch);
    replay_raster_s += probe_now() - t0;
    replay_batch.prep_count = 0;
    replay_batch.state_count = 0;
    replay_batch.state_in_batch = 0;
}

// Whether a draw's vertices can be moved between where two frames have them: the same number,
// none behind the eye in either, none jumping more than a quarter of the view's width or height.
static int movable(const struct vertex *a, const struct vertex *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float wa = a[i].pos[3], wb = b[i].pos[3];
        if (!(wa > 0.0f) || !(wb > 0.0f)) return 0;
        float dx = a[i].pos[0] / wa - b[i].pos[0] / wb, dy = a[i].pos[1] / wa - b[i].pos[1] / wb;
        if (dx > 0.5f || dx < -0.5f || dy > 0.5f || dy < -0.5f) return 0;
    }
    return 1;
}

// Draws `cur` into the shadow, every vertex of a draw `prev` has too moved `1 - t` of the way back
// to where `prev` had it: t = 1 is `cur` itself.
// A replay for a temporal upscaler (FSR 3, DLSS) draws the top screen moved by a sub-pixel jitter,
// writes each pixel's motion back to the frame before, and stops at the top screen's transfer,
// leaving its depth in the shadow; the rest of the frame is not needed.
struct capture {
    float jitter[2];                                     // the viewport's offset, target pixels
    uint32_t top_src, top_flags;                         // the top screen's transfer: source, flags
    float *motion;                                       // target layout, two floats a pixel
    uint8_t *reactive;                                   // target layout, a byte a pixel
    size_t motion_room;
    uint32_t width, height;                              // the top target
    uint32_t depth_base, depth_bytes, depth_max, depth_func;
    uint32_t moving, jumping;                            // the top target's draws, matched
    float scene[2];                                      // the typical move, earlier less now (NDC)
    int done;
};

static int float_order(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void replay_to(const struct frame_rec *prev, const struct frame_rec *cur, float t,
                      struct capture *cap);
static void replay(const struct frame_rec *prev, const struct frame_rec *cur, float t) {
    replay_to(prev, cur, t, NULL);
}

static void replay_to(const struct frame_rec *prev, const struct frame_rec *cur, float t,
                      struct capture *cap) {
    static struct vertex *moved;
    static uint32_t moved_room;
    if (cap) {
        cap->done = 0;
        cap->top_src = 0;
        cap->moving = cap->jumping = 0;
        for (uint32_t i = 0; i < cur->ev_count && !cap->top_src; i++) {
            const struct rec_event *e = &cur->ev[i];
            if (e->kind == EV_TRANSFER && (e->d >> 16) == 400u && (e->d & 0xFFFFu) == 240u) {
                cap->top_src = e->a;
                cap->top_flags = e->e;
            }
        }
        if (!cap->top_src) return;
        // How the scene moved, for draws with no counterpart to say it themselves -- what the game
        // builds anew each frame, several small things in one draw whose count changes: the
        // median move of the draws that have one. Static scenery drawn that way moves with the
        // camera, and that is the camera.
        // Only the scenery counts -- what tests and writes depth; the interface, which sits still
        // on the screen whatever the camera does, would pull it to nothing -- and each draw by its
        // size in vertices.
        static float (*moves)[2];
        static uint32_t moves_room;
        uint32_t n = 0, total = 0;
        cap->scene[0] = cap->scene[1] = 0.0f;
        if (prev && GROW(moves, moves_room, cur->draw_count)) {
            for (uint32_t i = 0; i < cur->draw_count; i++) {
                const struct rec_draw *d = &cur->draw[i], *p = rec_linked(prev, cur, d);
                const struct draw_state *S = &cur->state[d->state];
                if (!p || !p->placed || !d->placed || S->color_base != cap->top_src
                        || !S->depth_test || !S->depth_write) {
                    continue;
                }
                moves[n][0] = p->cx - d->cx;
                moves[n][1] = d->vertices;
                n++;
                total += d->vertices;
            }
            for (int axis = 0; axis < 2 && n; axis++) {
                if (axis == 1) {
                    n = 0;
                    for (uint32_t i = 0; i < cur->draw_count; i++) {
                        const struct rec_draw *d = &cur->draw[i], *p = rec_linked(prev, cur, d);
                        const struct draw_state *S = &cur->state[d->state];
                        if (!p || !p->placed || !d->placed || S->color_base != cap->top_src
                                || !S->depth_test || !S->depth_write) {
                            continue;
                        }
                        moves[n][0] = p->cy - d->cy;
                        moves[n][1] = d->vertices;
                        n++;
                    }
                }
                qsort(moves, n, sizeof *moves, float_order);  // by the move; the weight follows
                uint32_t half = total / 2u, sum = 0;
                for (uint32_t i = 0; i < n; i++) {
                    sum += (uint32_t)moves[i][1];
                    if (sum >= half) {
                        cap->scene[axis] = moves[i][0];
                        break;
                    }
                }
            }
        }
    }
    // A cut -- most of what is in both frames jumped -- is not a movement to show the middle of:
    // the later frame is drawn as it is.
    if (prev && t < 1.0f) {
        uint32_t smooth = 0, jumped = 0;
        for (uint32_t i = 0; i < cur->draw_count; i++) {
            const struct rec_draw *d = &cur->draw[i], *p = rec_linked(prev, cur, d);
            if (!p || p->vertices != d->vertices) continue;
            if (movable(prev->vert + p->first_vertex, cur->vert + d->first_vertex, d->vertices)) smooth++;
            else jumped++;
        }
        if (jumped > smooth) t = 1.0f;
    }
    for (uint32_t i = 0; i < cur->ev_count; i++) {
        const struct rec_event *e = &cur->ev[i];
        if (e->kind == EV_FILL) {
            replay_flush();
            uint32_t start = e->a, end = e->b, value = e->c, step = e->d;
            if (!albw_shadowed(start) || !albw_shadowed(end - 1u)) continue;
            uint8_t *mem = albw_mem + albw_shadow(start);
            uint32_t len = (end - start) / step * step;
            uint8_t pattern[4];
            for (uint32_t k = 0; k < 4; k++) pattern[k] = (uint8_t)(value >> (k * 8));
            if (step == 4) {
                for (uint32_t at = 0; at < len; at += 4) memcpy(mem + at, &value, 4);
            } else if (step == 2) {
                uint16_t v = (uint16_t)value;
                for (uint32_t at = 0; at < len; at += 2) memcpy(mem + at, &v, 2);
            } else {
                for (uint32_t at = 0; at < len; at += 3) memcpy(mem + at, pattern, 3);
            }
            continue;
        }
        if (e->kind == EV_TEXCOPY) continue;             // its draws read the game's own copy
        if (e->kind == EV_TRANSFER) {
            replay_flush();
            uint32_t in_bytes = (e->c & 0xFFFFu) * (e->c >> 16) * albw_pixel_bytes((e->e >> 8) & 7u);
            uint32_t out_bytes = (e->d & 0xFFFFu) * (e->d >> 16) * albw_pixel_bytes((e->e >> 12) & 7u);
            if (!in_bytes || !out_bytes || !albw_shadowed(e->a) || !albw_shadowed(e->a + in_bytes - 1u)
                    || !albw_shadowed(e->b) || !albw_shadowed(e->b + out_bytes - 1u)) {
                continue;
            }
            albw_transfer_copy(albw_shadow(e->a), albw_shadow(e->b), e->c, e->d, e->e);
            if (cap && e->a == cap->top_src && (e->d >> 16) == 400u) {
                cap->done = 1;                           // the top screen is all it wants
                replay_batch.motion = NULL;
                return;
            }
            continue;
        }
        const struct rec_draw *d = &cur->draw[e->a];
        struct draw_state S = cur->state[d->state];
        int top = cap && S.color_base == cap->top_src;
        if (!shadow_state(&S)) continue;
        if (top) {
            // The jitter, as a move of the viewport; and where the depth is, for afterwards.
            S.offset_x += cap->jitter[0];
            S.offset_y += cap->jitter[1];
            cap->width = S.width;
            cap->height = S.height_m1 + 1u;
            cap->depth_base = S.depth_base;
            cap->depth_bytes = S.depth_bytes;
            cap->depth_max = S.depth_max;
            if (S.depth_test) cap->depth_func = S.depth_func;
            size_t need = (size_t)cap->width * cap->height * 2u;
            if (need > cap->motion_room) {
                float *bigger = realloc(cap->motion, need * sizeof *bigger);
                if (!bigger) continue;
                cap->motion = bigger;
                uint8_t *more = realloc(cap->reactive, need / 2u);
                if (!more) continue;
                cap->reactive = more;
                cap->motion_room = need;
                memset(cap->motion, 0, need * sizeof *bigger);
                memset(cap->reactive, 0, need / 2u);
            }
        }
        if (replay_batch.prep_count && (!same_target(&S, &replay_batch.states[0])
                                        || replay_batch.motion != (top ? cap->motion : NULL))) {
            replay_flush();
        }
        replay_batch.state_in_batch = 0;
        replay_batch.motion = top ? cap->motion : NULL;
        replay_batch.reactive = top ? cap->reactive : NULL;
        if (top) {
            replay_batch.jitter[0] = cap->jitter[0];
            replay_batch.jitter[1] = cap->jitter[1];
        }
        const struct vertex *v = cur->vert + d->first_vertex;
        const struct rec_draw *p = prev && (t < 1.0f || top) ? rec_linked(prev, cur, d) : NULL;
        int moves = p && p->vertices == d->vertices && GROW(moved, moved_room, d->vertices)
                 && movable(prev->vert + p->first_vertex, v, d->vertices);
        if (top && p) {
            if (moves) cap->moving++;
            else cap->jumping++;
        }
        if (top && albw_env("ALBW_FSR3_DEBUG")) {
            printf("[fsr3 ] draw %u: %u vertices, link %d (%s), centre %.3f %.3f%s, depth write %d, "
                   "test %d func %u, blend %d\n", e->a, d->vertices, d->link, p ? (moves ? "moves" : "jumps") : "none",
                   d->cx, d->cy, d->placed ? "" : " (unplaced)", S.depth_write, S.depth_test, S.depth_func, S.blend);
        }
        if (moves || top) {
            // Moved part of the way, and, for an upscaler, knowing where it was.
            if (!GROW(moved, moved_room, d->vertices)) continue;
            const struct vertex *from = moves ? prev->vert + p->first_vertex : v;
            for (uint32_t k = 0; k < d->vertices; k++) {
                if (moves && t < 1.0f) lerp_vertex(&from[k], &v[k], t, &moved[k]);
                else moved[k] = v[k];
                memcpy(moved[k].prev, top ? from[k].pos : moved[k].pos, sizeof moved[k].prev);
                if (top && !moves) {
                    // No counterpart: where the scene's move would have had it.
                    moved[k].prev[0] += cap->scene[0] * moved[k].prev[3];
                    moved[k].prev[1] += cap->scene[1] * moved[k].prev[3];
                }
            }
            v = moved;
        }
        const uint32_t *c = cur->corner + d->first_corner;
        double ts = probe_now();
        for (uint32_t k = 0; k + 2 < d->corners; k += 3) {
            struct vertex tri[3] = { v[c[k]], v[c[k + 1]], v[c[k + 2]] };
            draw_triangle(&replay_batch, &S, tri);
        }
        replay_setup_s += probe_now() - ts;
    }
    replay_flush();
    replay_batch.motion = NULL;
}

// The frame as the replay drew it, one screen as rows of RGB. Returns 0 where the record did not
// draw that screen.
static int replay_screen(const struct frame_rec *r, int screen, uint8_t *rgb) {
    if (!r->screen_ok[screen]) return 0;
    albw_screen_read(&r->screen[screen], ALBW_SHADOW_BASE - ALBW_SHADOW_FROM, screen, rgb);
    return 1;
}

// Every frame recorded and played back unmoved, compared with what the game drew.
static void replay_check(uint32_t shared_addr, uint32_t thread_index, const struct frame_rec *r) {
    static uint8_t real[400 * 240 * 3], again[400 * 240 * 3];
    static unsigned long frames, differing;
    (void)shared_addr;
    (void)thread_index;
    static double spent;
    static unsigned long replays;
    double t0 = probe_now();
    replay(NULL, r, 1.0f);
    spent += probe_now() - t0;
    if (++replays % 100u == 0u && albw_env("ALBW_REPLAY_VERBOSE")) {
        printf("[replay] %.2f ms a replay, over the last 100\n", spent * 10.0);
        spent = 0;
    }
    for (int screen = 0; screen < 2; screen++) {
        uint32_t width = screen ? 320u : 400u;
        // The game's own picture of this frame is in the framebuffer the record wrote it to.
        if (!r->screen_ok[screen]) continue;
        albw_screen_read(&r->screen[screen], 0, screen, real);
        if (!replay_screen(r, screen, again)) continue;
        size_t n = (size_t)width * 240u * 3u, off = 0;
        for (size_t k = 0; k < n; k++) off += real[k] != again[k];
        frames++;
        if (off) {
            differing++;
            if (differing <= 20) {
                printf("[replay] frame %lu %s screen: %zu of %zu bytes differ from the game's\n",
                       r->frame, screen ? "bottom" : "top", off, n);
            }
        }
    }
    if (frames && frames % 600u == 0u) {
        printf("[replay] %lu screens played back, %lu differing\n", frames, differing);
    }
    if (albw_env("ALBW_REPLAY_VERBOSE")) {
        printf("[replay] frame %lu: %u draws, %u vertices, %u events, %u lighting tables; "
               "%lu screens so far, %lu differing\n", r->frame, r->draw_count, r->vert_count,
               r->ev_count, r->lut_count, frames, differing);
    }
}

// ALBW_REPLAY_HALF=<frame> writes, into ALBW_REPLAY_DIR, that frame's top screen and the one
// before it as the replay draws them, and three frames between: a way to look at what the
// matching makes of a scene without a window.
static void write_ppm(const char *dir, unsigned long frame, const char *what, const uint8_t *rgb) {
    char path[600];
    snprintf(path, sizeof path, "%s/replay-%06lu-%s.ppm", dir, frame, what);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n400 240\n255\n");
    fwrite(rgb, 3, 400u * 240u, f);
    fclose(f);
}

static void replay_half(const struct frame_rec *prev, const struct frame_rec *cur) {
    const char *dir = albw_env("ALBW_REPLAY_DIR");
    if (!dir) dir = ".";
    static uint8_t rgb[400 * 240 * 3];
    replay(NULL, prev, 1.0f);
    if (replay_screen(prev, 0, rgb)) write_ppm(dir, cur->frame, "0-before", rgb);
    static const float at[3] = { 0.25f, 0.5f, 0.75f };
    static const char *const names[3] = { "1-quarter", "2-half", "3-three-quarters" };
    for (int i = 0; i < 3; i++) {
        replay(prev, cur, at[i]);
        if (replay_screen(cur, 0, rgb)) write_ppm(dir, cur->frame, names[i], rgb);
    }
    replay(NULL, cur, 1.0f);
    if (replay_screen(cur, 0, rgb)) write_ppm(dir, cur->frame, "4-after", rgb);
    // How much of it moved: draws matched, and of those, movable.
    uint32_t matched = 0, moved = 0;
    for (uint32_t i = 0; i < cur->draw_count; i++) {
        const struct rec_draw *d = &cur->draw[i], *p = rec_linked(prev, cur, d);
        if (!p) continue;
        matched++;
        if (p->vertices == d->vertices
                && movable(prev->vert + p->first_vertex, cur->vert + d->first_vertex, d->vertices)) {
            moved++;
        }
    }
    printf("[replay] frame %lu: %u draws, %u with a counterpart in the frame before, %u of them "
           "moved\n", cur->frame, cur->draw_count, matched, moved);

}

#ifdef ALBW_HAVE_FSR3
#include "albw_vkup.h"
// ALBW_FSR3_TEST=<from>:<count>[:<scale>] runs FSR 3 headless over that many frames from that one,
// the way the window would, and writes into ALBW_REPLAY_DIR what went in -- the jittered picture,
// its depth and motion -- and what came out: a way to judge the upscaler's input without a window.
static void write_rgb(const char *dir, unsigned long frame, const char *what, const uint8_t *rgb,
                      int w, int h) {
    char path[600];
    snprintf(path, sizeof path, "%s/fsr3-%06lu-%s.ppm", dir, frame, what);
    FILE *out = fopen(path, "wb");
    if (!out) return;
    fprintf(out, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, out);
    fclose(out);
}

static void fsr3_test(unsigned long frame, int published, int before) {
    static long from = -2, count, scale;
    if (from == -2) {
        const char *spec = albw_env("ALBW_FSR3_TEST");
        from = -1;
        if (spec && sscanf(spec, "%ld:%ld:%ld", &from, &count, &scale) < 2) from = -1;
        if (scale < 1) scale = 3;
    }
    if (from < 0 || (long)frame < from || (long)frame >= from + count || before < 0) return;
    const char *dir = albw_env("ALBW_REPLAY_DIR");
    if (!dir) dir = ".";
    static struct albw_upscale_frame f;
    static uint32_t index;
    int ow = 400 * (int)scale, oh = 240 * (int)scale;
    albw_vkup_jitter(index++, 400, ow, &f.jitter_x, &f.jitter_y);
    held_prev = before;
    held_cur = published;
    int ok = albw_raster_between_upscale(&f);
    held_prev = held_cur = -1;
    if (!ok) {
        printf("[fsr3 ] frame %lu: no upscaler input\n", frame);
        return;
    }
    static uint32_t *out;
    static uint8_t *rgb;
    if (!out) out = malloc((size_t)ow * oh * 4u);
    if (!rgb) rgb = malloc((size_t)ow * oh * 3u);
    struct albw_vkup_frame in = { 400, 240, f.rgb, NULL, f.depth, f.motion, f.reactive, f.jitter_x, f.jitter_y,
                                  (long)frame == from || f.reset, 0.5f, 16.7f, 1.0f, 1000.0f, 0.6f };
    if (!out || !rgb || !albw_fsr3_upscale(&in, out, ow, oh, ow * 4)) return;
    for (size_t i = 0; i < (size_t)ow * oh; i++) {
        rgb[i * 3] = (uint8_t)(out[i] >> 16);
        rgb[i * 3 + 1] = (uint8_t)(out[i] >> 8);
        rgb[i * 3 + 2] = (uint8_t)out[i];
    }
    write_rgb(dir, frame, "out", rgb, ow, oh);
    write_rgb(dir, frame, "in", f.rgb, 400, 240);
    static uint8_t vis[400 * 240 * 3];
    float far = 0.0f, near = 1.0f, most = 0.0f;
    for (int i = 0; i < 400 * 240; i++) {
        if (f.depth[i] < near) near = f.depth[i];
        if (f.depth[i] > far) far = f.depth[i];
        float m = fabsf(f.motion[i * 2]) + fabsf(f.motion[i * 2 + 1]);
        if (m > most) most = m;
    }
    for (int i = 0; i < 400 * 240; i++) {
        float d = far > near ? (f.depth[i] - near) / (far - near) : 0.0f;
        vis[i * 3] = vis[i * 3 + 1] = vis[i * 3 + 2] = (uint8_t)(d * 255.0f);
    }
    write_rgb(dir, frame, "depth", vis, 400, 240);
    for (int i = 0; i < 400 * 240; i++) {
        float mx = f.motion[i * 2], my = f.motion[i * 2 + 1];
        vis[i * 3] = (uint8_t)fminf(255.0f, fmaxf(0.0f, 128.0f + mx * 32.0f));
        vis[i * 3 + 1] = (uint8_t)fminf(255.0f, fmaxf(0.0f, 128.0f + my * 32.0f));
        vis[i * 3 + 2] = 128;
    }
    write_rgb(dir, frame, "motion", vis, 400, 240);
    printf("[fsr3 ] frame %lu: jitter %.3f %.3f, depth %.4f to %.4f, motion up to %.2f px, "
           "reset %d\n", frame, f.jitter_x, f.jitter_y, near, far, most, in.reset);
}
#endif

// ALBW_REC_DUMP=<frame>: what that frame's record holds, event by event -- fills, transfers, texture
// copies and draws with their targets, viewports and states -- for working out how a frame is made.
static void rec_dump(unsigned long frame, const struct frame_rec *r) {
    static long at = -2;
    if (at == -2) {
        const char *e = albw_env("ALBW_REC_DUMP");
        at = e ? strtol(e, NULL, 0) : -1;
    }
    if (at < 0 || (long)frame != at) return;
    printf("[rec  ] frame %lu: %u events, %u draws, %u vertices, %u corners, %u tables\n", frame,
           r->ev_count, r->draw_count, r->vert_count, r->corner_count, r->lut_count);
    for (uint32_t i = 0; i < r->ev_count; i++) {
        const struct rec_event *e = &r->ev[i];
        if (e->kind == EV_FILL) {
            printf("[rec  ] %4u fill %08x..%08x value %08x step %u\n", i, e->a, e->b, e->c, e->d);
        } else if (e->kind == EV_TRANSFER) {
            printf("[rec  ] %4u transfer %08x %ux%u -> %08x %ux%u flags %08x\n", i, e->a, e->c & 0xFFFFu,
                   e->c >> 16, e->b, e->d & 0xFFFFu, e->d >> 16, e->e);
        } else if (e->kind == EV_TEXCOPY) {
            printf("[rec  ] %4u texture copy %08x -> %08x, %u bytes, in %08x out %08x\n", i, e->a, e->b,
                   e->c, e->d, e->e);
        } else {
            const struct rec_draw *d = &r->draw[e->a];
            const struct draw_state *S = &r->state[d->state];
            printf("[rec  ] %4u draw %u: %u vertices %u corners into %08x %ux%u depth %08x, viewport "
                   "%g %g +%g %g, depth %g %g%s, scissor %d %d %d %d, test %d/%u write %d, blend %d "
                   "%08x, logic %u, mask %x, stencil %d, cull %u, tex %s%s%s, lit %d\n", i, e->a,
                   d->vertices, d->corners, S->color_base, S->width, S->height_m1 + 1u, S->depth_base,
                   S->half_w, S->half_h, S->offset_x, S->offset_y, S->depth_range, S->depth_near,
                   S->wbuffer ? " w" : "", S->scissor_x0, S->scissor_y0, S->scissor_x1, S->scissor_y1,
                   S->depth_test, S->depth_func, S->depth_write, S->blend, S->blend_func, S->logic_op,
                   S->color_mask, S->stencil, S->cull, S->tex[0].enabled ? "0" : "-",
                   S->tex[1].enabled ? "1" : "-", S->tex[2].enabled ? "2" : "-", S->lighting);
        }
    }
}

void albw_raster_frame_end(uint32_t shared_addr, uint32_t thread_index, unsigned long frame) {
    struct frame_rec *r = rec_now();
    if (!r) {
        rec_next_frame();
        return;
    }
    r->frame = frame;
    // Where this frame's picture of each screen went. The framebuffers are double-buffered: what
    // the screen shows at this point is still the frame before, and this one's transfers went into
    // the other buffer, to be shown from the next frame on. So the record's own transfers say it --
    // the first that fills a whole top screen (the left eye; the right comes after it), and the
    // last that fills a bottom one -- with the format and stride the screen is shown with. A frame
    // that drew no new picture of a screen cannot be replayed for it.
    for (int screen = 0; screen < 2; screen++) {
        struct albw_screen_source shown;
        r->screen_ok[screen] = 0;
        if (!albw_screen_source(shared_addr, thread_index, screen, &shown)) continue;
        uint32_t rows = screen ? 320u : 400u;
        for (uint32_t i = 0; i < r->ev_count; i++) {
            const struct rec_event *e = &r->ev[i];
            if (e->kind != EV_TRANSFER || (e->d >> 16) != rows || (e->d & 0xFFFFu) != 240u) continue;
            r->screen[screen] = shown;
            r->screen[screen].base = e->b;
            r->screen_ok[screen] = albw_shadowed(e->b);
            if (!screen) break;
        }
    }
    rec_link(rec_latest >= 0 ? &recs[rec_latest] : NULL, r);

    pthread_mutex_lock(&rec_mutex);
    rec_prev = rec_latest;
    rec_latest = rec_cur;
    pthread_mutex_unlock(&rec_mutex);
    int published = rec_latest, before = rec_prev;
    rec_next_frame();

    static long half_at = -2;
    if (half_at == -2) {
        const char *h = albw_env("ALBW_REPLAY_HALF");
        half_at = h ? strtol(h, NULL, 0) : -1;
    }
    if (published < 0) return;                           // dropped by a change of setting
    rec_dump(frame, &recs[published]);
    pthread_mutex_lock(&replay_mutex);
    if (half_at >= 0 && (long)frame == half_at && before >= 0) {
        replay_half(&recs[before], &recs[published]);
    }
    // The records stay put while this runs: the game's thread is the one that would reuse them.
    if (replay_checking) replay_check(shared_addr, thread_index, &recs[published]);
    pthread_mutex_unlock(&replay_mutex);
#ifdef ALBW_HAVE_FSR3
    fsr3_test(frame, published, before);
#endif
#ifdef ALBW_HAVE_GPU
    albw_gpu_test(frame);
    albw_gpu_fsr3_test(frame);
#endif
}

// The window's side (held_prev and held_cur are declared with the records).

int albw_raster_between_hold(unsigned long *later) {
    pthread_mutex_lock(&rec_mutex);
    int ok = recording > 0 && rec_prev >= 0 && rec_latest >= 0;
    if (ok) {
        held_prev = rec_prev;
        held_cur = rec_latest;
        rec_busy[held_prev]++;
        rec_busy[held_cur]++;
        *later = recs[held_cur].frame;
    }
    pthread_mutex_unlock(&rec_mutex);
    return ok;
}

void albw_raster_between_draw(float t, uint8_t *top, uint8_t *bottom) {
    if (held_cur < 0) return;
    pthread_mutex_lock(&replay_mutex);
    static double total; static unsigned n;
    double t0 = probe_now();
    replay(&recs[held_prev], &recs[held_cur], t);
    total += probe_now() - t0;
    if (++n % 100 == 0 && albw_env("ALBW_REPLAY_VERBOSE")) {
        printf("[replay] %.2f ms a frame in between: setup %.2f, rasterising %.2f\n", total * 10,
               replay_setup_s * 10, replay_raster_s * 10);
        total = replay_setup_s = replay_raster_s = 0;
    }
    replay_screen(&recs[held_cur], 0, top);
    replay_screen(&recs[held_cur], 1, bottom);
    pthread_mutex_unlock(&replay_mutex);
}

// The later held frame drawn again for a temporal upscaler: moved by the jitter, with each pixel's
// motion back to the earlier held frame and the depth, all turned the way the screen shows them.
static struct capture upscale_cap;

int albw_raster_between_upscale(struct albw_upscale_frame *f) {
    if (held_cur < 0) return 0;
    const struct frame_rec *cur = &recs[held_cur], *prev = &recs[held_prev];
    if (!cur->screen_ok[0]) return 0;
    pthread_mutex_lock(&replay_mutex);
    struct capture *cap = &upscale_cap;
    // Which way the top screen's transfer turns the target: the screen's x runs along the
    // target's rows, from the last row when the transfer flips; its y runs back along the columns.
    int flip = 0;
    for (uint32_t i = 0; i < cur->ev_count; i++) {
        const struct rec_event *e = &cur->ev[i];
        if (e->kind == EV_TRANSFER && (e->d >> 16) == 400u && (e->d & 0xFFFFu) == 240u) {
            flip = (int)(e->e & 1u);
            break;
        }
    }
    cap->jitter[0] = -f->jitter_y;
    cap->jitter[1] = flip ? f->jitter_x : -f->jitter_x;
    if (cap->motion) memset(cap->motion, 0, cap->motion_room * sizeof *cap->motion);
    if (cap->reactive) memset(cap->reactive, 0, cap->motion_room / 2u);
    replay_to(prev, cur, 1.0f, cap);
    int ok = cap->done && cap->width == 240u && cap->height == 400u && cap->motion
          && albw_shadowed(cap->depth_base - ALBW_SHADOW_BASE + ALBW_SHADOW_FROM);
    if (ok) {
        albw_screen_read(&cur->screen[0], ALBW_SHADOW_BASE - ALBW_SHADOW_FROM, 0, f->rgb);
        // A test that passes the larger value keeps the nearest surface at the top of the range;
        // the upscaler wants it at 0.
        int inverted = cap->depth_func == 6u || cap->depth_func == 7u;
        float scale = 1.0f / (float)cap->depth_max;
        for (uint32_t sy = 0; sy < 240u; sy++) {
            for (uint32_t sx = 0; sx < 400u; sx++) {
                uint32_t col = 239u - sy, row = flip ? 399u - sx : sx;
                uint32_t at = cap->depth_base + tiled_offset(col, row, 240u, cap->depth_bytes);
                uint32_t raw;
                if (cap->depth_bytes == 2) {
                    uint16_t h;
                    memcpy(&h, albw_mem + at, 2);
                    raw = h;
                } else {
                    raw = (uint32_t)albw_mem[at] | ((uint32_t)albw_mem[at + 1] << 8)
                        | ((uint32_t)albw_mem[at + 2] << 16);
                }
                float d = (float)raw * scale;
                size_t i = (size_t)sy * 400u + sx;
                f->depth[i] = inverted ? 1.0f - d : d;
                const float *m = cap->motion + ((size_t)row * 240u + col) * 2u;
                f->motion[i * 2u] = flip ? m[1] : -m[1];
                f->motion[i * 2u + 1u] = -m[0];
                f->reactive[i] = cap->reactive[(size_t)row * 240u + col];
            }
        }
        f->reset = cap->jumping > cap->moving;
    }
    pthread_mutex_unlock(&replay_mutex);
    return ok;
}

void albw_raster_between_release(void) {
    pthread_mutex_lock(&rec_mutex);
    if (held_cur >= 0) {
        rec_busy[held_prev]--;
        rec_busy[held_cur]--;
    }
    held_prev = held_cur = -1;
    pthread_mutex_unlock(&rec_mutex);
}

// ---------------------------------------------------------------------------------------------
// The GPU renderer's side (albw_gpu.h): a held record turned into what the card draws.

void albw_raster_set_gpu_display(int on) { gpu_display = on; }
int albw_raster_gpu_display(void) { return gpu_display; }

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// A draw's state as words, pica_state.h's layout. `lut_index` maps a table the state points at to
// its place in the frame's table buffer.
static void gpu_state_words(const struct draw_state *S, const struct frame_rec *r, uint32_t *w) {
    memset(w, 0, ST_WORDS * sizeof *w);
    for (int st = 0; st < 6; st++) {
        const struct stage_state *g = &S->stage[st];
        uint32_t src = 0, mod = 0;
        for (int i = 0; i < 3; i++) {
            src |= (uint32_t)(g->src[i] & 15u) << (i * 4);
            src |= (uint32_t)(g->src_a[i] & 15u) << (12 + i * 4);
            mod |= (uint32_t)(g->mod_rgb[i] & 15u) << (i * 4);
            mod |= (uint32_t)(g->mod_a[i] & 15u) << (12 + i * 4);
        }
        w[ST_STAGE + st * 4 + 0] = src;
        w[ST_STAGE + st * 4 + 1] = mod;
        w[ST_STAGE + st * 4 + 2] = (g->op_rgb & 15u) | ((g->op_a & 15u) << 4)
                                 | ((g->scale_rgb & 3u) << 8) | ((g->scale_a & 3u) << 12);
        w[ST_STAGE + st * 4 + 3] = g->constant;
    }
    w[ST_BUFFER_COLOR] = S->buffer_color;
    w[ST_UPDATE] = S->update;
    w[ST_ALPHA] = (S->alpha_test ? 1u : 0u) | ((S->alpha_func & 7u) << 4) | ((S->alpha_ref & 0xFFu) << 8);
    w[ST_FLAGS] = (S->lighting ? ST_F_LIGHTING : 0u) | (S->wbuffer ? ST_F_WBUFFER : 0u);
    w[ST_DEPTH_RANGE] = fbits(S->depth_range);
    w[ST_DEPTH_NEAR] = fbits(S->depth_near);
    for (int u = 0; u < 3; u++) {
        const struct tex_state *t = &S->tex[u];
        w[ST_TEX + u * 4 + 0] = (t->enabled ? 1u : 0u) | ((uint32_t)(t->coords & 3) << 1)
                              | ((t->wrap_s & 15u) << 4) | ((t->wrap_t & 15u) << 8);
        w[ST_TEX + u * 4 + 1] = t->border;
        w[ST_TEX + u * 4 + 2] = fbits(t->max_level ? t->lod_bias : 0.0f);
    }
    if (!S->lighting) return;
    const struct albw_lighting_state *L = &S->light;
    w[ST_L_CONFIG0] = L->config0;
    w[ST_L_CONFIG] = L->config;
    w[ST_L_BUMP] = (L->bump_mode & 3u) | ((L->bump_unit & 3u) << 4) | (L->rebuild_z ? 256u : 0u)
                 | (L->clamp_highlights ? 512u : 0u);
    w[ST_L_LIGHTS] = L->lights;
    w[ST_L_USE] = (L->use_d0 ? ST_U_D0 : 0u) | (L->use_d1 ? ST_U_D1 : 0u) | (L->use_rr ? ST_U_RR : 0u)
                | (L->use_rg ? ST_U_RG : 0u) | (L->use_rb ? ST_U_RB : 0u)
                | (L->use_fresnel ? ST_U_FRESNEL : 0u);
    for (int k = 0; k < 7; k++) {
        w[ST_L_SAMPLER + k] = (L->sampler[k].input & 7u) | (L->sampler[k].absolute ? 8u : 0u);
        w[ST_L_SCALE + k] = fbits(L->sampler[k].scale);
    }
    for (int c = 0; c < 3; c++) w[ST_L_GLOBAL + c] = fbits(L->global[c]);
    for (int t = 0; t < REC_LUTS; t++) {
        w[ST_L_LUT + t] = 0xFFFFFFFFu;
        if (!L->lut[t]) continue;
        for (uint32_t i = 0; i < r->lut_count; i++) {
            if (r->lut[i] == L->lut[t]) { w[ST_L_LUT + t] = i * 256u; break; }
        }
    }
    for (uint32_t slot = 0; slot < L->lights && slot < 8u; slot++) {
        const struct albw_light *l = &L->light[slot];
        uint32_t *o = w + ST_LIGHT + slot * ST_LIGHT_WORDS;
        for (int c = 0; c < 3; c++) {
            o[LT_POSITION + c] = fbits(l->position[c]);
            o[LT_SPOT + c] = fbits(l->spot[c]);
            o[LT_SPEC0 + c] = fbits(l->spec0[c]);
            o[LT_SPEC1 + c] = fbits(l->spec1[c]);
            o[LT_DIFFUSE + c] = fbits(l->diffuse[c]);
            o[LT_AMBIENT + c] = fbits(l->ambient[c]);
        }
        o[LT_NUM] = l->num;
        o[LT_CONFIG] = l->light_config;
        o[LT_USE] = (l->use_dist ? 1u : 0u) | (l->use_spot ? 2u : 0u);
        o[LT_ATTEN_SCALE] = fbits(l->atten_scale);
        o[LT_ATTEN_BIAS] = fbits(l->atten_bias);
    }
}

static void gpu_vertex(const struct vertex *v, const float quat[4], struct albw_gpu_vertex *o,
                       const float prev[4]) {
    memcpy(o->pos, v->pos, sizeof o->pos);
    memcpy(o->color, v->color, sizeof o->color);
    o->tc01[0] = v->tc[0][0];
    o->tc01[1] = v->tc[0][1];
    o->tc01[2] = v->tc[1][0];
    o->tc01[3] = v->tc[1][1];
    o->tc2w[0] = v->tc[2][0];
    o->tc2w[1] = v->tc[2][1];
    o->tc2w[2] = v->tc0_w;
    o->tc2w[3] = 0.0f;
    memcpy(o->quat, quat, sizeof o->quat);
    o->view[0] = v->view[0];
    o->view[1] = v->view[1];
    o->view[2] = v->view[2];
    o->view[3] = 0.0f;
    memcpy(o->prev, prev ? prev : v->pos, sizeof o->prev);
}

// How the scenery moved from the frame before to this one, in NDC, for the draws that have no
// counterpart to say it themselves: the vertex-weighted median move of the draws that test and
// write depth into `target` and have one (the interface, which stands still whatever the camera
// does, would pull it to nothing). As replay_to works it out for the processor's upscaler path.
static void scene_move(const struct frame_rec *prev, const struct frame_rec *cur, uint32_t target,
                       float out[2]) {
    static float (*moves)[2];
    static uint32_t moves_room;
    out[0] = out[1] = 0.0f;
    if (!prev || !GROW(moves, moves_room, cur->draw_count)) return;
    for (int axis = 0; axis < 2; axis++) {
        uint32_t n = 0, total = 0;
        for (uint32_t i = 0; i < cur->draw_count; i++) {
            const struct rec_draw *d = &cur->draw[i], *p = rec_linked(prev, cur, d);
            const struct draw_state *S = &cur->state[d->state];
            if (!p || !p->placed || !d->placed || S->color_base != target || !S->depth_test
                    || !S->depth_write) {
                continue;
            }
            moves[n][0] = axis ? p->cy - d->cy : p->cx - d->cx;
            moves[n][1] = (float)d->vertices;
            total += d->vertices;
            n++;
        }
        qsort(moves, n, sizeof *moves, float_order);
        uint32_t half = total / 2u, sum = 0;
        for (uint32_t i = 0; i < n; i++) {
            sum += (uint32_t)moves[i][1];
            if (sum >= half) {
                out[axis] = moves[i][0];
                break;
            }
        }
    }
}

int albw_raster_gpu_frame(struct albw_gpu_frame *f, float t) {
    if (held_cur < 0) return 0;
    const struct frame_rec *cur = &recs[held_cur];
    const struct frame_rec *prev = held_prev >= 0 ? &recs[held_prev] : NULL;
    static struct vertex *moved;
    static uint32_t moved_room;
    f->frame = cur->frame;
    f->vert_count = f->draw_count = f->state_count = f->lut_count = f->ev_count = 0;
    f->screen_ev[0] = f->screen_ev[1] = -1;

    // A cut is drawn as the later frame, as a replay draws it, and tells an upscaler to start over.
    f->cut = !prev;
    if (prev && (t < 1.0f || f->want_prev)) {
        uint32_t smooth = 0, jumped = 0;
        for (uint32_t i = 0; i < cur->draw_count; i++) {
            const struct rec_draw *d = &cur->draw[i], *p = rec_linked(prev, cur, d);
            if (!p || p->vertices != d->vertices) continue;
            if (movable(prev->vert + p->first_vertex, cur->vert + d->first_vertex, d->vertices)) smooth++;
            else jumped++;
        }
        if (jumped > smooth) {
            t = 1.0f;
            f->cut = 1;
        }
    }

    if (!GROW(f->lut, f->lut_room, cur->lut_count * 256u)) return 0;
    for (uint32_t i = 0; i < cur->lut_count; i++) memcpy(f->lut + i * 256u, cur->lut[i], 256u * 4u);
    f->lut_count = cur->lut_count;

    uint32_t corners = 0;
    for (uint32_t i = 0; i < cur->draw_count; i++) corners += cur->draw[i].corners;
    if (!GROW(f->vert, f->vert_room, corners) || !GROW(f->draw, f->draw_room, cur->draw_count)
            || !GROW(f->ev, f->ev_room, cur->ev_count)
            || !GROW(f->state, f->state_room, (cur->state_count + 1u) * ST_WORDS)) {
        return 0;
    }

    // For a temporal upscaler: the top screen's target, and how its scenery moved.
    uint32_t top_target = 0;
    float scene[2] = { 0.0f, 0.0f };
    static float (*prevpos)[4];
    static uint32_t prevpos_room;
    if (f->want_prev) {
        for (uint32_t i = 0; i < cur->ev_count && !top_target; i++) {
            const struct rec_event *e = &cur->ev[i];
            if (e->kind == EV_TRANSFER && (e->d >> 16) == 400u && (e->d & 0xFFFFu) == 240u) top_target = e->a;
        }
        scene_move(prev, cur, top_target, scene);
    }

    uint32_t words[ST_WORDS];
    int32_t last_state = -1;
    for (uint32_t i = 0; i < cur->ev_count; i++) {
        const struct rec_event *e = &cur->ev[i];
        struct albw_gpu_event *o = &f->ev[f->ev_count];
        if (e->kind != EV_DRAW) {
            *o = (struct albw_gpu_event){ e->kind == EV_FILL ? ALBW_GPU_FILL
                                          : e->kind == EV_TRANSFER ? ALBW_GPU_TRANSFER
                                          : ALBW_GPU_TEXCOPY, e->a, e->b, e->c, e->d, e->e };
            if (e->kind == EV_TRANSFER && (e->d & 0xFFFFu) == 240u) {
                // The first transfer of a whole top screen is the top screen's picture (the
                // right eye's is not drawn), and the last of a bottom one the bottom's.
                if ((e->d >> 16) == 400u && f->screen_ev[0] < 0) f->screen_ev[0] = (int32_t)f->ev_count;
                if ((e->d >> 16) == 320u) f->screen_ev[1] = (int32_t)f->ev_count;
            }
            f->ev_count++;
            continue;
        }
        const struct rec_draw *d = &cur->draw[e->a];
        const struct draw_state *S = &cur->state[d->state];
        if (!S->color_base) continue;

        // The state, as words; a draw whose state is the last one's shares it.
        gpu_state_words(S, cur, words);
        if (last_state < 0 || memcmp(words, f->state + (uint32_t)last_state * ST_WORDS, sizeof words)) {
            memcpy(f->state + f->state_count * ST_WORDS, words, sizeof words);
            last_state = (int32_t)f->state_count++;
        }

        // The vertices, moved part of the way back where the draw has a counterpart that moves.
        const struct vertex *v = cur->vert + d->first_vertex;
        const struct vertex *from = NULL;
        if (prev && (t < 1.0f || f->want_prev)) {
            const struct rec_draw *p = rec_linked(prev, cur, d);
            if (p && p->vertices == d->vertices
                    && movable(prev->vert + p->first_vertex, v, d->vertices)) {
                from = prev->vert + p->first_vertex;
            }
        }
        if (from && t < 1.0f && GROW(moved, moved_room, d->vertices)) {
            for (uint32_t k = 0; k < d->vertices; k++) lerp_vertex(&from[k], &v[k], t, &moved[k]);
            v = moved;
        }
        // And where each was a frame before, for the upscaler: its counterpart's place, or, with
        // none, where the scenery's move would have had it.
        const float (*pp)[4] = NULL;
        if (f->want_prev && GROW(prevpos, prevpos_room, d->vertices)) {
            for (uint32_t k = 0; k < d->vertices; k++) {
                if (from) {
                    memcpy(prevpos[k], from[k].pos, sizeof prevpos[k]);
                } else {
                    memcpy(prevpos[k], v[k].pos, sizeof prevpos[k]);
                    if (S->color_base == top_target) {
                        prevpos[k][0] += scene[0] * prevpos[k][3];
                        prevpos[k][1] += scene[1] * prevpos[k][3];
                    }
                }
            }
            pp = (const float (*)[4])prevpos;
        }
        struct albw_gpu_draw *g = &f->draw[f->draw_count];
        g->state = (uint32_t)last_state;
        g->first_vertex = f->vert_count;
        const uint32_t *c = cur->corner + d->first_corner;
        for (uint32_t k = 0; k + 2 < d->corners; k += 3) {
            const struct vertex *tri[3] = { &v[c[k]], &v[c[k + 1]], &v[c[k + 2]] };
            const uint32_t idx[3] = { c[k], c[k + 1], c[k + 2] };
            int finite = 1;
            for (int j = 0; j < 3 && finite; j++) {
                for (int q = 0; q < 4; q++) finite &= isfinite(tri[j]->pos[q]) != 0;
            }
            if (!finite) continue;
            // The normals' quaternions into the first corner's hemisphere, as prepare_triangle
            // turns them.
            for (int j = 0; j < 3; j++) {
                float q[4];
                memcpy(q, tri[j]->quat, sizeof q);
                if (j) {
                    float dot = 0.0f;
                    for (int m = 0; m < 4; m++) dot += tri[0]->quat[m] * q[m];
                    if (dot < 0.0f) for (int m = 0; m < 4; m++) q[m] = -q[m];
                }
                gpu_vertex(tri[j], q, &f->vert[f->vert_count++], pp ? pp[idx[j]] : NULL);
            }
        }
        g->vertices = f->vert_count - g->first_vertex;
        if (!g->vertices) continue;
        g->blend = (uint8_t)S->blend;
        g->color_mask = (uint8_t)(S->color_mask & 7u);
        g->keep_color = !S->blend && S->logic_op == 6u;
        g->depth_test = (uint8_t)S->depth_test;
        g->depth_func = (uint8_t)S->depth_func;
        g->depth_write = (uint8_t)S->depth_write;
        g->cull = (uint8_t)S->cull;
        g->stencil = (uint8_t)S->stencil;
        g->stencil_func = (uint8_t)S->stencil_func;
        g->stencil_fail = (uint8_t)S->stencil_fail;
        g->stencil_zfail = (uint8_t)S->stencil_zfail;
        g->stencil_zpass = (uint8_t)S->stencil_zpass;
        g->stencil_ref = (uint8_t)S->stencil_ref;
        g->stencil_in_mask = (uint8_t)S->stencil_in_mask;
        g->stencil_write_mask = (uint8_t)S->stencil_write_mask;
        g->blend_func = S->blend_func;
        g->blend_color = S->blend_color;
        g->color_base = S->color_base;
        g->depth_base = S->depth_base;
        g->width = S->width;
        g->height = S->height_m1 + 1u;
        g->color_bytes = S->color_bytes;
        g->depth_bytes = S->depth_bytes;
        g->half_w = S->half_w;
        g->half_h = S->half_h;
        g->offset_x = S->offset_x;
        g->offset_y = S->offset_y;
        g->scissor[0] = S->scissor_x0;
        g->scissor[1] = S->scissor_y0;
        g->scissor[2] = S->scissor_x1;
        g->scissor[3] = S->scissor_y1;
        for (int u = 0; u < 3; u++) {
            const struct tex_state *tx = &S->tex[u];
            struct albw_gpu_texture *gt = &g->tex[u];
            memset(gt, 0, sizeof *gt);
            gt->enabled = (uint8_t)(tx->enabled && tx->usable);
            if (!tx->enabled) continue;
            gt->base = tx->base;
            gt->width = tx->width;
            gt->height = tx->height;
            gt->format = tx->format;
            gt->levels = (uint8_t)(tx->max_level + 1);
            gt->min_level = (uint8_t)tx->min_level;
            gt->mag_linear = (uint8_t)tx->linear;
            gt->min_linear = (uint8_t)(tx->max_level ? tx->min_linear : tx->linear);
            gt->mip_linear = (uint8_t)tx->mip_linear;
            gt->wrap_s = (uint8_t)(tx->wrap_s & 3u);
            gt->wrap_t = (uint8_t)(tx->wrap_t & 3u);
        }
        *o = (struct albw_gpu_event){ ALBW_GPU_DRAW, f->draw_count, 0, 0, 0, 0 };
        f->ev_count++;
        f->draw_count++;
    }
    return f->draw_count > 0 || f->ev_count > 0;
}

uint32_t albw_raster_texture_bytes(uint32_t width, uint32_t height, uint32_t format) {
    return width * height * texel_bits(format) / 8u;
}

void albw_raster_decode_texture(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                                uint8_t *rgba) {
    if (format == TEX_ETC1 || format == TEX_ETC1A4) {
        // Block by block, each decoded once: the per-thread cache the samplers use is the game's.
        uint32_t block_bytes = format == TEX_ETC1 ? 8u : 16u;
        for (uint32_t by = 0; by < height; by += 4u) {
            for (uint32_t bx = 0; bx < width; bx += 4u) {
                // (bx, by) in the stored, bottom-up order; the sampler's row is height - 1 - y.
                uint32_t tile = ((by >> 3) * (width >> 3) + (bx >> 3)) * block_bytes * 4u;
                uint32_t sub = ((bx >> 2) & 1u) + 2u * ((by >> 2) & 1u);
                uint32_t at = base + tile + sub * block_bytes;
                rgba32 texel[16];
                etc1_block(format == TEX_ETC1A4 ? at + 8u : at, texel);
                uint32_t alpha_lo = format == TEX_ETC1A4 ? albw_r32(at) : 0u;
                uint32_t alpha_hi = format == TEX_ETC1A4 ? albw_r32(at + 4u) : 0u;
                for (uint32_t x = 0; x < 4u; x++) {
                    for (uint32_t y = 0; y < 4u; y++) {
                        uint32_t j = x * 4u + y;
                        uint32_t a = 255u;
                        if (format == TEX_ETC1A4) {
                            uint32_t word = j >= 8u ? alpha_hi : alpha_lo;
                            a = expand4((word >> ((j & 7u) * 4u)) & 0xFu);
                        }
                        uint32_t sx = bx + x, sy = height - 1u - (by + y);
                        if (sx >= width || sy >= height) continue;
                        rgba32 v = (texel[j] & 0x00FFFFFFu) | (a << 24);
                        memcpy(rgba + ((size_t)sy * width + sx) * 4u, &v, 4);
                    }
                }
            }
        }
        return;
    }
    struct tex_state t;
    memset(&t, 0, sizeof t);
    static const texel_reader readers[16] = {
        read_rgba8, read_rgb8, read_rgb5a1, read_rgb565, read_rgba4, read_ia8, read_rg8,
        read_i8, read_a8, read_ia4, read_i4, read_a4, read_etc1, read_etc1a4, read_none, read_none,
    };
    t.base = base;
    t.width = width;
    t.height = height;
    t.format = format;
    uint32_t bits = texel_bits(format);
    t.row_bytes = width * 8u * (bits >= 8u ? bits / 8u : 1u);
    texel_reader read = readers[format & 15u];
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            rgba32 v = read(&t, x, y);
            memcpy(rgba + ((size_t)y * width + x) * 4u, &v, 4);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// One draw.
//
// The topology register says how the vertices become triangles. This game's draws are lists of
// triangles, six indices making two, but a strip and a fan cost three lines each.

static unsigned long render_until = 0;
// The first frame to render, absolute. Without it, rendering starts at the first frame that draws --
// which for this game is a fade from black and says nothing about whether the picture is right.
static unsigned long render_from = 0;
static int render_checked;
static int render_always;

void albw_raster_render_always(void) { render_always = 1; }

// The render target itself, as a picture, untiled here rather than by the display transfer. This is
// the rasteriser's own output with nothing else in the way: if this is blank the rasteriser is wrong,
// and if this has a picture in it and the screen does not, the transfer is.
//
// ALBW_DUMP_TARGET names a directory; at most forty files are written, one per draw, so the
// progression through a frame is visible rather than only its end.
static void dump_target(void) {
    const char *dir = albw_env("ALBW_DUMP_TARGET");
    if (!dir) return;
    static unsigned written;
    const char *max_s = albw_env("ALBW_DUMP_TARGET_MAX");
    if (written >= (max_s ? (unsigned)atoi(max_s) : 40u)) return;

    uint32_t base = to_guest(albw_pica_reg(R_COLOR_ADDR) << 3);
    uint32_t width = fb_width(), height = fb_height(), bpp = color_bpp();
    uint32_t format = color_format();
    if (!base || !width || !height) return;

    char path[512];
    snprintf(path, sizeof path, "%s/target-%03u-frame%06lu.ppm", dir, written,
             albw_kernel_frame());
    FILE *f = fopen(path, "wb");
    if (!f) return;
    written++;
    // The target is 240 across and 400 down, which is the screen turned on its side; written out
    // that way so it can be compared with the framebuffer dump without another transformation.
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t rgb[3] = { 0, 0, 0 };
            albw_pixel_read(base + albw_tiled_offset(x, height - 1u - y, width, bpp), format, rgb);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

// The vertices of the draw in hand, shaded by worker `id` of `count`: every count-th one.
static uint32_t shade_entry, shade_count;
static const uint32_t *shade_indices;                    // NULL: vertex k is the k-th of the draw
static struct vertex *shade_target;
static volatile int shade_failed;

#define SHADE_GROUP 32u                                  // vertices per item of a shading job

static void shade_job(uint32_t item, int worker) {
    (void)worker;
    uint32_t end = (item + 1u) * SHADE_GROUP;
    if (end > shade_count) end = shade_count;
    // Four at a time where the program lets them run side by side; one at a time where it does not,
    // and for the few left over.
    for (uint32_t k = item * SHADE_GROUP; k < end; k += 4u) {
        uint32_t lanes = end - k < 4u ? end - k : 4u;
        struct albw_pica_vertex_in in[4];
        for (uint32_t l = 0; l < lanes; l++) {
            uint32_t n = k + l;
            albw_pica_fetch_vertex(shade_indices ? shade_indices[n] : albw_pica_vertex_index(n, 0),
                                   &in[l]);
        }
        float out[4][7][4];
        int ran = 0;
        if (lanes == 4u) {
            const float (*const lane_in[4])[4] = { in[0].attr, in[1].attr, in[2].attr, in[3].attr };
            ran = albw_shader_run4(shade_entry, lane_in, in[0].count, out) == 1;
        }
        for (uint32_t l = 0; l < lanes; l++) {
            if (!ran && !albw_shader_run(shade_entry, in[l].attr, in[l].count, out[l])) {
                shade_failed = 1;
                return;
            }
            apply_semantics(out[l], &shade_target[k + l]);
        }
    }
}

// An indexed draw names most of its vertices several times -- a quad is six indices and four
// vertices, a mesh's vertex sits in about six triangles -- and the shader gives the same answer every
// time, since it starts from nothing but the vertex's attributes and the uniforms. So each distinct
// index is shaded once, and `slot_of` says, for each of the draw's indices, which result it takes.
// Returns the number of distinct vertices, listed in `unique`; 0 if the indices span too wide a
// range to be worth a table, and the draw is shaded one index at a time as before.
static uint32_t gather_indices(uint32_t count, uint32_t **slot_of_out, uint32_t **unique_out) {
    static uint32_t *index_of, *slot_of, *unique, room;
    static int32_t *table;
    static uint32_t table_room;
    if (count > room) {
        uint32_t *a = realloc(index_of, count * sizeof *a);
        if (a) index_of = a;
        uint32_t *b = realloc(slot_of, count * sizeof *b);
        if (b) slot_of = b;
        uint32_t *c = realloc(unique, count * sizeof *c);
        if (c) unique = c;
        if (!a || !b || !c) return 0;
        room = count;
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t n = 0; n < count; n++) {
        uint32_t index = albw_pica_vertex_index(n, 1);
        index_of[n] = index;
        if (index < lo) lo = index;
        if (index > hi) hi = index;
    }
    uint32_t span = hi - lo + 1u;
    if (span > 65536u && span > count * 4u) return 0;
    if (span > table_room) {
        int32_t *bigger = realloc(table, span * sizeof *bigger);
        if (!bigger) return 0;
        table = bigger;
        table_room = span;
    }
    memset(table, 0xFF, span * sizeof *table);
    uint32_t distinct = 0;
    for (uint32_t n = 0; n < count; n++) {
        int32_t *at = &table[index_of[n] - lo];
        if (*at < 0) {
            *at = (int32_t)distinct;
            unique[distinct++] = index_of[n];
        }
        slot_of[n] = (uint32_t)*at;
    }
    *slot_of_out = slot_of;
    *unique_out = unique;
    return distinct;
}

// What the target actually holds after a draw, at one pixel in the middle. A picture that never
// reaches the screen has two very different causes -- nothing was drawn, or something else overwrote
// it -- and this is the only place that can tell them apart.
static void probe_target(void) {
    if (!albw_env("ALBW_PROBE")) return;
    uint32_t at = color_at(fb_width() / 2u, fb_height() / 2u);
    if (!at) return;
    printf("[raster] frame %lu: the target's middle pixel is now %04x (at %08x)\n",
           albw_kernel_frame(), albw_r16(at), at);
}

// A pass that costs nothing but says where the interesting frames are: the first frame whose draws
// read a texture, and the first frame that draws a lot. Rendering is far too slow to run over a
// twenty-thousand-frame boot, so the window has to be chosen, and choosing it blindly means rendering
// a fade from black and concluding the rasteriser is broken.
static void scan_draw(void) {
    if (!albw_env("ALBW_SCAN")) return;
    static int said_textured, said_busy;
    static unsigned long frame_draws, last_frame;
    unsigned long frame = albw_kernel_frame();
    if (frame != last_frame) {
        last_frame = frame;
        frame_draws = 0;
    }
    frame_draws++;
    uint32_t units = albw_pica_reg(R_TEX_CONFIG);
    if (!said_textured && (units & 1u) && albw_pica_reg(tex_addr_reg[0])) {
        said_textured = 1;
        printf("[raster] frame %lu is the first whose draws read a texture: %ux%u format %u\n",
               frame, (albw_pica_reg(tex_base[0] + 1u) >> 16) & 0x7FFu,
               albw_pica_reg(tex_base[0] + 1u) & 0x7FFu, albw_pica_reg(tex_format_reg[0]) & 0xFu);
    }
    if (!said_busy && frame_draws >= 20) {
        said_busy = 1;
        printf("[raster] frame %lu is the first with twenty draws in it\n", frame);
    }
}

void albw_raster_draw(int indexed) {
    scan_draw();
    if (!render_checked) {
        render_checked = 1;
        const char *s = albw_env("ALBW_RENDER");
        render_until = s ? strtoul(s, NULL, 0) : 0;
        const char *from = albw_env("ALBW_RENDER_FROM");
        render_from = from ? strtoul(from, NULL, 0) : 0;
    }
    // With a window open every frame is drawn; without one, only the window ALBW_RENDER names.
    if (!render_always) {
        if (!render_until) return;
        // ALBW_RENDER counts frames from the first one that draws, not from the start of the run: the
        // game spends a hundred frames on its service handshakes before the first draw, so a count from
        // zero renders nothing and says nothing about why.
        static unsigned long first_frame;
        static int said;
        if (!said) {
            said = 1;
            first_frame = render_from ? render_from : albw_kernel_frame();
            printf("[raster] the first draw arrives on frame %lu; rendering %lu frame(s) from %lu\n",
                   albw_kernel_frame(), render_until, first_frame);
        }
        unsigned long now = albw_kernel_frame();
        if (now < first_frame || now - first_frame >= render_until) return;
    }

    // Every distinct render target the rendered frames draw into, once each. Which surface a draw
    // targets is not fixed -- the settled register dump shows the last one only -- and a picture that
    // never reaches the screen is usually a draw into a surface nothing transfers from.
    static uint32_t seen_targets[8];
    static int seen_count;
    uint32_t target = to_guest(albw_pica_reg(R_COLOR_ADDR) << 3);
    int known = 0;
    for (int i = 0; i < seen_count; i++) if (seen_targets[i] == target) known = 1;
    if (albw_env("ALBW_PROBE_TARGETS")) {
        static unsigned told;
        if (told++ < 400) {
            uint32_t m = albw_pica_reg(R_DEPTH_TEST);
            printf("[raster] frame %lu draw into %08x %ux%u, %u vertices, shader %04x: depth test %s "
                   "func %u write %u, colour mask %x, alpha test %08x, blend %08x/%08x\n",
                   albw_kernel_frame(), target, fb_width(), fb_height(), albw_pica_reg(R_COUNT),
                   albw_pica_reg(R_VS_ENTRY) & 0xFFFFu, (m & 1u) ? "on" : "off", (m >> 4) & 7u,
                   (m >> 12) & 1u, (m >> 8) & 0xFu, albw_pica_reg(R_ALPHA_TEST),
                   albw_pica_reg(R_BLEND_OP), albw_pica_reg(R_BLEND_FUNC));
            printf("[raster]   allow: colour read %x write %x, depth read %x write %x\n",
                   albw_pica_reg(0x112u), albw_pica_reg(0x113u), albw_pica_reg(0x114u),
                   albw_pica_reg(0x115u));
        }
    }
    if (!known && seen_count < 8) {
        seen_targets[seen_count++] = target;
        printf("[raster] drawing into %08x, %ux%u, colour format %u, depth at %08x\n",
               target, fb_width(), fb_height(), (albw_pica_reg(R_COLOR_FORMAT) >> 16) & 7u,
               to_guest(albw_pica_reg(R_DEPTH_ADDR) << 3));
    }

    // The texture units this draw reads, the first few times, with a couple of texels. A black
    // picture and a black texture look the same from the framebuffer.
    static unsigned shown_textures;
    const char *probe_n = albw_env("ALBW_PROBE");
    if (probe_n && shown_textures < (unsigned)(atoi(probe_n) > 1 ? atoi(probe_n) : 8)) {
        shown_textures++;
        uint32_t units = albw_pica_reg(R_TEX_CONFIG);
        printf("[raster] draw's textures (config %08x):", units);
        for (int u = 0; u < 3; u++) {
            uint32_t dim = albw_pica_reg(tex_base[u] + 1u);
            uint32_t at = to_guest(albw_pica_reg(tex_addr_reg[u]) << 3);
            printf("  [%d %s %ux%u fmt %u at %08x", u, ((units >> u) & 1u) ? "on" : "off",
                   (dim >> 16) & 0x7FFu, dim & 0x7FFu,
                   albw_pica_reg(tex_format_reg[u]) & 0xFu, at);
            if (((units >> u) & 1u) && at) {
                uint8_t px[4];
                sample_texture(u, 0.5f, 0.5f, px);
                printf(" middle %u %u %u a%u", px[0], px[1], px[2], px[3]);
                sample_texture(u, 0.25f, 0.75f, px);
                printf(", quarter %u %u %u a%u", px[0], px[1], px[2], px[3]);
            }
            printf("]");
        }
        printf("\n");
        // Every stage, and the registers that change what the stages see: the buffer update mask,
        // the texture configuration, and whether fragment lighting is on, since with it on the
        // "primary fragment colour" a stage can name is the lighting result, which this does not
        // compute.
        for (int st = 0; st < 6; st++) {
            uint32_t at = texenv_base[st];
            printf("[raster]   stage %d: src %08x mod %08x op %08x const %08x scale %08x\n", st,
                   albw_pica_reg(at), albw_pica_reg(at + 1), albw_pica_reg(at + 2),
                   albw_pica_reg(at + 3), albw_pica_reg(at + 4));
        }
        static int lighting_dumped;
        if (albw_lighting_enabled() && !lighting_dumped) {
            lighting_dumped = 1;
            albw_lighting_dump();
        }
        printf("[raster]   buffer %08x, texunits %08x, lighting %08x, shader entry %04x, "
               "%u vertices\n", albw_pica_reg(R_TEXENV_BUFFER), units, albw_pica_reg(0x08Fu),
               albw_pica_reg(R_VS_ENTRY) & 0xFFFFu, albw_pica_reg(R_COUNT));
    }

    snapshot_state();
    // A batch has one target: its bands are that target's rows, and a draw that samples what an
    // earlier one rendered does so from a different target. So a change of target finishes the batch
    // first. So does a very long one, which is only memory.
    if (game.prep_count && (!same_target(&ds, &game.states[0]) || game.prep_count >= (1u << 18))) {
        after_left_eye = 0;                              // not a plain second eye: draw it all
        albw_raster_flush();
        snapshot_state();                                // its tables were let go with the batch
    }
    game.state_in_batch = 0;
    uint32_t count = albw_pica_reg(R_COUNT);
    // The hardware takes any count; the bound is only against a register that holds garbage. 4096,
    // which this had, dropped the title scene's large pieces, which are 4173 vertices each.
    if (count < 3 || count > (1u << 20)) {
        draws_skipped++;
        skipped_for_count++;
        static unsigned said;
        if (said++ < 6) printf("[raster] a draw of %u vertices is skipped\n", count);
        return;
    }
    uint32_t topology = (albw_pica_reg(R_PRIMITIVE) >> 8) & 3u;
    uint32_t entry = albw_pica_reg(R_VS_ENTRY) & 0xFFFFu;

    // The shaded vertices, in the order the draw walks them. Shading each one as it comes is simple
    // and correct; the hardware caches by index and that is an optimisation for later.
    static struct vertex *shaded;
    static uint32_t shaded_room;
    if (count > shaded_room) {
        struct vertex *bigger = realloc(shaded, count * sizeof *shaded);
        if (!bigger) return;
        shaded = bigger;
        shaded_room = count;
    }
    // Each distinct vertex once; `slot_of[n]` is where the draw's n-th vertex ended up, or NULL when
    // that is simply n.
    uint32_t *slot_of = NULL, *unique = NULL;
    uint32_t to_shade = indexed ? gather_indices(count, &slot_of, &unique) : 0;
    if (!to_shade) {
        to_shade = count;
        slot_of = NULL;
        if (indexed) {
            // Too sparse a set of indices for the table: shade them in order, each as it comes.
            static uint32_t *in_order, in_order_room;
            if (count > in_order_room) {
                uint32_t *bigger = realloc(in_order, count * sizeof *bigger);
                if (!bigger) return;
                in_order = bigger;
                in_order_room = count;
            }
            for (uint32_t n = 0; n < count; n++) in_order[n] = albw_pica_vertex_index(n, 1);
            unique = in_order;
        }
    }
    // Every vertex is shaded independently, so a large draw's are shared out among the workers; a
    // small one is not worth waking them for.
    albw_shader_prepare_lanes();
    shade_entry = entry;
    shade_indices = unique;
    shade_count = to_shade;
    shade_target = shaded;
    shade_failed = 0;
    double ts0 = probe_now();
    uint32_t groups = (to_shade + SHADE_GROUP - 1u) / SHADE_GROUP;
    if (to_shade >= 96) pool_run(shade_job, groups);
    else for (uint32_t g = 0; g < groups; g++) shade_job(g, 0);
    double ts1 = probe_now();
    if (to_shade >= 96) { t_shade_pool += ts1 - ts0; n_shade_pool++; }
    else { t_shade_alone += ts1 - ts0; n_shade_alone++; }
    n_draws_timed++;
    vertices_named += count;
    vertices_shaded += to_shade;
#define VERTEX(n) shaded[slot_of ? slot_of[n] : (n)]
    if (shade_failed) {
        draws_skipped++;
        static unsigned said;
        if (said++ < 4) printf("[raster] the shader at %04x did not finish\n", entry);
        return;
    }

    // A draw's positions as the shader left them, for ALBW_PROBE_POSITIONS draws with that many
    // vertices -- the clearing draw at the start of the title scene's frames is 168.
    const char *pp = albw_env("ALBW_PROBE_POSITIONS");
    if (pp && count == (uint32_t)atoi(pp)) {
        static unsigned shown_draws;
        if (shown_draws++ < 1) {
            float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
            for (uint32_t n = 0; n < count; n++) {
                float w = VERTEX(n).pos[3], zz = VERTEX(n).pos[2] / w;
                if (zz < zmin) zmin = zz;
                if (zz > zmax) zmax = zz;
                if (w < wmin) wmin = w;
                if (w > wmax) wmax = w;
            }
            printf("[raster] %u vertices: z/w from %g to %g, w from %g to %g; first:", count, zmin,
                   zmax, wmin, wmax);
            for (uint32_t n = 0; n < count && n < 4; n++) {
                printf(" (%g %g %g %g)", VERTEX(n).pos[0], VERTEX(n).pos[1], VERTEX(n).pos[2],
                       VERTEX(n).pos[3]);
            }
            printf("\n");
            printf("[raster]   attribute formats %08x %08x, base %08x, permutation %08x %08x\n",
                   albw_pica_reg(0x201u), albw_pica_reg(0x202u), albw_pica_reg(0x200u),
                   albw_pica_reg(0x2BBu), albw_pica_reg(0x2BCu));
            for (uint32_t l = 0; l < 12; l++) {
                uint32_t hi = albw_pica_reg(0x205u + l * 3u);
                if (!(hi >> 28)) continue;
                printf("[raster]   loader %u: offset %08x components %08x %08x\n", l,
                       albw_pica_reg(0x203u + l * 3u), albw_pica_reg(0x204u + l * 3u), hi);
            }
            for (uint32_t n = 0; n < 3; n++) {
                struct albw_pica_vertex_in in;
                albw_pica_fetch_vertex(albw_pica_vertex_index(n, indexed), &in);
                printf("[raster]   vertex %u (index %u):", n, albw_pica_vertex_index(n, indexed));
                for (uint32_t a = 0; a < in.count; a++) {
                    printf(" a%u(%g %g %g %g)", a, in.attr[a][0], in.attr[a][1], in.attr[a][2],
                           in.attr[a][3]);
                }
                printf("\n");
            }
        }
    }

    rec_draw(shaded, slot_of ? to_shade : count, slot_of, count, topology, entry);
    // The GPU renderer shows the picture, from the record: the processor need not draw it.
    if (gpu_display) return;

    double tp0 = probe_now();
    struct vertex tri[3];
    switch (topology) {
        case 1:                                          // a strip: every vertex adds a triangle
            for (uint32_t n = 2; n < count; n++) {
                // The winding alternates, which the rasteriser would otherwise cull away.
                tri[0] = VERTEX(n - 2);
                tri[1] = VERTEX((n & 1u) ? n : n - 1);
                tri[2] = VERTEX((n & 1u) ? n - 1 : n);
                draw_triangle(&game, &ds, tri);
            }
            break;
        case 2:                                          // a fan around the first vertex
            for (uint32_t n = 2; n < count; n++) {
                tri[0] = VERTEX(0);
                tri[1] = VERTEX(n - 1);
                tri[2] = VERTEX(n);
                draw_triangle(&game, &ds, tri);
            }
            break;
        default:
            for (uint32_t n = 0; n + 2 < count; n += 3) {
                tri[0] = VERTEX(n);
                tri[1] = VERTEX(n + 1);
                tri[2] = VERTEX(n + 2);
                draw_triangle(&game, &ds, tri);
            }
            break;
    }
    t_setup += probe_now() - tp0;
#undef VERTEX
    // The diagnostics below read the target, so they need this draw's pixels in it now; and
    // ALBW_NO_BATCH=1 draws every draw by itself, to tell a batching mistake from any other.
    if (albw_env("ALBW_PROBE") || albw_env("ALBW_DUMP_TARGET") || albw_env("ALBW_NO_BATCH")) {
        albw_raster_flush();
    }
    probe_target();
    dump_target();
}

double albw_raster_busy(void) {
    return t_shade_pool + t_shade_alone + t_setup + t_rast_pool + t_rast_alone;
}

void albw_raster_report(void) {
    if (!render_until && !render_always) return;
    printf("[time ] %lu draws: shading %.3f s (%lu shared out, %.3f s alone), triangle setup %.3f s, "
           "rasterising %.3f s (%lu shared out, %.3f s alone)\n", n_draws_timed, t_shade_pool,
           n_shade_pool, t_shade_alone, t_setup, t_rast_pool, n_rast_pool, t_rast_alone);
    printf("[time ] %lu vertices named, %lu shaded\n", vertices_named, vertices_shaded);
    printf("[time ] %lu batches drawn, %lu right-eye passes left undrawn\n", flushes,
           right_eyes_skipped);
    pixels_drawn = 0;
    for (int w = 0; w <= MAX_WORKERS; w++) pixels_drawn += pixels_by_worker[w].n;
    uint32_t which = 0;
    unsigned long unsupported = albw_shader_unsupported(&which);
    printf("[raster] %lu triangle(s), %lu pixel(s); %lu wholly outside the view; skipped %lu "
           "triangle(s) and %lu draw(s)\n", triangles_drawn, pixels_drawn, triangles_clipped_away,
           triangles_skipped, draws_skipped);
    if (unsupported) {
        printf("[raster] the shader met %lu instruction(s) this machine does not run "
               "(last was opcode %02x)\n", unsupported, which);
    }
    for (int m = 0; m < MISS_COUNT; m++) {
        if (miss_draws[m]) printf("[raster] %lu draw state(s) asked for %s\n", miss_draws[m], miss_name[m]);
    }
    unsigned long off_end = 0, long_run = 0;
    albw_shader_failures(&off_end, &long_run);
    if (off_end || long_run || skipped_for_count) {
        printf("[raster] skipped draws: %lu for their vertex count, %lu ran off the end of the "
               "program, %lu ran too long\n", skipped_for_count, off_end, long_run);
    }
}

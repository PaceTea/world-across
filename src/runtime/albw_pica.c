#include "albw_pica.h"

#include "albw_cpu.h"
#include "albw_kernel.h"
#include "albw_lighting.h"
#include "albw_raster.h"
#include "albw_shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t regs[ALBW_PICA_REGS];
static unsigned long writes_per_reg[ALBW_PICA_REGS];

// Addresses the GPU is known to be using, learned from the fills and transfers rather than from a
// register map. Whichever register turns out to hold one of these is that surface's register.
#define ALBW_PICA_SURFACES 8
static struct { uint32_t addr; const char *what; } surfaces[ALBW_PICA_SURFACES];
static int surface_count;

void albw_pica_note_surface(uint32_t addr, const char *what) {
    if (!addr) return;
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].addr == addr) return;
    }
    if (surface_count < ALBW_PICA_SURFACES) {
        surfaces[surface_count].addr = addr;
        surfaces[surface_count].what = what;
        surface_count++;
    }
}

uint32_t albw_pica_reg(uint32_t index) {
    return index < ALBW_PICA_REGS ? regs[index] : 0;
}

// Whether a register holds one of the surfaces we already know about.
//
// The hardware works in physical addresses and in units of eight bytes, and the guest works in
// virtual ones. Register 0x11D held 0x03017710 while the colour buffer the fills clear is at
// 0x1F0BB880: shift the register left by three and that is 0x180BB880, which is the same memory
// seen physically, VRAM being physical 0x18000000 and virtual 0x1F000000. The depth buffer matches
// the same way in 0x11C. So the two render target registers are identified by arithmetic that comes
// out exact, not by a guess at a register map -- and the physical mapping falls out with them,
// which a rasteriser needs anyway.
//
// An earlier version of this called anything within a memory range an address, and duly reported
// the byte pattern 0x1F1F0D0C as one. Only an exact match against a known surface counts.
#define ALBW_VRAM_PHYS    0x18000000u
#define ALBW_VRAM_VIRT    0x1F000000u
#define ALBW_VRAM_SIZE    0x00600000u
#define ALBW_FCRAM_PHYS   0x20000000u
#define ALBW_FCRAM_VIRT   0x14000000u
#define ALBW_FCRAM_SIZE   0x08000000u

static uint32_t physical_to_virtual(uint32_t phys) {
    if (phys >= ALBW_VRAM_PHYS && phys < ALBW_VRAM_PHYS + ALBW_VRAM_SIZE) {
        return phys - ALBW_VRAM_PHYS + ALBW_VRAM_VIRT;
    }
    if (phys >= ALBW_FCRAM_PHYS && phys < ALBW_FCRAM_PHYS + ALBW_FCRAM_SIZE) {
        return phys - ALBW_FCRAM_PHYS + ALBW_FCRAM_VIRT;
    }
    return 0;
}

static const char *surface_name(uint32_t value) {
    uint32_t candidates[2] = { physical_to_virtual(value << 3), physical_to_virtual(value) };
    for (int c = 0; c < 2; c++) {
        if (!candidates[c]) continue;
        for (int i = 0; i < surface_count; i++) {
            if (candidates[c] == surfaces[i].addr) return surfaces[i].what;
        }
    }
    return NULL;
}

// The two registers written with exactly 1 and nothing else, thirty-six and sixteen times in a
// settled frame: the draw triggers. One takes its vertices in order, the other through an index
// array, which is what the two of them being distinct means.
#define ALBW_PICA_DRAW_ARRAYS   0x22Eu
#define ALBW_PICA_DRAW_ELEMENTS 0x22Fu

// Registers the draws themselves identify. The count is 6 almost every time, which is two triangles
// -- a quad -- and the index configuration's top bit distinguishes 16-bit indices from 8-bit ones.
// Named after what the numbers do rather than after a register map.
#define ALBW_PICA_INDEX_CONFIG  0x227u
#define ALBW_PICA_VERTEX_COUNT  0x228u
// Added to every index of a non-indexed draw; zero in everything seen so far.
#define ALBW_PICA_VERTEX_OFFSET 0x22Au
// The base every attribute buffer and the index array are addressed from, in the same eight-byte
// physical units as the render targets. It reads 0x03000000, which is physical 0x18000000: the start
// of VRAM. The offsets are added to that *physically*, and the sum for this game lands in FCRAM --
// which is why four attempts at this failed while they mapped the base to a virtual address first and
// then added. Mapping last is the whole of it: 0x18000000 + 0x0A8088A0 is 0x228088A0, FCRAM physical,
// which the guest sees at 0x168088A0, and there sit the indices 0 2 1 1 2 3.
#define ALBW_PICA_ATTR_BASE     0x200u

static unsigned long draws;
static unsigned long list_jumps;                         // command buffer jumps followed

#ifdef ALBW_TRACK_WRITES
// Guest writes since the last draw, coalesced into contiguous runs. A ring, because a frame's worth
// of writes is far more than is worth keeping and only the most recent matter.
#define ALBW_WRITE_RUNS 4096
static struct { uint32_t from, to; } runs[ALBW_WRITE_RUNS];
static uint32_t run_count, run_next;

void albw_note_write(uint32_t addr, uint32_t len) {
    // Extending the run just written to costs one comparison and collapses a buffer being filled
    // sequentially -- which is how these buffers are filled -- into a single entry.
    if (run_count) {
        uint32_t last = (run_next + ALBW_WRITE_RUNS - 1) % ALBW_WRITE_RUNS;
        if (runs[last].to == addr) {
            runs[last].to = addr + len;
            return;
        }
        if (addr + len == runs[last].from) {
            runs[last].from = addr;
            return;
        }
    }
    runs[run_next].from = addr;
    runs[run_next].to = addr + len;
    run_next = (run_next + 1) % ALBW_WRITE_RUNS;
    if (run_count < ALBW_WRITE_RUNS) run_count++;
}

// The runs that could hold an index array: long enough for the indices, short enough not to be a
// texture, and in memory the GPU reads from.
//
// This found one. At 0x1F1CF400 sit the bytes 00 01 02 02 03 01 04 05 06 06 05 07, which is two
// triangles per quad for quad after quad -- an index buffer of *8-bit* indices, arriving by a
// kernel-side copy rather than being built a word at a time, so it is loaded data and not generated.
// What is still missing is the register that points at it: nothing in the file holds the 0x03039E80
// such a register would read, and the per-draw value of 0x227 does not decode to it under any reading
// tried. The indices are found; the pointer to them is not.
static void report_recent_writes(uint32_t count) {
    uint32_t want = count * 2u;
    printf("[pica ] regions the guest wrote before this draw that could hold %u indices:\n", count);
    unsigned shown = 0;
    for (uint32_t i = 0; i < run_count && shown < 10; i++) {
        uint32_t k = (run_next + ALBW_WRITE_RUNS - 1 - i) % ALBW_WRITE_RUNS;
        uint32_t len = runs[k].to - runs[k].from;
        if (len < want || len > 0x400u) continue;
        uint32_t phys_base, virt_base;
        if (runs[k].from >= ALBW_VRAM_VIRT && runs[k].from < ALBW_VRAM_VIRT + ALBW_VRAM_SIZE) {
            phys_base = ALBW_VRAM_PHYS; virt_base = ALBW_VRAM_VIRT;
        } else if (runs[k].from >= ALBW_FCRAM_VIRT
                   && runs[k].from < ALBW_FCRAM_VIRT + ALBW_FCRAM_SIZE) {
            phys_base = ALBW_FCRAM_PHYS; virt_base = ALBW_FCRAM_VIRT;
        } else {
            continue;
        }
        uint32_t as_register = (runs[k].from - virt_base + phys_base) >> 3;
        printf("[pica ]   %08x..%08x (%u bytes, a register would read %08x):",
               runs[k].from, runs[k].to, len, as_register);
        // And whether any register does, now, at the draw. The settled dump cannot answer this: a
        // pointer the guest rewrites per draw holds whatever the last draw left in it.
        for (uint32_t r = 0; r < ALBW_PICA_REGS; r++) {
            if (!writes_per_reg[r]) continue;
            if ((regs[r] & 0x0FFFFFFFu) == (as_register & 0x0FFFFFFFu)) {
                printf("  [register 0x%03x holds it]", r);
            }
        }
        // As halfwords and as bytes. The one that found the index buffer was the bytes: read as
        // halfwords the same memory says 256, 514, 769 and looks like nothing.
        for (uint32_t j = 0; j < count && j < 12; j++) {
            printf(" %u", albw_r16(runs[k].from + j * 2));
        }
        printf("  as bytes:");
        for (uint32_t j = 0; j < count && j < 12; j++) {
            printf(" %u", albw_r8(runs[k].from + j));
        }
        printf("\n");
        shown++;
    }
    if (!shown) printf("[pica ]   none of that shape\n");
    run_count = run_next = 0;
}
#endif

// Looks for the index array rather than deducing where it is, on the reasoning that identified the
// render targets: know the address, then find the register that holds it.
//
// It does not work here, and the apparatus is kept for saying so. Six tiny indices covering a quad's
// corners are not a distinctive enough pattern -- the hits are all coincidences inside VRAM data,
// none of them matches any register, and loosening or tightening the pattern trades one kind of
// noise for another. Two readings of the index register have now failed the same way.
//
// The method that would work is the other one: the processor writes that buffer shortly before it
// draws, so recording which regions of memory the guest wrote since the last draw would single it
// out. That needs write tracking, which nothing here does yet, and is worth more than a third guess
// at the register layout.
static void find_index_arrays(uint32_t count) {
    if (count < 6 || count > 12) return;
    printf("[pica ] looking for a %u-index array in memory:\n", count);

    unsigned hits = 0;
    static const struct { uint32_t from, to; const char *what; } where[] = {
        { ALBW_VRAM_VIRT, ALBW_VRAM_VIRT + ALBW_VRAM_SIZE, "VRAM" },
        { ALBW_FCRAM_VIRT, ALBW_FCRAM_VIRT + ALBW_FCRAM_SIZE, "the linear heap" },
    };
    for (size_t region = 0; region < sizeof where / sizeof where[0] && hits < 8; region++) {
        for (uint32_t at = where[region].from; at + count * 2 <= where[region].to && hits < 8;
             at += 2) {
            uint32_t seen = 0;
            int ok = 1;
            for (uint32_t i = 0; i < count; i++) {
                uint16_t v = albw_r16(at + i * 2);
                if (v > 8) { ok = 0; break; }
                seen |= 1u << v;
            }
            if (!ok || (seen & 0x9u) != 0x9u) continue;   // must cover both 0 and 3
            int distinct = 0;
            for (int b = 0; b < 9; b++) if (seen & (1u << b)) distinct++;
            if (distinct < 4) continue;

            printf("[pica ]   %08x in %s:", at, where[region].what);
            for (uint32_t i = 0; i < count; i++) printf(" %u", albw_r16(at + i * 2));
            // And what a register would have to hold to point here.
            printf("   (a register would read %08x)\n",
                   (at - where[region].from
                    + (region ? ALBW_FCRAM_PHYS : ALBW_VRAM_PHYS)) >> 3);
            hits++;
        }
    }
    if (!hits) printf("[pica ]   nothing that shape is in memory\n");
}

// Everything the geometry pipeline's registers hold at a draw, printed rather than interpreted.
// The four refuted attempts all guessed at one register at a time; a whole block of thirty-two,
// captured at the moment a draw reads them, is what lets the addressing be worked out from the
// numbers instead.
static void dump_geometry_state(void) {
    printf("[pica ] geometry registers at this draw:\n");
    for (uint32_t r = 0x200u; r <= 0x22Fu; r++) {
        if (!writes_per_reg[r]) continue;
        uint32_t virt = physical_to_virtual(regs[r] << 3);
        printf("[pica ]   0x%03x = %08x  (%lu write%s)", r, regs[r], writes_per_reg[r],
               writes_per_reg[r] == 1 ? "" : "s");
        if (virt && albw_guest_ok(virt, 16)) {
            printf("  <<3 -> %08x:", virt);
            for (int w = 0; w < 4; w++) printf(" %08x", albw_r32(virt + w * 4));
        }
        printf("\n");
    }
}

// ---------------------------------------------------------------------------------------------
// Fixed attributes, and the 24-bit float the shader pipeline uses.
//
// The hardware's float is 24 bits wide: one sign bit, seven of exponent biased by 63, sixteen of
// mantissa. Uniforms and fixed attributes arrive in it, three words carrying four of them packed
// big-endian, so a value straddles a word boundary and has to be reassembled before it means
// anything.
float albw_pica_float24(uint32_t raw) {
    raw &= 0xFFFFFFu;
    uint32_t sign = (raw >> 23) & 1u;
    uint32_t exponent = (raw >> 16) & 0x7Fu;
    uint32_t mantissa = raw & 0xFFFFu;
    float value;
    if (exponent == 0x7F) {
        // The largest exponent is the hardware's infinity and not-a-number, as in IEEE.
        value = mantissa ? __builtin_nanf("") : __builtin_inff();
    } else if (exponent == 0) {
        // Subnormals: no implied leading one, and the exponent is the smallest normal's.
        value = (float)mantissa / 65536.0f * 1.7763568394002505e-15f;  // 2^-49
    } else {
        uint32_t bits = (sign << 31) | ((exponent - 63u + 127u) << 23) | (mantissa << 7);
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    return sign ? -value : value;
}

// Four packed into three words, filled from w down to x -- as the uniforms are in their packed
// form (albw_shader.c), where reading them the other way gave not-a-number. Read x first, as this
// once did, a fixed bone weight of (1, 0, 0, 0) came out (0, 0, 0, 1): the parts of a model that
// hang from one bone by a fixed weight -- eyes, mouths, a sword in the hand -- took the transform of
// whatever bone the fourth index named, and sat beside the face.
static void unpack_float24x4(const uint32_t w[3], float out[4]) {
    out[3] = albw_pica_float24(w[0] >> 8);
    out[2] = albw_pica_float24(((w[0] & 0xFFu) << 16) | ((w[1] >> 16) & 0xFFFFu));
    out[1] = albw_pica_float24(((w[1] & 0xFFFFu) << 8) | ((w[2] >> 24) & 0xFFu));
    out[0] = albw_pica_float24(w[2] & 0xFFFFFFu);
}

static float fixed_attrs[ALBW_PICA_MAX_ATTRS][4];
static uint32_t fixed_index;
static uint32_t fixed_words[3];
static uint32_t fixed_held;
static unsigned long immediate_vertices;

float albw_pica_fixed_attr(uint32_t attr, int component) {
    if (attr >= ALBW_PICA_MAX_ATTRS) return 0.0f;
    return fixed_attrs[attr][component & 3];
}

// One word of a fixed attribute, or of an immediate-mode vertex. Index 15 does not name an attribute:
// it means the guest is handing whole vertices to the pipeline through this port instead of putting
// them in memory, which is a second way to draw and is counted here rather than guessed at.
static void fixed_attr_word(uint32_t value) {
    fixed_words[fixed_held++] = value;
    if (fixed_held < 3) return;
    fixed_held = 0;
    if (fixed_index == 0xFu) {
        immediate_vertices++;
        return;
    }
    if (fixed_index < ALBW_PICA_MAX_ATTRS) {
        unpack_float24x4(fixed_words, fixed_attrs[fixed_index]);
    }
}

// ---------------------------------------------------------------------------------------------
// Fetching a vertex.
//
// The attributes are described in two places. What each attribute *is* -- how many components and of
// what type -- sits in 0x201 and 0x202, four bits per attribute. Where each one is *read from* sits in
// twelve loaders of three registers, starting at 0x203: an offset from the base, a list of which
// attributes this loader's components supply, and a stride.
//
// The game's first draws use three attributes and three loaders: a float3 with stride 12, a float2
// with stride 8, a float1 with stride 4. One loader per attribute, which is the simple case, but the
// general form costs little more and the code says what it reads rather than assuming.
#define ALBW_PICA_ATTR_FORMAT_LOW  0x201u
#define ALBW_PICA_ATTR_FORMAT_HIGH 0x202u
#define ALBW_PICA_LOADER(i)        (0x203u + (i) * 3u)
#define ALBW_PICA_FIXED_INDEX      0x232u
// Where the shader starts, in its own instruction words.
#define ALBW_PICA_VS_ENTRY         0x2BAu
#define ALBW_PICA_FIXED_DATA       0x233u

// A component's type, from the two low bits of its format nibble. The sizes are what the hardware
// reads; the conversion to float is the hardware's too -- the integer types arrive as integers, not
// as normalised fractions, which matters for colours submitted as bytes.
static uint32_t component_bytes(uint32_t type) {
    switch (type & 3u) {
        case 0: return 1;                                // signed byte
        case 1: return 1;                                // unsigned byte
        case 2: return 2;                                // signed short
        default: return 4;                               // float
    }
}

static float component_read(uint32_t at, uint32_t type) {
    switch (type & 3u) {
        case 0: return (float)(int8_t)albw_r8(at);
        case 1: return (float)albw_r8(at);
        case 2: return (float)(int16_t)albw_r16(at);
        default: {
            uint32_t bits = albw_r32(at);
            float f;
            memcpy(&f, &bits, sizeof f);
            return f;
        }
    }
}

// The format nibble of one attribute: low two bits the type, high two the component count less one.
static uint32_t attr_format(uint32_t attr) {
    if (attr < 8) return (regs[ALBW_PICA_ATTR_FORMAT_LOW] >> (attr * 4)) & 0xFu;
    return (regs[ALBW_PICA_ATTR_FORMAT_HIGH] >> ((attr - 8) * 4)) & 0xFu;
}

// One vertex's attributes, as the shader will see them: four floats each, the missing components
// filled with 0, 0, 0, 1 the way the hardware fills them.
void albw_pica_fetch_vertex(uint32_t index, struct albw_pica_vertex_in *out) {
    uint32_t total = ((regs[ALBW_PICA_ATTR_FORMAT_HIGH] >> 28) & 0xFu) + 1u;
    uint32_t fixed_mask = (regs[ALBW_PICA_ATTR_FORMAT_HIGH] >> 16) & 0xFFFu;
    uint32_t base = regs[ALBW_PICA_ATTR_BASE] << 3;

    out->count = total;
    for (uint32_t a = 0; a < ALBW_PICA_MAX_ATTRS; a++) {
        out->attr[a][0] = out->attr[a][1] = out->attr[a][2] = 0.0f;
        out->attr[a][3] = 1.0f;
    }
    // A fixed attribute is the same for every vertex and comes from the register set the guest wrote
    // it into rather than from memory.
    for (uint32_t a = 0; a < ALBW_PICA_MAX_ATTRS; a++) {
        if (!(fixed_mask & (1u << a))) continue;
        for (int c = 0; c < 4; c++) out->attr[a][c] = albw_pica_fixed_attr(a, c);
    }

    for (uint32_t l = 0; l < 12; l++) {
        uint32_t offset = regs[ALBW_PICA_LOADER(l)];
        uint32_t low = regs[ALBW_PICA_LOADER(l) + 1];
        uint32_t high = regs[ALBW_PICA_LOADER(l) + 2];
        uint32_t components = (high >> 28) & 0xFu;
        uint32_t stride = (high >> 16) & 0xFFFu;
        if (!components || !stride) continue;

        uint32_t at = physical_to_virtual(base + offset + index * stride);
        if (!at) continue;
        const uint32_t base_at = at;
        for (uint32_t c = 0; c < components && c < 12; c++) {
            uint32_t which = c < 8 ? (low >> (c * 4)) & 0xFu : (high >> ((c - 8) * 4)) & 0xFu;
            if (which >= 12) {
                // 12 to 15 are padding: four to sixteen bytes skipped, no attribute -- from a word
                // boundary, as the attributes themselves start from one of their own size.
                at = base_at + ((at - base_at + 3u) & ~3u);
                at += (which - 11u) * 4u;
                continue;
            }
            uint32_t format = attr_format(which);
            uint32_t type = format & 3u, size = ((format >> 2) & 3u) + 1u;
            uint32_t width = component_bytes(type);
            // Each attribute starts at a multiple of its component's size within the vertex: a short
            // after three bytes, or a float after a short, skips the bytes up to its boundary.
            at = base_at + ((at - base_at + width - 1u) & ~(width - 1u));
            if (!(fixed_mask & (1u << which))) {
                for (uint32_t k = 0; k < size; k++) {
                    out->attr[which][k] = component_read(at + k * width, type);
                }
                if (size < 4) out->attr[which][3] = size == 3 ? 1.0f : out->attr[which][3];
            }
            at += size * width;
        }
    }
}

// The index of vertex `n` of this draw, whether the draw walks the array or an index buffer.
uint32_t albw_pica_vertex_index(uint32_t n, int indexed) {
    if (!indexed) return regs[ALBW_PICA_VERTEX_OFFSET] + n;
    uint32_t config = regs[ALBW_PICA_INDEX_CONFIG];
    uint32_t at = physical_to_virtual((regs[ALBW_PICA_ATTR_BASE] << 3) + (config & 0x7FFFFFFFu));
    if (!at) return 0;
    return (config >> 31) ? albw_r16(at + n * 2) : albw_r8(at + n);
}

// The first draw's vertices, printed. The positions are whatever the guest put in memory, before the
// shader has run, so this says what the shader is fed and not where anything lands on screen.
static void dump_first_vertices(int indexed, uint32_t count) {
    printf("[pica ] the vertices this draw reads:\n");
    for (uint32_t n = 0; n < count && n < 8; n++) {
        uint32_t index = albw_pica_vertex_index(n, indexed);
        struct albw_pica_vertex_in v;
        albw_pica_fetch_vertex(index, &v);
        printf("[pica ]   %u -> index %u:", n, index);
        for (uint32_t a = 0; a < v.count && a < ALBW_PICA_MAX_ATTRS; a++) {
            printf("  a%u(%g %g %g %g)", a, v.attr[a][0], v.attr[a][1], v.attr[a][2], v.attr[a][3]);
        }
        printf("\n");
        // And the same vertex after the shader, which is where it actually goes.
        float out[7][4];
        if (albw_shader_run(regs[ALBW_PICA_VS_ENTRY] & 0xFFFFu, v.attr, v.count, out)) {
            printf("[pica ]     shaded:");
            for (int o = 0; o < 5; o++) {
                printf("  o%d(%g %g %g %g)", o, out[o][0], out[o][1], out[o][2], out[o][3]);
            }
            printf("\n");
        } else {
            uint32_t which = 0;
            printf("[pica ]     the shader did not finish (unsupported opcode %02x)\n",
                   (albw_shader_unsupported(&which), which));
        }
    }
}

static void report_draw(uint32_t trigger) {
    draws++;
    if (!albw_env("ALBW_DUMP_DRAWS")) return;
    // Only the first few of each kind: a settled frame has about fifty and a run has thousands.
    static unsigned long shown[2];
    unsigned which = trigger == ALBW_PICA_DRAW_ELEMENTS;
    if (shown[which]++ >= 6) return;

    uint32_t index_config = regs[ALBW_PICA_INDEX_CONFIG];
    printf("[pica ] draw %lu: %u vertices %s, attribute base %08x",
           draws, regs[ALBW_PICA_VERTEX_COUNT],
           which ? "through an index array" : "in order", regs[ALBW_PICA_ATTR_BASE]);
#ifdef ALBW_TRACK_WRITES
    static int listed;
    if (which && !listed) {
        listed = 1;
        report_recent_writes(regs[ALBW_PICA_VERTEX_COUNT]);
    }
#endif
    static int state_dumped;
    if (!state_dumped) {
        state_dumped = 1;
        dump_geometry_state();
    }
    static int searched;
    if (which && !searched && albw_env("ALBW_FIND_INDICES")) {
        searched = 1;
        find_index_arrays(regs[ALBW_PICA_VERTEX_COUNT]);
    }
    if (which) {
        // The test, kept whatever it says. Six vertices drawn as two triangles have to be small
        // numbers, so if the base and the offset are being read correctly they appear here and if
        // they are not, that appears too. Reading 0x200 as the base does not work; the next
        // candidate is one run away rather than an argument.
        // The offset is added in the *physical* domain and only then mapped back. That is the whole
        // of what the four earlier attempts got wrong: the base register reads 0x03000000, which is
        // physical 0x18000000 -- the start of VRAM -- and the offset is large enough to carry the sum
        // out of VRAM and into FCRAM, which the guest sees somewhere else entirely. Mapping first and
        // adding afterwards lands 0x0A800000 past the end of VRAM's window and reports nonsense.
        int wide = (index_config >> 31) != 0;
        uint32_t at = physical_to_virtual((regs[ALBW_PICA_ATTR_BASE] << 3)
                                          + (index_config & 0x7FFFFFFFu));
        printf(", %u-bit indices at %08x:", wide ? 16 : 8, at);
        uint32_t base = at;
        uint32_t count = regs[ALBW_PICA_VERTEX_COUNT];
        if (base && albw_guest_ok(at, count * (wide ? 2u : 1u))) {
            for (uint32_t i = 0; i < count && i < 12; i++) {
                printf(" %u", wide ? albw_r16(at + i * 2) : albw_r8(at + i));
            }
        } else {
            printf(" outside guest memory");
        }
    }
    printf("\n");
    static int vertices_shown;
    if (!vertices_shown) {
        vertices_shown = 1;
        dump_first_vertices(which, regs[ALBW_PICA_VERTEX_COUNT]);
    }
}

void albw_pica_dump_registers(void) {
    printf("[pica ] register file, after %s:\n", "the state had settled");
    unsigned shown = 0;
    for (uint32_t r = 0; r < ALBW_PICA_REGS; r++) {
        if (!writes_per_reg[r]) continue;
        const char *name = surface_name(regs[r]);
        printf("[pica ]   0x%03x = %08x  (%lu write%s)", r, regs[r], writes_per_reg[r],
               writes_per_reg[r] == 1 ? "" : "s");
        if (name) {
            printf("  <- %s", name);
        } else {
            // Whatever this would be if it were an address: the value in the eight-byte units the
            // hardware uses, mapped from physical back to what the guest sees. The first words
            // there are printed rather than judged, because a value can land in a memory range by
            // coincidence -- the byte pattern 0x03020100 does -- and what is at the address is what
            // tells a buffer from a coincidence.
            uint32_t virt = physical_to_virtual(regs[r] << 3);
            if (virt && albw_guest_ok(virt, 8)) {
                printf("  ? %08x -> %08x %08x", virt, albw_r32(virt), albw_r32(virt + 4));
            }
        }
        printf("\n");
        shown++;
    }
    printf("[pica ] %u register(s) written, %lu draw(s), %lu immediate vertex/vertices, %lu jump(s)\n",
           shown, draws, immediate_vertices, list_jumps);
}

// The two command buffers a list can jump to: their sizes, their addresses, and the registers whose
// write makes the jump, all in the hardware's eight-byte physical units.
//
// The game uses them as a call made of two jumps. The main list points buffer 1 at the word just
// past the jump and buffer 0 at a sub-list it built earlier, then jumps through buffer 0; the
// sub-list ends by jumping through buffer 1, back. Everything this game draws in 3D is in those
// sub-lists -- following only the list the GSP was handed, as this did, ran the state set-up and
// none of the draws, and the screen kept whatever the previous pass had left in the buffer.
#define ALBW_PICA_CMDBUF_SIZE0 0x238u
#define ALBW_PICA_CMDBUF_ADDR0 0x23Au
#define ALBW_PICA_CMDBUF_JUMP0 0x23Cu
#define ALBW_PICA_CMDBUF_JUMP1 0x23Du

void albw_pica_execute(uint32_t addr, uint32_t size) {
    if (!albw_guest_ok(addr, size)) {
        printf("[pica ] command list at %08x+%u is outside guest memory\n", addr, size);
        return;
    }

    int touched_geometry = 0;
    uint32_t at = addr, end = addr + (size & ~7u);
    // A list that jumps to itself would run forever; a real one jumps a handful of times.
    unsigned jumps_left = 100000;
    while (at + 8 <= end) {
        uint32_t data = albw_r32(at);
        uint32_t header = albw_r32(at + 4);
        uint32_t reg = header & 0xFFFFu;
        uint32_t mask = (header >> 16) & 0xFu;
        uint32_t extra = (header >> 20) & 0xFFu;
        int consecutive = (header >> 31) & 1;
        int jumped = 0;

        // Each data word goes to the same register, or to the next one along, depending on the
        // flag. The mask selects which bytes of the register the write reaches.
        for (uint32_t k = 0; k <= extra && !jumped; k++) {
            uint32_t target = consecutive ? reg + k : reg;
            uint32_t value = k == 0 ? data : albw_r32(at + 8 + (k - 1) * 4);
            if (target >= ALBW_PICA_REGS) continue;
            uint32_t keep = 0;
            for (uint32_t b = 0; b < 4; b++) {
                if (!(mask & (1u << b))) keep |= 0xFFu << (b * 8);
            }
            regs[target] = (regs[target] & keep) | (value & ~keep);
            writes_per_reg[target]++;
            if (target >= 0x200u && target <= 0x22Fu) touched_geometry = 1;
            // The shader's program, descriptors and uniforms go past as register writes rather
            // than living in memory, so they have to be caught here.
            switch (target) {
                case 0x2B0u: albw_shader_bool_uniforms(value); break;
                case 0x2B1u: case 0x2B2u: case 0x2B3u: case 0x2B4u:
                    albw_shader_int_uniform(target - 0x2B1u, value); break;
                case 0x2BBu: albw_shader_permutation(value, regs[0x2BCu]); break;
                case 0x2BCu: albw_shader_permutation(regs[0x2BBu], value); break;
                case 0x2C0u: albw_shader_uniform_index(value); break;
                case 0x2C1u: case 0x2C2u: case 0x2C3u: case 0x2C4u:
                case 0x2C5u: case 0x2C6u: case 0x2C7u: case 0x2C8u:
                    albw_shader_uniform_word(value); break;
                case 0x2CBu: albw_shader_code_index(value); break;
                case 0x2CCu: case 0x2CDu: case 0x2CEu: case 0x2CFu:
                case 0x2D0u: case 0x2D1u: case 0x2D2u: case 0x2D3u:
                    albw_shader_code_word(value); break;
                case 0x1C5u: albw_lighting_lut_select(value); break;
                case 0x1C8u: case 0x1C9u: case 0x1CAu: case 0x1CBu:
                case 0x1CCu: case 0x1CDu: case 0x1CEu: case 0x1CFu:
                    albw_lighting_lut_word(value); break;
                case 0x2D5u: albw_shader_opdesc_index(value); break;
                case 0x2D6u: case 0x2D7u: case 0x2D8u: case 0x2D9u:
                case 0x2DAu: case 0x2DBu: case 0x2DCu: case 0x2DDu:
                    albw_shader_opdesc_word(value); break;
                default: break;
            }
            // Where in guest memory a register write came from, for ALBW_PROBE_REGWRITE=reg:value:
            // the command list word that carries it, which a store watch can then trace back to the
            // code that built the list.
            static int prw_checked;
            static const char *prw;
            if (!prw_checked) {
                prw_checked = 1;
                prw = albw_env("ALBW_PROBE_REGWRITE");
            }
            if (prw) {
                char *rest;
                uint32_t want_reg = (uint32_t)strtoul(prw, &rest, 0);
                uint32_t want_val = *rest == ':' ? (uint32_t)strtoul(rest + 1, NULL, 0) : 0;
                static unsigned prw_said;
                if (target == want_reg && value == want_val && prw_said++ < 4) {
                    uint32_t word = k == 0 ? at : at + 8 + (k - 1) * 4;
                    printf("[pica ] register %03x = %08x comes from the list word at %08x\n",
                           target, value, word);
                }
            }
            if (target == ALBW_PICA_FIXED_INDEX) {
                fixed_index = value & 0xFu;
                fixed_held = 0;
            } else if (target >= ALBW_PICA_FIXED_DATA && target <= ALBW_PICA_FIXED_DATA + 2u) {
                fixed_attr_word(value);
            }
            if (target == ALBW_PICA_DRAW_ARRAYS || target == ALBW_PICA_DRAW_ELEMENTS) {
                albw_shader_dump(regs[ALBW_PICA_VS_ENTRY] & 0xFFFFu);
                report_draw(target);
                albw_raster_draw(target == ALBW_PICA_DRAW_ELEMENTS);
            }
            if (target == ALBW_PICA_CMDBUF_JUMP0 || target == ALBW_PICA_CMDBUF_JUMP1) {
                uint32_t which = target - ALBW_PICA_CMDBUF_JUMP0;
                uint32_t to = physical_to_virtual(regs[ALBW_PICA_CMDBUF_ADDR0 + which] << 3);
                uint32_t len = (regs[ALBW_PICA_CMDBUF_SIZE0 + which] & 0xFFFFFu) << 3;
                if (!jumps_left-- || !to || !albw_guest_ok(to, len)) {
                    printf("[pica ] a list jumps to %08x+%u, which is not guest memory; stopping it\n",
                           regs[ALBW_PICA_CMDBUF_ADDR0 + which] << 3, len);
                    return;
                }
                at = to;
                end = to + len;
                jumped = 1;
                list_jumps++;
            }
        }
        if (jumped) continue;

        uint32_t step = 8u + extra * 4u;
        at += (step + 7u) & ~7u;
    }

    // Not the first such list: at that point the frame's fills have not happened yet and nothing
    // is known about the surfaces, so the dump could not name any of them. After a few dozen the
    // state is settled and the addresses are in hand.
    static unsigned long geometry_lists;
    static int dumped;
    if (touched_geometry) geometry_lists++;
    if (geometry_lists >= 50 && !dumped && albw_env("ALBW_DUMP_REGS")) {
        dumped = 1;
        albw_pica_dump_registers();
    }
}

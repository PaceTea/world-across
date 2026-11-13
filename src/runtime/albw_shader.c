#include "albw_shader.h"

#include "albw_kernel.h"
#include "albw_pica.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The program, the operand descriptors and the uniforms, caught as the command list writes them.
static uint32_t code[ALBW_VS_CODE_WORDS];
static uint32_t opdesc[ALBW_VS_OPDESCS];
static float uniforms[ALBW_VS_UNIFORMS][4];
static int32_t int_uniforms[4][4];
static uint32_t bool_uniforms;

static uint32_t code_at, opdesc_at, uniform_at;
static int uniform_float32;
static uint32_t uniform_held;
static uint32_t uniform_words[4];
static uint32_t code_high_water;
static unsigned long packed_uploads, wide_uploads;

// Which input register each attribute is routed to. Identity unless the guest says otherwise.
static uint32_t permutation[ALBW_PICA_MAX_ATTRS];

static void disassemble(FILE *f, uint32_t pc, uint32_t w);

void albw_shader_permutation(uint32_t low, uint32_t high) {
    for (uint32_t a = 0; a < ALBW_PICA_MAX_ATTRS; a++) {
        permutation[a] = a < 8 ? (low >> (a * 4)) & 0xFu : (high >> ((a - 8) * 4)) & 0xFu;
    }
}

void albw_shader_code_index(uint32_t value) { code_at = value & 0x1FFu; }

static int decoded_valid;                                // the decoded program matches the words

void albw_shader_code_word(uint32_t value) {
    if (code_at < ALBW_VS_CODE_WORDS) {
        code[code_at] = value;
        decoded_valid = 0;
        if (code_at + 1 > code_high_water) code_high_water = code_at + 1;
        code_at++;
    }
}

void albw_shader_opdesc_index(uint32_t value) { opdesc_at = value & 0x7Fu; }

void albw_shader_opdesc_word(uint32_t value) {
    if (opdesc_at < ALBW_VS_OPDESCS) {
        opdesc[opdesc_at++] = value;
        decoded_valid = 0;
    }
}

// The index register also says which of the two upload formats the words are in: four IEEE words per
// vector, or four of the hardware's 24-bit floats packed into three. This game uses the first, which
// is why the uniform data reads as recognisable IEEE bit patterns.
void albw_shader_uniform_index(uint32_t value) {
    uniform_at = value & 0x7Fu;
    uniform_float32 = (value >> 31) & 1;
    uniform_held = 0;
}

static unsigned long uniforms_version;

void albw_shader_uniform_word(uint32_t value) {
    uniforms_version++;
    uniform_words[uniform_held++] = value;
    uint32_t need = uniform_float32 ? 4u : 3u;
    if (uniform_held < need) return;
    uniform_held = 0;
    // Every write to the first four uniforms, with the mode and the raw words. They hold the
    // projection matrix, and in one shader they read as not-a-number, so what arrives there is worth
    // seeing rather than guessing at.
    if (albw_env("ALBW_PROBE") && uniform_at < 4) {
        printf("[shader] c%u <- %s %08x %08x %08x %08x\n", uniform_at,
               uniform_float32 ? "wide" : "packed", uniform_words[0], uniform_words[1],
               uniform_words[2], uniform_float32 ? uniform_words[3] : 0);
    }
    if (uniform_at < ALBW_VS_UNIFORMS) {
        if (uniform_float32) {
            // Written x, y, z, w but arriving w first: the hardware fills the vector from the top.
            for (int c = 0; c < 4; c++) {
                memcpy(&uniforms[uniform_at][3 - c], &uniform_words[c], sizeof(float));
            }
            wide_uploads++;
        } else {
            // Packed three words to four values, and filled from w down like the other mode -- which
            // is what the game says: read the other way round, the first four uniforms come out as
            // not-a-number and every position the shader computes with them follows.
            uniforms[uniform_at][3] = albw_pica_float24(uniform_words[0] >> 8);
            uniforms[uniform_at][2] = albw_pica_float24(((uniform_words[0] & 0xFFu) << 16)
                                                        | ((uniform_words[1] >> 16) & 0xFFFFu));
            uniforms[uniform_at][1] = albw_pica_float24(((uniform_words[1] & 0xFFFFu) << 8)
                                                        | ((uniform_words[2] >> 24) & 0xFFu));
            uniforms[uniform_at][0] = albw_pica_float24(uniform_words[2] & 0xFFFFFFu);
            packed_uploads++;
        }
    }
    uniform_at++;
}

void albw_shader_bool_uniforms(uint32_t value) { bool_uniforms = value & 0xFFFFu; }

void albw_shader_int_uniform(uint32_t which, uint32_t value) {
    if (which >= 4) return;
    for (int c = 0; c < 4; c++) int_uniforms[which][c] = (value >> (c * 8)) & 0xFFu;
}

// The program as words, and the uniforms that are not all zero. A disassembler comes next; this is
// what confirms the capture works before anything interprets it.
void albw_shader_dump(uint32_t entry_point) {
    const char *where = albw_env("ALBW_DUMP_SHADER");
    if (!where) return;
    // One file per entry point. The code memory holds several programs at once and is rewritten as
    // the game goes, so a single dump of "the shader" is a dump of whichever one happened to be
    // resident at the first draw -- which is not the one that matters when a later one misbehaves.
    static uint32_t dumped[16];
    static int dumped_count;
    for (int i = 0; i < dumped_count; i++) {
        if (dumped[i] == entry_point) return;
    }
    if (dumped_count >= 16) return;
    dumped[dumped_count++] = entry_point;

    char path[512];
    snprintf(path, sizeof path, "%s/vs-%04x.txt", where, entry_point);
    FILE *f = strcmp(where, "-") ? fopen(path, "w") : stdout;
    if (!f) {
        printf("[shader] cannot write %s\n", where);
        return;
    }
    fprintf(f, "# entry point %u, %u instruction word(s) uploaded\n", entry_point, code_high_water);
    fprintf(f, "# bool uniforms %04x, int uniforms", bool_uniforms);
    for (int i = 0; i < 4; i++) {
        fprintf(f, " (%d %d %d %d)", int_uniforms[i][0], int_uniforms[i][1],
                int_uniforms[i][2], int_uniforms[i][3]);
    }
    fprintf(f, "\n# attribute permutation");
    for (uint32_t a = 0; a < ALBW_PICA_MAX_ATTRS; a++) fprintf(f, " a%u->v%u", a, permutation[a]);
    fprintf(f, "\n# %lu uniform upload(s) as four IEEE words, %lu as three packed 24-bit ones\n",
            wide_uploads, packed_uploads);
    fprintf(f, "code\n");
    for (uint32_t i = 0; i < code_high_water; i++) disassemble(f, i, code[i]);
    fprintf(f, "opdesc\n");
    for (uint32_t i = 0; i < ALBW_VS_OPDESCS; i++) {
        if (opdesc[i]) fprintf(f, "%02x %08x\n", i, opdesc[i]);
    }
    fprintf(f, "uniforms\n");
    for (uint32_t i = 0; i < ALBW_VS_UNIFORMS; i++) {
        if (uniforms[i][0] || uniforms[i][1] || uniforms[i][2] || uniforms[i][3]) {
            fprintf(f, "c%-2u %g %g %g %g\n", i, uniforms[i][0], uniforms[i][1],
                    uniforms[i][2], uniforms[i][3]);
        }
    }
    if (f != stdout) fclose(f);
    printf("[shader] wrote the program at entry %04x to %s\n", entry_point, path);
}

// ---------------------------------------------------------------------------------------------
// The instruction set.
//
// Every field position below was read off this game's own shader rather than taken from a document,
// and each one is confirmed by the program making sense: the first instructions come out as
//
//     mov r14.w, c83.z      ; c83.z is 1.0
//     mov r14.xyz, v0.xyz
//     dp4 r15.x, c0, r14  ... dp4 r15.w, c3, r14
//     dp4 r14.x, c4, r15  ... dp4 r14.w, c7, r15
//     mov o0, r14
//
// which is a vertex being multiplied by two matrices and written to the position output. A wrong
// field position turns that into nonsense immediately, which is what makes this checkable. One field
// was wrong at first and showed it: `mad`'s second source is seven bits at bit 10, not five at bit
// 12, and with the wrong one the program read a vertex attribute the draw does not bind.
#define OP(w)        ((w) >> 26)
#define DESC(w)      ((w) & 0x7Fu)
#define SRC1(w)      (((w) >> 12) & 0x7Fu)
#define SRC2(w)      (((w) >> 7) & 0x1Fu)
#define SRC1_INV(w)  (((w) >> 14) & 0x1Fu)
#define SRC2_INV(w)  (((w) >> 7) & 0x7Fu)
#define ADDR_REG(w)  (((w) >> 19) & 3u)
#define DEST(w)      (((w) >> 21) & 0x1Fu)

// The multiply-add forms keep the opcode in the top three bits, so the destination reaches up into
// what looks like the opcode field elsewhere.
#define MAD_DESC(w)  ((w) & 0x1Fu)
#define MAD_SRC3(w)  (((w) >> 5) & 0x1Fu)
#define MAD_SRC2(w)  (((w) >> 10) & 0x7Fu)
#define MAD_SRC1(w)  (((w) >> 17) & 0x1Fu)
#define MADI_SRC3(w) (((w) >> 5) & 0x7Fu)
#define MADI_SRC2(w) (((w) >> 12) & 0x1Fu)
#define MAD_ADDR(w)  (((w) >> 22) & 3u)
#define MAD_DEST(w)  (((w) >> 24) & 0x1Fu)

// Flow control.
#define FLOW_DEST(w) (((w) >> 10) & 0xFFFu)
#define FLOW_NUM(w)  ((w) & 0xFFu)
#define FLOW_OP(w)   (((w) >> 22) & 3u)
#define FLOW_REFY(w) (((w) >> 24) & 1u)
#define FLOW_REFX(w) (((w) >> 25) & 1u)
#define FLOW_BOOL(w) (((w) >> 22) & 0xFu)
#define FLOW_INT(w)  (((w) >> 22) & 3u)

// Opcodes, by what they do. The arithmetic ones are the low half; flow control is 0x20 upwards; the
// multiply-add forms take the whole top quarter because their destination field runs into it.
enum {
    OP_ADD = 0x00, OP_DP3 = 0x01, OP_DP4 = 0x02, OP_DPH = 0x03, OP_DST = 0x04,
    OP_EX2 = 0x05, OP_LG2 = 0x06, OP_LITP = 0x07, OP_MUL = 0x08, OP_SGE = 0x09,
    OP_SLT = 0x0A, OP_FLR = 0x0B, OP_MAX = 0x0C, OP_MIN = 0x0D, OP_RCP = 0x0E,
    OP_RSQ = 0x0F, OP_MOVA = 0x12, OP_MOV = 0x13,
    OP_DPHI = 0x18, OP_DSTI = 0x19, OP_SGEI = 0x1A, OP_SLTI = 0x1B,
    OP_BREAK = 0x20, OP_NOP = 0x21, OP_END = 0x22, OP_BREAKC = 0x23,
    OP_CALL = 0x24, OP_CALLC = 0x25, OP_CALLU = 0x26, OP_IFU = 0x27, OP_IFC = 0x28,
    OP_LOOP = 0x29, OP_EMIT = 0x2A, OP_SETEMIT = 0x2B, OP_JMPC = 0x2C, OP_JMPU = 0x2D,
    OP_CMP1 = 0x2E, OP_CMP2 = 0x2F
};

static int is_unary(uint32_t op) {
    switch (op) {
        case OP_EX2: case OP_LG2: case OP_LITP: case OP_FLR:
        case OP_RCP: case OP_RSQ: case OP_MOVA: case OP_MOV: return 1;
        default: return 0;
    }
}

static int op_is_call_conditional(uint32_t op) { return op == 0x25u; }   // CALLC

static int is_inverted(uint32_t op) {
    return op == OP_DPHI || op == OP_DSTI || op == OP_SGEI || op == OP_SLTI;
}

// ---------------------------------------------------------------------------------------------
// The disassembler. Kept because it is the only way to see what the game asked for, and because a
// register dump of a shader machine says nothing without it.
static const char MASK[4] = { 'x', 'y', 'z', 'w' };

static void name_src(uint32_t n, char *out) {
    if (n < 0x10) sprintf(out, "v%u", n);
    else if (n < 0x20) sprintf(out, "r%u", n - 0x10);
    else sprintf(out, "c%u", n - 0x20);
}

static void name_dest(uint32_t n, char *out) {
    if (n < 0x10) sprintf(out, "o%u", n);
    else sprintf(out, "r%u", n - 0x10);
}

// A source's swizzle: four two-bit selectors, the first destination component in the high bits.
static void name_swizzle(uint32_t selector, int negate, char *out) {
    int w = 0;
    if (negate) out[w++] = '-';
    for (int i = 0; i < 4; i++) out[w++] = MASK[(selector >> ((3 - i) * 2)) & 3];
    out[w] = '\0';
}

// The destination mask: bit 3 is x, bit 0 is w.
static void name_mask(uint32_t desc, char *out) {
    int w = 0;
    for (int i = 0; i < 4; i++) {
        if (desc & (8u >> i)) out[w++] = MASK[i];
    }
    if (!w) out[w++] = '-';
    out[w] = '\0';
}

static const char *op_name(uint32_t op) {
    switch (op) {
        case OP_ADD: return "add";  case OP_DP3: return "dp3";  case OP_DP4: return "dp4";
        case OP_DPH: return "dph";  case OP_DST: return "dst";  case OP_EX2: return "ex2";
        case OP_LG2: return "lg2";  case OP_LITP: return "litp"; case OP_MUL: return "mul";
        case OP_SGE: return "sge";  case OP_SLT: return "slt";  case OP_FLR: return "flr";
        case OP_MAX: return "max";  case OP_MIN: return "min";  case OP_RCP: return "rcp";
        case OP_RSQ: return "rsq";  case OP_MOVA: return "mova"; case OP_MOV: return "mov";
        case OP_DPHI: return "dphi"; case OP_DSTI: return "dsti";
        case OP_SGEI: return "sgei"; case OP_SLTI: return "slti";
        case OP_BREAK: return "break"; case OP_NOP: return "nop"; case OP_END: return "end";
        case OP_BREAKC: return "breakc"; case OP_CALL: return "call"; case OP_CALLC: return "callc";
        case OP_CALLU: return "callu"; case OP_IFU: return "ifu"; case OP_IFC: return "ifc";
        case OP_LOOP: return "loop"; case OP_EMIT: return "emit"; case OP_SETEMIT: return "setemit";
        case OP_JMPC: return "jmpc"; case OP_JMPU: return "jmpu";
        case OP_CMP1: case OP_CMP2: return "cmp";
        default: return op >= 0x38 ? "mad" : (op >= 0x30 ? "madi" : "?");
    }
}

static const char *cond_name(uint32_t op) {
    switch (op) {
        case 0: return "x||y";
        case 1: return "x&&y";
        case 2: return "x";
        default: return "y";
    }
}

static void disassemble(FILE *f, uint32_t pc, uint32_t w) {
    uint32_t op = OP(w);
    char a[16], b[16], c[16], sa[8], sb[8], sc[8], m[8];
    fprintf(f, "%03x  %08x  ", pc, w);

    if (op >= 0x30) {
        uint32_t d = opdesc[MAD_DESC(w) & (ALBW_VS_OPDESCS - 1)];
        int inverted = op < 0x38;
        name_dest(MAD_DEST(w), a);
        name_src(MAD_SRC1(w), b);
        name_src(inverted ? MADI_SRC2(w) : MAD_SRC2(w), c);
        char d3[16];
        name_src(inverted ? MADI_SRC3(w) : MAD_SRC3(w), d3);
        name_mask(d, m);
        name_swizzle((d >> 5) & 0xFF, (d >> 4) & 1, sa);
        name_swizzle((d >> 14) & 0xFF, (d >> 13) & 1, sb);
        name_swizzle((d >> 23) & 0xFF, (d >> 22) & 1, sc);
        fprintf(f, "%s %s.%s, %s.%s, %s.%s, %s.%s\n", op_name(op), a, m, b, sa, c, sb, d3, sc);
        return;
    }
    if (op < 0x20) {
        uint32_t d = opdesc[DESC(w)];
        int inv = is_inverted(op);
        name_dest(DEST(w), a);
        name_src(inv ? SRC1_INV(w) : SRC1(w), b);
        name_src(inv ? SRC2_INV(w) : SRC2(w), c);
        name_mask(d, m);
        name_swizzle((d >> 5) & 0xFF, (d >> 4) & 1, sa);
        name_swizzle((d >> 14) & 0xFF, (d >> 13) & 1, sb);
        fprintf(f, "%s %s.%s, %s.%s", op_name(op), a, m, b, sa);
        if (!is_unary(op)) fprintf(f, ", %s.%s", c, sb);
        if (ADDR_REG(w)) fprintf(f, "   [+%s]", ADDR_REG(w) == 1 ? "a0.x"
                                              : (ADDR_REG(w) == 2 ? "a0.y" : "aL"));
        fprintf(f, "\n");
        return;
    }
    switch (op) {
        case OP_NOP: case OP_END: case OP_EMIT: case OP_BREAK:
            fprintf(f, "%s\n", op_name(op));
            return;
        case OP_JMPU:
            fprintf(f, "jmpu b%u%s, %03x\n", FLOW_BOOL(w),
                    (FLOW_NUM(w) & 1) ? " == false" : "", FLOW_DEST(w));
            return;
        case OP_CALLU: case OP_IFU:
            fprintf(f, "%s b%u, %03x..%03x\n", op_name(op), FLOW_BOOL(w),
                    FLOW_DEST(w), FLOW_DEST(w) + FLOW_NUM(w));
            return;
        case OP_LOOP:
            fprintf(f, "loop i%u, ..%03x\n", FLOW_INT(w), FLOW_DEST(w));
            return;
        case OP_CMP1: case OP_CMP2: {
            static const char *ops[8] = { "==", "!=", "<", "<=", ">", ">=", "?", "?" };
            uint32_t d = opdesc[DESC(w)];
            name_src(SRC1(w), b);
            name_src(SRC2(w), c);
            name_swizzle((d >> 5) & 0xFF, (d >> 4) & 1, sa);
            name_swizzle((d >> 14) & 0xFF, (d >> 13) & 1, sb);
            fprintf(f, "cmp %s.%s %s/%s %s.%s\n", b, sa, ops[(w >> 24) & 7], ops[(w >> 21) & 7],
                    c, sb);
            return;
        }
        default:
            fprintf(f, "%s %s ref %u%u, %03x..%03x\n", op_name(op), cond_name(FLOW_OP(w)),
                    FLOW_REFX(w), FLOW_REFY(w), FLOW_DEST(w), FLOW_DEST(w) + FLOW_NUM(w));
            return;
    }
}

// ---------------------------------------------------------------------------------------------
// The machine.
//
// Sixteen input registers, sixteen temporaries, the uniforms, seven outputs, two comparison flags and
// three address registers. Flow control is a stack, as it is in the hardware: a call, an `if` and a
// `loop` all push an address at which something happens, and the machine checks the top of the stack
// every instruction. That is not a convenience -- `loop` needs its body's end to come back to, and
// `if` needs the else block's end to skip to, and both can nest.
#define ALBW_VS_STACK 16

struct state {
    float v[16][4];
    float r[16][4];
    float o[7][4];
    int32_t addr[3];                                     // a0.x, a0.y and the loop counter aL
    int cmp[2];
};

struct frame {
    uint32_t until;                                      // the address at which this frame acts
    uint32_t back_to;                                    // where to go when it is done
    uint32_t again_from;                                 // where a loop starts over
    int32_t repeats;
    int32_t step;                                        // what aL gains each time round
};

static unsigned long unsupported_op;
// The other two ways a program fails: it runs past the end of the code memory, or it does not end
// within the step budget -- both of which a flow-control mistake here produces.
static unsigned long ran_off_end, too_long;
static uint32_t unsupported_which;

// ---------------------------------------------------------------------------------------------
// The program, decoded.
//
// Every field an arithmetic instruction has -- its sources, their swizzles and signs from the operand
// descriptor, the destination and its mask, which source the address register offsets -- taken
// apart once when the program or the descriptors change, instead of for every instruction of every
// vertex. Flow control keeps its word and is read as before.
enum { K_ARITH, K_MAD, K_OTHER };

struct decoded {
    uint8_t kind, op;
    uint8_t dest, mask;                                  // mask: x is 8, as the descriptor has it
    uint8_t addr;                                        // 0, or which address register plus one
    uint8_t offset_slot;                                 // the source the address register offsets
    uint8_t src[3];
    uint8_t sel[3][4];
    uint8_t neg[3];
};

static struct decoded program[ALBW_VS_CODE_WORDS];

static void decode_sources(struct decoded *d, uint32_t desc) {
    static const int sel_shift[3] = { 5, 14, 23 }, neg_bit[3] = { 4, 13, 22 };
    for (int k = 0; k < 3; k++) {
        uint32_t selector = (desc >> sel_shift[k]) & 0xFFu;
        for (int i = 0; i < 4; i++) d->sel[k][i] = (uint8_t)((selector >> ((3 - i) * 2)) & 3u);
        d->neg[k] = (uint8_t)((desc >> neg_bit[k]) & 1u);
    }
    d->mask = (uint8_t)(desc & 0xFu);
}

void albw_shader_prepare(void) {
    if (decoded_valid) return;
    for (uint32_t pc = 0; pc < ALBW_VS_CODE_WORDS; pc++) {
        uint32_t w = code[pc], op = OP(w);
        struct decoded *d = &program[pc];
        memset(d, 0, sizeof *d);
        d->op = (uint8_t)op;
        if (op >= 0x30) {
            int inverted = op < 0x38;
            d->kind = K_MAD;
            decode_sources(d, opdesc[MAD_DESC(w) & (ALBW_VS_OPDESCS - 1)]);
            d->src[0] = (uint8_t)MAD_SRC1(w);
            d->src[1] = (uint8_t)(inverted ? MADI_SRC2(w) : MAD_SRC2(w));
            d->src[2] = (uint8_t)(inverted ? MADI_SRC3(w) : MAD_SRC3(w));
            d->addr = (uint8_t)MAD_ADDR(w);
            d->offset_slot = inverted ? 2 : 1;
            d->dest = (uint8_t)MAD_DEST(w);
        } else if (op < 0x20) {
            int inv = is_inverted(op);
            d->kind = K_ARITH;
            decode_sources(d, opdesc[DESC(w)]);
            d->src[0] = (uint8_t)(inv ? SRC1_INV(w) : SRC1(w));
            d->src[1] = (uint8_t)(inv ? SRC2_INV(w) : SRC2(w));
            d->addr = (uint8_t)ADDR_REG(w);
            d->offset_slot = inv ? 1 : 0;
            d->dest = (uint8_t)DEST(w);
        } else {
            d->kind = K_OTHER;
        }
    }
    decoded_valid = 1;
}

static const float zero_vector[4] = { 0, 0, 0, 0 };

// A source operand, swizzled and possibly negated.
static inline void read_src(const struct state *st, uint32_t which, const uint8_t sel[4], int negate,
                            float out[4]) {
    const float *from;
    if (which < 0x10) from = st->v[which];
    else if (which < 0x20) from = st->r[which - 0x10];
    else if (which < 0x20 + ALBW_VS_UNIFORMS) from = uniforms[which - 0x20];
    else from = zero_vector;
    if (negate) {
        for (int i = 0; i < 4; i++) out[i] = -from[sel[i]];
    } else {
        for (int i = 0; i < 4; i++) out[i] = from[sel[i]];
    }
}

// Writes the components the descriptor's mask names, and leaves the rest alone. Anything that is not
// an output or a temporary goes nowhere.
static inline void write_dest(struct state *st, uint32_t which, uint32_t mask, const float value[4]) {
    float *to;
    if (which < 7) to = st->o[which];
    else if (which >= 0x10 && which < 0x20) to = st->r[which - 0x10];
    else return;
    for (int i = 0; i < 4; i++) {
        if (mask & (8u >> i)) to[i] = value[i];
    }
}

static int condition_holds(const struct state *st, uint32_t w) {
    int x = (int)FLOW_REFX(w) == st->cmp[0];
    int y = (int)FLOW_REFY(w) == st->cmp[1];
    switch (FLOW_OP(w)) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
    }
}

static int compare(float a, float b, uint32_t how) {
    switch (how & 7u) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return a < b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return a >= b;
        default: return 1;                               // the two spare codes always hold
    }
}

int albw_shader_run(uint32_t entry_point, const float in[][4], uint32_t attributes,
                    float out[7][4]) {
    albw_shader_prepare();
    struct state st;
    memset(&st, 0, sizeof st);
    // Only the attributes the draw binds. The permutation register holds nothing for the rest, which
    // reads as a route to v0 -- so routing all twelve overwrites the position with an unbound
    // attribute's default of (0, 0, 0, 1) and every vertex collapses to the same point. That is
    // exactly what it did.
    if (attributes > ALBW_PICA_MAX_ATTRS) attributes = ALBW_PICA_MAX_ATTRS;
    for (uint32_t a = 0; a < attributes; a++) {
        uint32_t reg = permutation[a];
        if (reg < 16) memcpy(st.v[reg], in[a], sizeof st.v[0]);
    }

    struct frame stack[ALBW_VS_STACK];
    int depth = 0;
    uint32_t pc = entry_point;

    // The hardware runs a fixed number of instructions at most; a program that does not end is a bug
    // in the capture, not something to hang on.
    for (unsigned long steps = 0; steps < 200000; steps++) {
        // Anything the top of the stack has to do at this address happens before the instruction.
        while (depth > 0 && pc == stack[depth - 1].until) {
            struct frame *top = &stack[depth - 1];
            st.addr[2] += top->step;
            if (top->repeats-- <= 0) {
                pc = top->back_to;
                depth--;
            } else {
                pc = top->again_from;
            }
        }
        if (pc >= ALBW_VS_CODE_WORDS) {
            ran_off_end++;
            return 0;
        }
        const struct decoded *d = &program[pc];
        uint32_t next = pc + 1;

        if (d->kind == K_MAD) {
            // The address register offsets whichever source is the wide one, since that is the only
            // one that can name a uniform and so the only one an index makes sense for.
            uint32_t s[3] = { d->src[0], d->src[1], d->src[2] };
            if (d->addr) s[d->offset_slot] += (uint32_t)st.addr[d->addr - 1];
            float a[4], b[4], c[4], v[4];
            read_src(&st, s[0], d->sel[0], d->neg[0], a);
            read_src(&st, s[1], d->sel[1], d->neg[1], b);
            read_src(&st, s[2], d->sel[2], d->neg[2], c);
            for (int i = 0; i < 4; i++) v[i] = a[i] * b[i] + c[i];
            write_dest(&st, d->dest, d->mask, v);
            pc = next;
            continue;
        }

        if (d->kind == K_ARITH) {
            uint32_t s[2] = { d->src[0], d->src[1] };
            if (d->addr) s[d->offset_slot] += (uint32_t)st.addr[d->addr - 1];
            float a[4], b[4], v[4] = { 0, 0, 0, 0 };
            read_src(&st, s[0], d->sel[0], d->neg[0], a);
            read_src(&st, s[1], d->sel[1], d->neg[1], b);
            switch (d->op) {
                case OP_ADD: for (int i = 0; i < 4; i++) v[i] = a[i] + b[i]; break;
                case OP_MUL: for (int i = 0; i < 4; i++) v[i] = a[i] * b[i]; break;
                case OP_MAX: for (int i = 0; i < 4; i++) v[i] = a[i] > b[i] ? a[i] : b[i]; break;
                case OP_MIN: for (int i = 0; i < 4; i++) v[i] = a[i] < b[i] ? a[i] : b[i]; break;
                case OP_SGE: case OP_SGEI:
                    for (int i = 0; i < 4; i++) v[i] = a[i] >= b[i] ? 1.0f : 0.0f;
                    break;
                case OP_SLT: case OP_SLTI:
                    for (int i = 0; i < 4; i++) v[i] = a[i] < b[i] ? 1.0f : 0.0f;
                    break;
                case OP_FLR: for (int i = 0; i < 4; i++) v[i] = floorf(a[i]); break;
                case OP_MOV: memcpy(v, a, sizeof v); break;
                case OP_DP3: {
                    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
                    for (int i = 0; i < 4; i++) v[i] = dot;
                    break;
                }
                case OP_DP4: {
                    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
                    for (int i = 0; i < 4; i++) v[i] = dot;
                    break;
                }
                case OP_DPH: case OP_DPHI: {
                    // The first vector's w is taken as one, which is how a point is multiplied by a
                    // matrix row without writing the one out.
                    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
                    for (int i = 0; i < 4; i++) v[i] = dot;
                    break;
                }
                case OP_DST: case OP_DSTI:
                    v[0] = 1.0f; v[1] = a[1] * b[1]; v[2] = a[2]; v[3] = b[3];
                    break;
                case OP_EX2: {
                    float e = exp2f(a[0]);
                    for (int i = 0; i < 4; i++) v[i] = e;
                    break;
                }
                case OP_LG2: {
                    float l = log2f(a[0]);
                    for (int i = 0; i < 4; i++) v[i] = l;
                    break;
                }
                case OP_RCP: {
                    float e = a[0] == 0.0f ? 0.0f : 1.0f / a[0];
                    for (int i = 0; i < 4; i++) v[i] = e;
                    break;
                }
                case OP_RSQ: {
                    float e = a[0] <= 0.0f ? 0.0f : 1.0f / sqrtf(a[0]);
                    for (int i = 0; i < 4; i++) v[i] = e;
                    break;
                }
                case OP_LITP:
                    v[0] = a[0] > 0.0f ? a[0] : 0.0f;
                    v[1] = a[1] > 0.0f ? a[1] : 0.0f;
                    v[2] = a[2];
                    v[3] = a[3] > 0.0f ? a[3] : 0.0f;
                    break;
                case OP_MOVA:
                    st.addr[0] = (int32_t)a[0];
                    st.addr[1] = (int32_t)a[1];
                    pc = next;
                    continue;
                default:
                    unsupported_op++;
                    unsupported_which = d->op;
                    return 0;
            }
            write_dest(&st, d->dest, d->mask, v);
            pc = next;
            continue;
        }

        uint32_t w = code[pc];
        uint32_t op = d->op;
        switch (op) {
            case OP_NOP: break;
            case OP_END:
                memcpy(out, st.o, sizeof st.o);
                return 1;
            case OP_CMP1: case OP_CMP2: {
                uint32_t desc = opdesc[DESC(w)];
                float a[4], b[4];
                uint8_t sa[4], sb[4];
                for (int i = 0; i < 4; i++) {
                    sa[i] = (uint8_t)((((desc >> 5) & 0xFFu) >> ((3 - i) * 2)) & 3u);
                    sb[i] = (uint8_t)((((desc >> 14) & 0xFFu) >> ((3 - i) * 2)) & 3u);
                }
                read_src(&st, SRC1(w), sa, (desc >> 4) & 1, a);
                read_src(&st, SRC2(w), sb, (desc >> 13) & 1, b);
                st.cmp[0] = compare(a[0], b[0], (w >> 24) & 7);
                st.cmp[1] = compare(a[1], b[1], (w >> 21) & 7);
                break;
            }
            case OP_JMPC:
                if (condition_holds(&st, w)) next = FLOW_DEST(w);
                break;
            case OP_JMPU:
                // The low bit of the count inverts the test, which is how one opcode serves both
                // "jump if set" and "jump if clear".
                if (((bool_uniforms >> FLOW_BOOL(w)) & 1u) != (FLOW_NUM(w) & 1u)) {
                    next = FLOW_DEST(w);
                }
                break;
            case OP_CALL: case OP_CALLC: case OP_CALLU: {
                int take = op == OP_CALL
                         || (op == OP_CALLC && condition_holds(&st, w))
                         || (op == OP_CALLU && ((bool_uniforms >> FLOW_BOOL(w)) & 1u));
                if (take && depth < ALBW_VS_STACK) {
                    stack[depth].until = FLOW_DEST(w) + FLOW_NUM(w);
                    stack[depth].back_to = next;
                    stack[depth].again_from = FLOW_DEST(w);
                    stack[depth].repeats = 0;
                    stack[depth].step = 0;
                    depth++;
                    next = FLOW_DEST(w);
                }
                break;
            }
            case OP_IFU: case OP_IFC: {
                int take = op == OP_IFU ? (int)((bool_uniforms >> FLOW_BOOL(w)) & 1u)
                                       : condition_holds(&st, w);
                if (depth >= ALBW_VS_STACK) break;
                if (take) {
                    // The true block runs and the else block, which follows it, is skipped.
                    stack[depth].until = FLOW_DEST(w);
                    stack[depth].back_to = FLOW_DEST(w) + FLOW_NUM(w);
                } else {
                    stack[depth].until = FLOW_DEST(w) + FLOW_NUM(w);
                    stack[depth].back_to = FLOW_DEST(w) + FLOW_NUM(w);
                    next = FLOW_DEST(w);
                }
                stack[depth].again_from = stack[depth].until;
                stack[depth].repeats = 0;
                stack[depth].step = 0;
                depth++;
                break;
            }
            case OP_LOOP: {
                if (depth >= ALBW_VS_STACK) break;
                uint32_t which = FLOW_INT(w);
                st.addr[2] = int_uniforms[which][1];     // the counter starts at y
                stack[depth].until = FLOW_DEST(w) + 1u;
                stack[depth].back_to = FLOW_DEST(w) + 1u;
                stack[depth].again_from = next;
                stack[depth].repeats = int_uniforms[which][0];
                stack[depth].step = int_uniforms[which][2];
                depth++;
                break;
            }
            case OP_BREAK: case OP_BREAKC:
                if (op == OP_BREAK || condition_holds(&st, w)) {
                    if (depth > 0) {
                        next = stack[depth - 1].back_to;
                        depth--;
                    }
                }
                break;
            case OP_EMIT: case OP_SETEMIT:
                // Geometry shader only, and this game runs none: 0x229 says the stage is off.
                break;
            default:
                unsupported_op++;
                unsupported_which = op;
                return 0;
        }
        pc = next;
    }
    too_long++;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Four vertices at once.
//
// The same program, run on four vertices side by side: every register component holds one lane per
// vertex, so a swizzle is a choice of which vector to take, and the arithmetic is the processor's
// four-wide arithmetic doing exactly what the one-wide version does to each lane, in the same order.
// The results are the same bits. A branch on a comparison whose lanes disagree cannot be taken for
// all four; then this gives up and the caller runs them one at a time.
typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));

struct state4 {
    v4f v[16][4];
    v4f r[16][4];
    v4f o[7][4];
    int32_t addr[3][4];
    int cmp[2][4];
};

// The uniforms as the lanes see them: each component repeated four times, made once per program
// run of a draw rather than per instruction.
static v4f uniforms4[ALBW_VS_UNIFORMS][4];
static unsigned long uniforms4_version = ~0ul;

void albw_shader_prepare_lanes(void) {
    albw_shader_prepare();
    if (uniforms4_version == uniforms_version) return;
    for (uint32_t u = 0; u < ALBW_VS_UNIFORMS; u++) {
        for (int c = 0; c < 4; c++) {
            float x = uniforms[u][c];
            uniforms4[u][c] = (v4f){ x, x, x, x };
        }
    }
    uniforms4_version = uniforms_version;
}

static const v4f zero4[4];

static inline const v4f *region4(const struct state4 *st, uint32_t which) {
    if (which < 0x10) return st->v[which];
    if (which < 0x20) return st->r[which - 0x10];
    if (which < 0x20 + ALBW_VS_UNIFORMS) return uniforms4[which - 0x20];
    return zero4;
}

// The scalar a lane sees at register `which`, component `c`.
static inline float lane_value(const struct state4 *st, uint32_t which, int c, int lane) {
    if (which < 0x10) return st->v[which][c][lane];
    if (which < 0x20) return st->r[which - 0x10][c][lane];
    if (which < 0x20 + ALBW_VS_UNIFORMS) return uniforms[which - 0x20][c];
    return 0.0f;
}

// A source for all four lanes. With the address register in play and the lanes disagreeing on it,
// each lane reads its own register.
static inline void read_src4(const struct state4 *st, const struct decoded *d, int slot,
                             v4f out[4]) {
    uint32_t which = d->src[slot];
    if (d->addr && d->offset_slot == slot) {
        const int32_t *a = st->addr[d->addr - 1];
        if (a[0] != a[1] || a[0] != a[2] || a[0] != a[3]) {
            for (int c = 0; c < 4; c++) {
                for (int l = 0; l < 4; l++) {
                    out[c][l] = lane_value(st, which + (uint32_t)a[l], d->sel[slot][c], l);
                }
                if (d->neg[slot]) out[c] = -out[c];
            }
            return;
        }
        which += (uint32_t)a[0];
    }
    const v4f *from = region4(st, which);
    for (int c = 0; c < 4; c++) out[c] = from[d->sel[slot][c]];
    if (d->neg[slot]) for (int c = 0; c < 4; c++) out[c] = -out[c];
}

static inline void write_dest4(struct state4 *st, uint32_t which, uint32_t mask, const v4f v[4]) {
    v4f *to;
    if (which < 7) to = st->o[which];
    else if (which >= 0x10 && which < 0x20) to = st->r[which - 0x10];
    else return;
    for (int c = 0; c < 4; c++) {
        if (mask & (8u >> c)) to[c] = v[c];
    }
}

static inline v4f select4(v4i m, v4f a, v4f b) {
    return (v4f)((m & (v4i)a) | (~m & (v4i)b));
}

// Whether a condition holds for all four lanes (1), none (0), or some (-1).
static int condition4(const struct state4 *st, uint32_t w) {
    int any = 0, all = 1;
    for (int l = 0; l < 4; l++) {
        int x = (int)FLOW_REFX(w) == st->cmp[0][l];
        int y = (int)FLOW_REFY(w) == st->cmp[1][l];
        int holds;
        switch (FLOW_OP(w)) {
            case 0: holds = x || y; break;
            case 1: holds = x && y; break;
            case 2: holds = x; break;
            default: holds = y; break;
        }
        any |= holds;
        all &= holds;
    }
    return all ? 1 : (any ? -1 : 0);
}

int albw_shader_run4(uint32_t entry_point, const float (*const in[4])[4], uint32_t attributes,
                     float out[4][7][4]) {
    albw_shader_prepare_lanes();
    struct state4 st;
    memset(&st, 0, sizeof st);
    if (attributes > ALBW_PICA_MAX_ATTRS) attributes = ALBW_PICA_MAX_ATTRS;
    for (uint32_t a = 0; a < attributes; a++) {
        uint32_t reg = permutation[a];
        if (reg >= 16) continue;
        for (int c = 0; c < 4; c++) {
            st.v[reg][c] = (v4f){ in[0][a][c], in[1][a][c], in[2][a][c], in[3][a][c] };
        }
    }

    struct frame stack[ALBW_VS_STACK];
    int depth = 0;
    uint32_t pc = entry_point;
    const v4f one = { 1.0f, 1.0f, 1.0f, 1.0f }, nil = { 0.0f, 0.0f, 0.0f, 0.0f };

    for (unsigned long steps = 0; steps < 200000; steps++) {
        while (depth > 0 && pc == stack[depth - 1].until) {
            struct frame *top = &stack[depth - 1];
            for (int l = 0; l < 4; l++) st.addr[2][l] += top->step;
            if (top->repeats-- <= 0) {
                pc = top->back_to;
                depth--;
            } else {
                pc = top->again_from;
            }
        }
        if (pc >= ALBW_VS_CODE_WORDS) return 0;
        const struct decoded *d = &program[pc];
        uint32_t next = pc + 1;

        if (d->kind == K_MAD) {
            v4f a[4], b[4], c[4], v[4];
            read_src4(&st, d, 0, a);
            read_src4(&st, d, 1, b);
            read_src4(&st, d, 2, c);
            for (int i = 0; i < 4; i++) v[i] = a[i] * b[i] + c[i];
            write_dest4(&st, d->dest, d->mask, v);
            pc = next;
            continue;
        }

        if (d->kind == K_ARITH) {
            v4f a[4], b[4], v[4] = { nil, nil, nil, nil };
            read_src4(&st, d, 0, a);
            read_src4(&st, d, 1, b);
            switch (d->op) {
                case OP_ADD: for (int i = 0; i < 4; i++) v[i] = a[i] + b[i]; break;
                case OP_MUL: for (int i = 0; i < 4; i++) v[i] = a[i] * b[i]; break;
                case OP_MAX: for (int i = 0; i < 4; i++) v[i] = select4(a[i] > b[i], a[i], b[i]); break;
                case OP_MIN: for (int i = 0; i < 4; i++) v[i] = select4(a[i] < b[i], a[i], b[i]); break;
                case OP_SGE: case OP_SGEI:
                    for (int i = 0; i < 4; i++) v[i] = select4(a[i] >= b[i], one, nil);
                    break;
                case OP_SLT: case OP_SLTI:
                    for (int i = 0; i < 4; i++) v[i] = select4(a[i] < b[i], one, nil);
                    break;
                case OP_FLR:
                    for (int i = 0; i < 4; i++) for (int l = 0; l < 4; l++) v[i][l] = floorf(a[i][l]);
                    break;
                case OP_MOV: for (int i = 0; i < 4; i++) v[i] = a[i]; break;
                case OP_DP3: {
                    v4f dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
                    for (int i = 0; i < 4; i++) v[i] = dot;
                    break;
                }
                case OP_DP4: {
                    v4f dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
                    for (int i = 0; i < 4; i++) v[i] = dot;
                    break;
                }
                case OP_DPH: case OP_DPHI: {
                    v4f dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
                    for (int i = 0; i < 4; i++) v[i] = dot;
                    break;
                }
                case OP_DST: case OP_DSTI:
                    v[0] = one; v[1] = a[1] * b[1]; v[2] = a[2]; v[3] = b[3];
                    break;
                case OP_EX2: {
                    v4f e;
                    for (int l = 0; l < 4; l++) e[l] = exp2f(a[0][l]);
                    for (int i = 0; i < 4; i++) v[i] = e;
                    break;
                }
                case OP_LG2: {
                    v4f e;
                    for (int l = 0; l < 4; l++) e[l] = log2f(a[0][l]);
                    for (int i = 0; i < 4; i++) v[i] = e;
                    break;
                }
                case OP_RCP: {
                    v4f e;
                    for (int l = 0; l < 4; l++) e[l] = a[0][l] == 0.0f ? 0.0f : 1.0f / a[0][l];
                    for (int i = 0; i < 4; i++) v[i] = e;
                    break;
                }
                case OP_RSQ: {
                    v4f e;
                    for (int l = 0; l < 4; l++) e[l] = a[0][l] <= 0.0f ? 0.0f : 1.0f / sqrtf(a[0][l]);
                    for (int i = 0; i < 4; i++) v[i] = e;
                    break;
                }
                case OP_LITP:
                    for (int l = 0; l < 4; l++) {
                        v[0][l] = a[0][l] > 0.0f ? a[0][l] : 0.0f;
                        v[1][l] = a[1][l] > 0.0f ? a[1][l] : 0.0f;
                        v[2][l] = a[2][l];
                        v[3][l] = a[3][l] > 0.0f ? a[3][l] : 0.0f;
                    }
                    break;
                case OP_MOVA:
                    for (int l = 0; l < 4; l++) {
                        st.addr[0][l] = (int32_t)a[0][l];
                        st.addr[1][l] = (int32_t)a[1][l];
                    }
                    pc = next;
                    continue;
                default:
                    return 0;
            }
            write_dest4(&st, d->dest, d->mask, v);
            pc = next;
            continue;
        }

        uint32_t w = code[pc];
        switch (d->op) {
            case OP_NOP: break;
            case OP_END:
                for (int l = 0; l < 4; l++) {
                    for (int o = 0; o < 7; o++) {
                        for (int c = 0; c < 4; c++) out[l][o][c] = st.o[o][c][l];
                    }
                }
                return 1;
            case OP_CMP1: case OP_CMP2: {
                uint32_t desc = opdesc[DESC(w)];
                struct decoded cd;
                memset(&cd, 0, sizeof cd);
                decode_sources(&cd, desc);
                cd.src[0] = (uint8_t)SRC1(w);
                cd.src[1] = (uint8_t)SRC2(w);
                v4f a[4], b[4];
                read_src4(&st, &cd, 0, a);
                read_src4(&st, &cd, 1, b);
                for (int l = 0; l < 4; l++) {
                    st.cmp[0][l] = compare(a[0][l], b[0][l], (w >> 24) & 7);
                    st.cmp[1][l] = compare(a[1][l], b[1][l], (w >> 21) & 7);
                }
                break;
            }
            case OP_JMPC: {
                int c = condition4(&st, w);
                if (c < 0) return -1;
                if (c) next = FLOW_DEST(w);
                break;
            }
            case OP_JMPU:
                if (((bool_uniforms >> FLOW_BOOL(w)) & 1u) != (FLOW_NUM(w) & 1u)) next = FLOW_DEST(w);
                break;
            case OP_CALL: case OP_CALLC: case OP_CALLU: {
                int take;
                if (op_is_call_conditional(d->op)) {
                    int c = condition4(&st, w);
                    if (c < 0) return -1;
                    take = c;
                } else {
                    take = d->op == OP_CALL || ((bool_uniforms >> FLOW_BOOL(w)) & 1u);
                }
                if (take && depth < ALBW_VS_STACK) {
                    stack[depth].until = FLOW_DEST(w) + FLOW_NUM(w);
                    stack[depth].back_to = next;
                    stack[depth].again_from = FLOW_DEST(w);
                    stack[depth].repeats = 0;
                    stack[depth].step = 0;
                    depth++;
                    next = FLOW_DEST(w);
                }
                break;
            }
            case OP_IFU: case OP_IFC: {
                int take;
                if (d->op == OP_IFU) {
                    take = (int)((bool_uniforms >> FLOW_BOOL(w)) & 1u);
                } else {
                    take = condition4(&st, w);
                    if (take < 0) return -1;
                }
                if (depth >= ALBW_VS_STACK) break;
                if (take) {
                    stack[depth].until = FLOW_DEST(w);
                    stack[depth].back_to = FLOW_DEST(w) + FLOW_NUM(w);
                } else {
                    stack[depth].until = FLOW_DEST(w) + FLOW_NUM(w);
                    stack[depth].back_to = FLOW_DEST(w) + FLOW_NUM(w);
                    next = FLOW_DEST(w);
                }
                stack[depth].again_from = stack[depth].until;
                stack[depth].repeats = 0;
                stack[depth].step = 0;
                depth++;
                break;
            }
            case OP_LOOP: {
                if (depth >= ALBW_VS_STACK) break;
                uint32_t which = FLOW_INT(w);
                for (int l = 0; l < 4; l++) st.addr[2][l] = int_uniforms[which][1];
                stack[depth].until = FLOW_DEST(w) + 1u;
                stack[depth].back_to = FLOW_DEST(w) + 1u;
                stack[depth].again_from = next;
                stack[depth].repeats = int_uniforms[which][0];
                stack[depth].step = int_uniforms[which][2];
                depth++;
                break;
            }
            case OP_BREAK: case OP_BREAKC: {
                int take = 1;
                if (d->op == OP_BREAKC) {
                    take = condition4(&st, w);
                    if (take < 0) return -1;
                }
                if (take && depth > 0) {
                    next = stack[depth - 1].back_to;
                    depth--;
                }
                break;
            }
            case OP_EMIT: case OP_SETEMIT:
                break;
            default:
                return 0;
        }
        pc = next;
    }
    return 0;
}

unsigned long albw_shader_unsupported(uint32_t *which) {
    if (which) *which = unsupported_which;
    return unsupported_op;
}

void albw_shader_failures(unsigned long *off_end, unsigned long *long_run) {
    if (off_end) *off_end = ran_off_end;
    if (long_run) *long_run = too_long;
}

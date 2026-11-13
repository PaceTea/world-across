#include "albw_lighting.h"

#include "albw_pica.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Registers.
//
// The lighting block starts at 0x140 with eight lights of sixteen registers each, then the global
// state. Positions are the hardware's 16-bit floats, spot directions 13-bit fixed point, the
// distance attenuation bias and scale 20-bit floats, colours 8 bits a channel in 10-bit fields.
#define R_LIGHT(i)          (0x140u + (i) * 0x10u)
#define R_GLOBAL_AMBIENT    0x1C0u
#define R_NUM_LIGHTS        0x1C2u                       // the number of lights, less one
#define R_CONFIG0           0x1C3u
#define R_CONFIG1           0x1C4u
#define R_LUT_SELECT        0x1C5u
#define R_DISABLE           0x1C6u
#define R_LUT_ABS           0x1D0u                       // a set bit turns absolute input off
#define R_LUT_INPUT         0x1D1u
#define R_LUT_SCALE         0x1D2u
#define R_LIGHT_SLOTS       0x1D9u                       // which light each enabled slot is
#define R_LIGHTING_ON       0x08Fu

static uint32_t reg(uint32_t index) { return albw_pica_reg(index); }

// ---------------------------------------------------------------------------------------------
// The lookup tables: twenty-four of 256 entries. An entry is a 12-bit value and a 12-bit signed
// difference to the next, both in units of 1/4095, so a lookup between entries is a value plus a
// fraction of the difference.
//
// Draws are rasterised in batches, after the command list has moved on, and the game rewrites its
// tables between draws -- thirty-odd times a frame on the title screen. So a draw's snapshot keeps
// pointers to the tables as they were, and the first write to a table some snapshot still points at
// copies it first. The copies are handed back once the batch is drawn.
#define LUTS ALBW_LIGHTING_LUTS
static uint32_t initial_luts[LUTS][256];
static uint32_t *luts[LUTS];                             // each table's current contents
static int lut_shared[LUTS];                             // a waiting snapshot points at luts[t]
static uint32_t **lut_spare, **lut_retired;
static uint32_t spare_count, spare_room, retired_count, retired_room;
static uint32_t lut_type, lut_index;

static void luts_init(void) {
    if (luts[0]) return;
    for (int t = 0; t < LUTS; t++) luts[t] = initial_luts[t];
}

static int keep(uint32_t ***list, uint32_t *count, uint32_t *room, uint32_t *table) {
    if (*count == *room) {
        uint32_t bigger_room = *room ? *room * 2u : 64u;
        uint32_t **bigger = realloc(*list, bigger_room * sizeof *bigger);
        if (!bigger) return 0;
        *list = bigger;
        *room = bigger_room;
    }
    (*list)[(*count)++] = table;
    return 1;
}

void albw_lighting_lut_select(uint32_t value) {
    lut_index = value & 0xFFu;
    lut_type = (value >> 8) & 0x1Fu;
}

void albw_lighting_lut_word(uint32_t value) {
    luts_init();
    if (lut_type < LUTS) {
        if (lut_shared[lut_type]) {
            uint32_t *fresh = spare_count ? lut_spare[--spare_count] : malloc(256 * sizeof *fresh);
            // Out of memory, the table is written in place: a waiting draw sees the new contents,
            // which is wrong for one batch rather than fatal.
            if (fresh && keep(&lut_retired, &retired_count, &retired_room, luts[lut_type])) {
                memcpy(fresh, luts[lut_type], 256 * sizeof *fresh);
                luts[lut_type] = fresh;
            }
            lut_shared[lut_type] = 0;
        }
        luts[lut_type][lut_index] = value;
    }
    lut_index = (lut_index + 1u) & 0xFFu;
}

void albw_lighting_release(void) {
    while (retired_count) {
        uint32_t *table = lut_retired[--retired_count];
        if (!keep(&lut_spare, &spare_count, &spare_room, table)) break;
    }
    memset(lut_shared, 0, sizeof lut_shared);
}

static float lut_lookup(const struct albw_lighting_state *ls, uint32_t table, uint32_t index,
                        float delta) {
    if (table >= LUTS) return 1.0f;
    uint32_t e = ls->lut[table][index & 0xFFu];
    float value = (float)(e & 0xFFFu) / 4095.0f;
    int32_t diff = (int32_t)((e >> 12) & 0xFFFu);
    if (diff & 0x800) diff -= 0x1000;
    return value + (float)diff / 4095.0f * delta;
}

// Which tables a sampler is: two distributions, the Fresnel term, three reflection colours, then a
// spotlight table and a distance attenuation table for each light.
enum { LUT_D0 = 0, LUT_D1 = 1, LUT_FRESNEL = 3, LUT_RB = 4, LUT_RG = 5, LUT_RR = 6,
       LUT_SPOT = 8, LUT_DIST = 16 };

// The eight configurations say which samplers a draw may use at all; a sampler the configuration
// lacks reads as one, whatever its enable bit says.
static int sampler_supported(uint32_t config, int sampler) {
    switch (sampler) {
        case LUT_D0: return config != 1;
        case LUT_D1: return config != 0 && config != 1 && config != 5;
        case LUT_SPOT: return config != 2 && config != 3;
        case LUT_FRESNEL: return config != 0 && config != 2 && config != 4;
        case LUT_RR: return config != 3;
        default: return config == 4 || config == 5 || config == 8;   // the green and blue ones
    }
}

// ---------------------------------------------------------------------------------------------
// The hardware's small floats.
static float small_float(uint32_t raw, int mantissa_bits, int exponent_bits) {
    uint32_t mantissa = raw & ((1u << mantissa_bits) - 1u);
    uint32_t exponent = (raw >> mantissa_bits) & ((1u << exponent_bits) - 1u);
    uint32_t sign = (raw >> (mantissa_bits + exponent_bits)) & 1u;
    int bias = (1 << (exponent_bits - 1)) - 1;
    float value;
    if (exponent == 0) {
        value = ldexpf((float)mantissa, 1 - bias - mantissa_bits);
    } else {
        value = ldexpf(1.0f + (float)mantissa / (float)(1u << mantissa_bits), (int)exponent - bias);
    }
    return sign ? -value : value;
}

static float float16(uint32_t raw) { return small_float(raw & 0xFFFFu, 10, 5); }
static float float20(uint32_t raw) { return small_float(raw & 0xFFFFFu, 12, 7); }

static float fixed13(uint32_t raw) {
    int32_t v = (int32_t)(raw & 0x1FFFu);
    if (v & 0x1000) v -= 0x2000;
    return (float)v / 2047.0f;
}

static void light_colour(uint32_t raw, float out[3]) {
    // Ten bits a channel, but 255 is full.
    out[0] = (float)((raw >> 20) & 0x3FFu) / 255.0f;
    out[1] = (float)((raw >> 10) & 0x3FFu) / 255.0f;
    out[2] = (float)(raw & 0x3FFu) / 255.0f;
}

// ---------------------------------------------------------------------------------------------
// Vectors.
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void normalise(float v[3]) {
    float len = sqrtf(dot3(v, v));
    if (len > 0.0f) {
        v[0] /= len; v[1] /= len; v[2] /= len;
    }
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Rotates v by the quaternion q (x, y, z, w): v + 2 q.xyz x (q.xyz x v + q.w v).
static void quat_rotate(const float q[4], const float v[3], float out[3]) {
    float t[3], u[3];
    cross3(q, v, t);
    for (int i = 0; i < 3; i++) t[i] += q[3] * v[i];
    cross3(q, t, u);
    for (int i = 0; i < 3; i++) out[i] = v[i] + 2.0f * u[i];
}

int albw_lighting_enabled(void) {
    return (reg(R_LIGHTING_ON) & 1u) && !(reg(R_DISABLE) & 1u);
}

// ---------------------------------------------------------------------------------------------
// The draw's lighting state, decoded once.
//
// Every register the computation below reads, turned into the floats and flags it uses, when a draw
// starts. Decoding them per fragment -- the small floats go through ldexpf -- was most of what a
// lit pixel cost, and nothing in a draw can change them.
enum { S_D0 = 0, S_D1 = 1, S_SPOT = 2, S_FRESNEL = 3, S_RB = 4, S_RG = 5, S_RR = 6 };

void albw_lighting_snapshot(struct albw_lighting_state *out) {
    struct albw_lighting_state ls;
    static const float scales[8] = { 1.0f, 2.0f, 4.0f, 8.0f, 0.0f, 0.0f, 0.25f, 0.5f };
    uint32_t config0 = reg(R_CONFIG0), config1 = reg(R_CONFIG1);
    ls.config0 = config0;
    ls.config = (config0 >> 4) & 0xFu;
    ls.bump_mode = (config0 >> 28) & 3u;
    ls.bump_unit = (config0 >> 22) & 3u;
    ls.rebuild_z = !((config0 >> 30) & 1u);
    ls.clamp_highlights = (config0 >> 27) & 1u;
    uint32_t config = ls.config;
    ls.use_d0 = !((config1 >> 16) & 1u) && sampler_supported(config, LUT_D0);
    ls.use_d1 = !((config1 >> 17) & 1u) && sampler_supported(config, LUT_D1);
    // The disable bits run red, green, blue from bit 20 -- the opposite order of the tables.
    ls.use_rr = !((config1 >> 20) & 1u) && sampler_supported(config, LUT_RR);
    ls.use_rg = !((config1 >> 21) & 1u) && sampler_supported(config, LUT_RG);
    ls.use_rb = !((config1 >> 22) & 1u) && sampler_supported(config, LUT_RB);
    ls.use_fresnel = !((config1 >> 19) & 1u) && sampler_supported(config, LUT_FRESNEL);
    for (int k = 0; k < 7; k++) {
        int shift = k * 4;
        ls.sampler[k].input = (reg(R_LUT_INPUT) >> shift) & 7u;
        ls.sampler[k].absolute = !((reg(R_LUT_ABS) >> (shift + 1)) & 1u);
        ls.sampler[k].scale = scales[(reg(R_LUT_SCALE) >> shift) & 7u];
    }

    ls.lights = (reg(R_NUM_LIGHTS) & 7u) + 1u;
    for (uint32_t slot = 0; slot < ls.lights; slot++) {
        struct albw_light *L = &ls.light[slot];
        uint32_t num = (reg(R_LIGHT_SLOTS) >> (slot * 4u)) & 7u;
        uint32_t base = R_LIGHT(num);
        L->num = num;
        L->light_config = reg(base + 9);
        L->position[0] = float16(reg(base + 4));
        L->position[1] = float16(reg(base + 4) >> 16);
        L->position[2] = float16(reg(base + 5));
        L->spot[0] = fixed13(reg(base + 6));
        L->spot[1] = fixed13(reg(base + 6) >> 16);
        L->spot[2] = fixed13(reg(base + 7));
        L->use_dist = !((config1 >> (24 + num)) & 1u);
        L->use_spot = !((config1 >> (8 + num)) & 1u) && sampler_supported(config, LUT_SPOT);
        L->atten_scale = float20(reg(base + 11));
        L->atten_bias = float20(reg(base + 10));
        light_colour(reg(base + 0), L->spec0);
        light_colour(reg(base + 1), L->spec1);
        light_colour(reg(base + 2), L->diffuse);
        light_colour(reg(base + 3), L->ambient);
    }
    light_colour(reg(R_GLOBAL_AMBIENT), ls.global);
    luts_init();
    for (int t = 0; t < LUTS; t++) {
        ls.lut[t] = luts[t];
        lut_shared[t] = 1;
    }
    *out = ls;
}

// One lookup: the input the register names for this sampler, made absolute or not, scaled.
static float lut_sample(const struct albw_lighting_state *ls, uint32_t table,
                        const struct albw_lighting_sampler *sp, const float n[3], const float v[3],
                        const float l[3], const float h[3], const float spot[3],
                        const float tangent[3]) {
    float x;
    switch (sp->input) {
        case 0: x = dot3(n, h); break;                   // normal and half vector
        case 1: x = dot3(v, h); break;                   // view and half vector
        case 2: x = dot3(n, v); break;                   // normal and view
        case 3: x = dot3(l, n); break;                   // light and normal
        case 4: x = dot3(l, spot); break;                // light and spot direction
        case 5: {                                        // the half vector's angle about the normal
            if (ls->config != 8) { x = 0.0f; break; }
            float proj[3];
            float nh = dot3(n, h);
            for (int i = 0; i < 3; i++) proj[i] = h[i] - n[i] * nh;
            x = dot3(proj, tangent);
            break;
        }
        default: x = 0.0f; break;
    }

    uint32_t index;
    float delta;
    if (sp->absolute) {
        if (x < 0.0f) x = -x;
        float f = floorf(x * 256.0f);
        if (f < 0.0f) f = 0.0f;
        if (f > 255.0f) f = 255.0f;
        index = (uint32_t)f;
        delta = x * 256.0f - f;
    } else {
        float f = floorf(x * 128.0f);
        if (f < -128.0f) f = -128.0f;
        if (f > 127.0f) f = 127.0f;
        index = (uint32_t)(int32_t)f & 0xFFu;
        delta = x * 128.0f - f;
    }
    return sp->scale * lut_lookup(ls, table, index, delta);
}

void albw_lighting_compute(const struct albw_lighting_state *ls, const float quat[4],
                           const float view[3], const uint32_t textures[4], uint32_t *primary,
                           uint32_t *secondary) {
    float q[4];
    memcpy(q, quat, sizeof q);
    float qlen = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (qlen > 0.0f) for (int i = 0; i < 4; i++) q[i] /= qlen;

    // The surface's own normal is straight out of it, unless a texture unit holds a normal map or a
    // tangent map -- the title scene's ground takes its normals from unit 1, whose texels sit around
    // (128, 128, 255) as a normal map's do. Either way the quaternion turns the result into eye space.
    float surface_n[3] = { 0.0f, 0.0f, 1.0f }, surface_t[3] = { 1.0f, 0.0f, 0.0f };
    if (ls->bump_mode == 1u || ls->bump_mode == 2u) {
        float p[3];
        for (int i = 0; i < 3; i++) {
            p[i] = 2.0f * (float)((textures[ls->bump_unit] >> (8 * i)) & 0xFFu) / 255.0f - 1.0f;
        }
        if (ls->bump_mode == 1u) {
            memcpy(surface_n, p, sizeof p);
            // Unless told not to, z is rebuilt from x and y, which is the more precise of the two.
            if (ls->rebuild_z) {
                float zz = 1.0f - p[0] * p[0] - p[1] * p[1];
                surface_n[2] = zz > 0.0f ? sqrtf(zz) : 0.0f;
            }
        } else {
            memcpy(surface_t, p, sizeof p);
        }
    }
    float n[3], tangent[3];
    quat_rotate(q, surface_n, n);
    quat_rotate(q, surface_t, tangent);

    float v[3] = { view[0], view[1], view[2] };
    float norm_view[3] = { v[0], v[1], v[2] };
    normalise(norm_view);

    float diffuse_sum[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float specular_sum[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    for (uint32_t slot = 0; slot < ls->lights; slot++) {
        const struct albw_light *L = &ls->light[slot];
        uint32_t num = L->num;
        uint32_t light_config = L->light_config;

        float l[3];
        if (light_config & 1u) {                         // directional
            memcpy(l, L->position, sizeof l);
        } else {
            for (int i = 0; i < 3; i++) l[i] = L->position[i] + v[i];
        }
        normalise(l);
        float h[3] = { norm_view[0] + l[0], norm_view[1] + l[1], norm_view[2] + l[2] };
        float h_len2 = dot3(h, h);
        float h_norm[3] = { h[0], h[1], h[2] };
        normalise(h_norm);

        float dist_atten = 1.0f;
        if (L->use_dist) {
            float d[3] = { -v[0] - L->position[0], -v[1] - L->position[1], -v[2] - L->position[2] };
            float distance = sqrtf(dot3(d, d));
            float at = L->atten_scale * distance + L->atten_bias;
            if (at < 0.0f) at = 0.0f;
            if (at > 1.0f) at = 1.0f;
            float f = floorf(at * 256.0f);
            if (f > 255.0f) f = 255.0f;
            dist_atten = lut_lookup(ls, LUT_DIST + num, (uint32_t)f, at * 256.0f - f);
        }

        float spot_atten = 1.0f;
        if (L->use_spot) {
            spot_atten = lut_sample(ls, LUT_SPOT + num, &ls->sampler[S_SPOT], n, norm_view, l, h_norm,
                                    L->spot, tangent);
        }

        float d0 = 1.0f;
        if (ls->use_d0) {
            d0 = lut_sample(ls, LUT_D0, &ls->sampler[S_D0], n, norm_view, l, h_norm, L->spot, tangent);
        }
        float specular_0[3] = { d0 * L->spec0[0], d0 * L->spec0[1], d0 * L->spec0[2] };

        float refl[3];
        refl[0] = ls->use_rr ? lut_sample(ls, LUT_RR, &ls->sampler[S_RR], n, norm_view, l, h_norm, L->spot,
                                         tangent) : 1.0f;
        refl[1] = ls->use_rg ? lut_sample(ls, LUT_RG, &ls->sampler[S_RG], n, norm_view, l, h_norm, L->spot,
                                         tangent) : refl[0];
        refl[2] = ls->use_rb ? lut_sample(ls, LUT_RB, &ls->sampler[S_RB], n, norm_view, l, h_norm, L->spot,
                                         tangent) : refl[0];

        float d1 = 1.0f;
        if (ls->use_d1) {
            d1 = lut_sample(ls, LUT_D1, &ls->sampler[S_D1], n, norm_view, l, h_norm, L->spot, tangent);
        }
        float specular_1[3];
        for (int i = 0; i < 3; i++) specular_1[i] = d1 * refl[i] * L->spec1[i];

        if (ls->use_fresnel) {
            float fresnel = lut_sample(ls, LUT_FRESNEL, &ls->sampler[S_FRESNEL], n, norm_view, l, h_norm,
                                       L->spot, tangent);
            if ((ls->config0 >> 2) & 1u) diffuse_sum[3] = fresnel;
            if ((ls->config0 >> 3) & 1u) specular_sum[3] = fresnel;
        }

        float ln = dot3(l, n);
        float lit = (light_config >> 1) & 1u ? fabsf(ln) : (ln > 0.0f ? ln : 0.0f);
        float clamp = ls->clamp_highlights ? (ln <= 0.0f ? 0.0f : 1.0f) : 1.0f;

        if ((light_config >> 2) & 3u) {
            float geo = h_len2 == 0.0f ? 0.0f : lit / h_len2;
            if (geo > 1.0f) geo = 1.0f;
            if ((light_config >> 2) & 1u) for (int i = 0; i < 3; i++) specular_0[i] *= geo;
            if ((light_config >> 3) & 1u) for (int i = 0; i < 3; i++) specular_1[i] *= geo;
        }

        float atten = dist_atten * spot_atten;
        for (int i = 0; i < 3; i++) {
            diffuse_sum[i] += (L->diffuse[i] * lit + L->ambient[i]) * atten;
            specular_sum[i] += (specular_0[i] + specular_1[i]) * clamp * atten;
        }
    }

    for (int i = 0; i < 3; i++) diffuse_sum[i] += ls->global[i];

    uint32_t p = 0, s2 = 0;
    for (int i = 0; i < 4; i++) {
        float d = diffuse_sum[i] < 0.0f ? 0.0f : (diffuse_sum[i] > 1.0f ? 1.0f : diffuse_sum[i]);
        float s = specular_sum[i] < 0.0f ? 0.0f : (specular_sum[i] > 1.0f ? 1.0f : specular_sum[i]);
        p |= (uint32_t)(uint8_t)(d * 255.0f) << (8 * i);
        s2 |= (uint32_t)(uint8_t)(s * 255.0f) << (8 * i);
    }
    *primary = p;
    *secondary = s2;
}

// The whole lighting state, once: lights, configuration, and a few entries of each table that has
// any. A dark or transparent lit surface is either empty tables or a wrong reading of the registers,
// and this says which.
void albw_lighting_dump(void) {
    printf("[light] config0 %08x config1 %08x lights %u slots %08x abs %08x input %08x scale %08x "
           "ambient %08x on %u/%u\n", reg(R_CONFIG0), reg(R_CONFIG1), (reg(R_NUM_LIGHTS) & 7u) + 1u,
           reg(R_LIGHT_SLOTS), reg(R_LUT_ABS), reg(R_LUT_INPUT), reg(R_LUT_SCALE),
           reg(R_GLOBAL_AMBIENT), reg(R_LIGHTING_ON) & 1u, reg(R_DISABLE) & 1u);
    for (uint32_t i = 0; i <= (reg(R_NUM_LIGHTS) & 7u); i++) {
        uint32_t b = R_LIGHT((reg(R_LIGHT_SLOTS) >> (i * 4u)) & 7u);
        printf("[light]   light: spec0 %08x spec1 %08x diffuse %08x ambient %08x pos %08x %08x "
               "config %08x\n", reg(b), reg(b + 1), reg(b + 2), reg(b + 3), reg(b + 4), reg(b + 5),
               reg(b + 9));
    }
    luts_init();
    for (int t = 0; t < LUTS; t++) {
        int any = 0;
        for (int e = 0; e < 256; e++) if (luts[t][e]) { any = 1; break; }
        if (!any) continue;
        printf("[light]   table %2d:", t);
        for (int e = 0; e < 256; e += 32) printf(" %06x", luts[t][e]);
        printf("\n");
    }
}


#include "albw_upscale.h"
#include "albw_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// FSR 1 on the processor. The filter is AMD's (third_party/fsr1/ffx_fsr1.h, MIT): FsrEasuF and
// FsrRcasF, followed step by step. What is this port's own is only how the work is laid out:
//
// - The input is turned into floats once, with two pixels of border repeated outwards. The shader's
//   sampler clamps at the edges; with the border no tap needs to.
// - EASU works out an edge direction and length around each of the four input pixels nearest an
//   output pixel, and blends the four. Those values depend on the input pixel alone, so they are
//   worked out once per input pixel here, instead of once per output pixel and quad.
// - The twelve taps' weights are computed four taps at a time, and a tap's colour is one vector
//   of red, green and blue, so a tap is a load and a multiply-add.
// - Between the passes the picture is held in eight bits a channel, as the GPU's target holds it.
//
// The order of some additions differs from the shader's, so the result can differ from the GPU's
// in the last bit of a channel here and there; the picture is the same.

typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));

#define BORDER 2

static struct {
    int w, h, pw;                                        // the input, and its padded row length
    v4f *rgb;                                            // padded: red, green, blue, 0
    float *luma, *dir_x, *dir_y, *len;                   // padded; the last three from -1 to w, h
    size_t room;
    const uint8_t *src;
    int ow, oh;
    uint8_t *mid;                                        // EASU's result, four bytes a pixel
    size_t mid_room;
    uint8_t *dst;
    int dst_pitch;
    float con[4];                                        // FsrEasuCon's con0
    float sharp;                                         // FsrRcasCon's con.x
} job;

static inline v4f splat(float x) { return (v4f){ x, x, x, x }; }
static inline v4f vsel(v4i m, v4f a, v4f b) { return (v4f)((m & (v4i)a) | (~m & (v4i)b)); }
static inline v4f vmin(v4f a, v4f b) { return vsel(a < b, a, b); }
static inline v4f vmax(v4f a, v4f b) { return vsel(a > b, a, b); }

static inline float as_float(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static inline uint32_t as_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
// ffx_a.h's approximations, which the filter's shape is tuned with.
static inline float rcp_lo(float a) { return as_float(0x7ef07ebbu - as_bits(a)); }
static inline float rcp_med(float a) {
    float b = as_float(0x7ef19fffu - as_bits(a));
    return b * (-b * a + 2.0f);
}
static inline float rsq_lo(float a) { return as_float(0x5f347d74u - (as_bits(a) >> 1)); }
static inline float sat(float a) { return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
// Plain comparisons: fminf, fmaxf and floorf are calls into the maths library on a baseline x86,
// and they were most of the time. No NaN reaches these.
static inline float min2(float a, float b) { return a < b ? a : b; }
static inline float max2(float a, float b) { return a > b ? a : b; }
static inline float abs1(float a) { return a < 0.0f ? -a : a; }
static inline int floor_int(float a) {
    int i = (int)a;
    return (float)i > a ? i - 1 : i;
}

static inline int at(int x, int y) { return (y + BORDER) * job.pw + (x + BORDER); }

// One padded input row: the channels as the sampler returns them, and the luma EASU works with
// (twice the luma, in its two multiply-adds).
static void planes_row(uint32_t item, int worker) {
    (void)worker;
    int y = (int)item - BORDER;
    int sy = y < 0 ? 0 : (y >= job.h ? job.h - 1 : y);
    for (int x = -BORDER; x < job.w + BORDER; x++) {
        int sx = x < 0 ? 0 : (x >= job.w ? job.w - 1 : x);
        const uint8_t *p = job.src + ((size_t)sy * (size_t)job.w + (size_t)sx) * 3u;
        int i = at(x, y);
        float r = p[0] * (1.0f / 255.0f), g = p[1] * (1.0f / 255.0f), b = p[2] * (1.0f / 255.0f);
        job.rgb[i] = (v4f){ r, g, b, 0.0f };
        job.luma[i] = b * 0.5f + (r * 0.5f + g);
    }
}

// FsrEasuSetF for an input pixel as the centre of its '+', without the bilinear weight, which the
// output pixel applies. Covers the pixels one beyond the picture on each side, which is as far as
// an output pixel's four nearest reach.
static void direction_row(uint32_t item, int worker) {
    (void)worker;
    int y = (int)item - 1;
    for (int x = -1; x <= job.w; x++) {
        int c = at(x, y);
        float lA = job.luma[c - job.pw], lB = job.luma[c - 1], lC = job.luma[c];
        float lD = job.luma[c + 1], lE = job.luma[c + job.pw];
        float dc = lD - lC, cb = lC - lB;
        float len_x = rcp_lo(max2(abs1(dc), abs1(cb)));
        float dir_x = lD - lB;
        len_x = sat(abs1(dir_x) * len_x);
        len_x *= len_x;
        float ec = lE - lC, ca = lC - lA;
        float len_y = rcp_lo(max2(abs1(ec), abs1(ca)));
        float dir_y = lE - lA;
        len_y = sat(abs1(dir_y) * len_y);
        len_y *= len_y;
        job.dir_x[c] = dir_x;
        job.dir_y[c] = dir_y;
        job.len[c] = len_x + len_y;
    }
}

// FsrEasuF, eight rows of output at a time.
#define ROWS_PER_ITEM 8
static void easu_rows(uint32_t item, int worker) {
    (void)worker;
    // The twelve taps as offsets from 'f', in three groups of four.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    static const v4f tx[3] = { { 0, 1, -1, 0 }, { 0, -1, 1, 2 }, { 2, 1, 1, 0 } };   // b c i j,
    static const v4f ty[3] = { { -1, -1, 1, 1 }, { 0, 0, 1, 1 }, { 0, 0, 2, 2 } };   // f e k l,
    const int pw = job.pw;                                                            // h g o n
    const int tap[12] = { -pw, -pw + 1, pw - 1, pw, 0, -1, pw + 1, pw + 2, 2, 1, 2 * pw + 1, 2 * pw };

    int y_end = (int)(item + 1) * ROWS_PER_ITEM;
    if (y_end > job.oh) y_end = job.oh;
    for (int oy = (int)item * ROWS_PER_ITEM; oy < y_end; oy++) {
        float ppy = (float)oy * job.con[1] + job.con[3];
        int fy = floor_int(ppy);
        ppy -= (float)fy;
        uint8_t *out = job.mid + (size_t)oy * (size_t)job.ow * 4u;
        for (int ox = 0; ox < job.ow; ox++, out += 4) {
            float ppx = (float)ox * job.con[0] + job.con[2];
            int fx = floor_int(ppx);
            ppx -= (float)fx;
            int f = at(fx, fy), g = f + 1, j = f + pw, k = f + pw + 1;

            // The four '+'s, bilinearly.
            float wf = (1.0f - ppx) * (1.0f - ppy), wg = ppx * (1.0f - ppy);
            float wj = (1.0f - ppx) * ppy, wk = ppx * ppy;
            float dir_x = job.dir_x[f] * wf + job.dir_x[g] * wg + job.dir_x[j] * wj + job.dir_x[k] * wk;
            float dir_y = job.dir_y[f] * wf + job.dir_y[g] * wg + job.dir_y[j] * wj + job.dir_y[k] * wk;
            float len = job.len[f] * wf + job.len[g] * wg + job.len[j] * wj + job.len[k] * wk;

            // Normalise with the approximation, and clean up close to zero.
            float dir_r = dir_x * dir_x + dir_y * dir_y;
            int zero = dir_r < 1.0f / 32768.0f;
            dir_r = rsq_lo(dir_r);
            if (zero) {
                dir_r = 1.0f;
                dir_x = 1.0f;
            }
            dir_x *= dir_r;
            dir_y *= dir_r;
            len = len * 0.5f;
            len *= len;
            float stretch = (dir_x * dir_x + dir_y * dir_y) * rcp_lo(max2(abs1(dir_x), abs1(dir_y)));
            float len2_x = 1.0f + (stretch - 1.0f) * len, len2_y = 1.0f + -0.5f * len;
            float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
            float clp = rcp_lo(lob);

            // FsrEasuTapF, four taps at a time.
            v4f color = splat(0.0f), weight = splat(0.0f);
            const v4f *c = job.rgb + f;
            for (int q = 0; q < 3; q++) {
                v4f off_x = tx[q] - ppx, off_y = ty[q] - ppy;
                v4f vx = (off_x * dir_x + off_y * dir_y) * len2_x;
                v4f vy = (off_x * -dir_y + off_y * dir_x) * len2_y;
                v4f d2 = vmin(vx * vx + vy * vy, splat(clp));
                v4f wb = splat(2.0f / 5.0f) * d2 - 1.0f;
                v4f wa = lob * d2 - 1.0f;
                wb *= wb;
                wa *= wa;
                wb = splat(25.0f / 16.0f) * wb - (25.0f / 16.0f - 1.0f);
                v4f w = wb * wa;
                weight += w;
                color += c[tap[4 * q + 0]] * w[0] + c[tap[4 * q + 1]] * w[1]
                       + c[tap[4 * q + 2]] * w[2] + c[tap[4 * q + 3]] * w[3];
            }
            // Normalise, and dering against the four nearest.
            float inv = 1.0f / (weight[0] + weight[1] + weight[2] + weight[3]);
            v4f lo = vmin(vmin(vmin(job.rgb[f], job.rgb[g]), job.rgb[j]), job.rgb[k]);
            v4f hi = vmax(vmax(vmax(job.rgb[f], job.rgb[g]), job.rgb[j]), job.rgb[k]);
            v4f pix = vmin(hi, vmax(lo, color * inv)) * 255.0f + 0.5f;
            out[0] = (uint8_t)pix[0];                    // 0 to 255.5 already, by the deringing
            out[1] = (uint8_t)pix[1];
            out[2] = (uint8_t)pix[2];
            out[3] = 255;
        }
    }
}

// FsrRcasF, eight rows at a time, from EASU's result into the destination, the three channels
// side by side in a vector.
#define RCAS_LIMIT (0.25f - (1.0f / 16.0f))

typedef uint8_t v4b __attribute__((vector_size(4)));
static inline v4f load_px(const uint8_t *p) {
    v4b bytes;
    memcpy(&bytes, p, sizeof bytes);
    return __builtin_convertvector(bytes, v4f) * (1.0f / 255.0f);
}

static void rcas_rows(uint32_t item, int worker) {
    (void)worker;
    const int ow = job.ow, oh = job.oh;
    int y_end = (int)(item + 1) * ROWS_PER_ITEM;
    if (y_end > oh) y_end = oh;
    for (int y = (int)item * ROWS_PER_ITEM; y < y_end; y++) {
        // The shader's loads clamp at the picture's edges; so do these.
        const uint8_t *row = job.mid + (size_t)y * (size_t)ow * 4u;
        const uint8_t *above = y > 0 ? row - (size_t)ow * 4u : row;
        const uint8_t *below = y + 1 < oh ? row + (size_t)ow * 4u : row;
        uint32_t *out = (uint32_t *)(job.dst + (size_t)y * (size_t)job.dst_pitch);
        for (int x = 0; x < ow; x++) {
            int l = x > 0 ? x - 1 : 0, r = x + 1 < ow ? x + 1 : ow - 1;
            v4f b = load_px(above + x * 4), d = load_px(row + l * 4), e = load_px(row + x * 4);
            v4f f = load_px(row + r * 4), h = load_px(below + x * 4);
            v4f mn4 = vmin(vmin(vmin(b, d), f), h);
            v4f mx4 = vmax(vmax(vmax(b, d), f), h);
            // Where a limit would divide nothing by nothing the shader's max takes the other side,
            // past the NaN; so does this, by leaving that side out.
            v4f hit_min = vmin(mn4, e) / (4.0f * mx4);
            v4f hit_max = (1.0f - vmax(mx4, e)) / (4.0f * mn4 - 4.0f);
            v4f none = splat(-INFINITY);
            v4f lobe4 = vmax(vsel(mx4 > 0.0f, -hit_min, none), vsel(mn4 < 1.0f, hit_max, none));
            float lobe = max2(max2(lobe4[0], lobe4[1]), lobe4[2]);
            lobe = max2(-RCAS_LIMIT, min2(lobe, 0.0f)) * job.sharp;
            float rcp_l = rcp_med(4.0f * lobe + 1.0f);
            v4f pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcp_l * 255.0f + 0.5f;
            v4i c = __builtin_convertvector(vmin(vmax(pix, splat(0.0f)), splat(255.0f)), v4i);
            out[x] = 0xFF000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
        }
    }
}

void albw_upscale_fsr1(const uint8_t *src, int src_w, int src_h, uint32_t *dst, int dst_w,
                       int dst_h, int dst_pitch, float stops) {
    if (src_w < 1 || src_h < 1 || dst_w < 1 || dst_h < 1) return;
    job.w = src_w;
    job.h = src_h;
    job.pw = src_w + 2 * BORDER;
    size_t plane = (size_t)job.pw * (size_t)(src_h + 2 * BORDER);
    if (plane > job.room) {
        free(job.rgb);
        // Four floats of colour and four planes of one: eight floats a padded pixel.
        job.rgb = aligned_alloc(16, plane * 8u * sizeof(float));
        if (!job.rgb) {
            job.room = 0;
            return;
        }
        job.room = plane;
    }
    job.luma = (float *)(job.rgb + plane);
    job.dir_x = job.luma + plane;
    job.dir_y = job.dir_x + plane;
    job.len = job.dir_y + plane;
    size_t mid = (size_t)dst_w * (size_t)dst_h * 4u;
    if (mid > job.mid_room) {
        free(job.mid);
        job.mid = malloc(mid);
        if (!job.mid) {
            job.mid_room = 0;
            return;
        }
        job.mid_room = mid;
    }
    job.src = src;
    job.ow = dst_w;
    job.oh = dst_h;
    job.dst = (uint8_t *)dst;
    job.dst_pitch = dst_pitch;
    // FsrEasuCon, with the whole input as the viewport.
    job.con[0] = (float)src_w / (float)dst_w;
    job.con[1] = (float)src_h / (float)dst_h;
    job.con[2] = 0.5f * (float)src_w / (float)dst_w - 0.5f;
    job.con[3] = 0.5f * (float)src_h / (float)dst_h - 0.5f;
    job.sharp = exp2f(-stops);

    albw_raster_parallel(planes_row, (uint32_t)(src_h + 2 * BORDER));
    albw_raster_parallel(direction_row, (uint32_t)(src_h + 2));
    uint32_t items = (uint32_t)((dst_h + ROWS_PER_ITEM - 1) / ROWS_PER_ITEM);
    albw_raster_parallel(easu_rows, items);
    albw_raster_parallel(rcas_rows, items);
}

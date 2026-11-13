// The GPU renderer (albw_gpu.h): a recorded frame drawn on the graphics card through SDL's GPU API,
// at a whole multiple of the console's resolution.
//
// What the console's GPU keeps in VRAM -- the render target, its depth and stencil -- lives here
// as textures the size of the target times the scale, and lives on from frame to frame as VRAM
// does: the game clears by drawing as often as by filling. Textures are decoded from guest memory
// when a draw first samples them and kept while their bytes stay the same; a texture the game made
// by copying a target is a copy of that target's texture, taken on the card when the game took its
// own. Each display transfer that makes a screen's picture becomes a copy of the target, turned the
// way the screen shows it, into that screen's texture -- which is what the window then shows.
#include "albw_gpu.h"

#include "albw_kernel.h"
#include "albw_raster.h"
#include "albw_vkup.h"
#include "../shaders/pica_state.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pica_vert.h"
#include "pica_frag.h"
#include "blit_vert.h"
#include "blit_frag.h"

static SDL_GPUDevice *dev;
static SDL_GPUShader *sh_vert, *sh_frag, *sh_blit_vert, *sh_blit_frag;
static SDL_GPUTextureFormat depth_format;
static SDL_GPUTexture *black;                            // 1x1, opaque: an unusable unit
static SDL_GPUSampler *blit_sampler;
static SDL_GPUGraphicsPipeline *blit_pipe;
static unsigned long frames_drawn;
// Multisampling: the samples a target's pixel has, as asked for and as the card can do it.
static int samples_wanted = 1, samples = 1;

static SDL_GPUSampleCount sample_count(int n) {
    return n >= 8 ? SDL_GPU_SAMPLECOUNT_8 : n >= 4 ? SDL_GPU_SAMPLECOUNT_4
         : n >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
}

#define COLOR_FORMAT SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM

static void say(const char *what) {
    printf("[gpu  ] %s: %s\n", what, SDL_GetError());
}

// ---------------------------------------------------------------------------------------------
// Pipelines, one per combination of the fixed-function state a draw asks for.
struct pipe_key {
    uint8_t aux;                                         // with the temporal upscaler's targets
    uint8_t samples, has_depth, blend, color_mask, keep_color, depth_test, depth_func, depth_write, cull;
    uint8_t stencil, stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
    uint8_t stencil_in_mask, stencil_write_mask;
    uint32_t blend_func;
};
struct pipe {
    struct pipe_key key;
    SDL_GPUGraphicsPipeline *p;
};
static struct pipe *pipes;
static int pipe_count, pipe_room;

static SDL_GPUBlendFactor blend_factor(uint32_t f, int *uses_constant_alpha) {
    switch (f & 15u) {
        case 0: return SDL_GPU_BLENDFACTOR_ZERO;
        case 1: return SDL_GPU_BLENDFACTOR_ONE;
        case 2: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
        case 3: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        case 4: return SDL_GPU_BLENDFACTOR_DST_COLOR;
        case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
        case 6: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        case 7: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
        case 9: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        case 10: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        case 11: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
        // SDL has no constant alpha; the constant is set to its alpha in every channel instead.
        case 12: *uses_constant_alpha = 1; return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        case 13: *uses_constant_alpha = 1; return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    }
}

static SDL_GPUBlendOp blend_op(uint32_t e) {
    switch (e & 7u) {
        case 0: return SDL_GPU_BLENDOP_ADD;
        case 1: return SDL_GPU_BLENDOP_SUBTRACT;
        case 2: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
        case 3: return SDL_GPU_BLENDOP_MIN;
        default: return SDL_GPU_BLENDOP_MAX;
    }
}

static SDL_GPUCompareOp compare_op(uint32_t f) {
    static const SDL_GPUCompareOp ops[8] = {
        SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_COMPAREOP_EQUAL,
        SDL_GPU_COMPAREOP_NOT_EQUAL, SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL,
    };
    return ops[f & 7u];
}

static SDL_GPUStencilOp stencil_op(uint32_t o) {
    static const SDL_GPUStencilOp ops[8] = {
        SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_ZERO, SDL_GPU_STENCILOP_REPLACE,
        SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP, SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP,
        SDL_GPU_STENCILOP_INVERT, SDL_GPU_STENCILOP_INCREMENT_AND_WRAP,
        SDL_GPU_STENCILOP_DECREMENT_AND_WRAP,
    };
    return ops[o & 7u];
}

static void pipe_key_of(const struct albw_gpu_draw *d, struct pipe_key *k) {
    memset(k, 0, sizeof *k);
    k->samples = (uint8_t)samples;
    k->has_depth = d->depth_base != 0;
    k->blend = d->blend;
    k->color_mask = d->color_mask;
    k->keep_color = d->keep_color;
    // Without a depth buffer there is nothing to test or write, as the console's rasteriser has it.
    k->depth_test = k->has_depth && d->depth_test;
    k->depth_func = k->depth_test ? d->depth_func : 1u;
    k->depth_write = k->has_depth && d->depth_write;
    k->cull = d->cull;
    k->stencil = k->has_depth && d->stencil;
    if (k->stencil) {
        k->stencil_func = d->stencil_func;
        k->stencil_fail = d->stencil_fail;
        k->stencil_zfail = d->stencil_zfail;
        k->stencil_zpass = d->stencil_zpass;
        k->stencil_in_mask = d->stencil_in_mask;
        k->stencil_write_mask = d->stencil_write_mask;
    }
    k->blend_func = d->blend ? d->blend_func : 0u;
}

// The formats of the temporal upscaler's targets: motion, depth, reactive.
#define MOTION_FORMAT SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT
#define DEPTHCOPY_FORMAT SDL_GPU_TEXTUREFORMAT_R32_FLOAT
#define REACTIVE_FORMAT SDL_GPU_TEXTUREFORMAT_R8_UNORM

static SDL_GPUGraphicsPipeline *pipeline_for(const struct albw_gpu_draw *d, int aux) {
    struct pipe_key k;
    pipe_key_of(d, &k);
    k.aux = (uint8_t)aux;
    for (int i = 0; i < pipe_count; i++) {
        if (!memcmp(&pipes[i].key, &k, sizeof k)) return pipes[i].p;
    }

    SDL_GPUVertexBufferDescription vb = { 0 };
    vb.slot = 0;
    vb.pitch = sizeof(struct albw_gpu_vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[7];
    for (int i = 0; i < 7; i++) {
        attrs[i].location = (Uint32)i;
        attrs[i].buffer_slot = 0;
        attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[i].offset = (Uint32)(i * 16);
    }

    SDL_GPUColorTargetDescription ct = { 0 };
    ct.format = COLOR_FORMAT;
    // Only red, green and blue are written: the console's targets keep no alpha, so what a blend
    // reads as the destination's alpha is always one, as the target was cleared to.
    SDL_GPUColorComponentFlags mask = 0;
    if (!k.keep_color) {
        if (k.color_mask & 1u) mask |= SDL_GPU_COLORCOMPONENT_R;
        if (k.color_mask & 2u) mask |= SDL_GPU_COLORCOMPONENT_G;
        if (k.color_mask & 4u) mask |= SDL_GPU_COLORCOMPONENT_B;
    }
    ct.blend_state.color_write_mask = mask;
    ct.blend_state.enable_color_write_mask = true;
    if (k.blend) {
        int ca = 0;
        uint32_t f = k.blend_func;
        ct.blend_state.enable_blend = true;
        ct.blend_state.color_blend_op = blend_op(f);
        ct.blend_state.alpha_blend_op = blend_op(f >> 8);
        ct.blend_state.src_color_blendfactor = blend_factor(f >> 16, &ca);
        ct.blend_state.dst_color_blendfactor = blend_factor(f >> 20, &ca);
        ct.blend_state.src_alpha_blendfactor = blend_factor(f >> 24, &ca);
        ct.blend_state.dst_alpha_blendfactor = blend_factor(f >> 28, &ca);
    }

    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_zero(ci);
    ci.vertex_shader = sh_vert;
    ci.fragment_shader = sh_frag;
    ci.vertex_input_state.vertex_buffer_descriptions = &vb;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = attrs;
    ci.vertex_input_state.num_vertex_attributes = 7;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    // The console culls by the sign of a triangle's area in its own screen space, where y runs up;
    // seen in SDL's, one with a positive area winds clockwise.
    ci.rasterizer_state.cull_mode = k.cull == 1 ? SDL_GPU_CULLMODE_BACK
                                  : k.cull == 2 ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.rasterizer_state.enable_depth_clip = true;
    ci.multisample_state.sample_count = sample_count(k.samples);
    // The depth buffer is always tested, so that it can be written: a draw that writes depth
    // without testing it compares with "always".
    ci.depth_stencil_state.enable_depth_test = k.has_depth != 0;
    ci.depth_stencil_state.enable_depth_write = k.depth_write != 0;
    ci.depth_stencil_state.compare_op = compare_op(k.depth_func);
    if (k.stencil) {
        SDL_GPUStencilOpState so;
        so.fail_op = stencil_op(k.stencil_fail);
        so.depth_fail_op = stencil_op(k.stencil_zfail);
        so.pass_op = stencil_op(k.stencil_zpass);
        so.compare_op = compare_op(k.stencil_func);
        ci.depth_stencil_state.front_stencil_state = so;
        ci.depth_stencil_state.back_stencil_state = so;
        ci.depth_stencil_state.compare_mask = k.stencil_in_mask;
        ci.depth_stencil_state.write_mask = k.stencil_write_mask;
        ci.depth_stencil_state.enable_stencil_test = true;
    }
    // With the upscaler's targets beside the colour: motion and the reactive value from every
    // fragment drawn, the depth from those that write it; none blended.
    SDL_GPUColorTargetDescription cts[4];
    cts[0] = ct;
    SDL_zero(cts[1]);
    SDL_zero(cts[2]);
    SDL_zero(cts[3]);
    cts[1].format = MOTION_FORMAT;
    cts[1].blend_state.enable_color_write_mask = true;
    cts[1].blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G;
    cts[2].format = DEPTHCOPY_FORMAT;
    cts[2].blend_state.enable_color_write_mask = true;
    cts[2].blend_state.color_write_mask = k.depth_write ? SDL_GPU_COLORCOMPONENT_R : 0;
    cts[3].format = REACTIVE_FORMAT;
    cts[3].blend_state.enable_color_write_mask = true;
    cts[3].blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R;
    ci.target_info.color_target_descriptions = k.aux ? cts : &ct;
    ci.target_info.num_color_targets = k.aux ? 4 : 1;
    ci.target_info.depth_stencil_format = depth_format;
    ci.target_info.has_depth_stencil_target = k.has_depth != 0;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    if (!p) {
        say("a pipeline");
        return NULL;
    }
    if (pipe_count == pipe_room) {
        int room = pipe_room ? pipe_room * 2 : 32;
        struct pipe *bigger = realloc(pipes, (size_t)room * sizeof *bigger);
        if (!bigger) return p;
        pipes = bigger;
        pipe_room = room;
    }
    pipes[pipe_count].key = k;
    pipes[pipe_count].p = p;
    pipe_count++;
    return p;
}

// ---------------------------------------------------------------------------------------------
// Samplers.
struct samp {
    uint32_t key;
    SDL_GPUSampler *s;
};
static struct samp samps[64];
static int samp_count;

static SDL_GPUSamplerAddressMode address_mode(uint32_t wrap) {
    switch (wrap & 3u) {
        case 2: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        case 3: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
        default: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;   // the border is the shader's
    }
}

static SDL_GPUSampler *sampler_for(const struct albw_gpu_texture *t, int levels) {
    uint32_t key = (uint32_t)t->mag_linear | ((uint32_t)t->min_linear << 1)
                 | ((uint32_t)t->mip_linear << 2) | ((uint32_t)(t->wrap_s & 3u) << 3)
                 | ((uint32_t)(t->wrap_t & 3u) << 5) | ((uint32_t)(t->min_level & 15u) << 8)
                 | ((uint32_t)(levels & 15) << 12);
    for (int i = 0; i < samp_count; i++) if (samps[i].key == key) return samps[i].s;
    SDL_GPUSamplerCreateInfo ci;
    SDL_zero(ci);
    ci.mag_filter = t->mag_linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.min_filter = t->min_linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mipmap_mode = t->mip_linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = address_mode(t->wrap_s);
    ci.address_mode_v = address_mode(t->wrap_t);
    ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.min_lod = levels > 1 ? (float)t->min_level : 0.0f;
    ci.max_lod = levels > 1 ? (float)(levels - 1) : 0.0f;
    SDL_GPUSampler *s = SDL_CreateGPUSampler(dev, &ci);
    if (!s) {
        say("a sampler");
        return NULL;
    }
    if (samp_count < (int)(sizeof samps / sizeof samps[0])) {
        samps[samp_count].key = key;
        samps[samp_count].s = s;
        samp_count++;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Targets: the console's colour buffers and depth buffers, by address.
struct surface {
    uint32_t base, width, height;                        // console pixels
    uint32_t bytes;                                      // a pixel's, in guest memory
    int scale;
    SDL_GPUTexture *tex;                                 // what is sampled and copied from
    SDL_GPUTexture *ms;                                  // multisampled, drawn into; NULL without
    int samples;
    int depth;                                           // a depth buffer, with stencil
    int clear;                                           // a fill is waiting for the next pass
    SDL_FColor clear_color;
    float clear_depth;
    uint8_t clear_stencil;
    unsigned long used;
};
static struct surface surfaces[16];
static int surface_count;

static struct surface *surface_find(uint32_t base, int depth) {
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].base == base && surfaces[i].depth == depth) return &surfaces[i];
    }
    return NULL;
}

static struct surface *surface_get(uint32_t base, uint32_t width, uint32_t height, uint32_t bytes,
                                   int depth, int scale) {
    struct surface *s = surface_find(base, depth);
    if (s && s->width == width && s->height == height && s->scale == scale && s->samples == samples) {
        s->bytes = bytes;
        return s;
    }
    if (!s) {
        if (surface_count == (int)(sizeof surfaces / sizeof surfaces[0])) {
            // The least recently used goes.
            int oldest = 0;
            for (int i = 1; i < surface_count; i++) if (surfaces[i].used < surfaces[oldest].used) oldest = i;
            if (surfaces[oldest].tex) SDL_ReleaseGPUTexture(dev, surfaces[oldest].tex);
            if (surfaces[oldest].ms) SDL_ReleaseGPUTexture(dev, surfaces[oldest].ms);
            s = &surfaces[oldest];
        } else {
            s = &surfaces[surface_count++];
        }
        memset(s, 0, sizeof *s);
    } else {
        if (s->tex) SDL_ReleaseGPUTexture(dev, s->tex);
        if (s->ms) SDL_ReleaseGPUTexture(dev, s->ms);
        s->tex = s->ms = NULL;
    }
    s->base = base;
    s->width = width;
    s->height = height;
    s->bytes = bytes ? bytes : 2u;
    s->scale = scale;
    s->depth = depth;
    SDL_GPUTextureCreateInfo ci;
    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = depth ? depth_format : COLOR_FORMAT;
    ci.usage = depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
                     : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = width * (uint32_t)scale;
    ci.height = height * (uint32_t)scale;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    s->samples = samples;
    if (depth) {
        // A depth buffer is only ever drawn into: multisampled, it is the only one there is.
        ci.sample_count = sample_count(samples);
        s->tex = SDL_CreateGPUTexture(dev, &ci);
    } else {
        s->tex = SDL_CreateGPUTexture(dev, &ci);
        if (samples > 1) {
            ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            ci.sample_count = sample_count(samples);
            s->ms = SDL_CreateGPUTexture(dev, &ci);
            if (!s->ms) say("a multisampled target");
        }
    }
    if (!s->tex) say("a target");
    // A new target starts out as VRAM would be after a clear: black, far, no stencil.
    s->clear = 1;
    s->clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
    s->clear_depth = 1.0f;
    s->clear_stencil = 0;
    return s;
}

// ---------------------------------------------------------------------------------------------
// Textures from guest memory, decoded once and kept while their bytes stay the same.
struct texentry {
    uint32_t base, width, height, format, levels;
    uint64_t hash;
    SDL_GPUTexture *tex;
    unsigned long checked;                               // the frame its bytes were last hashed
    unsigned long used;
    struct texentry *chain;                              // the next in its bucket
};
// Each entry allocated by itself, so that a pointer to one stays good while others are added; found
// through a table of buckets by what the texture is.
static struct texentry **texes;
static int tex_count, tex_room;
static size_t tex_bytes_kept;
#define TEX_BUCKETS 4096u
static struct texentry *tex_bucket[TEX_BUCKETS];

static uint32_t tex_bucket_of(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                              uint32_t levels) {
    uint32_t h = base * 2654435761u ^ (width << 20) ^ (height << 8) ^ (format << 4) ^ levels;
    h ^= h >> 15;
    return (h * 0x2C1B3C6Du >> 7) & (TEX_BUCKETS - 1u);
}

// A fast hash of the texture's bytes, eight at a time.
static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h ^= v;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint32_t texture_span(uint32_t width, uint32_t height, uint32_t format, uint32_t levels) {
    uint32_t bytes = 0;
    for (uint32_t k = 0; k < levels; k++) {
        bytes += albw_raster_texture_bytes(width >> k, height >> k, format);
    }
    return bytes;
}

// The texture, decoded and ready to upload when its bytes have changed; `upload` is set to the
// entry that needs its bytes sent, NULL if none.
static struct texentry *texture_get(const struct albw_gpu_texture *t, struct texentry **upload) {
    *upload = NULL;
    uint32_t levels = t->levels ? t->levels : 1u;
    struct texentry *e = NULL;
    uint32_t bucket = tex_bucket_of(t->base, t->width, t->height, t->format, levels);
    for (struct texentry *c = tex_bucket[bucket]; c; c = c->chain) {
        if (c->base == t->base && c->width == t->width && c->height == t->height
                && c->format == t->format && c->levels == levels) {
            e = c;
            break;
        }
    }
    uint32_t span = texture_span(t->width, t->height, t->format, levels);
    if (!albw_guest_ok(t->base, span)) return NULL;
    if (e && e->checked == frames_drawn) {
        e->used = frames_drawn;
        return e;
    }
    uint64_t h = hash_bytes(albw_mem + t->base, span);
    if (e && e->hash == h) {
        e->checked = e->used = frames_drawn;
        return e;
    }
    if (!e) {
        if (tex_count == tex_room) {
            int room = tex_room ? tex_room * 2 : 256;
            struct texentry **bigger = realloc(texes, (size_t)room * sizeof *bigger);
            if (!bigger) return NULL;
            texes = bigger;
            tex_room = room;
        }
        e = calloc(1, sizeof *e);
        if (!e) return NULL;
        e->base = t->base;
        e->width = t->width;
        e->height = t->height;
        e->format = t->format;
        e->levels = levels;
        SDL_GPUTextureCreateInfo ci;
        SDL_zero(ci);
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = COLOR_FORMAT;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = t->width;
        ci.height = t->height;
        ci.layer_count_or_depth = 1;
        ci.num_levels = levels;
        e->tex = SDL_CreateGPUTexture(dev, &ci);
        if (!e->tex) {
            say("a texture");
            free(e);
            return NULL;
        }
        texes[tex_count++] = e;
        e->chain = tex_bucket[bucket];
        tex_bucket[bucket] = e;
        tex_bytes_kept += (size_t)t->width * t->height * 4u * 4u / 3u;
    }
    e->hash = h;
    e->checked = e->used = frames_drawn;
    *upload = e;
    return e;
}

// Textures nobody has drawn with for a while go, once there are many.
static void textures_trim(void) {
    if (tex_bytes_kept < (size_t)512 << 20) return;
    int kept = 0;
    for (int i = 0; i < tex_count; i++) {
        struct texentry *e = texes[i];
        if (frames_drawn - e->used < 600) {
            texes[kept++] = e;
            continue;
        }
        SDL_ReleaseGPUTexture(dev, e->tex);
        tex_bytes_kept -= (size_t)e->width * e->height * 4u * 4u / 3u;
        free(e);
    }
    tex_count = kept;
    memset(tex_bucket, 0, sizeof tex_bucket);
    for (int i = 0; i < tex_count; i++) {
        struct texentry *e = texes[i];
        uint32_t b = tex_bucket_of(e->base, e->width, e->height, e->format, e->levels);
        e->chain = tex_bucket[b];
        tex_bucket[b] = e;
    }
}

// ---------------------------------------------------------------------------------------------
// Texture copies of a target: the target's texture as it was when the game copied it, for the
// draws that sample the copy. A copy stays good while the guest memory it landed in still holds
// what the game's copy put there.
struct snapshot {
    uint32_t dst, bytes;                                 // where the game's copy landed
    uint64_t token;                                      // the hash of those bytes, after it
    uint32_t surf_base, surf_w, surf_h;                  // the target it is of
    uint32_t first_row;                                  // the target's row it starts at, as memory
                                                         // counts rows (from the bottom)
    int scale;
    SDL_GPUTexture *tex;
    int pending;                                         // to be copied in this frame's passes
};
static struct snapshot snaps[4];
static int snap_count;

static struct snapshot *snapshot_for(uint32_t base) {
    for (int i = 0; i < snap_count; i++) {
        if (snaps[i].dst == base && snaps[i].tex) return &snaps[i];
    }
    return NULL;
}

// The target a transfer or a copy reads from: the one whose memory holds its source.
static struct surface *surface_holding(uint32_t addr) {
    for (int k = 0; k < surface_count; k++) {
        struct surface *c = &surfaces[k];
        if (!c->depth && addr >= c->base && addr < c->base + c->width * c->height * c->bytes) return c;
    }
    return NULL;
}

// A texture copy of a target, seen in a frame about to be drawn: a picture of the target is kept
// for the address the game copied to, taken on the card at the copy's place among the frame's
// events, so that the draws after it -- in this frame and the ones after -- can sample it.
static void snapshot_prepare(const struct albw_gpu_event *e) {
    struct surface *s = surface_holding(e->a);
    if (!s || !s->tex) return;
    struct snapshot *sn = NULL;
    for (int k = 0; k < snap_count; k++) if (snaps[k].dst == e->b) sn = &snaps[k];
    if (!sn) {
        if (snap_count < (int)(sizeof snaps / sizeof snaps[0])) {
            sn = &snaps[snap_count++];
            memset(sn, 0, sizeof *sn);
        } else {
            sn = &snaps[0];                              // the oldest goes
        }
    }
    if (!sn->tex || sn->scale != s->scale || sn->surf_w != s->width || sn->surf_h != s->height) {
        if (sn->tex) SDL_ReleaseGPUTexture(dev, sn->tex);
        SDL_GPUTextureCreateInfo ci;
        SDL_zero(ci);
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = COLOR_FORMAT;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ci.width = s->width * (uint32_t)s->scale;
        ci.height = s->height * (uint32_t)s->scale;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        sn->tex = SDL_CreateGPUTexture(dev, &ci);
        if (!sn->tex) {
            say("a picture of a target");
            return;
        }
    }
    sn->dst = e->b;
    sn->surf_base = s->base;
    sn->surf_w = s->width;
    sn->surf_h = s->height;
    sn->scale = s->scale;
    sn->first_row = (e->a - s->base) / (s->width * s->bytes);
    // The bytes the game's copy wrote, gaps included, to tell later whether they are still its.
    uint32_t out_gap = (e->e >> 16) * 16u, out_width = (e->e & 0xFFFFu) * 16u;
    uint32_t size = e->c & ~15u;
    uint32_t span = out_gap && out_width ? size / out_width * (out_width + out_gap) : size;
    sn->bytes = span;
    sn->token = albw_guest_ok(sn->dst, span) ? hash_bytes(albw_mem + sn->dst, span) : 0;
    sn->pending = 1;
}

// ---------------------------------------------------------------------------------------------
// The screens' pictures.
static SDL_GPUTexture *screen_tex[2];
static int screen_scale[2];

static SDL_GPUTexture *screen_texture(int which, int scale) {
    if (screen_tex[which] && screen_scale[which] == scale) return screen_tex[which];
    if (screen_tex[which]) SDL_ReleaseGPUTexture(dev, screen_tex[which]);
    SDL_GPUTextureCreateInfo ci;
    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = COLOR_FORMAT;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = (which ? 320u : 400u) * (uint32_t)scale;
    ci.height = 240u * (uint32_t)scale;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    screen_tex[which] = SDL_CreateGPUTexture(dev, &ci);
    screen_scale[which] = scale;
    if (!screen_tex[which]) say("a screen");
    return screen_tex[which];
}

// ---------------------------------------------------------------------------------------------
static SDL_GPUShader *shader(const unsigned char *code, size_t size, SDL_GPUShaderStage stage,
                             Uint32 samplers, Uint32 storage_buffers, Uint32 uniforms) {
    SDL_GPUShaderCreateInfo ci;
    SDL_zero(ci);
    ci.code = code;
    ci.code_size = size;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    ci.num_samplers = samplers;
    ci.num_storage_buffers = storage_buffers;
    ci.num_uniform_buffers = uniforms;
    return SDL_CreateGPUShader(dev, &ci);
}

void albw_gpu_set_samples(int n) { samples_wanted = n < 1 ? 1 : n; }

int albw_gpu_open(SDL_GPUDevice *device) {
    if (dev == device && sh_frag) return 1;
    if (!device) return 0;
    if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV)) {
        printf("[gpu  ] the device takes no SPIR-V shaders\n");
        return 0;
    }
    dev = device;
    sh_vert = shader(pica_vert_spv, sizeof pica_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
    sh_frag = shader(pica_frag_spv, sizeof pica_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 2, 1);
    sh_blit_vert = shader(blit_vert_spv, sizeof blit_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
    sh_blit_frag = shader(blit_frag_spv, sizeof blit_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1);
    if (!sh_vert || !sh_frag || !sh_blit_vert || !sh_blit_frag) {
        say("the shaders");
        dev = NULL;
        return 0;
    }
    depth_format = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                                                SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                 ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = COLOR_FORMAT;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    black = SDL_CreateGPUTexture(dev, &ti);
    SDL_GPUTransferBufferCreateInfo tb = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 4, 0 };
    SDL_GPUTransferBuffer *up = SDL_CreateGPUTransferBuffer(dev, &tb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!black || !up || !cmd) {
        say("setting up");
        dev = NULL;
        return 0;
    }
    uint8_t *p = SDL_MapGPUTransferBuffer(dev, up, false);
    p[0] = p[1] = p[2] = 0;
    p[3] = 255;
    SDL_UnmapGPUTransferBuffer(dev, up);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = { up, 0, 1, 1 };
    SDL_GPUTextureRegion dst = { black, 0, 0, 0, 0, 0, 1, 1, 1 };
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev, up);

    SDL_GPUSamplerCreateInfo si;
    SDL_zero(si);
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    blit_sampler = SDL_CreateGPUSampler(dev, &si);

    SDL_GPUColorTargetDescription ct = { 0 };
    ct.format = COLOR_FORMAT;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_zero(ci);
    ci.vertex_shader = sh_blit_vert;
    ci.fragment_shader = sh_blit_frag;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.enable_depth_clip = true;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.target_info.color_target_descriptions = &ct;
    ci.target_info.num_color_targets = 1;
    blit_pipe = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    if (!blit_sampler || !blit_pipe) {
        say("the screens' copy");
        dev = NULL;
        return 0;
    }
    printf("[gpu  ] drawing on the graphics card (%s), depth as %s\n", SDL_GetGPUDeviceDriver(dev),
           depth_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32FS8");
    return 1;
}

static void temporal_release(void);

void albw_gpu_close(void) {
    if (!dev) return;
    SDL_WaitForGPUIdle(dev);
    for (int i = 0; i < pipe_count; i++) SDL_ReleaseGPUGraphicsPipeline(dev, pipes[i].p);
    pipe_count = 0;
    for (int i = 0; i < samp_count; i++) SDL_ReleaseGPUSampler(dev, samps[i].s);
    samp_count = 0;
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].tex) SDL_ReleaseGPUTexture(dev, surfaces[i].tex);
        if (surfaces[i].ms) SDL_ReleaseGPUTexture(dev, surfaces[i].ms);
    }
    surface_count = 0;
    for (int i = 0; i < tex_count; i++) {
        SDL_ReleaseGPUTexture(dev, texes[i]->tex);
        free(texes[i]);
    }
    tex_count = 0;
    tex_bytes_kept = 0;
    memset(tex_bucket, 0, sizeof tex_bucket);
    for (int i = 0; i < snap_count; i++) if (snaps[i].tex) SDL_ReleaseGPUTexture(dev, snaps[i].tex);
    snap_count = 0;
    for (int i = 0; i < 2; i++) {
        if (screen_tex[i]) SDL_ReleaseGPUTexture(dev, screen_tex[i]);
        screen_tex[i] = NULL;
    }
    temporal_release();
    SDL_ReleaseGPUGraphicsPipeline(dev, blit_pipe);
    SDL_ReleaseGPUSampler(dev, blit_sampler);
    SDL_ReleaseGPUTexture(dev, black);
    SDL_ReleaseGPUShader(dev, sh_vert);
    SDL_ReleaseGPUShader(dev, sh_frag);
    SDL_ReleaseGPUShader(dev, sh_blit_vert);
    SDL_ReleaseGPUShader(dev, sh_blit_frag);
    sh_vert = sh_frag = sh_blit_vert = sh_blit_frag = NULL;
    dev = NULL;
}

// ---------------------------------------------------------------------------------------------
// Buffers that grow, and the transfer buffer everything goes up through.
struct gbuf {
    SDL_GPUBuffer *buf;
    uint32_t size;
};
static struct gbuf vbuf, sbuf, lbuf;
static SDL_GPUTransferBuffer *upload;
static uint32_t upload_size;

static int gbuf_fit(struct gbuf *b, uint32_t need, SDL_GPUBufferUsageFlags usage) {
    if (need < 16u) need = 16u;
    if (b->buf && b->size >= need) return 1;
    if (b->buf) SDL_ReleaseGPUBuffer(dev, b->buf);
    uint32_t size = b->size ? b->size : 65536u;
    while (size < need) size *= 2u;
    SDL_GPUBufferCreateInfo ci = { usage, size, 0 };
    b->buf = SDL_CreateGPUBuffer(dev, &ci);
    b->size = b->buf ? size : 0u;
    if (!b->buf) say("a buffer");
    return b->buf != NULL;
}

static uint8_t *upload_map(uint32_t need) {
    if (!upload || upload_size < need) {
        if (upload) SDL_ReleaseGPUTransferBuffer(dev, upload);
        uint32_t size = upload_size ? upload_size : (1u << 22);
        while (size < need) size *= 2u;
        SDL_GPUTransferBufferCreateInfo ci = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0 };
        upload = SDL_CreateGPUTransferBuffer(dev, &ci);
        upload_size = upload ? size : 0u;
        if (!upload) {
            say("the upload buffer");
            return NULL;
        }
    }
    return SDL_MapGPUTransferBuffer(dev, upload, true);
}

// ---------------------------------------------------------------------------------------------
// Drawing a frame.

// Per draw: which texture each unit samples, and how its coordinates are mapped onto it.
struct binding {
    SDL_GPUTexture *tex[3];
    SDL_GPUSampler *samp[3];
    float uv[3][4];
};

struct draw_uniforms {
    uint32_t info[4];
    float uv[3][4];
    float view[4], jitter[4];
};

struct blit_uniforms {
    float size[4], row0[4], row1[4];
};

// How a unit's coordinates land on a picture of a target: the target's rows are kept the way the
// console's sampler counts a texture's (row 0 at t = 0), so a texture that is the target is the
// target as it is; a copy of part of one is that part, scaled.
static void snapshot_uv(const struct snapshot *s, const struct albw_gpu_texture *t, float uv[4]) {
    // The texture's row k (from t = 0) is its memory row height - 1 - k, which the copy took from
    // the target's memory row first_row + height - 1 - k: the target's row (as the sampler counts
    // them) surf_h - 1 - that.
    float W = (float)t->width, H = (float)t->height, Ws = (float)s->surf_w, Hs = (float)s->surf_h;
    uv[0] = W / Ws;
    uv[1] = H / Hs;
    uv[2] = 0.0f;
    uv[3] = (Hs - (float)s->first_row - H) / Hs;
}

static void transfer_map(const struct surface *s, const struct albw_gpu_event *e, struct blit_uniforms *u,
                         int which) {
    uint32_t src_w = e->c & 0xFFFFu, src_h = e->c >> 16;
    uint32_t dst_w = e->d & 0xFFFFu, dst_h = e->d >> 16;
    uint32_t in_bpp = s->bytes;
    uint32_t rows = src_h < dst_h ? src_h : dst_h;
    uint32_t cols = src_w < dst_w ? src_w : dst_w;
    float first = (float)((e->a - s->base) / (s->width * in_bpp));    // memory rows
    int flip = (int)(e->e & 1u);
    float Hs = (float)s->height, Ws = (float)s->width;
    float screen_w = which ? 320.0f : 400.0f;
    // The screen's x (0 to 1) runs along the framebuffer's rows, its y back along the columns:
    // framebuffer row r = x * rows takes the target's memory row first + r, or first + src_h - r
    // flipped; its column c = cols - y * cols, the target's own.
    u->size[0] = screen_w * (float)s->scale;
    u->size[1] = 240.0f * (float)s->scale;
    u->size[2] = u->size[3] = 0.0f;
    // source u = c / Ws = (cols - y * cols) / Ws
    u->row0[0] = 0.0f;
    u->row0[1] = -(float)cols / Ws;
    u->row0[2] = (float)cols / Ws;
    u->row0[3] = 0.0f;
    // source v = (Hs - memory row) / Hs
    if (!flip) {
        u->row1[0] = -(float)rows / Hs;
        u->row1[1] = 0.0f;
        u->row1[2] = (Hs - first) / Hs;
    } else {
        u->row1[0] = (float)rows / Hs;
        u->row1[1] = 0.0f;
        u->row1[2] = (Hs - first - (float)src_h) / Hs;
    }
    u->row1[3] = 0.0f;
}

static void fill_to_clear(struct surface *s, const struct albw_gpu_event *e) {
    uint32_t value = e->c, step = e->d;
    s->clear = 1;
    if (s->depth) {
        if (step == 4) {
            s->clear_depth = (float)(value & 0xFFFFFFu) / 16777215.0f;
            s->clear_stencil = (uint8_t)(value >> 24);
        } else if (step == 2) {
            s->clear_depth = (float)(value & 0xFFFFu) / 65535.0f;
            s->clear_stencil = 0;
        } else {
            s->clear_depth = (float)(value & 0xFFFFFFu) / 16777215.0f;
            s->clear_stencil = 0;
        }
        return;
    }
    float r, g, b;
    if (step == 2) {                                     // RGB565, as this game's target is
        r = (float)((value >> 11) & 31u) / 31.0f;
        g = (float)((value >> 5) & 63u) / 63.0f;
        b = (float)(value & 31u) / 31.0f;
    } else if (step == 4) {                              // RGBA8, red in the top byte
        r = (float)((value >> 24) & 255u) / 255.0f;
        g = (float)((value >> 16) & 255u) / 255.0f;
        b = (float)((value >> 8) & 255u) / 255.0f;
    } else {                                             // RGB8, blue first
        r = (float)((value >> 16) & 255u) / 255.0f;
        g = (float)((value >> 8) & 255u) / 255.0f;
        b = (float)(value & 255u) / 255.0f;
    }
    s->clear_color = (SDL_FColor){ r, g, b, 1.0f };
}

// The temporal upscaler's side of a frame: its targets, the top screen's colour as it was at its
// transfer, and what is read back.
static struct albw_gpu_temporal *temporal;              // the frame being drawn wants it
static SDL_GPUTexture *aux_tex[3], *capture_tex;
static uint32_t aux_w, aux_h;
// Two read-back buffers, taken in turn, so that one frame's can be read while the next is drawn.
static SDL_GPUTransferBuffer *readbacks[2];
static uint32_t readback_sizes[2];
static int readback_mapped[2], readback_next;
static SDL_GPUTransferBuffer *readback;
static uint32_t readback_size;

void albw_gpu_temporal_done_slot(int slot) {
    if (slot < 0 || slot > 1 || !readback_mapped[slot]) return;
    SDL_UnmapGPUTransferBuffer(dev, readbacks[slot]);
    readback_mapped[slot] = 0;
}

void albw_gpu_temporal_done(void) {
    albw_gpu_temporal_done_slot(0);
    albw_gpu_temporal_done_slot(1);
}

static void temporal_release(void) {
    albw_gpu_temporal_done();
    for (int i = 0; i < 2; i++) {
        if (readbacks[i]) SDL_ReleaseGPUTransferBuffer(dev, readbacks[i]);
        readbacks[i] = NULL;
        readback_sizes[i] = 0;
    }
    for (int i = 0; i < 3; i++) {
        if (aux_tex[i]) SDL_ReleaseGPUTexture(dev, aux_tex[i]);
        aux_tex[i] = NULL;
    }
    if (capture_tex) SDL_ReleaseGPUTexture(dev, capture_tex);
    capture_tex = NULL;
    aux_w = aux_h = 0;
}

static int temporal_targets(uint32_t w, uint32_t h) {
    if (aux_tex[0] && aux_w == w && aux_h == h) return 1;
    for (int i = 0; i < 3; i++) {
        if (aux_tex[i]) SDL_ReleaseGPUTexture(dev, aux_tex[i]);
        aux_tex[i] = NULL;
    }
    if (capture_tex) SDL_ReleaseGPUTexture(dev, capture_tex);
    capture_tex = NULL;
    static const SDL_GPUTextureFormat formats[3] = { MOTION_FORMAT, DEPTHCOPY_FORMAT, REACTIVE_FORMAT };
    SDL_GPUTextureCreateInfo ci;
    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    for (int i = 0; i < 3; i++) {
        ci.format = formats[i];
        aux_tex[i] = SDL_CreateGPUTexture(dev, &ci);
        if (!aux_tex[i]) {
            say("the upscaler's targets");
            return 0;
        }
    }
    ci.format = COLOR_FORMAT;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    capture_tex = SDL_CreateGPUTexture(dev, &ci);
    if (!capture_tex) return 0;
    aux_w = w;
    aux_h = h;
    return 1;
}

static int draw_frame(const struct albw_gpu_frame *f, int scale, SDL_GPUTexture *out[2]);

int albw_gpu_draw(const struct albw_gpu_frame *f, int scale, SDL_GPUTexture *out[2]) {
    temporal = NULL;
    return draw_frame(f, scale, out);
}

int albw_gpu_draw_temporal(const struct albw_gpu_frame *f, int scale, struct albw_gpu_temporal *t,
                           SDL_GPUTexture *out[2]) {
    if (!dev || !f || f->screen_ev[0] < 0) return 0;
    // The buffer to read back into: the one not handed out, or, with both out, the older.
    int slot = readback_mapped[readback_next] ? !readback_next : readback_next;
    albw_gpu_temporal_done_slot(slot);
    readback = readbacks[slot];
    readback_size = readback_sizes[slot];
    t->slot = slot;
    temporal = t;
    int ok = draw_frame(f, scale, out);
    temporal = NULL;
    readbacks[slot] = readback;
    readback_sizes[slot] = readback_size;
    if (ok) {
        readback_mapped[slot] = 1;
        readback_next = !slot;
    }
    return ok;
}

static int draw_frame(const struct albw_gpu_frame *f, int scale, SDL_GPUTexture *out[2]) {
    if (!dev || !f) return 0;
    if (scale < 1) scale = 1;
    if (scale > 16) scale = 16;
    frames_drawn++;
    // As many samples as asked for, or the most both a colour and a depth target can have below it.
    samples = 1;
    // An upscaler does its own anti-aliasing, from the jitter; its targets are single-sampled.
    for (int n = temporal ? 1 : samples_wanted >= 8 ? 8 : samples_wanted >= 4 ? 4 : samples_wanted >= 2 ? 2 : 1;
         n > 1; n /= 2) {
        if (SDL_GPUTextureSupportsSampleCount(dev, COLOR_FORMAT, sample_count(n))
                && SDL_GPUTextureSupportsSampleCount(dev, depth_format, sample_count(n))) {
            samples = n;
            break;
        }
    }
    out[0] = screen_texture(0, scale);
    out[1] = screen_texture(1, scale);
    if (!out[0] || !out[1]) return 0;

    // What each draw samples, worked out in the order of the frame's events, since a texture copy
    // part of the way through makes a texture the draws after it sample.
    static struct binding *binds;
    static uint32_t bind_room;
    static struct texentry **uploads;
    static uint32_t upload_room;
    if (f->draw_count > bind_room) {
        struct binding *bigger = realloc(binds, f->draw_count * sizeof *bigger);
        if (!bigger) return 0;
        binds = bigger;
        bind_room = f->draw_count;
    }
    uint32_t upload_count = 0;
    size_t texture_bytes = 0;
    for (uint32_t i = 0; i < f->ev_count; i++) {
        const struct albw_gpu_event *e = &f->ev[i];
        if (e->kind == ALBW_GPU_TEXCOPY) {
            snapshot_prepare(e);
            continue;
        }
        if (e->kind != ALBW_GPU_DRAW) continue;
        const struct albw_gpu_draw *d = &f->draw[e->a];
        struct binding *b = &binds[e->a];
        for (int u = 0; u < 3; u++) {
            const struct albw_gpu_texture *t = &d->tex[u];
            b->tex[u] = black;
            b->samp[u] = blit_sampler;
            b->uv[u][0] = b->uv[u][1] = 1.0f;
            b->uv[u][2] = b->uv[u][3] = 0.0f;
            if (!t->enabled || !t->width || !t->height) continue;
            // A target, sampled as a texture: what the card drew there.
            struct surface *s = surface_find(t->base, 0);
            if (s && s->tex && s->width == t->width) {
                b->tex[u] = s->tex;
                b->samp[u] = sampler_for(t, 1);
                b->uv[u][1] = (float)t->height / (float)s->height;
                b->uv[u][3] = 1.0f - b->uv[u][1];
                continue;
            }
            // A texture the game made by copying a target, while its bytes are still the copy's.
            struct snapshot *sn = snapshot_for(t->base);
            if (sn) {
                uint32_t span = sn->bytes;
                if (sn->pending || (albw_guest_ok(sn->dst, span)
                                    && hash_bytes(albw_mem + sn->dst, span) == sn->token)) {
                    b->tex[u] = sn->tex;
                    b->samp[u] = sampler_for(t, 1);
                    snapshot_uv(sn, t, b->uv[u]);
                    continue;
                }
            }
            struct texentry *up = NULL;
            struct texentry *te = texture_get(t, &up);
            if (!te) continue;
            b->tex[u] = te->tex;
            b->samp[u] = sampler_for(t, (int)te->levels);
            if (up) {
                if (upload_count == upload_room) {
                    uint32_t room = upload_room ? upload_room * 2u : 64u;
                    struct texentry **bigger = realloc(uploads, room * sizeof *bigger);
                    if (!bigger) continue;
                    uploads = bigger;
                    upload_room = room;
                }
                uploads[upload_count++] = up;
                for (uint32_t k = 0; k < up->levels; k++) {
                    texture_bytes += (size_t)(up->width >> k) * (up->height >> k) * 4u;
                }
            }
        }
        if (!b->samp[0] || !b->samp[1] || !b->samp[2]) return 0;
    }
    // Everything the frame needs, up in one go.
    uint32_t vbytes = f->vert_count * (uint32_t)sizeof(struct albw_gpu_vertex);
    uint32_t sbytes = f->state_count * ST_WORDS * 4u;
    uint32_t lbytes = f->lut_count * 256u * 4u;
    if (!gbuf_fit(&vbuf, vbytes, SDL_GPU_BUFFERUSAGE_VERTEX)
            || !gbuf_fit(&sbuf, sbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ)
            || !gbuf_fit(&lbuf, lbytes ? lbytes : 1024u, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ)) {
        return 0;
    }
    size_t total = (size_t)vbytes + sbytes + lbytes + texture_bytes + 256u;
    if (total > 0x7FFFFFFFu) return 0;
    uint8_t *map = upload_map((uint32_t)total);
    if (!map) return 0;
    uint32_t at = 0;
    memcpy(map + at, f->vert, vbytes);
    uint32_t v_at = at;
    at += vbytes;
    memcpy(map + at, f->state, sbytes);
    uint32_t s_at = at;
    at += sbytes;
    memcpy(map + at, f->lut, lbytes);
    uint32_t l_at = at;
    at += lbytes;
    static uint32_t *level_at;
    static uint32_t level_room;
    if (upload_count * 8u > level_room) {
        uint32_t *bigger = realloc(level_at, upload_count * 8u * sizeof *bigger);
        if (!bigger) {
            SDL_UnmapGPUTransferBuffer(dev, upload);
            return 0;
        }
        level_at = bigger;
        level_room = upload_count * 8u;
    }
    for (uint32_t i = 0; i < upload_count; i++) {
        const struct texentry *te = uploads[i];
        uint32_t base = te->base;
        for (uint32_t k = 0; k < te->levels && k < 8u; k++) {
            uint32_t w = te->width >> k, h = te->height >> k;
            level_at[i * 8u + k] = at;
            albw_raster_decode_texture(base, w, h, te->format, map + at);
            at += w * h * 4u;
            base += albw_raster_texture_bytes(w, h, te->format);
        }
    }
    SDL_UnmapGPUTransferBuffer(dev, upload);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) {
        say("a command buffer");
        return 0;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation loc = { upload, v_at };
    SDL_GPUBufferRegion reg = { vbuf.buf, 0, vbytes };
    if (vbytes) SDL_UploadToGPUBuffer(cp, &loc, &reg, true);
    loc.offset = s_at;
    reg = (SDL_GPUBufferRegion){ sbuf.buf, 0, sbytes };
    if (sbytes) SDL_UploadToGPUBuffer(cp, &loc, &reg, true);
    loc.offset = l_at;
    reg = (SDL_GPUBufferRegion){ lbuf.buf, 0, lbytes };
    if (lbytes) SDL_UploadToGPUBuffer(cp, &loc, &reg, true);
    for (uint32_t i = 0; i < upload_count; i++) {
        const struct texentry *te = uploads[i];
        for (uint32_t k = 0; k < te->levels && k < 8u; k++) {
            uint32_t w = te->width >> k, h = te->height >> k;
            SDL_GPUTextureTransferInfo src = { upload, level_at[i * 8u + k], w, h };
            SDL_GPUTextureRegion dst = { te->tex, k, 0, 0, 0, 0, w, h, 1 };
            SDL_UploadToGPUTexture(cp, &src, &dst, k == 0);
        }
    }
    SDL_EndGPUCopyPass(cp);

    // The events, in order.
    SDL_GPURenderPass *pass = NULL;
    struct surface *pass_color = NULL, *pass_depth = NULL;
    SDL_GPUGraphicsPipeline *bound = NULL;
    uint32_t blend_constant = 0xFFFFFFFFu;
    int ref = -1;
    // For an upscaler: the top screen's target, whose draws up to its transfer are jittered and
    // write the upscaler's targets as well.
    uint32_t top_base = temporal && f->screen_ev[0] >= 0 ? f->ev[f->screen_ev[0]].a : 0;
    int pass_aux = 0, aux_fresh = 1, captured = 0;
    for (uint32_t i = 0; i < f->ev_count; i++) {
        const struct albw_gpu_event *e = &f->ev[i];
        if (e->kind == ALBW_GPU_FILL) {
            if (pass) {
                SDL_EndGPURenderPass(pass);
                pass = NULL;
            }
            for (int k = 0; k < surface_count; k++) {
                struct surface *s = &surfaces[k];
                if (s->base == e->a) fill_to_clear(s, e);
            }
            continue;
        }
        if (e->kind == ALBW_GPU_TRANSFER) {
            int which = (int)i == f->screen_ev[0] ? 0 : (int)i == f->screen_ev[1] ? 1 : -1;
            if (which < 0) continue;
            if (pass) {
                SDL_EndGPURenderPass(pass);
                pass = NULL;
            }
            struct surface *s = surface_holding(e->a);
            if (!s || !s->tex) continue;
            if (temporal && which == 0 && temporal_targets(s->width * (uint32_t)s->scale,
                                                           s->height * (uint32_t)s->scale)) {
                // The top screen's colour for the upscaler, before the bottom screen is drawn
                // over the same target.
                SDL_GPUCopyPass *c3 = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureLocation from = { s->tex, 0, 0, 0, 0, 0 };
                SDL_GPUTextureLocation to = { capture_tex, 0, 0, 0, 0, 0 };
                SDL_CopyGPUTextureToTexture(c3, &from, &to, aux_w, aux_h, 1, false);
                SDL_EndGPUCopyPass(c3);
                captured = 1;
            }
            struct blit_uniforms bu;
            transfer_map(s, e, &bu, which);
            SDL_GPUColorTargetInfo ct;
            SDL_zero(ct);
            ct.texture = out[which];
            ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *bp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
            if (!bp) continue;
            SDL_BindGPUGraphicsPipeline(bp, blit_pipe);
            SDL_GPUTextureSamplerBinding sb = { s->tex, blit_sampler };
            SDL_BindGPUFragmentSamplers(bp, 0, &sb, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &bu, sizeof bu);
            SDL_DrawGPUPrimitives(bp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(bp);
            bound = NULL;
            continue;
        }
        if (e->kind == ALBW_GPU_TEXCOPY) {
            struct snapshot *sn = NULL;
            for (int k = 0; k < snap_count; k++) if (snaps[k].dst == e->b && snaps[k].pending) sn = &snaps[k];
            if (!sn) continue;
            struct surface *s = surface_find(sn->surf_base, 0);
            sn->pending = 0;
            if (!s || !s->tex || !sn->tex) continue;
            if (pass) {
                SDL_EndGPURenderPass(pass);
                pass = NULL;
            }
            SDL_GPUCopyPass *c2 = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureLocation from = { s->tex, 0, 0, 0, 0, 0 };
            SDL_GPUTextureLocation to = { sn->tex, 0, 0, 0, 0, 0 };
            SDL_CopyGPUTextureToTexture(c2, &from, &to, s->width * (uint32_t)s->scale,
                                        s->height * (uint32_t)s->scale, 1, false);
            SDL_EndGPUCopyPass(c2);
            continue;
        }

        const struct albw_gpu_draw *d = &f->draw[e->a];
        struct surface *sc = surface_get(d->color_base, d->width, d->height, d->color_bytes, 0, scale);
        struct surface *sd = d->depth_base ? surface_get(d->depth_base, d->width, d->height,
                                                         d->depth_bytes, 1, scale)
                                           : NULL;
        if (!sc || !sc->tex) continue;
        sc->used = frames_drawn;
        if (sd) sd->used = frames_drawn;
        int aux = temporal && !captured && d->color_base == top_base
               && temporal_targets(sc->width * (uint32_t)sc->scale, sc->height * (uint32_t)sc->scale);
        if (pass && (pass_color != sc || pass_depth != sd || sc->clear || (sd && sd->clear)
                     || pass_aux != aux)) {
            SDL_EndGPURenderPass(pass);
            pass = NULL;
        }
        if (!pass) {
            SDL_GPUColorTargetInfo ct;
            SDL_zero(ct);
            ct.load_op = sc->clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            ct.clear_color = sc->clear_color;
            if (sc->ms) {
                // Drawn multisampled; at the pass's end the samples are kept for the next one and
                // resolved into the texture everything else reads.
                ct.texture = sc->ms;
                ct.resolve_texture = sc->tex;
                ct.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
            } else {
                ct.texture = sc->tex;
                ct.store_op = SDL_GPU_STOREOP_STORE;
            }
            sc->clear = 0;
            SDL_GPUDepthStencilTargetInfo dt;
            SDL_zero(dt);
            if (sd && sd->tex) {
                dt.texture = sd->tex;
                dt.load_op = sd->clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
                dt.stencil_load_op = dt.load_op;
                dt.clear_depth = sd->clear_depth;
                dt.clear_stencil = sd->clear_stencil;
                dt.store_op = SDL_GPU_STOREOP_STORE;
                dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
                sd->clear = 0;
            }
            SDL_GPUColorTargetInfo cts[4];
            cts[0] = ct;
            if (aux) {
                // Cleared at the frame's first pass that has them: no motion, far, not blended.
                static const SDL_FColor clears[3] = { { 0, 0, 0, 0 }, { 1, 1, 1, 1 }, { 0, 0, 0, 0 } };
                for (int k = 0; k < 3; k++) {
                    SDL_zero(cts[k + 1]);
                    cts[k + 1].texture = aux_tex[k];
                    cts[k + 1].load_op = aux_fresh ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
                    cts[k + 1].clear_color = clears[k];
                    cts[k + 1].store_op = SDL_GPU_STOREOP_STORE;
                }
                aux_fresh = 0;
            }
            pass = SDL_BeginGPURenderPass(cmd, cts, aux ? 4 : 1, sd && sd->tex ? &dt : NULL);
            if (!pass) continue;
            pass_aux = aux;
            pass_color = sc;
            pass_depth = sd;
            bound = NULL;
            blend_constant = 0xFFFFFFFFu;
            ref = -1;
            SDL_GPUBufferBinding vb = { vbuf.buf, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_GPUBuffer *storage[2] = { sbuf.buf, lbuf.buf };
            SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
        }
        SDL_GPUGraphicsPipeline *p = pipeline_for(d, aux);
        if (!p) continue;
        if (p != bound) {
            SDL_BindGPUGraphicsPipeline(pass, p);
            bound = p;
        }
        float sc_f = (float)scale;
        float jx = aux ? temporal->jitter[0] : 0.0f, jy = aux ? temporal->jitter[1] : 0.0f;
        SDL_GPUViewport vp = { d->offset_x * sc_f + jx, d->offset_y * sc_f + jy, 2.0f * d->half_w * sc_f,
                               2.0f * d->half_h * sc_f, 0.0f, 1.0f };
        SDL_SetGPUViewport(pass, &vp);
        int x0 = d->scissor[0] < 0 ? 0 : d->scissor[0], y0 = d->scissor[1] < 0 ? 0 : d->scissor[1];
        int x1 = d->scissor[2] >= (int)d->width ? (int)d->width - 1 : d->scissor[2];
        int y1 = d->scissor[3] >= (int)d->height ? (int)d->height - 1 : d->scissor[3];
        if (x1 < x0 || y1 < y0) continue;
        SDL_Rect sr = { x0 * scale, y0 * scale, (x1 - x0 + 1) * scale, (y1 - y0 + 1) * scale };
        SDL_SetGPUScissor(pass, &sr);
        if (d->stencil && ref != d->stencil_ref) {
            SDL_SetGPUStencilReference(pass, d->stencil_ref);
            ref = d->stencil_ref;
        }
        if (d->blend) {
            uint32_t c = d->blend_color;
            uint32_t f16 = d->blend_func;
            uint32_t fs[4] = { (f16 >> 16) & 15u, (f16 >> 20) & 15u, (f16 >> 24) & 15u, (f16 >> 28) & 15u };
            int alpha_only = 0;
            for (int k = 0; k < 4; k++) alpha_only |= fs[k] == 12u || fs[k] == 13u;
            if (alpha_only) c = (c >> 24) * 0x01010101u;
            if (c != blend_constant) {
                SDL_FColor fc = { (float)(c & 255u) / 255.0f, (float)((c >> 8) & 255u) / 255.0f,
                                  (float)((c >> 16) & 255u) / 255.0f, (float)(c >> 24) / 255.0f };
                SDL_SetGPUBlendConstants(pass, fc);
                blend_constant = c;
            }
        }
        const struct binding *b = &binds[e->a];
        SDL_GPUTextureSamplerBinding tsb[3];
        for (int u = 0; u < 3; u++) {
            tsb[u].texture = b->tex[u];
            tsb[u].sampler = b->samp[u];
        }
        SDL_BindGPUFragmentSamplers(pass, 0, tsb, 3);
        struct draw_uniforms du;
        du.info[0] = d->state * ST_WORDS;
        du.info[1] = d->depth_test && (d->depth_func == 6 || d->depth_func == 7);
        du.info[2] = du.info[3] = 0;
        memcpy(du.uv, b->uv, sizeof du.uv);
        du.view[0] = d->half_w * sc_f;
        du.view[1] = d->half_h * sc_f;
        du.view[2] = d->offset_x * sc_f;
        du.view[3] = d->offset_y * sc_f;
        du.jitter[0] = jx;
        du.jitter[1] = jy;
        du.jitter[2] = d->blend && !d->depth_write ? 200.0f / 255.0f : 0.0f;
        du.jitter[3] = 0.0f;
        SDL_PushGPUFragmentUniformData(cmd, 0, &du, sizeof du);
        SDL_DrawGPUPrimitives(pass, d->vertices, 1, d->first_vertex, 0);
    }
    if (pass) SDL_EndGPURenderPass(pass);
    if (!temporal) {
        SDL_SubmitGPUCommandBuffer(cmd);
        textures_trim();
        return 1;
    }
    // The upscaler's inputs, back to the processor: the window's thread waits for the card here.
    if (!captured) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return 0;
    }
    uint32_t px = aux_w * aux_h;
    uint32_t need = px * (4u + 8u + 4u + 1u);
    if (!readback || readback_size < need) {
        if (readback) SDL_ReleaseGPUTransferBuffer(dev, readback);
        SDL_GPUTransferBufferCreateInfo ci = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, need, 0 };
        readback = SDL_CreateGPUTransferBuffer(dev, &ci);
        readback_size = readback ? need : 0u;
        if (!readback) {
            SDL_SubmitGPUCommandBuffer(cmd);
            return 0;
        }
    }
    SDL_GPUCopyPass *dp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTexture *from[4] = { capture_tex, aux_tex[0], aux_tex[1], aux_tex[2] };
    uint32_t bytes[4] = { 4u, 8u, 4u, 1u }, offset = 0;
    uint32_t offsets[4];
    for (int k = 0; k < 4; k++) {
        offsets[k] = offset;
        SDL_GPUTextureRegion src = { from[k], 0, 0, 0, 0, 0, aux_w, aux_h, 1 };
        SDL_GPUTextureTransferInfo dst = { readback, offset, aux_w, aux_h };
        SDL_DownloadFromGPUTexture(dp, &src, &dst);
        offset += px * bytes[k];
    }
    SDL_EndGPUCopyPass(dp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return 0;
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);
    // Handed over where they lie, in the read-back buffer, until albw_gpu_temporal_done.
    const uint8_t *m = SDL_MapGPUTransferBuffer(dev, readback, false);
    if (!m) return 0;
    temporal->width = aux_w;
    temporal->height = aux_h;
    temporal->rgba = m + offsets[0];
    temporal->motion = (const float *)(m + offsets[1]);
    temporal->depth = (const float *)(m + offsets[2]);
    temporal->reactive = m + offsets[3];
    textures_trim();
    return 1;
}

// ---------------------------------------------------------------------------------------------
// An upscaler's picture of the top screen back on the card, turned the way the screen shows it.
static SDL_GPUTexture *upscaled_in, *upscaled_out;
static int upscaled_w, upscaled_h;
static SDL_GPUTransferBuffer *upscaled_up;
static uint32_t upscaled_up_size;

SDL_GPUTexture *albw_gpu_show_upscaled(const void *pixels, int width, int height, int rgba) {
    if (!dev || width < 1 || height < 1) return NULL;
    static int upscaled_rgba = -1;
    if (!upscaled_in || upscaled_w != width || upscaled_h != height || upscaled_rgba != rgba) {
        upscaled_rgba = rgba;
        if (upscaled_in) SDL_ReleaseGPUTexture(dev, upscaled_in);
        if (upscaled_out) SDL_ReleaseGPUTexture(dev, upscaled_out);
        SDL_GPUTextureCreateInfo ci;
        SDL_zero(ci);
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        // The words, as they lie in memory: blue first, or red.
        ci.format = rgba ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = (Uint32)width;
        ci.height = (Uint32)height;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        upscaled_in = SDL_CreateGPUTexture(dev, &ci);
        ci.format = COLOR_FORMAT;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ci.width = (Uint32)height;
        ci.height = (Uint32)width;
        upscaled_out = SDL_CreateGPUTexture(dev, &ci);
        if (!upscaled_in || !upscaled_out) {
            say("the upscaled screen");
            return NULL;
        }
        upscaled_w = width;
        upscaled_h = height;
    }
    uint32_t bytes = (uint32_t)width * (uint32_t)height * 4u;
    if (!upscaled_up || upscaled_up_size < bytes) {
        if (upscaled_up) SDL_ReleaseGPUTransferBuffer(dev, upscaled_up);
        SDL_GPUTransferBufferCreateInfo ci = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0 };
        upscaled_up = SDL_CreateGPUTransferBuffer(dev, &ci);
        upscaled_up_size = upscaled_up ? bytes : 0u;
        if (!upscaled_up) return NULL;
    }
    uint8_t *map = SDL_MapGPUTransferBuffer(dev, upscaled_up, true);
    if (!map) return NULL;
    memcpy(map, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(dev, upscaled_up);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return NULL;
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = { upscaled_up, 0, (Uint32)width, (Uint32)height };
    SDL_GPUTextureRegion dst = { upscaled_in, 0, 0, 0, 0, 0, (Uint32)width, (Uint32)height, 1 };
    SDL_UploadToGPUTexture(cp, &src, &dst, true);
    SDL_EndGPUCopyPass(cp);
    // The screen's x runs back along the target's rows (as the top screen's transfer reads them),
    // its y back along the columns.
    struct blit_uniforms bu = {
        { (float)height, (float)width, 0.0f, 0.0f },
        { 0.0f, -1.0f, 1.0f, 0.0f },
        { -1.0f, 0.0f, 1.0f, 0.0f },
    };
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = upscaled_out;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *bp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (bp) {
        SDL_BindGPUGraphicsPipeline(bp, blit_pipe);
        SDL_GPUTextureSamplerBinding sb = { upscaled_in, blit_sampler };
        SDL_BindGPUFragmentSamplers(bp, 0, &sb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, &bu, sizeof bu);
        SDL_DrawGPUPrimitives(bp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(bp);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    return upscaled_out;
}

// ---------------------------------------------------------------------------------------------
// ALBW_GPU_TEST=<first>[-<last>]:<scale>:<directory>[:alone], headless: those frames drawn on a GPU
// device of its own, at that scale, as the window would draw them one after another, and the last
// one's screens written as gpu-<frame>-top.ppm and gpu-<frame>-bottom.ppm -- to be set beside the
// frame dump of the frame after (a frame's picture is shown from the next one). With ":alone" the
// processor draws nothing from the first frame on, as with the window: what the card draws then
// has nothing of the processor's in it.
static void write_ppm(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; i++) fwrite(rgba + i * 4u, 1, 3, f);
    fclose(f);
}

void albw_gpu_test(unsigned long frame) {
    static long at = -2, last = -1;
    static int scale = 1, alone;
    static char dir[512];
    if (at == -2) {
        const char *e = albw_env("ALBW_GPU_TEST");
        at = -1;
        if (e) {
            char *end;
            at = strtol(e, &end, 0);
            last = at;
            if (*end == '-') last = strtol(end + 1, &end, 0);
            if (*end == ':') scale = (int)strtol(end + 1, &end, 0);
            if (*end == ':') snprintf(dir, sizeof dir, "%s", end + 1);
            else snprintf(dir, sizeof dir, ".");
            char *colon = strrchr(dir, ':');
            if (colon && !strcmp(colon, ":alone")) {
                *colon = '\0';
                alone = 1;
            }
        }
    }
    if (at < 0 || (long)frame < at - 1 || (long)frame > last) return;
    if ((long)frame == at - 1) {
        if (alone) albw_raster_set_gpu_display(1);   // from the next frame on, the card's alone
        return;
    }
    if (!dev) {
        // No window: the offscreen video driver is enough for a device, and touches no display.
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            say("[gpu test] no video");
            return;
        }
        SDL_GPUDevice *d = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
        if (!d) {
            say("[gpu test] no device");
            return;
        }
        if (!albw_gpu_open(d)) return;
        const char *n = albw_env("ALBW_GPU_SAMPLES");
        if (n) albw_gpu_set_samples(atoi(n));
    }
    unsigned long later;
    if (!albw_raster_between_hold(&later)) {
        printf("[gpu  ] test: no record of frame %lu\n", frame);
        return;
    }
    static struct albw_gpu_frame f;
    int ok = albw_raster_gpu_frame(&f, 1.0f);
    albw_raster_between_release();
    if (!ok) return;
    SDL_GPUTexture *out[2];
    uint64_t t0 = SDL_GetTicksNS();
    if (!albw_gpu_draw(&f, scale, out)) {
        printf("[gpu  ] test: the frame did not draw\n");
        return;
    }
    if ((long)frame != last) return;
    for (int which = 0; which < 2; which++) {
        uint32_t w = (which ? 320u : 400u) * (uint32_t)scale, h = 240u * (uint32_t)scale;
        SDL_GPUTransferBufferCreateInfo ci = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4u, 0 };
        SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(dev, &ci);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!down || !cmd) return;
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src = { out[which], 0, 0, 0, 0, 0, w, h, 1 };
        SDL_GPUTextureTransferInfo dst = { down, 0, w, h };
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        const uint8_t *p = SDL_MapGPUTransferBuffer(dev, down, false);
        char path[600];
        snprintf(path, sizeof path, "%s/gpu-%lu-%s.ppm", dir, frame, which ? "bottom" : "top");
        if (p) write_ppm(path, p, w, h);
        SDL_UnmapGPUTransferBuffer(dev, down);
        SDL_ReleaseGPUTransferBuffer(dev, down);
    }
    printf("[gpu  ] test: frame %lu drawn at %dx in %.2f ms (with the downloads), %u draws, %u "
           "vertices, %u states, %d pipelines, %d textures\n", frame, scale,
           (double)(SDL_GetTicksNS() - t0) / 1e6, f.draw_count, f.vert_count, f.state_count,
           pipe_count, tex_count);
}

// ALBW_GPU_FSR3_TEST=<first>-<last>:<render scale>:<shown scale>:<directory>, headless: those frames
// drawn on the card at the render scale with the upscaler's jitter and targets, each upscaled by
// FSR 3 to the shown scale; the last one's result written as fsr3-<frame>.ppm, beside the same frame
// drawn straight at the shown scale as gpu-<frame>-native.ppm.
static void write_ppm_words(const char *path, const uint32_t *px, uint32_t w, uint32_t h, int turn) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    // Turned as the top screen shows it: the screen's (x, y) is the target's (w-1-y, h-1-x).
    uint32_t ow = turn ? h : w, oh = turn ? w : h;
    fprintf(f, "P6\n%u %u\n255\n", ow, oh);
    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t v = turn ? px[(size_t)(h - 1u - x) * w + (w - 1u - y)] : px[(size_t)y * w + x];
            uint8_t rgb[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

void albw_gpu_fsr3_test(unsigned long frame) {
    static long first = -2, last = -1;
    static int render_scale = 2, shown_scale = 4;
    static char dir[512];
    static uint32_t index;
    if (first == -2) {
        const char *e = albw_env("ALBW_GPU_FSR3_TEST");
        first = -1;
        if (e) {
            char *end;
            first = strtol(e, &end, 0);
            last = first;
            if (*end == '-') last = strtol(end + 1, &end, 0);
            if (*end == ':') render_scale = (int)strtol(end + 1, &end, 0);
            if (*end == ':') shown_scale = (int)strtol(end + 1, &end, 0);
            snprintf(dir, sizeof dir, "%s", *end == ':' ? end + 1 : ".");
        }
    }
    if (first < 0 || (long)frame < first || (long)frame > last) return;
    if (!dev) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_Init(SDL_INIT_VIDEO)) return;
        SDL_GPUDevice *d = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
        if (!d || !albw_gpu_open(d)) return;
    }
    unsigned long later;
    if (!albw_raster_between_hold(&later)) return;
    static struct albw_gpu_frame f;
    f.want_prev = 1;
    int ok = albw_raster_gpu_frame(&f, 1.0f);
    albw_raster_between_release();
    if (!ok) return;
    uint32_t rw = 240u * (uint32_t)render_scale, rh = 400u * (uint32_t)render_scale;
    uint32_t ow = 240u * (uint32_t)shown_scale, oh = 400u * (uint32_t)shown_scale;
    struct albw_gpu_temporal t;
    memset(&t, 0, sizeof t);
    albw_vkup_jitter(index, (int)rh, (int)oh, &t.jitter[0], &t.jitter[1]);
    SDL_GPUTexture *out[2];
    if (!albw_gpu_draw_temporal(&f, render_scale, &t, out)) {
        printf("[gpu  ] fsr3 test: frame %lu did not draw\n", frame);
        return;
    }
    static uint32_t *up;
    static size_t up_room;
    if (up_room < (size_t)ow * oh) {
        free(up);
        up = malloc((size_t)ow * oh * 4u);
        up_room = up ? (size_t)ow * oh : 0;
        if (!up) return;
    }
    struct albw_vkup_frame in = {
        (int)t.width, (int)t.height, NULL, t.rgba, t.depth, t.motion, t.reactive,
        t.jitter[0], t.jitter[1], (long)frame == first, 0.8f, 16.7f, 1.0f, 1000.0f, 0.6f,
    };
    uint64_t t0 = SDL_GetTicksNS();
    ok = albw_fsr3_upscale(&in, up, (int)ow, (int)oh, (int)ow * 4);
    uint64_t t1 = SDL_GetTicksNS();
    index++;
    if ((long)frame != last) {
        albw_gpu_temporal_done();
        return;
    }
    if ((long)frame != last) return;
    printf("[gpu  ] fsr3 test: frame %lu, %ux%u to %ux%u: %s in %.2f ms\n", frame, rw, rh, ow, oh,
           ok ? "upscaled" : "FAILED", (double)(t1 - t0) / 1e6);
    char path[600];
    snprintf(path, sizeof path, "%s/fsr3-%lu.ppm", dir, frame);
    if (ok) write_ppm_words(path, up, ow, oh, 1);
    // The input, turned the same way, for seeing what FSR 3 was given.
    static uint32_t *inwords;
    static size_t in_room;
    if (in_room < (size_t)t.width * t.height) {
        free(inwords);
        inwords = malloc((size_t)t.width * t.height * 4u);
        in_room = inwords ? (size_t)t.width * t.height : 0;
    }
    if (inwords) {
        for (size_t i = 0; i < (size_t)t.width * t.height; i++) {
            inwords[i] = 0xFF000000u | ((uint32_t)t.rgba[i * 4] << 16) | ((uint32_t)t.rgba[i * 4 + 1] << 8)
                       | t.rgba[i * 4 + 2];
        }
        snprintf(path, sizeof path, "%s/fsr3-%lu-in.ppm", dir, frame);
        write_ppm_words(path, inwords, t.width, t.height, 1);
        // The motion, as colour: x in red, y in green, around grey.
        for (size_t i = 0; i < (size_t)t.width * t.height; i++) {
            float mx = t.motion[i * 2] * 8.0f + 128.0f, my = t.motion[i * 2 + 1] * 8.0f + 128.0f;
            uint32_t r = mx < 0 ? 0 : mx > 255 ? 255 : (uint32_t)mx;
            uint32_t g = my < 0 ? 0 : my > 255 ? 255 : (uint32_t)my;
            inwords[i] = 0xFF000000u | (r << 16) | (g << 8) | 128u;
        }
        snprintf(path, sizeof path, "%s/fsr3-%lu-motion.ppm", dir, frame);
        write_ppm_words(path, inwords, t.width, t.height, 1);
    }
    albw_gpu_temporal_done();
    // And the frame drawn straight at the shown scale.
    if (albw_gpu_draw(&f, shown_scale, out)) {
        uint32_t w = 400u * (uint32_t)shown_scale, h = 240u * (uint32_t)shown_scale;
        SDL_GPUTransferBufferCreateInfo ci = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4u, 0 };
        SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(dev, &ci);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (down && cmd) {
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src = { out[0], 0, 0, 0, 0, 0, w, h, 1 };
            SDL_GPUTextureTransferInfo dst = { down, 0, w, h };
            SDL_DownloadFromGPUTexture(cp, &src, &dst);
            SDL_EndGPUCopyPass(cp);
            SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            const uint8_t *p = SDL_MapGPUTransferBuffer(dev, down, false);
            snprintf(path, sizeof path, "%s/gpu-%lu-native.ppm", dir, frame);
            if (p) write_ppm(path, p, w, h);
            SDL_UnmapGPUTransferBuffer(dev, down);
        }
        if (down) SDL_ReleaseGPUTransferBuffer(dev, down);
    }
}

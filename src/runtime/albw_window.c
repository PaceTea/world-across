#include "albw_window.h"
#include "albw_dsp.h"
#include "albw_launcher.h"

#include "albw_gpu.h"
#include "albw_kernel.h"
#include "albw_raster.h"
#include "albw_screen.h"
#include "albw_upscale.h"
#include "albw_vkup.h"

#include <SDL3/SDL.h>

#ifdef ALBW_HAVE_FSR
#include "fsr_easu.h"
#include "fsr_rcas.h"
#endif

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The screens, as the console has them: the top one 400 by 240, the bottom one 320 by 240.
#define TOP_W    400
#define BOTTOM_W 320
#define SCREEN_H 240

// How the two screens are arranged in the window. Stacked is the console's own arrangement; side by
// side suits a wide display; top alone gives the picture the most room, at the cost of the touch
// screen.
enum layout { LAYOUT_STACKED, LAYOUT_SIDE, LAYOUT_TOP };

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *tex_top, *tex_bottom;
static enum layout layout = LAYOUT_STACKED;
static SDL_FRect rect_top, rect_bottom;
static int logical_w, logical_h;

// AMD FSR 1, when the renderer is SDL's GPU one and the shaders were built: EASU scales each screen
// into a target the size it is shown at, and RCAS sharpens that onto the window, pixel for pixel.
// A state per pass and screen, each holding its own sizes, so that the four draws of a frame never
// share uniforms that a later draw has already changed.
// The GPU renderer (albw_gpu.c): whether it opened on SDL's device, the last frame handed to it, and
// the two screens it drew, as textures SDL's renderer can show.
static int gpu_ok, gpu_has_picture;
static struct albw_gpu_frame gpu_frame;
static SDL_Texture *gpu_view[2];
static SDL_GPUTexture *gpu_view_of[2];
static int gpu_view_scale[2];
static SDL_GPURenderState *fsr_easu[2], *fsr_rcas[2];
static SDL_Texture *fsr_target[2];
static int fsr_ok;

// The same filter on the processor, into a texture the size each screen is shown at, for when the
// renderer cannot run the shaders. The last frame scaled is remembered, so that a paused game, drawn
// again and again under the menu, is not scaled again and again.
// It costs some 40 ns a pixel on one thread, so a screen is scaled again only when its picture
// changed -- the bottom one mostly does not -- and to no more than three times its size, the
// renderer's bilinear scaling doing the rest on a larger window.
#define CPU_MAX_SCALE 3
static SDL_Texture *cpu_target[2];
static uint8_t cpu_source[2][TOP_W * 240 * 3];
static int cpu_sharpness[2] = { -1, -1 };
static int cpu_valid[2];

int albw_window_offers(int setting, int choice, const char **why) {
    const char *reason = NULL;
    switch (setting) {
        case ALBW_SET_UPSCALE:
            if (choice == ALBW_UPSCALE_FSR1 && !fsr_ok)
                reason = "Needs SDL's GPU renderer (Vulkan). Software runs the same filter on the CPU.";
            if (gpu_ok && albw_settings()->renderer == ALBW_RENDERER_GPU && choice == ALBW_UPSCALE_SOFTWARE) {
                reason = "For the software renderer: the GPU renderer scales on the card.";
                break;
            }
            if (choice == ALBW_UPSCALE_FSR3) {
#ifdef ALBW_HAVE_FSR3
                if (!albw_fsr3_available())
                    reason = "Needs a Vulkan 1.3 graphics card; see the log for what was missing.";
#else
                reason = "Not in this build: it needs AMD's FidelityFX SDK 1.1.4 in external/.";
#endif
            }
            if (choice == ALBW_UPSCALE_DLSS) albw_dlss_available(&reason);
            break;
        case ALBW_SET_FRAME_RATE:
            if (choice != ALBW_RATE_CONSOLE
                    && (albw_settings()->upscale == ALBW_UPSCALE_FSR3 || albw_settings()->upscale == ALBW_UPSCALE_DLSS)
                    && albw_window_offers(ALBW_SET_UPSCALE, albw_settings()->upscale, NULL))
                reason = "Not with FSR 3 or DLSS yet: they upscale the game's own 60 frames.";
            break;
        case ALBW_SET_RENDERER:
            if (choice == ALBW_RENDERER_GPU && !gpu_ok)
                reason = "Needs SDL's GPU renderer on Vulkan, and shaders this build compiled.";
            break;
        case ALBW_SET_RESOLUTION:
        case ALBW_SET_AA:
            if (!gpu_ok || albw_settings()->renderer != ALBW_RENDERER_GPU)
                reason = "Applies to the GPU renderer; the processor draws at the console's own.";
            break;
        case ALBW_SET_FRAME_GEN:
            if (choice == ALBW_FRAMEGEN_FSR3)
                reason = "Coming: FSR 3's frame interpolation, on the same device as its upscaling.";
            if (choice == ALBW_FRAMEGEN_DLSS)
                reason = "Coming: needs an RTX 40 or newer, and the camera's matrices worked out first.";
            break;
        default:
            break;
    }
    if (why) *why = reason;
    return reason == NULL;
}

// The upscaler that is used: the one chosen, or -- where a settings file asks for one this build or
// machine does not have -- the best of the ones it does.
static int upscaler(void) {
    int u = albw_settings()->upscale;
    if (albw_window_offers(ALBW_SET_UPSCALE, u, NULL)) return u;
    return fsr_ok ? ALBW_UPSCALE_FSR1 : ALBW_UPSCALE_SOFTWARE;
}

static const char *upscaler_name(int u) {
    static const char *const names[ALBW_UPSCALERS] = {
        "no", "bilinear", "AMD FSR 1", "AMD FSR 3", "NVIDIA DLSS", "software FSR 1",
    };
    return u >= 0 && u < ALBW_UPSCALERS ? names[u] : "?";
}

#ifdef ALBW_HAVE_FSR
static SDL_GPUShader *make_shader(SDL_GPUDevice *dev, const unsigned char *code, size_t size) {
    SDL_GPUShaderCreateInfo ci;
    SDL_zero(ci);
    ci.code = code;
    ci.code_size = size;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    ci.num_samplers = 1;
    ci.num_uniform_buffers = 1;
    return SDL_CreateGPUShader(dev, &ci);
}

static void fsr_init(void) {
    SDL_GPUDevice *dev = SDL_GetGPURendererDevice(renderer);
    if (!dev) return;
    SDL_GPUShader *easu = make_shader(dev, fsr_easu_spv, sizeof fsr_easu_spv);
    SDL_GPUShader *rcas = make_shader(dev, fsr_rcas_spv, sizeof fsr_rcas_spv);
    if (!easu || !rcas) {
        printf("[window] FSR's shaders did not load: %s\n", SDL_GetError());
        return;
    }
    for (int i = 0; i < 2; i++) {
        SDL_GPURenderStateCreateInfo ci;
        SDL_zero(ci);
        ci.fragment_shader = easu;
        fsr_easu[i] = SDL_CreateGPURenderState(renderer, &ci);
        ci.fragment_shader = rcas;
        fsr_rcas[i] = SDL_CreateGPURenderState(renderer, &ci);
        if (!fsr_easu[i] || !fsr_rcas[i]) {
            printf("[window] FSR's render states: %s\n", SDL_GetError());
            return;
        }
    }
    fsr_ok = 1;
}
#endif

// One screen through both passes, onto `dst` in window pixels.
static void fsr_draw(int which, SDL_Texture *src, float src_w, float src_h, SDL_FRect dst) {
    if (dst.w < 1 || dst.h < 1) return;
    int w = (int)dst.w, h = (int)dst.h;
    float tw = 0, th = 0;
    if (fsr_target[which]) SDL_GetTextureSize(fsr_target[which], &tw, &th);
    if (!fsr_target[which] || (int)tw != w || (int)th != h) {
        if (fsr_target[which]) SDL_DestroyTexture(fsr_target[which]);
        fsr_target[which] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                              SDL_TEXTUREACCESS_TARGET, w, h);
        if (!fsr_target[which]) return;
        SDL_SetTextureBlendMode(fsr_target[which], SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(fsr_target[which], SDL_SCALEMODE_NEAREST);
    }
    float easu[4] = { src_w, src_h, (float)w, (float)h };
    float rcas[4] = { dst.x, dst.y, albw_fsr_stops[albw_settings()->sharpness], 0 };
    SDL_SetGPURenderStateFragmentUniforms(fsr_easu[which], 0, easu, sizeof easu);
    SDL_SetGPURenderStateFragmentUniforms(fsr_rcas[which], 0, rcas, sizeof rcas);

    SDL_SetRenderTarget(renderer, fsr_target[which]);
    SDL_SetGPURenderState(renderer, fsr_easu[which]);
    SDL_RenderTexture(renderer, src, NULL, NULL);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetGPURenderState(renderer, fsr_rcas[which]);
    SDL_RenderTexture(renderer, fsr_target[which], NULL, &dst);
    SDL_SetGPURenderState(renderer, NULL);
}

// Where the letterboxing puts the screens, worked out here in window pixels, because an upscaler's
// passes must land on whole pixels the size they were scaled to.
static void screens_in_pixels(SDL_FRect *top, SDL_FRect *bottom) {
    int W, H;
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_GetCurrentRenderOutputSize(renderer, &W, &H);
    float scale = fminf((float)W / logical_w, (float)H / logical_h);
    if (albw_settings()->integer_scale && scale >= 1.0f) scale = floorf(scale);
    float ox = floorf((W - logical_w * scale) / 2.0f), oy = floorf((H - logical_h * scale) / 2.0f);
    *top = (SDL_FRect){ ox + floorf(rect_top.x * scale), oy + floorf(rect_top.y * scale),
                        floorf(rect_top.w * scale), floorf(rect_top.h * scale) };
    *bottom = (SDL_FRect){ ox + floorf(rect_bottom.x * scale), oy + floorf(rect_bottom.y * scale),
                           floorf(rect_bottom.w * scale), floorf(rect_bottom.h * scale) };
}

static void fsr_screens(void) {
    SDL_FRect top, bottom;
    screens_in_pixels(&top, &bottom);
    if (tex_top) fsr_draw(0, tex_top, TOP_W, SCREEN_H, top);
    if (tex_bottom && rect_bottom.w > 0) fsr_draw(1, tex_bottom, BOTTOM_W, SCREEN_H, bottom);
}

static unsigned long shown_frame;
static uint8_t rgb_top[TOP_W * SCREEN_H * 3];
static uint8_t rgb_bottom[BOTTOM_W * SCREEN_H * 3];

static void cpu_draw(int which, SDL_Texture *src, const uint8_t *rgb, int src_w, SDL_FRect dst) {
    int w = (int)dst.w, h = (int)dst.h;
    if (w < 1 || h < 1) return;
    if (w <= src_w || h <= SCREEN_H) {
        // Not larger than the screen itself: nothing to scale up, so a plain bilinear draw.
        SDL_SetTextureScaleMode(src, SDL_SCALEMODE_LINEAR);
        SDL_RenderTexture(renderer, src, NULL, &dst);
        return;
    }
    if (w > src_w * CPU_MAX_SCALE || h > SCREEN_H * CPU_MAX_SCALE) {
        float k = fminf((float)(src_w * CPU_MAX_SCALE) / (float)w,
                        (float)(SCREEN_H * CPU_MAX_SCALE) / (float)h);
        w = (int)((float)w * k);
        h = (int)((float)h * k);
    }
    float tw = 0, th = 0;
    if (cpu_target[which]) SDL_GetTextureSize(cpu_target[which], &tw, &th);
    if (!cpu_target[which] || (int)tw != w || (int)th != h) {
        if (cpu_target[which]) SDL_DestroyTexture(cpu_target[which]);
        cpu_target[which] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!cpu_target[which]) return;
        cpu_valid[which] = 0;
    }
    // Pixel for pixel when it is the size shown, bilinear for the rest when it is not.
    SDL_SetTextureScaleMode(cpu_target[which], w == (int)dst.w && h == (int)dst.h
                                                   ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    int sharpness = albw_settings()->sharpness;
    size_t bytes = (size_t)src_w * SCREEN_H * 3u;
    if (!cpu_valid[which] || cpu_sharpness[which] != sharpness
            || memcmp(cpu_source[which], rgb, bytes)) {
        void *pixels;
        int pitch;
        if (!SDL_LockTexture(cpu_target[which], NULL, &pixels, &pitch)) return;
        albw_upscale_fsr1(rgb, src_w, SCREEN_H, pixels, w, h, pitch, albw_fsr_stops[sharpness]);
        SDL_UnlockTexture(cpu_target[which]);
        memcpy(cpu_source[which], rgb, bytes);
        cpu_sharpness[which] = sharpness;
        cpu_valid[which] = 1;
    }
    SDL_RenderTexture(renderer, cpu_target[which], NULL, &dst);
}

// AMD FSR 3: the top screen, drawn again for it -- jittered, with depth and motion -- once per frame
// of the game's, upscaled on the port's own Vulkan device and shown pixel for pixel; the bottom
// screen, which hardly moves, goes through FSR 1. The last picture is kept, for a paused game.
static void set_presentation(void);
static SDL_Texture *fsr3_target;
static struct albw_upscale_frame fsr3_in;
static uint32_t fsr3_index;
static uint64_t fsr3_last_ns;
static int fsr3_shown;

static int temporal(int up) { return up == ALBW_UPSCALE_FSR3 || up == ALBW_UPSCALE_DLSS; }

static void fsr3_frame(void) {
    int up = upscaler();
    SDL_FRect top, bottom;
    screens_in_pixels(&top, &bottom);
    set_presentation();
    int w = (int)top.w, h = (int)top.h;
    if (w <= TOP_W || h <= SCREEN_H) return;             // nothing to upscale to
    // DLSS goes to three times the input at most, its Ultra Performance; the window's bilinear
    // scaling does the rest.
    if (up == ALBW_UPSCALE_DLSS && (w > TOP_W * 3 || h > SCREEN_H * 3)) {
        float k = fminf((float)(TOP_W * 3) / (float)w, (float)(SCREEN_H * 3) / (float)h);
        w = (int)((float)w * k);
        h = (int)((float)h * k);
    }
    unsigned long later;
    if (!albw_raster_between_hold(&later)) return;
    albw_vkup_jitter(fsr3_index, TOP_W, w, &fsr3_in.jitter_x, &fsr3_in.jitter_y);
    int ok = albw_raster_between_upscale(&fsr3_in);
    albw_raster_between_release();
    if (!ok) return;
    float tw = 0, th = 0;
    if (fsr3_target) SDL_GetTextureSize(fsr3_target, &tw, &th);
    if (!fsr3_target || (int)tw != w || (int)th != h) {
        if (fsr3_target) SDL_DestroyTexture(fsr3_target);
        fsr3_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        w, h);
        if (!fsr3_target) return;
    }
    SDL_SetTextureScaleMode(fsr3_target, w == (int)top.w && h == (int)top.h ? SDL_SCALEMODE_NEAREST
                                                                           : SDL_SCALEMODE_LINEAR);
    uint64_t now = SDL_GetTicksNS();
    static const float sharp[ALBW_FSR_SHARPNESS_LEVELS] = { 1.0f, 0.8f, 0.6f, 0.4f, 0.2f };
    struct albw_vkup_frame in = {
        TOP_W, SCREEN_H, fsr3_in.rgb, NULL, fsr3_in.depth, fsr3_in.motion, fsr3_in.reactive,
        fsr3_in.jitter_x, fsr3_in.jitter_y,
        fsr3_in.reset || !fsr3_shown, sharp[albw_settings()->sharpness],
        fsr3_last_ns ? (float)(now - fsr3_last_ns) * 1e-6f : 16.7f, 1.0f, 1000.0f, 0.6f,
    };
    fsr3_last_ns = now;
    void *pixels;
    int pitch;
    if (!SDL_LockTexture(fsr3_target, NULL, &pixels, &pitch)) return;
    ok = up == ALBW_UPSCALE_DLSS ? albw_dlss_upscale(&in, pixels, w, h, pitch)
                                 : albw_fsr3_upscale(&in, pixels, w, h, pitch);
    SDL_UnlockTexture(fsr3_target);
    fsr3_index++;
    fsr3_shown = ok;
}

static int fsr3_screens(void) {
    if (!fsr3_shown || !fsr3_target) return 0;
    SDL_FRect top, bottom;
    screens_in_pixels(&top, &bottom);
    float tw = 0, th = 0;
    SDL_GetTextureSize(fsr3_target, &tw, &th);
    // The window changed size since: the next frame of the game's makes a new picture.
    float k = fminf(1.0f, fminf((float)(TOP_W * 3) / top.w, (float)(SCREEN_H * 3) / top.h));
    if (((int)tw != (int)top.w || (int)th != (int)top.h)
            && ((int)tw != (int)(top.w * k) || (int)th != (int)(top.h * k))) {
        return 0;
    }
    SDL_RenderTexture(renderer, fsr3_target, NULL, &top);
    if (tex_bottom && rect_bottom.w > 0) {
        if (fsr_ok) fsr_draw(1, tex_bottom, BOTTOM_W, SCREEN_H, bottom);
        else cpu_draw(1, tex_bottom, rgb_bottom, BOTTOM_W, bottom);
    }
    return 1;
}

static void cpu_screens(void) {
    SDL_FRect top, bottom;
    screens_in_pixels(&top, &bottom);
    if (tex_top) cpu_draw(0, tex_top, rgb_top, TOP_W, top);
    if (tex_bottom && rect_bottom.w > 0) cpu_draw(1, tex_bottom, rgb_bottom, BOTTOM_W, bottom);
}

// ---------------------------------------------------------------------------------------------
// The GPU renderer: whether it draws the picture, at what multiple of the console's resolution,
// and the frame `t` of the way between the last two the game recorded, drawn by it.
static int gpu_rendering(void) {
    return gpu_ok && albw_settings()->renderer == ALBW_RENDERER_GPU;
}

static int gpu_scale(void) {
    int r = albw_settings()->resolution;
    if (r > 0 && r < ALBW_RESOLUTIONS) return r;
    // As many pixels as the top screen takes up in the window, rounded up: never less sharp than
    // the window can show.
    SDL_FRect top, bottom;
    screens_in_pixels(&top, &bottom);
    set_presentation();
    int k = (int)ceilf(fmaxf(top.w / (float)TOP_W, top.h / (float)SCREEN_H));
    return k < 1 ? 1 : (k > 8 ? 8 : k);
}

// What drawing a frame on the card costs the window's thread, for ALBW_FPS=1.
static uint64_t gpu_ns, gpu_most_ns;
static unsigned gpu_draws;

static int gpu_view_wrap(SDL_GPUTexture *out[2], int scale);
static int gpu_up_ok;

// With the last two records held (albw_raster_between_hold), which this lets go.
static int gpu_draw_held(float t) {
#ifdef ALBW_HAVE_GPU
    uint64_t t0 = SDL_GetTicksNS();
    int ok = albw_raster_gpu_frame(&gpu_frame, t);
    albw_raster_between_release();
    if (!ok) return 0;
    SDL_GPUTexture *out[2];
    int scale = gpu_scale();
    static const int aa_samples[ALBW_AAS] = { 1, 2, 4, 8 };
    int aa = albw_settings()->antialias;
    albw_gpu_set_samples(aa >= 0 && aa < ALBW_AAS ? aa_samples[aa] : 1);
    if (!albw_gpu_draw(&gpu_frame, scale, out)) return 0;
    if (!gpu_view_wrap(out, scale)) return 0;
    uint64_t spent = SDL_GetTicksNS() - t0;
    gpu_ns += spent;
    if (spent > gpu_most_ns) gpu_most_ns = spent;
    gpu_draws++;
    gpu_up_ok = 0;
    return 1;
#else
    (void)t;
    albw_raster_between_release();
    return 0;
#endif
}

// The renderer's two screens as textures SDL's renderer can show.
static int gpu_view_wrap(SDL_GPUTexture *out[2], int scale) {
    for (int i = 0; i < 2; i++) {
        // A new texture -- the scale changed -- can land where the old one was; the scale says so.
        if (gpu_view[i] && gpu_view_of[i] == out[i] && gpu_view_scale[i] == scale) continue;
        if (gpu_view[i]) SDL_DestroyTexture(gpu_view[i]);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, out[i]);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, (i ? BOTTOM_W : TOP_W) * scale);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, SCREEN_H * scale);
        gpu_view[i] = SDL_CreateTextureWithProperties(renderer, props);
        SDL_DestroyProperties(props);
        gpu_view_of[i] = gpu_view[i] ? out[i] : NULL;
        gpu_view_scale[i] = scale;
        if (!gpu_view[i]) {
            printf("[window] cannot show the GPU renderer's picture: %s\n", SDL_GetError());
            return 0;
        }
        SDL_SetTextureBlendMode(gpu_view[i], SDL_BLENDMODE_NONE);
    }
    gpu_has_picture = 1;
    return 1;
}

static int gpu_draw(float t) {
    unsigned long later;
    if (!albw_raster_between_hold(&later)) return 0;
    return gpu_draw_held(t);
}

// FSR 3 or DLSS on the GPU renderer's picture: the top screen drawn below the size it is shown at
// -- the resolution setting's multiple, or two thirds of the window's -- jittered, with its depth,
// motion and reactive mask, and upscaled to the window's pixels exactly; the bottom screen drawn
// as always. Each new frame of the game's once.
static SDL_Texture *gpu_up_view;
static SDL_GPUTexture *gpu_up_of;
static int gpu_up_w, gpu_up_h;
static uint32_t *gpu_up_pixels;
static size_t gpu_up_room;
static uint32_t gpu_up_index;
static uint64_t gpu_up_last_ns;

static int gpu_temporal_steps(void);
static uint64_t gpu_up_spent[3];                         // waiting + showing, drawing, making

static int gpu_temporal_draw(void) {
    uint64_t t0 = SDL_GetTicksNS();
    int ok = gpu_temporal_steps();
    uint64_t spent = SDL_GetTicksNS() - t0;
    if (ok) {
        gpu_ns += spent;
        if (spent > gpu_most_ns) gpu_most_ns = spent;
        gpu_draws++;
    }
    return ok;
}

// The upscaler's own thread. AMD's and NVIDIA's upscalers run on a Vulkan device of the port's
// own (SDL shares none of its own device's handles), so a frame's inputs are read back from SDL's
// device and go up to that one, and the picture comes back the same way: some milliseconds each,
// on a card both devices share. On a thread of its own the window waits for none of it: each frame
// is upscaled while the next is drawn, and shown when that one is -- a frame later.
static pthread_t up_thread;
static int up_thread_on;
static pthread_mutex_t up_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t up_cond = PTHREAD_COND_INITIALIZER;
static struct {
    int state;                                           // 0 nothing, 1 to upscale, 2 upscaled
    struct albw_vkup_frame in;
    int dlss, out_w, out_h, ok, slot;
} up_job;

static void *up_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&up_mutex);
    for (;;) {
        while (up_job.state != 1) pthread_cond_wait(&up_cond, &up_mutex);
        struct albw_vkup_frame in = up_job.in;
        int dlss = up_job.dlss, w = up_job.out_w, h = up_job.out_h;
        pthread_mutex_unlock(&up_mutex);
        int ok = dlss ? albw_dlss_upscale(&in, gpu_up_pixels, w, h, w * 4)
                      : albw_fsr3_upscale(&in, gpu_up_pixels, w, h, w * 4);
        pthread_mutex_lock(&up_mutex);
        up_job.ok = ok;
        up_job.state = 2;
        pthread_cond_broadcast(&up_cond);
    }
    return NULL;
}

// Waits until no frame is being upscaled, and takes what was; returns whether there was one.
static int up_settle(int *ok, int *slot, int *w, int *h) {
    pthread_mutex_lock(&up_mutex);
    while (up_job.state == 1) pthread_cond_wait(&up_cond, &up_mutex);
    int had = up_job.state == 2;
    if (ok) *ok = up_job.ok;
    if (slot) *slot = up_job.slot;
    if (w) *w = up_job.out_w;
    if (h) *h = up_job.out_h;
    up_job.state = 0;
    pthread_mutex_unlock(&up_mutex);
    return had;
}

static int gpu_temporal_steps(void) {
#ifdef ALBW_HAVE_GPU
    int up = upscaler();
    SDL_FRect top, bottom;
    screens_in_pixels(&top, &bottom);
    set_presentation();
    int sw = (int)top.w, sh = (int)top.h;                // the top screen as shown
    if (sw < TOP_W || sh < SCREEN_H) return 0;
    int r = albw_settings()->resolution;
    int scale = r > 0 && r < ALBW_RESOLUTIONS ? r : (int)lroundf((float)sh / SCREEN_H / 1.5f);
    // DLSS goes to three times its input at most; FSR 3 is made for the same.
    int least = (int)ceilf((float)sh / SCREEN_H / 3.0f);
    if (scale < least) scale = least;
    // Never more than it is shown at: an upscaler does not scale down (at the same size it is
    // anti-aliasing alone).
    if (scale * SCREEN_H > sh) scale = sh / SCREEN_H;
    if (scale < 1) scale = 1;
    // The upscaler works in the target's orientation: the screen's height across, its width down.
    int in_w = SCREEN_H * scale, in_h = TOP_W * scale, out_w = sh, out_h = sw;

    // The frame before, upscaled meanwhile, shown now.
    int done_ok, done_slot, done_w, done_h;
    uint64_t m0 = SDL_GetTicksNS();
    if (up_settle(&done_ok, &done_slot, &done_w, &done_h)) {
        albw_gpu_temporal_done_slot(done_slot);
        gpu_up_ok = 0;
        SDL_GPUTexture *shown = NULL;
        if (done_ok && done_w == out_w && done_h == out_h) {
            shown = albw_gpu_show_upscaled(gpu_up_pixels, out_w, out_h, 1);
        }
        if (shown && (!gpu_up_view || gpu_up_of != shown || gpu_up_w != sw || gpu_up_h != sh)) {
            if (gpu_up_view) SDL_DestroyTexture(gpu_up_view);
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, shown);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, sw);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, sh);
            gpu_up_view = SDL_CreateTextureWithProperties(renderer, props);
            SDL_DestroyProperties(props);
            gpu_up_of = gpu_up_view ? shown : NULL;
            gpu_up_w = sw;
            gpu_up_h = sh;
            if (gpu_up_view) {
                SDL_SetTextureBlendMode(gpu_up_view, SDL_BLENDMODE_NONE);
                SDL_SetTextureScaleMode(gpu_up_view, SDL_SCALEMODE_NEAREST);
            }
        }
        gpu_up_ok = shown && gpu_up_view;
    }
    uint64_t m1 = SDL_GetTicksNS();

    // This frame, drawn for the upscaler and handed to its thread.
    unsigned long later;
    if (!albw_raster_between_hold(&later)) return gpu_up_ok;
    gpu_frame.want_prev = 1;
    int ok = albw_raster_gpu_frame(&gpu_frame, 1.0f);
    gpu_frame.want_prev = 0;
    albw_raster_between_release();
    if (!ok) return gpu_up_ok;
    struct albw_gpu_temporal t;
    memset(&t, 0, sizeof t);
    albw_vkup_jitter(gpu_up_index, in_h, out_h, &t.jitter[0], &t.jitter[1]);
    SDL_GPUTexture *out[2];
    uint64_t m2 = SDL_GetTicksNS();
    if (!albw_gpu_draw_temporal(&gpu_frame, scale, &t, out)) return gpu_up_ok;
    uint64_t m3 = SDL_GetTicksNS();
    if ((int)t.width != in_w || (int)t.height != in_h) {
        albw_gpu_temporal_done_slot(t.slot);
        return gpu_up_ok;
    }
    if (gpu_up_room < (size_t)out_w * out_h) {
        free(gpu_up_pixels);                             // nobody is upscaling into it now
        gpu_up_pixels = malloc((size_t)out_w * out_h * 4u);
        gpu_up_room = gpu_up_pixels ? (size_t)out_w * out_h : 0;
        if (!gpu_up_pixels) {
            albw_gpu_temporal_done_slot(t.slot);
            return 0;
        }
    }
    if (!up_thread_on) {
        up_thread_on = pthread_create(&up_thread, NULL, up_main, NULL) == 0;
        if (!up_thread_on) {
            albw_gpu_temporal_done_slot(t.slot);
            return 0;
        }
    }
    uint64_t now = SDL_GetTicksNS();
    static const float sharp[ALBW_FSR_SHARPNESS_LEVELS] = { 1.0f, 0.8f, 0.6f, 0.4f, 0.2f };
    pthread_mutex_lock(&up_mutex);
    up_job.in = (struct albw_vkup_frame){
        in_w, in_h, NULL, t.rgba, t.depth, t.motion, t.reactive, t.jitter[0], t.jitter[1],
        gpu_frame.cut || !gpu_up_index, sharp[albw_settings()->sharpness],
        gpu_up_last_ns ? (float)(now - gpu_up_last_ns) * 1e-6f : 16.7f, 1.0f, 1000.0f, 0.6f, 1,
    };
    up_job.dlss = up == ALBW_UPSCALE_DLSS;
    up_job.out_w = out_w;
    up_job.out_h = out_h;
    up_job.slot = t.slot;
    up_job.state = 1;
    pthread_cond_broadcast(&up_cond);
    pthread_mutex_unlock(&up_mutex);
    gpu_up_last_ns = now;
    gpu_up_index++;
    gpu_up_spent[0] += m1 - m0;
    gpu_up_spent[1] += m3 - m2;
    gpu_up_spent[2] += m2 - m1;
    // The bottom screen as the plain path has it.
    if (!gpu_view_wrap(out, scale)) return 0;
    gpu_has_picture = 1;
    return 1;
#else
    return 0;
#endif
}

// The GPU renderer's screens, onto the window: drawn larger than they are shown, they are scaled
// down smoothly; smaller, as the upscaling setting says.
static int gpu_screens(void) {
    if (!gpu_rendering() || !gpu_has_picture || !gpu_view[0] || !gpu_view[1]) return 0;
    SDL_FRect rect[2];
    screens_in_pixels(&rect[0], &rect[1]);
    int up = upscaler();
    for (int i = 0; i < 2; i++) {
        if (i == 1 && rect_bottom.w <= 0) continue;
        if (i == 0 && gpu_up_ok && gpu_up_view && temporal(up)
                && gpu_up_w == (int)rect[0].w && gpu_up_h == (int)rect[0].h) {
            SDL_RenderTexture(renderer, gpu_up_view, NULL, &rect[0]);   // upscaled to these pixels
            continue;
        }
        float tw, th;
        SDL_GetTextureSize(gpu_view[i], &tw, &th);
        int smaller = tw < rect[i].w - 0.5f || th < rect[i].h - 0.5f;
        if (smaller && up == ALBW_UPSCALE_FSR1 && fsr_ok) {
            fsr_draw(i, gpu_view[i], tw, th, rect[i]);
            continue;
        }
        SDL_SetTextureScaleMode(gpu_view[i], smaller && up == ALBW_UPSCALE_OFF ? SDL_SCALEMODE_NEAREST
                                                                             : SDL_SCALEMODE_LINEAR);
        SDL_RenderTexture(renderer, gpu_view[i], NULL, &rect[i]);
    }
    set_presentation();
    return 1;
}

static struct albw_input input;
static int paused, overlay_paused;      // P, and the launcher opened over the game
static uint64_t next_frame_ns;
static int pads_wanted = 1;
static void apply_frame_rate(void);

// ---------------------------------------------------------------------------------------------
// Two threads.
//
// Once Play is pressed the game runs on a thread of its own and the window keeps the main one: the
// window shows what the game last drew, reads the controls, and draws the frames in between while
// the game works on its next. The two meet at the end of each of the game's frames, in
// albw_window_frame, under win_mutex; the record the frames in between are drawn from has its own
// lock (albw_raster.c). Everything SDL does happens on the main thread.
static pthread_mutex_t win_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t win_cond = PTHREAD_COND_INITIALIZER;
static pthread_t main_thread;
static int game_threaded;                                // the game runs on its own thread

// The game's side, handed over at the end of each of its frames: the screens as the console shows
// them, when each frame's work started and how long it took, and the numbers for the title bar.
static uint8_t pub_top[TOP_W * SCREEN_H * 3], pub_bottom[BOTTOM_W * SCREEN_H * 3];
static unsigned long pub_frame, pub_seq;
#define TIMES 16
static struct { unsigned long frame; uint64_t start_ns, work_ns; } pub_times[TIMES];
static char pub_title[96];
static int pub_title_new, game_ended;

// The window's side, read by the game at the end of each frame: the controls, and whether to wait,
// to stop, or to skip the wait.
static struct albw_input pub_input, game_input;
static int pub_pause, pub_quit, pub_fast;

// A message from the game's thread, which only the main thread can show.
static const char *msg_title, *msg_text;
static int msg_error, msg_state;                         // 1 waiting to be shown, 2 shown

// How the frames keep up, shown in the title bar: the rate, and how much of a frame's time the
// guest and the renderer took. A machine too slow for the game shows it here first.
static int env_on(const char *name);
static uint64_t pace_since_ns, pace_work_ns, pace_worst_ns, pace_wait_end_ns;

// The sound: the DSP's frames, 32,728 stereo samples a second, into a stream SDL resamples to the
// device. The game makes them as fast as it runs, so the stream is kept short -- what is queued
// past a fifth of a second is dropped, which only happens while Tab runs the game fast -- and its
// rate is nudged by up to half a per cent towards a twentieth of a second queued, so that the
// device's clock and the frame pacing cannot drift apart into gaps or a growing delay.
static SDL_AudioStream *audio;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static void audio_put(const int16_t *stereo, unsigned frames);
void albw_window_audio(const int16_t *stereo, unsigned frames) {
    pthread_mutex_lock(&audio_mutex);
    if (audio) audio_put(stereo, frames);
    pthread_mutex_unlock(&audio_mutex);
}

static void audio_put(const int16_t *stereo, unsigned frames) {
    const int bytes_per_second = ALBW_DSP_SAMPLE_RATE * 4;
    int queued = SDL_GetAudioStreamQueued(audio);
    if (queued > bytes_per_second / 5) return;
    float fill = (float)queued / (float)(bytes_per_second / 20);
    float ratio = 1.0f + (fill - 1.0f) * 0.005f;
    if (ratio < 0.995f) ratio = 0.995f;
    if (ratio > 1.005f) ratio = 1.005f;
    SDL_SetAudioStreamFrequencyRatio(audio, ratio);
    SDL_PutAudioStreamData(audio, stereo, (int)(frames * 4u));
}

static void open_audio_locked(void);
static void open_audio(void) {
    pthread_mutex_lock(&audio_mutex);
    open_audio_locked();
    pthread_mutex_unlock(&audio_mutex);
}

static void open_audio_locked(void) {
    if (!albw_settings()->sound || audio) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        printf("[window] no sound: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, ALBW_DSP_SAMPLE_RATE };
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!audio) {
        printf("[window] no sound: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(audio);
    printf("[window] sound on, %d Hz stereo (ALBW_MUTE=1 turns it off)\n", ALBW_DSP_SAMPLE_RATE);
}
static unsigned pace_frames;

static void pace_note(uint64_t work_start, uint64_t now) {
    uint64_t work = now - work_start;
    pace_work_ns += work;
    if (work > pace_worst_ns) pace_worst_ns = work;
    if (!pace_since_ns) pace_since_ns = now;
    if (++pace_frames < 60) return;
    double seconds = (double)(now - pace_since_ns) * 1e-9;
    double fps = seconds > 0 ? pace_frames / seconds : 0;
    double busy = (double)pace_work_ns * 1e-6 / pace_frames, worst = (double)pace_worst_ns * 1e-6;
    static const char name[] = "World Across - ";
    char title[96];
    snprintf(title, sizeof title, "%s%.1f fps, %.1f ms a frame (worst %.1f)", name, fps, busy, worst);
    pthread_mutex_lock(&win_mutex);
    snprintf(pub_title, sizeof pub_title, "%s", title);
    pub_title_new = 1;
    pthread_mutex_unlock(&win_mutex);
    if (env_on("ALBW_FPS")) printf("[window] %s\n", title + sizeof name - 1);
    pace_since_ns = now;
    pace_work_ns = pace_worst_ns = 0;
    pace_frames = 0;
}

// The console's frame rate: the LCDs refresh at 268,111,856 / 4,481,136 Hz, a little under 60.
#define FRAME_NS 16713000ull

static int env_on(const char *name) {
    const char *s = albw_env(name);
    return s && *s && strcmp(s, "0");
}

static void set_presentation(void) {
    // Letterboxed, so the screens keep their shape whatever the window does; whole-pixel scaling keeps
    // every pixel the same size, which some people find much easier to look at.
    SDL_SetRenderLogicalPresentation(renderer, logical_w, logical_h,
                                     albw_settings()->integer_scale
                                         ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                         : SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

static void place_screens(void) {
    switch (layout) {
        case LAYOUT_SIDE:
            logical_w = TOP_W + BOTTOM_W;
            logical_h = SCREEN_H;
            rect_top = (SDL_FRect){ 0, 0, TOP_W, SCREEN_H };
            rect_bottom = (SDL_FRect){ TOP_W, 0, BOTTOM_W, SCREEN_H };
            break;
        case LAYOUT_TOP:
            logical_w = TOP_W;
            logical_h = SCREEN_H;
            rect_top = (SDL_FRect){ 0, 0, TOP_W, SCREEN_H };
            rect_bottom = (SDL_FRect){ 0, 0, 0, 0 };
            break;
        default:
            logical_w = TOP_W;
            logical_h = SCREEN_H * 2;
            rect_top = (SDL_FRect){ 0, 0, TOP_W, SCREEN_H };
            rect_bottom = (SDL_FRect){ (TOP_W - BOTTOM_W) / 2.0f, SCREEN_H, BOTTOM_W, SCREEN_H };
            break;
    }
}

int albw_window_open(void) {
    if (window) return 1;
    // The tools run the port too, and they want no window: the discovery loop runs it hundreds of
    // times, and a reference run has to see exactly the input a headless run sees, which is none.
    if (env_on("ALBW_HEADLESS") || albw_env("ALBW_DISCOVER")) return 0;

    const struct albw_settings *set = albw_settings_load();
    layout = set->layout == ALBW_LAYOUT_SIDE ? LAYOUT_SIDE
           : set->layout == ALBW_LAYOUT_TOP ? LAYOUT_TOP : LAYOUT_STACKED;
    pads_wanted = set->gamepads;
    place_screens();

    SDL_SetAppMetadata("World Across", "0.1", "world-across");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        printf("[window] no window: %s -- running headless\n", SDL_GetError());
        return 0;
    }

    const char *s = albw_env("ALBW_SCALE");
    int scale = s ? atoi(s) : 2;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    window = SDL_CreateWindow("World Across", logical_w * scale, logical_h * scale,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        printf("[window] cannot create a window: %s -- running headless\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    // SDL's GPU renderer, which can run FSR's shaders; any other if it cannot be had.
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
    SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 0);
    renderer = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    if (!renderer) {
        printf("[window] no GPU renderer (%s); drawing without FSR\n", SDL_GetError());
        renderer = SDL_CreateRenderer(window, NULL);
    }
#ifdef ALBW_HAVE_FSR
    else fsr_init();
#endif
#ifdef ALBW_HAVE_GPU
    if (renderer && SDL_GetGPURendererDevice(renderer)) gpu_ok = albw_gpu_open(SDL_GetGPURendererDevice(renderer));
#endif
    if (!renderer) {
        printf("[window] cannot create a renderer: %s -- running headless\n", SDL_GetError());
        SDL_DestroyWindow(window);
        window = NULL;
        SDL_Quit();
        return 0;
    }
    apply_frame_rate();
    set_presentation();
    if (set->fullscreen) SDL_SetWindowFullscreen(window, true);

    tex_top = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                TOP_W, SCREEN_H);
    tex_bottom = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                   BOTTOM_W, SCREEN_H);
    SDL_ScaleMode mode = upscaler() == ALBW_UPSCALE_BILINEAR ? SDL_SCALEMODE_LINEAR
                                                             : SDL_SCALEMODE_NEAREST;
    if (tex_top) SDL_SetTextureScaleMode(tex_top, mode);
    if (tex_bottom) SDL_SetTextureScaleMode(tex_bottom, mode);

    open_audio();
    albw_launcher_init(window, renderer);
    printf("[window] %dx%d, layout %s, %s upscaling; WASD circle pad, arrows D-pad, K A, J B, "
           "I X, U Y, Q L, E R, Enter Start, Backspace Select, P pause, Tab fast, F11 fullscreen\n",
           logical_w * scale, logical_h * scale,
           layout == LAYOUT_SIDE ? "side by side" : (layout == LAYOUT_TOP ? "top only" : "stacked"),
           upscaler_name(upscaler()));
    next_frame_ns = SDL_GetTicksNS();
    return 1;
}

// The frame rate: frames in between need the game's frames recorded, and the display's rate is
// kept by waiting for its refresh; otherwise the pace is the console's, and nothing waits for the
// display.
static int frame_rate(void) {
    int r = albw_settings()->frame_rate;
    return albw_window_offers(ALBW_SET_FRAME_RATE, r, NULL) ? r : ALBW_RATE_CONSOLE;
}

static void apply_frame_rate(void) {
    int r = frame_rate();
    // The GPU renderer draws from the frames the game records, and while it shows the picture the
    // processor draws none of its own.
    albw_raster_set_recording(r != ALBW_RATE_CONSOLE || temporal(upscaler()) || gpu_rendering());
    albw_raster_set_gpu_display(gpu_rendering());
    SDL_SetRenderVSync(renderer, r == ALBW_RATE_DISPLAY ? 1 : 0);
}

// What the launcher's settings page changed, applied to the window as it is.
static void apply_settings(void) {
    const struct albw_settings *set = albw_settings();
    enum layout want = set->layout == ALBW_LAYOUT_SIDE ? LAYOUT_SIDE
                     : set->layout == ALBW_LAYOUT_TOP ? LAYOUT_TOP : LAYOUT_STACKED;
    if (want != layout) {
        layout = want;
        place_screens();
    }
    set_presentation();
    SDL_ScaleMode mode = upscaler() == ALBW_UPSCALE_BILINEAR ? SDL_SCALEMODE_LINEAR
                                                             : SDL_SCALEMODE_NEAREST;
    if (tex_top) SDL_SetTextureScaleMode(tex_top, mode);
    if (tex_bottom) SDL_SetTextureScaleMode(tex_bottom, mode);
    SDL_SetWindowFullscreen(window, set->fullscreen != 0);
    if (set->sound) open_audio();
    else {
        pthread_mutex_lock(&audio_mutex);
        if (audio) SDL_DestroyAudioStream(audio);
        audio = NULL;
        pthread_mutex_unlock(&audio_mutex);
    }
    pads_wanted = set->gamepads;
    apply_frame_rate();
}

int albw_window_launch(void) {
    if (!window || !albw_launcher_visible()) return 1;
    // The launcher alone, before the game exists: nothing runs and nothing sounds until Play.
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return 0;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                continue;
            }
            switch (albw_launcher_event(&e)) {
                case ALBW_LAUNCHER_QUIT: return 0;
                case ALBW_LAUNCHER_APPLY: apply_settings(); break;
                case ALBW_LAUNCHER_PLAY:
                    next_frame_ns = SDL_GetTicksNS();
                    return 1;                            // it fades out over the game's first frames
                default: break;
            }
        }
        // A game the launcher was making, made: Play goes ahead as if pressed now.
        if (albw_launcher_poll() == ALBW_LAUNCHER_PLAY) {
            next_frame_ns = SDL_GetTicksNS();
            return 1;
        }
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        albw_launcher_draw(NULL);
        static long shot_frame;
        const char *shot = albw_env("ALBW_SCREENSHOT");
        if (shot && ++shot_frame == 30 && strchr(shot, ':')) {
            SDL_Surface *img = SDL_RenderReadPixels(renderer, NULL);
            if (img) {
                SDL_SaveBMP(img, strchr(shot, ':') + 1);
                SDL_DestroySurface(img);
            }
            if (albw_env("ALBW_STOP_AT")) return 0;
        }
        set_presentation();
        SDL_RenderPresent(renderer);
        SDL_DelayNS(FRAME_NS);
    }
}

// A message box is the main thread's to show: from the game's thread it is handed over, and the
// game waits until the player has read it.
static void message(int error, const char *title, const char *text) {
    if (!game_threaded || pthread_equal(pthread_self(), main_thread)) {
        SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, title,
                                 text, window);
        return;
    }
    pthread_mutex_lock(&win_mutex);
    while (msg_state) pthread_cond_wait(&win_cond, &win_mutex);
    msg_title = title;
    msg_text = text;
    msg_error = error;
    msg_state = 1;
    while (msg_state == 1) pthread_cond_wait(&win_cond, &win_mutex);
    msg_state = 0;
    pthread_cond_broadcast(&win_cond);
    pthread_mutex_unlock(&win_mutex);
}

void albw_window_alert(const char *title, const char *text) { message(1, title, text); }

void albw_window_message(const char *title, const char *text) {
    if (!window) return;
    message(0, title, text);
}

void albw_window_close(void) {
    if (!window) return;
    if (audio) {
        SDL_DestroyAudioStream(audio);
        audio = NULL;
    }
    if (tex_top) SDL_DestroyTexture(tex_top);
    if (tex_bottom) SDL_DestroyTexture(tex_bottom);
    for (int i = 0; i < 2; i++) if (gpu_view[i]) SDL_DestroyTexture(gpu_view[i]);
    if (gpu_up_view) SDL_DestroyTexture(gpu_up_view);
    if (up_thread_on) up_settle(NULL, NULL, NULL, NULL);     // nothing reads the card's buffers now
#ifdef ALBW_HAVE_GPU
    albw_gpu_close();
#endif
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    window = NULL;
}

// The controls as the game last took them, at the end of its frame.
const struct albw_input *albw_window_input(void) { return game_threaded ? &game_input : &input; }

// ---------------------------------------------------------------------------------------------
// Input.

static int16_t axis_from_keys(int negative, int positive) {
    return (int16_t)((positive ? ALBW_CIRCLE_MAX : 0) - (negative ? ALBW_CIRCLE_MAX : 0));
}

static void read_input(void) {
    struct albw_input in = { 0 };
    const bool *k = SDL_GetKeyboardState(NULL);

    if (k[SDL_SCANCODE_K]) in.buttons |= ALBW_PAD_A;
    if (k[SDL_SCANCODE_J]) in.buttons |= ALBW_PAD_B;
    if (k[SDL_SCANCODE_I]) in.buttons |= ALBW_PAD_X;
    if (k[SDL_SCANCODE_U]) in.buttons |= ALBW_PAD_Y;
    if (k[SDL_SCANCODE_Q]) in.buttons |= ALBW_PAD_L;
    if (k[SDL_SCANCODE_E]) in.buttons |= ALBW_PAD_R;
    if (k[SDL_SCANCODE_RETURN]) in.buttons |= ALBW_PAD_START;
    if (k[SDL_SCANCODE_BACKSPACE]) in.buttons |= ALBW_PAD_SELECT;
    if (k[SDL_SCANCODE_UP]) in.buttons |= ALBW_PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) in.buttons |= ALBW_PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) in.buttons |= ALBW_PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) in.buttons |= ALBW_PAD_RIGHT;

    float cx = axis_from_keys(k[SDL_SCANCODE_A], k[SDL_SCANCODE_D]);
    float cy = axis_from_keys(k[SDL_SCANCODE_S], k[SDL_SCANCODE_W]);

    // Every gamepad at once, so that plugging one in is all it takes. The face buttons go by where
    // they sit, not by their labels: the console's A is on the right and its B at the bottom, and
    // that is what hands remember.
    // ALBW_GAMEPADS=0 leaves them all out, for a pad lying on the desk that the game would
    // otherwise hear.
    int count = 0;
    SDL_JoystickID *ids = pads_wanted ? SDL_GetGamepads(&count) : NULL;
    for (int i = 0; ids && i < count; i++) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) {
            pad = SDL_OpenGamepad(ids[i]);
            // Said once per pad, because a pad nobody is holding can still be the reason the game
            // moved on by itself.
            if (pad) printf("[window] gamepad: %s\n", SDL_GetGamepadName(pad));
        }
        if (!pad) continue;
        static const struct { SDL_GamepadButton b; uint32_t pad; } map[] = {
            { SDL_GAMEPAD_BUTTON_EAST, ALBW_PAD_A }, { SDL_GAMEPAD_BUTTON_SOUTH, ALBW_PAD_B },
            { SDL_GAMEPAD_BUTTON_NORTH, ALBW_PAD_X }, { SDL_GAMEPAD_BUTTON_WEST, ALBW_PAD_Y },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ALBW_PAD_L },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ALBW_PAD_R },
            { SDL_GAMEPAD_BUTTON_START, ALBW_PAD_START }, { SDL_GAMEPAD_BUTTON_BACK, ALBW_PAD_SELECT },
            { SDL_GAMEPAD_BUTTON_DPAD_UP, ALBW_PAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, ALBW_PAD_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, ALBW_PAD_LEFT },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ALBW_PAD_RIGHT },
        };
        for (size_t m = 0; m < sizeof map / sizeof map[0]; m++) {
            if (SDL_GetGamepadButton(pad, map[m].b)) in.buttons |= map[m].pad;
        }
        // The triggers as L and R as well: on many pads they are where the fingers rest.
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) in.buttons |= ALBW_PAD_L;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) in.buttons |= ALBW_PAD_R;

        float sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float sy = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        if (sqrtf(sx * sx + sy * sy) > 0.25f) {          // a dead zone, so a resting stick is still
            cx = sx * ALBW_CIRCLE_MAX;
            cy = sy * ALBW_CIRCLE_MAX;
        }
    }
    SDL_free(ids);

    // The circle pad is round: pushed into a corner it reaches no further than straight ahead.
    float len = sqrtf(cx * cx + cy * cy);
    if (len > ALBW_CIRCLE_MAX) {
        cx = cx * ALBW_CIRCLE_MAX / len;
        cy = cy * ALBW_CIRCLE_MAX / len;
    }
    in.circle_x = (int16_t)lroundf(cx);
    in.circle_y = (int16_t)lroundf(cy);
    // Past a quarter of the way the pad also counts as a direction button, as the console reports it.
    if (in.circle_x > 40) in.buttons |= ALBW_PAD_CIRCLE_RIGHT;
    if (in.circle_x < -40) in.buttons |= ALBW_PAD_CIRCLE_LEFT;
    if (in.circle_y > 40) in.buttons |= ALBW_PAD_CIRCLE_UP;
    if (in.circle_y < -40) in.buttons |= ALBW_PAD_CIRCLE_DOWN;

    // The mouse on the bottom screen is a finger on the touch screen.
    float mx, my, lx, ly;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    if ((buttons & SDL_BUTTON_LMASK) && rect_bottom.w > 0
            && SDL_RenderCoordinatesFromWindow(renderer, mx, my, &lx, &ly)
            && lx >= rect_bottom.x && lx < rect_bottom.x + rect_bottom.w
            && ly >= rect_bottom.y && ly < rect_bottom.y + rect_bottom.h) {
        in.touching = 1;
        in.touch_x = (uint16_t)(lx - rect_bottom.x);
        in.touch_y = (uint16_t)(ly - rect_bottom.y);
    }
    // The launcher has the controls while it is up; the game hears nothing.
    if (albw_launcher_visible()) memset(&in, 0, sizeof in);
    // The first changes of the buttons, so a run that moved on without anybody touching anything
    // says which control did it.
    static unsigned changes;
    if (in.buttons != input.buttons && changes < 8) {
        changes++;
        printf("[window] buttons %08x, circle pad %d %d\n", in.buttons, in.circle_x, in.circle_y);
    }
    input = in;
}

// ---------------------------------------------------------------------------------------------
// One frame.

// What the window shows a second, for the title bar and ALBW_FPS=1: all frames, and how many of
// them in between.
static unsigned shows, shows_between;
static uint64_t shows_since;
static float shown_rate;

static void count_show(int between) {
    uint64_t now = SDL_GetTicksNS();
    if (!shows_since) shows_since = now;
    shows++;
    shows_between += (unsigned)between;
    if (now - shows_since < 1000000000ull) return;
    shown_rate = (float)(shows * 1e9 / (double)(now - shows_since));
    if (env_on("ALBW_FPS")) {
        printf("[window] shown %.1f frames a second, %u of them in between", shows * 1e9 / (double)(now - shows_since),
               shows_between);
        if (gpu_draws) {
            printf("; the card's frames cost this thread %.2f ms each (most %.2f)",
                   (double)gpu_ns / gpu_draws / 1e6, (double)gpu_most_ns / 1e6);
            if (gpu_up_spent[1]) {
                printf(" -- waiting for the upscaler and showing it %.2f, making the frame %.2f, drawing "
                       "and reading back %.2f", (double)gpu_up_spent[0] / gpu_draws / 1e6,
                       (double)gpu_up_spent[2] / gpu_draws / 1e6, (double)gpu_up_spent[1] / gpu_draws / 1e6);
            }
            memset(gpu_up_spent, 0, sizeof gpu_up_spent);
        }
        printf("\n");
        gpu_ns = gpu_most_ns = 0;
        gpu_draws = 0;
    }
    shows = shows_between = 0;
    shows_since = now;
}

static void show(void) {
    if (tex_top) SDL_UpdateTexture(tex_top, NULL, rgb_top, TOP_W * 3);
    if (tex_bottom) SDL_UpdateTexture(tex_bottom, NULL, rgb_bottom, BOTTOM_W * 3);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    int up = upscaler();
    if (gpu_screens()) {
        // drawn by the graphics card
    } else if (temporal(up) && fsr3_screens()) {
        set_presentation();
    } else if ((up == ALBW_UPSCALE_FSR1 || temporal(up)) && fsr_ok) {
        fsr_screens();
        set_presentation();
    } else if (up == ALBW_UPSCALE_SOFTWARE) {
        cpu_screens();
        set_presentation();
    } else {
        if (tex_top) SDL_RenderTexture(renderer, tex_top, NULL, &rect_top);
        if (tex_bottom && rect_bottom.w > 0) SDL_RenderTexture(renderer, tex_bottom, NULL, &rect_bottom);
    }
    if (albw_launcher_visible()) {
        // Over the whole window, in its own pixels, not the screens' letterboxed ones.
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        albw_launcher_draw(gpu_rendering() && gpu_has_picture && gpu_view[0] ? gpu_view[0] : tex_top);
        set_presentation();
    }
    // ALBW_SCREENSHOT=<frame>:<file.bmp> keeps the whole window as it looked at that frame.
    static long shot_at = -2;
    static const char *shot_file;
    if (shot_at == -2) {
        const char *e = albw_env("ALBW_SCREENSHOT");
        shot_at = e ? strtol(e, NULL, 10) : -1;
        shot_file = e && strchr(e, ':') ? strchr(e, ':') + 1 : NULL;
    }
    // The first frame shown from that one on: a window that fell behind for a frame skips one.
    if (shot_file && shot_at >= 0 && (long)shown_frame >= shot_at) {
        shot_at = -1;
        SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
        if (shot) {
            SDL_SaveBMP(shot, shot_file);
            SDL_DestroySurface(shot);
        }
    }
    SDL_RenderPresent(renderer);
}

// Events that are not input state: closing, pausing, fullscreen. Returns 0 to quit.
static int handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (albw_launcher_visible() && e.type != SDL_EVENT_QUIT
                && e.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            switch (albw_launcher_event(&e)) {
                case ALBW_LAUNCHER_QUIT: return 0;
                case ALBW_LAUNCHER_APPLY: apply_settings(); break;
                case ALBW_LAUNCHER_PLAY:
                    if (overlay_paused) paused = 0;
                    overlay_paused = 0;
                    memset(&input, 0, sizeof input);
                    break;
                default: break;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode != SDL_SCANCODE_F11) continue;
        }
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                return 0;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                // A key released while the window was not looking would otherwise stay held.
                memset(&input, 0, sizeof input);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if ((e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_F1)
                        && !albw_launcher_visible()) {
                    // The menu, or its settings, over the game; the game waits underneath.
                    albw_launcher_show(e.key.scancode == SDL_SCANCODE_F1);
                    if (albw_launcher_visible()) {
                        overlay_paused = !paused;
                        paused = 1;
                    }
                } else if (e.key.scancode == SDL_SCANCODE_P) {
                    paused = !paused;
                    SDL_SetWindowTitle(window, paused ? "World Across (paused, P to go on)"
                                                      : "World Across");
                } else if (e.key.scancode == SDL_SCANCODE_F11) {
                    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
                    SDL_SetWindowFullscreen(window, !(flags & SDL_WINDOW_FULLSCREEN));
                }
                break;
            default:
                break;
        }
    }
    return 1;
}

int albw_window_frame(uint32_t shared_addr, uint32_t thread_index, unsigned long frame) {
    if (!window) return 1;
    // Everything since the last frame's wait ended was the guest's and the renderer's work.
    uint64_t now = SDL_GetTicksNS();
    uint64_t work_start = pace_wait_end_ns ? pace_wait_end_ns : now;

    static uint8_t cap_top[TOP_W * SCREEN_H * 3], cap_bottom[BOTTOM_W * SCREEN_H * 3];
    uint32_t w;
    if (albw_screen_capture(shared_addr, thread_index, 0, cap_top, &w) && w != TOP_W) {
        memset(cap_top, 0, sizeof cap_top);
    }
    if (albw_screen_capture(shared_addr, thread_index, 1, cap_bottom, &w) && w != BOTTOM_W) {
        memset(cap_bottom, 0, sizeof cap_bottom);
    }
    pace_note(work_start, now);

    pthread_mutex_lock(&win_mutex);
    memcpy(pub_top, cap_top, sizeof pub_top);
    memcpy(pub_bottom, cap_bottom, sizeof pub_bottom);
    pub_frame = frame;
    pub_seq++;
    pub_times[pub_seq % TIMES].frame = frame;
    pub_times[pub_seq % TIMES].start_ns = work_start;
    pub_times[pub_seq % TIMES].work_ns = now - work_start;
    // While paused -- P, or the menu over the game -- the guest simply does not get its next frame:
    // everything it does waits here, so nothing in it can tell, and nothing in it can come loose.
    while (pub_pause && !pub_quit) pthread_cond_wait(&win_cond, &win_mutex);
    int quit = pub_quit, fast = pub_fast;
    game_input = pub_input;
    pthread_mutex_unlock(&win_mutex);
    if (quit) return 0;

    // The pace. A frame that comes early waits for its time; one that comes late does not try to
    // catch up, because catching up is a burst of frames nobody can see. Tab held runs unthrottled,
    // for the stretches -- a long text, a long walk -- where waiting helps nobody.
    now = SDL_GetTicksNS();
    if (fast || env_on("ALBW_UNTHROTTLED")) {
        next_frame_ns = now;
        pace_wait_end_ns = now;
        return 1;
    }
    next_frame_ns += FRAME_NS;
    if (next_frame_ns > now) {
        SDL_DelayNS(next_frame_ns - now);
    } else if (now - next_frame_ns > FRAME_NS) {
        next_frame_ns = now;
    }
    pace_wait_end_ns = SDL_GetTicksNS();
    return 1;
}

// ---------------------------------------------------------------------------------------------
// The main thread while the game runs.

struct game_job {
    const char *(*run)(void);
    const char *why;
};

static void *game_main(void *arg) {
    struct game_job *job = arg;
    albw_raster_game_thread();
    job->why = job->run();
    pthread_mutex_lock(&win_mutex);
    game_ended = 1;
    pthread_mutex_unlock(&win_mutex);
    return NULL;
}

// Where between the last two recorded frames the window is now, from 0 (the earlier) to 1 (the
// later). A frame is recorded as its work ends, which is some milliseconds after it started; the
// one after it is due a frame's time later. So the earlier frame is shown when a frame's work is
// normally done -- the longest of the last few, and a millisecond -- and the later a frame's time
// after that, by which time the one after it is normally there to go on to.
static float between_t(unsigned long later, uint64_t now) {
    uint64_t start = 0, work = 0;
    int found = 0;
    pthread_mutex_lock(&win_mutex);
    for (int i = 0; i < TIMES; i++) {
        if (pub_times[i].work_ns > work) work = pub_times[i].work_ns;
        if (pub_times[i].frame == later && pub_times[i].start_ns) {
            start = pub_times[i].start_ns;
            found = 1;
        }
    }
    pthread_mutex_unlock(&win_mutex);
    if (!found) return 1.0f;
    work += 1000000u;
    if (work > FRAME_NS) work = FRAME_NS;
    if (now < start + work) return 0.0f;
    float t = (float)(now - start - work) / (float)FRAME_NS;
    return t > 1.0f ? 1.0f : t;
}

// How many frames in between the game leaves room for. A frame in between costs about what the
// game's own drawing does, and it is drawn on the same processor: drawn without limit it takes
// the game below its 60 frames a second, which is worse than no frames in between at all. So the
// game's own work is watched, frame by frame, and the frames in between given what it leaves:
// fewer whenever a frame of the game's took more than 85 per cent of its time, a few more again
// while it takes under 70.
static float between_allowed = 1.0f;

static void between_adapt(uint64_t work_ns, float most) {
    if (work_ns > FRAME_NS * 85u / 100u) between_allowed *= 0.6f;
    else if (work_ns < FRAME_NS * 70u / 100u) between_allowed += 0.15f;
    if (between_allowed > most) between_allowed = most;
    if (between_allowed < 0.0f) between_allowed = 0.0f;
}

// The display's refresh rate, for the most frames in between that it can show.
static float display_hz(void) {
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode && mode->refresh_rate > 1.0f ? mode->refresh_rate : 60.0f;
}

const char *albw_window_play(const char *(*run)(void)) {
    struct game_job job = { run, NULL };
    main_thread = pthread_self();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)64 << 20);  // the generated code's frames are large
    pthread_t game;
    game_threaded = 1;
    if (pthread_create(&game, &attr, game_main, &job) != 0) {
        printf("[window] cannot start the game's thread; running it on this one\n");
        game_threaded = 0;
        pthread_attr_destroy(&attr);
        return run();
    }
    pthread_attr_destroy(&attr);

    unsigned long shown_seq = 0;
    uint64_t quit_at = 0, last_present = 0, last_between = 0;
    for (;;) {
        uint64_t now = SDL_GetTicksNS();
        // A message the game's thread is waiting to have shown.
        pthread_mutex_lock(&win_mutex);
        if (msg_state == 1) {
            const char *title = msg_title, *text = msg_text;
            int error = msg_error;
            pthread_mutex_unlock(&win_mutex);
            SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION,
                                     title, text, window);
            pthread_mutex_lock(&win_mutex);
            msg_state = 2;
            pthread_cond_broadcast(&win_cond);
        }
        int ended = game_ended;
        pthread_mutex_unlock(&win_mutex);
        if (ended) break;

        if (quit_at) {
            // Closing: the game stops at the end of its frame. One that never gets there -- stuck
            // in a loop of its own -- is not waited for long.
            if (now - quit_at > 3000000000ull) {
                printf("[window] the game did not stop within three seconds; leaving anyway\n");
                exit(0);
            }
            SDL_PumpEvents();
            SDL_DelayNS(1000000);
            continue;
        }
        if (!handle_events()) {
            quit_at = now;
            pthread_mutex_lock(&win_mutex);
            pub_quit = 1;
            pthread_cond_broadcast(&win_cond);
            pthread_mutex_unlock(&win_mutex);
            continue;
        }
        read_input();
        const bool *k = SDL_GetKeyboardState(NULL);
        int fast = k[SDL_SCANCODE_TAB] && !albw_launcher_visible();

        pthread_mutex_lock(&win_mutex);
        pub_input = input;
        pub_fast = fast;
        if (pub_pause != paused) {
            pub_pause = paused;
            pthread_cond_broadcast(&win_cond);
        }
        unsigned long seq = pub_seq;
        int fresh = seq != shown_seq;
        if (fresh) {
            memcpy(rgb_top, pub_top, sizeof rgb_top);
            memcpy(rgb_bottom, pub_bottom, sizeof rgb_bottom);
            shown_frame = pub_frame;
            shown_seq = seq;
        }
        uint64_t work = fresh ? pub_times[seq % TIMES].work_ns : 0;
        char title[128] = "";
        if (pub_title_new && !paused) {
            // With frames in between, what the window shows as well as what the game draws.
            if (frame_rate() != ALBW_RATE_CONSOLE && shown_rate > 0.0f) {
                snprintf(title, sizeof title, "%s, %.0f shown", pub_title, shown_rate);
            } else {
                snprintf(title, sizeof title, "%s", pub_title);
            }
            pub_title_new = 0;
        }
        pthread_mutex_unlock(&win_mutex);
        if (title[0]) SDL_SetWindowTitle(window, title);

        // FSR 3: each new frame of the game's, drawn again for it and upscaled.
        if (temporal(upscaler()) && fresh && !paused && !fast && !gpu_rendering()) fsr3_frame();
        // The GPU renderer draws each new frame of the game's as it comes -- for FSR 3 or DLSS
        // at the size they take, upscaled.
        if (gpu_rendering() && fresh) {
            if (!(temporal(upscaler()) && !paused && !fast && gpu_temporal_draw())) gpu_draw(1.0f);
        }

        // Frames in between, while the game is running and at its own pace, as many as it leaves
        // room for: no more often than its frame time divided among them.
        int rate = frame_rate();
        if (rate != ALBW_RATE_CONSOLE && !paused && !fast && !albw_launcher_visible()) {
            float most = rate == ALBW_RATE_DISPLAY ? display_hz() / 60.0f - 1.0f : 8.0f;
            // On the graphics card a frame in between costs the game nothing.
            if (gpu_rendering()) between_allowed = most;
            else if (fresh && work) between_adapt(work, most);
            uint64_t every = (uint64_t)((float)FRAME_NS / (between_allowed + 1.0f));
            unsigned long later;
            if (now - last_between < every) {
                if (fresh) {
                    show();
                    count_show(0);
                    last_present = SDL_GetTicksNS();
                } else {
                    SDL_DelayNS(500000);
                }
                continue;
            }
            if (albw_raster_between_hold(&later)) {
                // A frame is drawn only when it would differ from the last one shown: while the
                // window waits for the game's next frame it is the same picture, and drawing it
                // again only takes the workers from the game.
                static unsigned long drawn_later;
                static float drawn_t = -1.0f;
                float t = between_t(later, now);
                if (later == drawn_later && t == drawn_t) {
                    albw_raster_between_release();
                    SDL_DelayNS(500000);
                    continue;
                }
                last_between = now;
                if (gpu_rendering()) {
                    gpu_draw_held(t);
                } else {
                    albw_raster_between_draw(t, rgb_top, rgb_bottom);
                    albw_raster_between_release();
                }
                drawn_later = later;
                drawn_t = t;
                show();
                count_show(t > 0.0f && t < 1.0f);
                last_present = SDL_GetTicksNS();
                continue;
            }
        }
        // Otherwise a new frame is shown when it comes, and the menu, which moves, at the
        // console's rate; in between there is nothing to do but wait a little.
        if (fresh || (now - last_present >= FRAME_NS
                      && (albw_launcher_visible() || paused))) {
            show();
            count_show(0);
            last_present = SDL_GetTicksNS();
        } else {
            SDL_DelayNS(500000);
        }
    }
    pthread_join(game, NULL);
    game_threaded = 0;
    return job.why;
}

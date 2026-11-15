#ifndef ALBW_GPU_H
#define ALBW_GPU_H

#include <stdint.h>

// The GPU renderer: a recorded frame (albw_raster.c) drawn again on the graphics card, at a whole
// multiple of the console's resolution, for the window to show.
//
// The game's own drawing stays where it was -- its vertices are shaded on the processor, and every
// frame is recorded -- but with this renderer showing the picture, the processor does not set up or
// rasterise a single triangle: nothing the game runs ever reads what the GPU drew (VRAM is never
// touched by its code; ALBW_WATCH_VRAM=1 checks that), so the console's own picture is not needed.
// The display transfers say where each screen's picture is; texture copies of a target are copied
// on the card too.
//
// The frame handed over is plain data -- no SDL here -- so that albw_raster.c, which knows the
// draws, can write it without knowing the card, and albw_gpu.c, which knows the card, can draw it
// without knowing the draws.

// One corner of a triangle as the shader left it, sorted by what each output means. Triangles are
// listed corner by corner, no indices: a corner's normal quaternion is turned into its first
// corner's hemisphere, which only the triangle can say.
struct albw_gpu_vertex {
    float pos[4];
    float color[4];
    float tc01[4];                  // the first two coordinate pairs
    float tc2w[4];                  // the third pair, the first set's w, unused
    float quat[4];
    float view[4];                  // the view vector; w unused
    float prev[4];                  // for a temporal upscaler: where it was in the frame before,
                                    // in clip space; the position itself otherwise
};

// A texture unit as a draw samples it.
struct albw_gpu_texture {
    uint32_t base, width, height, format;
    uint8_t enabled, levels;        // levels: the full-size one and its mipmaps
    uint8_t min_level, mag_linear, min_linear, mip_linear, wrap_s, wrap_t;
};

struct albw_gpu_draw {
    uint32_t state;                 // which of the frame's states (pica_state.h)
    uint32_t first_vertex, vertices;
    // The pipeline.
    uint8_t blend, color_mask, depth_test, depth_func, depth_write, cull, keep_color;
    uint8_t stencil, stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
    uint8_t stencil_ref, stencil_in_mask, stencil_write_mask;
    uint32_t blend_func, blend_color;
    // Where it draws.
    uint32_t color_base, depth_base, width, height, color_bytes, depth_bytes;
    float half_w, half_h, offset_x, offset_y;
    int32_t scissor[4];             // x0, y0, x1, y1, inclusive
    struct albw_gpu_texture tex[3];
};

enum { ALBW_GPU_DRAW, ALBW_GPU_FILL, ALBW_GPU_TRANSFER, ALBW_GPU_TEXCOPY };
struct albw_gpu_event {
    uint32_t kind, a, b, c, d, e;   // as the record's: a draw's index; a fill's start, end, value,
};                                  // step; a transfer's source, destination, sizes, flags; a
                                    // texture copy's source, destination, size, line sizes

struct albw_gpu_frame {
    int want_prev;                  // asked for: each vertex's place in the frame before
    float prev_t;                   // where that frame was, of the way from the earlier record to
                                    // the later (0: the earlier itself; below 0, before it)
    unsigned long frame;            // the game's frame it shows
    struct albw_gpu_vertex *vert;
    uint32_t vert_count, vert_room;
    struct albw_gpu_draw *draw;
    uint32_t draw_count, draw_room;
    uint32_t *state;                // ST_WORDS a state
    uint32_t state_count, state_room;
    uint32_t *lut;                  // 256 entries a table
    uint32_t lut_count, lut_room;
    struct albw_gpu_event *ev;
    uint32_t ev_count, ev_room;
    int32_t screen_ev[2];           // the transfer that makes each screen's picture, or -1
    int cut;                        // most of what both frames drew jumped: nothing to carry over
};

// albw_raster.c's side: the frame `t` of the way from the earlier held record to the later one
// (albw_raster_between_hold), as above. Returns 0 if there is nothing to draw.
int albw_raster_gpu_frame(struct albw_gpu_frame *f, float t);

// Decodes a texture as it is in guest memory now into RGBA, eight bits a channel, red first, rows
// in the order a sampler counts them (the first is t = 0).
void albw_raster_decode_texture(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                                uint8_t *rgba);
// The bytes a texture of that size takes, for one level.
uint32_t albw_raster_texture_bytes(uint32_t width, uint32_t height, uint32_t format);

// The game's side: with the GPU renderer showing the picture, the processor's triangles are not
// set up or rasterised at all, and every frame is recorded.
void albw_raster_set_gpu_display(int on);
int albw_raster_gpu_display(void);

// The card's side (albw_gpu.c, built when the shaders can be compiled: ALBW_HAVE_GPU). The window
// hands over SDL's device; a frame is drawn into two textures it then shows, the top screen's and the
// bottom's, `scale` times the console's size.
struct SDL_GPUDevice;
struct SDL_GPUTexture;
int albw_gpu_open(struct SDL_GPUDevice *device);
int albw_gpu_draw(const struct albw_gpu_frame *f, int scale, struct SDL_GPUTexture *out[2]);
// Multisampled anti-aliasing: 1 for none, else 2, 4 or 8 samples a pixel, or the most the card can.
void albw_gpu_set_samples(int n);

// For a temporal upscaler (FSR 3, DLSS): the frame drawn with the top screen's draws moved by
// `jitter` (input pixels: the target's, x across its rows, y down them), and what the upscaler
// takes read back -- the top screen as the target held it at its transfer: colour, depth (0 near,
// 1 far), motion back to the frame before (input pixels, the jitter left out) and how much of each
// pixel is blended over what was there -- all in the target's orientation, `width` (240 times the
// scale) across and `height` (400 times) down. The frame must have been made with want_prev.
// The buffers are the renderer's read-back buffer itself, good until albw_gpu_temporal_done().
// Returns 0 if it could not.
struct albw_gpu_temporal {
    float jitter[2];
    int slot;                       // which read-back buffer holds them
    uint32_t width, height;
    const uint8_t *rgba;
    const float *depth, *motion;
    const uint8_t *reactive;
};
int albw_gpu_draw_temporal(const struct albw_gpu_frame *f, int scale, struct albw_gpu_temporal *t,
                           struct SDL_GPUTexture *out[2]);
// Lets the read-back buffer of `slot` go (the next frame drawn for an upscaler takes the other one,
// so one frame's can be read on another thread while the next is drawn); _done lets both go.
void albw_gpu_temporal_done_slot(int slot);
void albw_gpu_temporal_done(void);
// An upscaled top screen, as the upscaler gave it back -- the target's orientation, `width` across
// and `height` down, as 0xFFRRGGBB words or, with `rgba`, bytes red first -- turned the way the
// screen shows it, into a texture `height` wide and `width` high; with the overlay that frame's
// read-back `slot` drew laid over it (-1 for none).
struct SDL_GPUTexture *albw_gpu_show_upscaled(const void *pixels, int width, int height, int rgba,
                                              int slot);
void albw_gpu_close(void);
// ALBW_GPU_TEST and ALBW_GPU_FSR3_TEST (albw_gpu.c), from the game's thread at a frame's end.
void albw_gpu_test(unsigned long frame);
void albw_gpu_fsr3_test(unsigned long frame);

#endif

#ifndef ALBW_VKUP_H
#define ALBW_VKUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The temporal upscalers -- AMD FSR 3 (FidelityFX SDK 1.1.4, MIT) and NVIDIA DLSS (the DLSS SDK,
// under NVIDIA's licence) -- on a Vulkan device of the port's own.
//
// SDL's GPU renderer keeps its Vulkan device to itself, and the picture is drawn on the processor
// anyway, so the upscalers get a device of their own: each frame the top screen at its own size --
// colour, depth, motion and a reactive mask, a few hundred kilobytes -- goes up, the upscaler
// runs, and the upscaled picture comes back to be shown like any other. Each is built only when its
// SDK is beside the source (external/); DLSS runs only on NVIDIA's RTX cards.

// Whether this build has FSR 3 and the machine a device it runs on. Opens the device the first
// time; says why not, once, in the log.
int albw_fsr3_available(void);

// The same for DLSS; `why`, if not NULL, says what it would take.
int albw_dlss_available(const char **why);

// The sub-pixel offset to draw frame `index` of the sequence with, in pixels of the input picture
// (x right, y down), for an input `render_w` wide shown `display_w` wide: Halton(2, 3).
void albw_vkup_jitter(uint32_t index, int render_w, int display_w, float *x, float *y);

struct albw_vkup_frame {
    int render_w, render_h;                 // the input
    const uint8_t *rgb;                     // RGB, three bytes a pixel, top row first
    const uint8_t *rgba;                    // or RGBA, four, in its place where not NULL
    const float *depth;                     // 0 at the near plane, 1 at the far one
    const float *motion;                    // two floats a pixel: where it was in the frame
                                            // before, minus where it is, in pixels
    const uint8_t *reactive;                // a byte a pixel: how far to trust this frame over
                                            // the history -- for what is blended over the scene
    float jitter_x, jitter_y;               // what the frame was drawn with
    int reset;                              // the frame before has nothing to do with this one
    float sharpness;                        // 0 to 1; below 0 no sharpening (FSR 3 only)
    float frame_ms;                         // since the frame before
    float camera_near, camera_far, fov_y;   // for telling depth apart; rough values do
    int out_rgba;                           // the output as bytes, red first, not as words
};

// Upscales one frame to `out_w` by `out_h`, as 0xFFRRGGBB words (or RGBA bytes, with out_rgba),
// `out_pitch` bytes a row.
// Returns 0 if it could not (the picture is then left alone).
int albw_fsr3_upscale(const struct albw_vkup_frame *in, uint32_t *out, int out_w, int out_h,
                      int out_pitch);
int albw_dlss_upscale(const struct albw_vkup_frame *in, uint32_t *out, int out_w, int out_h,
                      int out_pitch);

void albw_vkup_close(void);

#ifdef __cplusplus
}
#endif

#endif

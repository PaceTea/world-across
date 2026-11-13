#ifndef ALBW_UPSCALE_H
#define ALBW_UPSCALE_H

#include <stdint.h>

// AMD FSR 1 on the processor, for a window whose renderer cannot run FSR's shaders: AMD's EASU
// scales `src` (RGB, three bytes a pixel, `src_w` by `src_h`) to `dst_w` by `dst_h`, and AMD's
// RCAS sharpens the result by `stops` (0 is the most, each stop halves it). `dst` receives
// 0xFFRRGGBB words, `dst_pitch` bytes a row. The rows are shared out among the renderer's threads.
//
// It is the same filter as the shaders in src/shaders, written out in C from third_party/fsr1, with
// the same intermediate rounding to eight bits between the passes; only the order of a few
// additions differs, so the two agree to within a step of the last bit here and there.
void albw_upscale_fsr1(const uint8_t *src, int src_w, int src_h, uint32_t *dst, int dst_w,
                       int dst_h, int dst_pitch, float stops);

#endif

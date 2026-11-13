#ifndef ALBW_SCREEN_H
#define ALBW_SCREEN_H

#include <stdint.h>

// Writing out what the LCDs would be showing.
//
// There is no window yet, so this is how anything about the picture can be checked at all: it
// reads the framebuffer the guest has told the GSP module to scan out and writes it as a PPM,
// which any viewer opens and no library is needed to produce. It is also how an implementation of
// DisplayTransfer can be judged -- run it, look at the frame - rather than by reasoning about
// tiling.
//
// Off unless ALBW_DUMP_FRAMES names a directory. ALBW_DUMP_EVERY sets the interval in frames,
// counting the first one either way, because the interesting frames are the first and then
// occasional later ones.
void albw_screen_dump(uint32_t shared_addr, uint32_t thread_index, unsigned long frame);

// One screen -- 0 the top, 1 the bottom -- as rows of RGB, top row first, into `rgb`, which must
// hold 400 by 240 pixels. Sets the width, 400 or 320. Returns 0 if the game has not set that screen
// up yet, leaving `rgb` alone.
int albw_screen_capture(uint32_t shared_addr, uint32_t thread_index, int screen, uint8_t *rgb,
                        uint32_t *width_out);

// The same in two steps: where the screen is scanned out from, as the GSP's shared memory says
// now, and that framebuffer read as RGB -- from `offset` bytes further on, which is how a frame
// drawn into the shadow (albw_kernel.h) is read.
struct albw_screen_source {
    uint32_t base, stride, format;
};
int albw_screen_source(uint32_t shared_addr, uint32_t thread_index, int screen,
                       struct albw_screen_source *out);
void albw_screen_read(const struct albw_screen_source *src, uint32_t offset, int screen,
                      uint8_t *rgb);

// What a display transfer copies: a render target, tiled unless the flags say linear, into a
// framebuffer, flipped if they say so, converted if the formats differ. Checks nothing: the
// caller has made sure both sides are inside memory.
void albw_transfer_copy(uint32_t src, uint32_t dst, uint32_t src_dim, uint32_t dst_dim,
                        uint32_t flags);

// Framebuffer colour formats, as the low three bits of a framebuffer format word or of a
// transfer's format field select them. Shared because the display transfer and the frame dump
// have to agree about what a pixel is.
#define ALBW_FB_RGBA8  0u
#define ALBW_FB_RGB8   1u
#define ALBW_FB_RGB565 2u
#define ALBW_FB_RGB5A1 3u
#define ALBW_FB_RGBA4  4u

uint32_t albw_pixel_bytes(uint32_t format);

// One pixel as eight bits per channel, and back. The 16-bit formats are expanded by replicating
// the high bits into the low ones, which is what makes white come out as 0xFF and not 0xF8.
void albw_pixel_read(uint32_t at, uint32_t format, uint8_t rgb[3]);
void albw_pixel_write(uint32_t at, uint32_t format, const uint8_t rgb[3]);

// Where a pixel sits in the tiled layout the GPU uses for render targets and textures: eight-by-eight
// tiles, and a Z-order curve inside each tile. Shared because the rasteriser writes through it and
// the display transfer reads through it, and if the two disagreed the picture would be scrambled in a
// way that looks like a rasteriser bug.
uint32_t albw_tiled_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t bpp);

#endif

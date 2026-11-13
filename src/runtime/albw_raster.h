#ifndef ALBW_RASTER_H
#define ALBW_RASTER_H

#include <stdint.h>

// Turning a draw into pixels.
//
// This is a software rasteriser for the PICA200, written against what the game's own command stream
// says rather than against a register map: the render targets, the viewport, the attribute layout and
// the shader were all identified by arithmetic that comes out exact, and each is noted where it is
// used. It draws into the guest's colour buffer, in the tiled layout the hardware uses, so the
// display transfer the kernel already implements carries the result to the screen unchanged.
//
// Off unless ALBW_RENDER is set, because a full-screen quad fifty times a frame for twenty thousand
// frames is more pixels than anybody wants to wait for. ALBW_RENDER names the last frame to render.
void albw_raster_draw(int indexed);

// Draws are rasterised in batches; this draws whatever is waiting. Anything that reads or replaces
// what the GPU draws with or into -- the end of a command list, a write to the lighting tables --
// calls it first.
void albw_raster_flush(void);

// The kernel's side of the batching: a command list has ended; a display transfer is about to
// read a target (whether it fills a whole top screen, which is how the right eye's pass is told
// apart); a fill, DMA or texture copy is about to touch memory. See the right eye in albw_raster.c.
void albw_raster_list_done(void);
void albw_raster_before_transfer(int whole_top_screen);
void albw_raster_before_memory(void);

// Render every frame from now on, whatever ALBW_RENDER says: a window is open and somebody is looking.
void albw_raster_render_always(void);

// What the rasteriser did and what it had to skip, printed once at the end.
void albw_raster_report(void);

// Seconds the game's drawing has taken so far -- shading, set-up and rasterising -- counted on the
// thread that waits for it.
double albw_raster_busy(void);

// Frames in between (see albw_raster.c): the record of every frame the game draws, which a frame
// between two of them is drawn from. Recording is on while a window wants such frames, or with
// ALBW_REPLAY_CHECK=1, which plays each frame back unmoved and compares it with the game's.
void albw_raster_set_recording(int on);
// The kernel's side: a fill, display transfer or texture copy was carried out, and a frame is over.
void albw_raster_note_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t step);
void albw_raster_note_transfer(uint32_t src, uint32_t dst, uint32_t src_dim, uint32_t dst_dim,
                               uint32_t flags);
void albw_raster_note_texture_copy(uint32_t src, uint32_t dst, uint32_t size, uint32_t in_dim,
                                   uint32_t out_dim);
void albw_raster_frame_end(uint32_t shared_addr, uint32_t thread_index, unsigned long frame);
// The window's side, from its own thread: holds the last two recorded frames, so that the game's
// thread records into others meanwhile, and says which frame the later one is (0 if there are not
// two yet); draws the frame `t` of the way from the earlier to the later -- 0 the earlier, 1 the
// later -- reading each screen they drew into `top` (400 by 240 RGB) and `bottom` (320 by 240),
// and leaving a screen they did not draw as it was; and lets them go.
int albw_raster_between_hold(unsigned long *later);
void albw_raster_between_draw(float t, uint8_t *top, uint8_t *bottom);
void albw_raster_between_release(void);

// What a temporal upscaler takes of the top screen, from the later held frame: the picture drawn
// moved by `jitter` (screen pixels, x right and y down), each pixel's depth (0 at the near plane,
// 1 at the far one) and its motion back to the earlier held frame (screen pixels, the jitter left
// out), and whether the frames have nothing to do with each other. Returns 0 if it cannot.
struct albw_upscale_frame {
    float jitter_x, jitter_y;
    uint8_t rgb[400 * 240 * 3];
    float depth[400 * 240];
    float motion[400 * 240 * 2];
    uint8_t reactive[400 * 240];        // how much of the pixel is blended over, 0 to 255
    int reset;
};
int albw_raster_between_upscale(struct albw_upscale_frame *f);

// Called first thing on the game's thread when it is not the main one, so the renderer's threads
// know whose work comes first.
void albw_raster_game_thread(void);

// Runs fn(item, worker) for items 0 to items - 1 on the renderer's threads, and returns when all
// are done -- for other work that splits into rows, such as scaling the picture for the window.
// Not while a batch is being drawn: the threads are the batch's then.
void albw_raster_parallel(void (*fn)(uint32_t item, int worker), uint32_t items);

#endif

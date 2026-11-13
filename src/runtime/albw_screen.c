#include "albw_screen.h"

#include "albw_cpu.h"
#include "albw_kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The two screens, as the hardware sees them: the framebuffer is stored a column at a time, so
// what the transfer calls the height is the screen's width. Both are 240 pixels the other way.
#define ALBW_TOP_WIDTH    400u
#define ALBW_BOTTOM_WIDTH 320u
#define ALBW_SCREEN_ROWS  240u

// The two tables are the Z-order curve within a tile: a pixel's offset inside its own tile is the x
// table's entry plus the y table's.
uint32_t albw_tiled_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t bpp) {
    static const uint32_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
    static const uint32_t yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    uint32_t inside = xl[x & 7u] + yl[y & 7u];
    return (y & ~7u) * width * bpp + (inside + (x & ~7u) * 8u) * bpp;
}

uint32_t albw_pixel_bytes(uint32_t format) {
    switch (format & 7u) {
        case ALBW_FB_RGBA8:  return 4;
        case ALBW_FB_RGB8:   return 3;
        default:             return 2;                  // the 16-bit formats
    }
}

void albw_pixel_read(uint32_t at, uint32_t format, uint8_t rgb[3]) {
    switch (format & 7u) {
        case ALBW_FB_RGBA8: {
            uint32_t v = albw_r32(at);
            rgb[0] = (uint8_t)(v >> 24);
            rgb[1] = (uint8_t)(v >> 16);
            rgb[2] = (uint8_t)(v >> 8);
            return;
        }
        case ALBW_FB_RGB8:
            // Stored blue first, as the hardware reads it.
            rgb[0] = albw_r8(at + 2);
            rgb[1] = albw_r8(at + 1);
            rgb[2] = albw_r8(at + 0);
            return;
        case ALBW_FB_RGB565: {
            uint16_t v = albw_r16(at);
            uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            rgb[0] = (uint8_t)((r << 3) | (r >> 2));
            rgb[1] = (uint8_t)((g << 2) | (g >> 4));
            rgb[2] = (uint8_t)((b << 3) | (b >> 2));
            return;
        }
        case ALBW_FB_RGB5A1: {
            uint16_t v = albw_r16(at);
            uint32_t r = (v >> 11) & 0x1F, g = (v >> 6) & 0x1F, b = (v >> 1) & 0x1F;
            rgb[0] = (uint8_t)((r << 3) | (r >> 2));
            rgb[1] = (uint8_t)((g << 3) | (g >> 2));
            rgb[2] = (uint8_t)((b << 3) | (b >> 2));
            return;
        }
        default: {                                      // RGBA4
            uint16_t v = albw_r16(at);
            uint32_t r = (v >> 12) & 0xF, g = (v >> 8) & 0xF, b = (v >> 4) & 0xF;
            rgb[0] = (uint8_t)(r * 17);
            rgb[1] = (uint8_t)(g * 17);
            rgb[2] = (uint8_t)(b * 17);
            return;
        }
    }
}

void albw_pixel_write(uint32_t at, uint32_t format, const uint8_t rgb[3]) {
    switch (format & 7u) {
        case ALBW_FB_RGBA8:
            albw_w32(at, ((uint32_t)rgb[0] << 24) | ((uint32_t)rgb[1] << 16)
                       | ((uint32_t)rgb[2] << 8) | 0xFFu);
            return;
        case ALBW_FB_RGB8:
            albw_w8(at + 0, rgb[2]);
            albw_w8(at + 1, rgb[1]);
            albw_w8(at + 2, rgb[0]);
            return;
        case ALBW_FB_RGB565:
            albw_w16(at, (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3)));
            return;
        case ALBW_FB_RGB5A1:
            albw_w16(at, (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 3) << 6)
                                  | ((rgb[2] >> 3) << 1) | 1u));
            return;
        default:                                        // RGBA4
            albw_w16(at, (uint16_t)(((rgb[0] >> 4) << 12) | ((rgb[1] >> 4) << 8)
                                  | ((rgb[2] >> 4) << 4) | 0xFu));
            return;
    }
}

// One screen as rows of RGB, top row first: what the console would show. The framebuffer runs a
// column at a time and bottom to top within a column, because the panel is mounted on its side, so a
// row here is a column there. Shared by the frame dumps and the window, so the two can never disagree
// about what is on the screen.
int albw_screen_source(uint32_t shared_addr, uint32_t thread_index, int screen,
                       struct albw_screen_source *out) {
    uint32_t width = screen ? ALBW_BOTTOM_WIDTH : ALBW_TOP_WIDTH;
    if (!shared_addr) return 0;
    uint32_t info = shared_addr + 0x200u + thread_index * 0x80u + (uint32_t)screen * 0x40u;
    // The header's index says which of the two entries is current.
    uint32_t entry = info + 4u + (albw_r8(info) & 1u) * 0x1Cu;
    out->base = albw_r32(entry + 4);                     // the left eye; the right is the same
    out->stride = albw_r32(entry + 12);                  // picture with the parallax applied
    out->format = albw_r32(entry + 16);
    return out->base && out->stride && albw_guest_ok(out->base, width * out->stride);
}

void albw_screen_read(const struct albw_screen_source *src, uint32_t offset, int screen,
                      uint8_t *rgb) {
    uint32_t width = screen ? ALBW_BOTTOM_WIDTH : ALBW_TOP_WIDTH;
    uint32_t base = src->base + offset, bpp = albw_pixel_bytes(src->format);
    for (uint32_t y = 0; y < ALBW_SCREEN_ROWS; y++) {
        for (uint32_t x = 0; x < width; x++) {
            albw_pixel_read(base + x * src->stride + (ALBW_SCREEN_ROWS - 1 - y) * bpp, src->format,
                            rgb + (y * width + x) * 3u);
        }
    }
}

int albw_screen_capture(uint32_t shared_addr, uint32_t thread_index, int screen, uint8_t *rgb,
                        uint32_t *width_out) {
    if (width_out) *width_out = screen ? ALBW_BOTTOM_WIDTH : ALBW_TOP_WIDTH;
    struct albw_screen_source src;
    if (!albw_screen_source(shared_addr, thread_index, screen, &src)) return 0;
    albw_screen_read(&src, 0, screen, rgb);
    return 1;
}

void albw_transfer_copy(uint32_t src, uint32_t dst, uint32_t src_dim, uint32_t dst_dim,
                        uint32_t flags) {
    uint32_t src_w = src_dim & 0xFFFFu, src_h = src_dim >> 16;
    uint32_t dst_w = dst_dim & 0xFFFFu, dst_h = dst_dim >> 16;
    uint32_t in_format = (flags >> 8) & 7u, out_format = (flags >> 12) & 7u;
    uint32_t in_bpp = albw_pixel_bytes(in_format), out_bpp = albw_pixel_bytes(out_format);
    uint32_t w = src_w < dst_w ? src_w : dst_w;
    uint32_t h = src_h < dst_h ? src_h : dst_h;
    if (!w || !h) return;

    // The source is a render target, and a render target is tiled: eight-by-eight tiles with a
    // Z-order curve inside each. Bit 1 of the flags says the source is linear after all, and bit 2
    // says to take the first `h` lines of a taller surface -- which is how the bottom screen's 320
    // lines come out of a 400-line buffer.
    int flip = flags & 1u;
    int source_linear = (flags >> 1) & 1u;

    // The usual transfer -- a tiled render target to a framebuffer of the same format -- as a copy,
    // with the tile arithmetic worked out once per row: per pixel through the general path it cost
    // ten milliseconds a frame, for three transfers of a screen each.
    if (in_format == out_format && !source_linear && (in_bpp == 2 || in_bpp == 4)) {
        static const uint32_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
        static const uint32_t yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
        for (uint32_t y = 0; y < h; y++) {
            uint32_t fy = flip ? src_h - 1u - y : y;
            const uint8_t *row = albw_mem + src + (fy & ~7u) * src_w * in_bpp;
            uint32_t ylo = yl[fy & 7u];
            uint8_t *to = albw_mem + dst + y * dst_w * out_bpp;
            if (in_bpp == 2) {
                for (uint32_t x = 0; x < w; x++) {
                    memcpy(to + x * 2u, row + (xl[x & 7u] + ylo + (x & ~7u) * 8u) * 2u, 2);
                }
            } else {
                for (uint32_t x = 0; x < w; x++) {
                    memcpy(to + x * 4u, row + (xl[x & 7u] + ylo + (x & ~7u) * 8u) * 4u, 4);
                }
            }
        }
        return;
    }

    for (uint32_t y = 0; y < h; y++) {
        uint32_t from_y = flip ? src_h - 1u - y : y;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t from = source_linear
                          ? src + (from_y * src_w + x) * in_bpp
                          : src + albw_tiled_offset(x, from_y, src_w, in_bpp);
            uint32_t to = dst + (y * dst_w + x) * out_bpp;
            // The usual case is the same format both sides, which is a copy of the pixel's bytes;
            // converting through eight bits a channel is only needed when they differ.
            if (in_format == out_format) {
                memcpy(albw_mem + to, albw_mem + from, in_bpp);
                continue;
            }
            uint8_t rgb[3];
            albw_pixel_read(from, in_format, rgb);
            albw_pixel_write(to, out_format, rgb);
        }
    }
}

void albw_screen_dump(uint32_t shared_addr, uint32_t thread_index, unsigned long frame) {
    const char *dir = albw_env("ALBW_DUMP_FRAMES");
    if (!dir || !shared_addr) return;

    const char *every_s = albw_env("ALBW_DUMP_EVERY");
    unsigned long every = every_s ? strtoul(every_s, NULL, 0) : 600;
    if (every < 1) every = 1;
    // ALBW_DUMP_FROM is the first frame worth a picture. Without it the first dump is frame one,
    // which is a hundred frames before the game draws anything -- a black screen that says nothing.
    const char *from_s = albw_env("ALBW_DUMP_FROM");
    unsigned long from = from_s ? strtoul(from_s, NULL, 0) : 1;
    if (frame < from) return;
    if (frame != from && (frame - from) % every) return;

    static uint8_t rgb[ALBW_TOP_WIDTH * ALBW_SCREEN_ROWS * 3];
    for (uint32_t screen = 0; screen < 2; screen++) {
        uint32_t width;
        if (!albw_screen_capture(shared_addr, thread_index, (int)screen, rgb, &width)) continue;

        char path[512];
        snprintf(path, sizeof path, "%s/frame-%06lu-%s.ppm", dir, frame,
                 screen ? "bottom" : "top");
        FILE *f = fopen(path, "wb");
        if (!f) {
            printf("[screen] cannot write %s\n", path);
            continue;
        }
        fprintf(f, "P6\n%u %u\n255\n", width, ALBW_SCREEN_ROWS);
        fwrite(rgb, 3, (size_t)width * ALBW_SCREEN_ROWS, f);
        fclose(f);
        printf("[screen] wrote %s\n", path);
    }
}

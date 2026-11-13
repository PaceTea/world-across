#ifndef ALBW_LIGHTING_H
#define ALBW_LIGHTING_H

#include <stdint.h>

// The PICA200's fragment lighting: what turns a surface's interpolated normal and view direction into
// the two colours the texture combiner can name as the "primary" and "secondary fragment colour".
//
// This game's 3D scenes are cel shaded through it -- the combiner stages build the shading out of
// both colours -- so without it every lit surface comes out as whatever the stand-in was. The
// lookup tables it uses go past as register writes, like the shader's program, and are caught here.

// One write to the lookup table registers: 0x1C5 selects the table and the entry, 0x1C8 to 0x1CF
// store an entry and move to the next.
void albw_lighting_lut_select(uint32_t value);
void albw_lighting_lut_word(uint32_t value);

// The lighting registers of one draw, decoded: what albw_lighting_compute works from. A draw keeps
// its own, because draws are rasterised in batches, after the registers have moved on. The lookup
// tables are pointed at, not copied: a write to a table a snapshot still points at goes to a copy.
#define ALBW_LIGHTING_LUTS 24
struct albw_lighting_sampler {
    uint32_t input;
    int absolute;
    float scale;
};

struct albw_light {
    uint32_t num, light_config;
    float position[3], spot[3];
    int use_dist, use_spot;
    float atten_scale, atten_bias;
    float spec0[3], spec1[3], diffuse[3], ambient[3];
};

struct albw_lighting_state {
    uint32_t config0, config;
    uint32_t bump_mode, bump_unit;
    int rebuild_z, clamp_highlights;
    int use_d0, use_d1, use_rr, use_rg, use_rb, use_fresnel;
    struct albw_lighting_sampler sampler[7];             // by the field's position: D0, D1, spot,
                                                         // Fresnel, blue, green, red
    uint32_t lights;
    struct albw_light light[8];
    float global[3];
    const uint32_t *lut[ALBW_LIGHTING_LUTS];
};

// Decodes the lighting registers for the draw about to be rendered.
void albw_lighting_snapshot(struct albw_lighting_state *out);

// Says that no snapshot taken so far is needed any more -- the batch they were for is drawn -- so
// the tables kept for them can be reused.
void albw_lighting_release(void);

// Whether the registers say lighting is on for the current draw.
int albw_lighting_enabled(void);

// The two colours for one fragment, eight bits a channel. `quat` is the interpolated normal
// quaternion and `view` the interpolated view vector, both straight from the vertex shader.
// `textures` are the four texture units as sampled for this fragment, for the normal or tangent map
// a draw may take from one of them. Colours are packed as the registers pack them, red in the low
// byte.
void albw_lighting_compute(const struct albw_lighting_state *ls, const float quat[4],
                           const float view[3], const uint32_t textures[4], uint32_t *primary,
                           uint32_t *secondary);

// Prints the lighting registers and the populated tables.
void albw_lighting_dump(void);

#endif

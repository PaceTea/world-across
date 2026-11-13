// The layout of one draw's state as the GPU renderer's fragment shader reads it: an array of 32-bit
// words, the same for every draw, in a storage buffer. Included by albw_raster.c, which writes it,
// and by pica.frag, which reads it -- so it holds nothing but #defines both languages understand.
// Floats are stored as their bits.
#ifndef PICA_STATE_H
#define PICA_STATE_H

// The six combiner stages, four words each: the sources (colour in bits 0-11, alpha in 12-23, four
// bits a source, unknown sources already turned into "previous"), the modifiers (colour four bits
// each from bit 0, alpha four bits each from bit 12), the operations and scales (colour op bits 0-3,
// alpha op 4-7, colour scale 8-9, alpha scale 12-13) and the constant colour.
#define ST_STAGE        0
#define ST_BUFFER_COLOR 24
#define ST_UPDATE       25      // the buffer update register as it is
#define ST_ALPHA        26      // test on in bit 0, function in 4-6, reference in 8-15
#define ST_FLAGS        27      // see ST_F_*
#define ST_DEPTH_RANGE  28      // float
#define ST_DEPTH_NEAR   29      // float

#define ST_F_LIGHTING   1u      // the fragment lighting runs and its colours are named
#define ST_F_WBUFFER    2u      // depth is multiplied back by w

// The three texture units, four words each: enabled in bit 0, which coordinates in 1-2, wrap s in
// 4-7, wrap t in 8-11; the border colour; the level-of-detail bias (float); unused.
#define ST_TEX          32

// Fragment lighting.
#define ST_L_CONFIG0    44      // the register
#define ST_L_CONFIG     45      // which of the eight configurations
#define ST_L_BUMP       46      // mode in 0-1, unit in 4-5, rebuild z in 8, clamp highlights in 9
#define ST_L_LIGHTS     47      // how many
#define ST_L_USE        48      // see ST_U_*
#define ST_L_SAMPLER    49      // seven: input in bits 0-2, absolute in 3
#define ST_L_SCALE      56      // seven floats
#define ST_L_GLOBAL     63      // three floats: the global ambient colour
#define ST_L_LUT        66      // twenty-four: where each table starts in the table buffer, in
                                // entries; 0xFFFFFFFF where the draw has none
#define ST_U_D0         1u
#define ST_U_D1         2u
#define ST_U_RR         4u
#define ST_U_RG         8u
#define ST_U_RB         16u
#define ST_U_FRESNEL    32u

// Eight lights, 24 words each: position, spot direction, specular 0, specular 1, diffuse, ambient
// (three floats each), the light's number, its configuration register, distance attenuation on in
// bit 0 and spot in bit 1, the attenuation's scale and bias (floats), and one unused word.
#define ST_LIGHT        96
#define ST_LIGHT_WORDS  24
#define LT_POSITION     0
#define LT_SPOT         3
#define LT_SPEC0        6
#define LT_SPEC1        9
#define LT_DIFFUSE      12
#define LT_AMBIENT      15
#define LT_NUM          18
#define LT_CONFIG       19
#define LT_USE          20
#define LT_ATTEN_SCALE  21
#define LT_ATTEN_BIAS   22

#define ST_WORDS        288

// The tables' numbers, as the lighting registers have them.
#define LUT_D0          0
#define LUT_D1          1
#define LUT_FRESNEL     3
#define LUT_RB          4
#define LUT_RG          5
#define LUT_RR          6
#define LUT_SPOT        8
#define LUT_DIST        16

#endif

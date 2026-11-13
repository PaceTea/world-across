#version 450
// The GPU renderer's fragment stage: what albw_raster.c's shade_pixel and albw_lighting.c do for a
// pixel, for one fragment -- the texture units, the fragment lighting, the six combiner stages and
// the alpha test -- with the depth worked out as the console works it out. Blending, the depth and
// stencil tests and the write masks are the pipeline's. The combiner keeps the console's eight bits
// a channel and its integer arithmetic, so a stage gives what the console's would.
#include "pica_state.h"

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tc01;
layout(location = 2) in vec4 v_tc2w;
layout(location = 3) in vec4 v_quat;
layout(location = 4) in vec4 v_view;
layout(location = 5) in vec4 v_prev;

layout(location = 0) out vec4 o_color;
// For a temporal upscaler, where the pass has the targets: each pixel's motion back to the frame
// before, its depth, and how much of it is blended over what was there. Discarded otherwise.
layout(location = 1) out vec2 o_motion;
layout(location = 2) out float o_depth;
layout(location = 3) out float o_reactive;

layout(set = 2, binding = 0) uniform sampler2D u_tex0;
layout(set = 2, binding = 1) uniform sampler2D u_tex1;
layout(set = 2, binding = 2) uniform sampler2D u_tex2;
layout(std430, set = 2, binding = 3) readonly buffer States { uint st[]; };
layout(std430, set = 2, binding = 4) readonly buffer Tables { uint lut[]; };

layout(set = 3, binding = 0) uniform Draw {
    uvec4 u_info;               // x: where the draw's state starts, in words; y: 1 to give the
                                // upscaler one minus the depth (a draw that tests "greater")
    vec4 u_uv[3];               // per unit: coordinates times xy, plus zw -- for a unit that samples
                                // part of a picture of a target
    vec4 u_view;                // the viewport without the jitter: half its size, its offset, in
                                // the target's pixels
    vec4 u_jitter;              // xy: the jitter, in the target's pixels; z: the reactive value
};

uint S(int i) { return st[u_info.x + uint(i)]; }
float SF(int i) { return uintBitsToFloat(st[u_info.x + uint(i)]); }

// ---------------------------------------------------------------------------------------------
// Colours as the console's combiner has them: eight bits a channel, as integers.
ivec4 unpack_rgba(uint v) {
    return ivec4(int(v & 0xFFu), int((v >> 8) & 0xFFu), int((v >> 16) & 0xFFu), int(v >> 24));
}

ivec3 modify_rgb(ivec4 v, uint how) {
    switch (how) {
        case 0u: return v.rgb;
        case 1u: return 255 - v.rgb;
        case 2u: return ivec3(v.a);
        case 3u: return ivec3(255 - v.a);
        case 4u: return ivec3(v.r);
        case 5u: return ivec3(255 - v.r);
        case 8u: return ivec3(v.g);
        case 9u: return ivec3(255 - v.g);
        case 12u: return ivec3(v.b);
        case 13u: return ivec3(255 - v.b);
        default: return v.rgb;
    }
}

int modify_alpha(ivec4 v, uint how) {
    switch (how & 7u) {
        case 0u: return v.a;
        case 1u: return 255 - v.a;
        case 2u: return v.r;
        case 3u: return 255 - v.r;
        case 4u: return v.g;
        case 5u: return 255 - v.g;
        case 6u: return v.b;
        default: return 255 - v.b;
    }
}

ivec3 combine3(uint op, ivec3 a, ivec3 b, ivec3 c) {
    switch (op) {
        case 0u: return a;
        case 1u: return a * b / 255;
        case 2u: return a + b;
        case 3u: return a + b - 128;
        case 4u: return (a * c + b * (255 - c)) / 255;
        case 5u: return a - b;
        case 8u: return a * b / 255 + c;
        case 9u: return (a + b) * c / 255;
        default: return a;
    }
}

int combine1(uint op, int a, int b, int c) {
    switch (op) {
        case 0u: return a;
        case 1u: return a * b / 255;
        case 2u: return a + b;
        case 3u: return a + b - 128;
        case 4u: return (a * c + b * (255 - c)) / 255;
        case 5u: return a - b;
        case 8u: return a * b / 255 + c;
        case 9u: return (a + b) * c / 255;
        default: return a;
    }
}

ivec3 saturate3(ivec3 v, uint scale) { return clamp(v << int(scale), ivec3(0), ivec3(255)); }
int saturate1(int v, uint scale) { return clamp(v << int(scale), 0, 255); }

bool compare_func(uint how, int a, int b) {
    switch (how & 7u) {
        case 0u: return false;
        case 1u: return true;
        case 2u: return a == b;
        case 3u: return a != b;
        case 4u: return a < b;
        case 5u: return a <= b;
        case 6u: return a > b;
        default: return a >= b;
    }
}

// ---------------------------------------------------------------------------------------------
// Textures. Coordinates and their derivatives are worked out once, where every fragment of a quad
// is still running, so that sampling inside a branch has gradients to go by.
ivec4 sample_unit(int u, vec2 st_in, vec2 dx, vec2 dy) {
    uint cfg = S(ST_TEX + u * 4);
    uint ws = (cfg >> 4) & 15u, wt = (cfg >> 8) & 15u;
    float bias = exp2(SF(ST_TEX + u * 4 + 2));
    vec2 uv = st_in * u_uv[u].xy + u_uv[u].zw;
    vec2 gx = dx * u_uv[u].xy * bias, gy = dy * u_uv[u].xy * bias;
    vec4 c;
    if (u == 0) c = textureGrad(u_tex0, uv, gx, gy);
    else if (u == 1) c = textureGrad(u_tex1, uv, gx, gy);
    else c = textureGrad(u_tex2, uv, gx, gy);
    // Clamped to the border: outside the texture, the border colour.
    if (((ws & 3u) == 1u && (st_in.x < 0.0 || st_in.x >= 1.0))
            || ((wt & 3u) == 1u && (st_in.y < 0.0 || st_in.y >= 1.0))) {
        return unpack_rgba(S(ST_TEX + u * 4 + 1));
    }
    return ivec4(c * 255.0 + 0.5);
}

// ---------------------------------------------------------------------------------------------
// Fragment lighting, as albw_lighting.c has it.
float lut_lookup(uint table, uint index, float delta) {
    if (table >= 24u) return 1.0;
    uint off = S(ST_L_LUT + int(table));
    if (off == 0xFFFFFFFFu) return 1.0;
    uint e = lut[off + (index & 0xFFu)];
    float value = float(e & 0xFFFu) / 4095.0;
    int diff = int((e >> 12) & 0xFFFu);
    if (diff >= 0x800) diff -= 0x1000;
    return value + float(diff) / 4095.0 * delta;
}

float lut_sample(uint table, int which, vec3 n, vec3 v, vec3 l, vec3 h, vec3 spot, vec3 tangent) {
    uint sp = S(ST_L_SAMPLER + which);
    uint input_kind = sp & 7u;
    bool absolute = (sp & 8u) != 0u;
    float x;
    switch (input_kind) {
        case 0u: x = dot(n, h); break;
        case 1u: x = dot(v, h); break;
        case 2u: x = dot(n, v); break;
        case 3u: x = dot(l, n); break;
        case 4u: x = dot(l, spot); break;
        case 5u: {
            if (S(ST_L_CONFIG) != 8u) { x = 0.0; break; }
            vec3 proj = h - n * dot(n, h);
            x = dot(proj, tangent);
            break;
        }
        default: x = 0.0; break;
    }
    uint index;
    float delta;
    if (absolute) {
        x = abs(x);
        float f = clamp(floor(x * 256.0), 0.0, 255.0);
        index = uint(f);
        delta = x * 256.0 - f;
    } else {
        float f = clamp(floor(x * 128.0), -128.0, 127.0);
        index = uint(int(f)) & 0xFFu;
        delta = x * 128.0 - f;
    }
    return SF(ST_L_SCALE + which) * lut_lookup(table, index, delta);
}

vec3 quat_rotate(vec4 q, vec3 v) {
    vec3 t = cross(q.xyz, v) + q.w * v;
    return v + 2.0 * cross(q.xyz, t);
}

vec3 safe_normalize(vec3 v) {
    float len = length(v);
    return len > 0.0 ? v / len : v;
}

void lighting(ivec4 textures[3], out ivec4 primary, out ivec4 secondary) {
    vec4 q = v_quat;
    float qlen = length(q);
    if (qlen > 0.0) q /= qlen;

    uint bump = S(ST_L_BUMP);
    uint bump_mode = bump & 3u, bump_unit = (bump >> 4) & 3u;
    vec3 surface_n = vec3(0.0, 0.0, 1.0), surface_t = vec3(1.0, 0.0, 0.0);
    if (bump_mode == 1u || bump_mode == 2u) {
        // Unit 3 is the procedural one, which this game does not use: it reads as nothing.
        ivec4 texel = bump_unit < 3u ? textures[bump_unit] : ivec4(0);
        vec3 p = 2.0 * vec3(texel.rgb) / 255.0 - 1.0;
        if (bump_mode == 1u) {
            surface_n = p;
            if ((bump & 256u) != 0u) {
                float zz = 1.0 - p.x * p.x - p.y * p.y;
                surface_n.z = zz > 0.0 ? sqrt(zz) : 0.0;
            }
        } else {
            surface_t = p;
        }
    }
    vec3 n = quat_rotate(q, surface_n);
    vec3 tangent = quat_rotate(q, surface_t);
    vec3 v = v_view.xyz;
    vec3 norm_view = safe_normalize(v);

    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 specular_sum = vec4(0.0, 0.0, 0.0, 1.0);
    uint use = S(ST_L_USE);
    uint config0 = S(ST_L_CONFIG0);
    bool clamp_highlights = (bump & 512u) != 0u;
    uint lights = S(ST_L_LIGHTS);
    for (uint slot = 0u; slot < lights && slot < 8u; slot++) {
        int L = ST_LIGHT + int(slot) * ST_LIGHT_WORDS;
        vec3 position = vec3(SF(L + LT_POSITION), SF(L + LT_POSITION + 1), SF(L + LT_POSITION + 2));
        vec3 spot = vec3(SF(L + LT_SPOT), SF(L + LT_SPOT + 1), SF(L + LT_SPOT + 2));
        vec3 spec0 = vec3(SF(L + LT_SPEC0), SF(L + LT_SPEC0 + 1), SF(L + LT_SPEC0 + 2));
        vec3 spec1 = vec3(SF(L + LT_SPEC1), SF(L + LT_SPEC1 + 1), SF(L + LT_SPEC1 + 2));
        vec3 diffuse = vec3(SF(L + LT_DIFFUSE), SF(L + LT_DIFFUSE + 1), SF(L + LT_DIFFUSE + 2));
        vec3 ambient = vec3(SF(L + LT_AMBIENT), SF(L + LT_AMBIENT + 1), SF(L + LT_AMBIENT + 2));
        uint num = S(L + LT_NUM);
        uint light_config = S(L + LT_CONFIG);
        uint luse = S(L + LT_USE);

        vec3 l = (light_config & 1u) != 0u ? position : position + v;
        l = safe_normalize(l);
        vec3 h = norm_view + l;
        float h_len2 = dot(h, h);
        vec3 h_norm = safe_normalize(h);

        float dist_atten = 1.0;
        if ((luse & 1u) != 0u) {
            vec3 d = -v - position;
            float distance = length(d);
            float at = clamp(SF(L + LT_ATTEN_SCALE) * distance + SF(L + LT_ATTEN_BIAS), 0.0, 1.0);
            float f = min(floor(at * 256.0), 255.0);
            dist_atten = lut_lookup(uint(LUT_DIST) + num, uint(f), at * 256.0 - f);
        }
        float spot_atten = 1.0;
        if ((luse & 2u) != 0u) {
            spot_atten = lut_sample(uint(LUT_SPOT) + num, 2, n, norm_view, l, h_norm, spot, tangent);
        }
        float d0 = 1.0;
        if ((use & ST_U_D0) != 0u) d0 = lut_sample(uint(LUT_D0), 0, n, norm_view, l, h_norm, spot, tangent);
        vec3 specular_0 = d0 * spec0;

        vec3 refl;
        refl.r = (use & ST_U_RR) != 0u ? lut_sample(uint(LUT_RR), 6, n, norm_view, l, h_norm, spot, tangent) : 1.0;
        refl.g = (use & ST_U_RG) != 0u ? lut_sample(uint(LUT_RG), 5, n, norm_view, l, h_norm, spot, tangent) : refl.r;
        refl.b = (use & ST_U_RB) != 0u ? lut_sample(uint(LUT_RB), 4, n, norm_view, l, h_norm, spot, tangent) : refl.r;

        float d1 = 1.0;
        if ((use & ST_U_D1) != 0u) d1 = lut_sample(uint(LUT_D1), 1, n, norm_view, l, h_norm, spot, tangent);
        vec3 specular_1 = d1 * refl * spec1;

        if ((use & ST_U_FRESNEL) != 0u) {
            float fresnel = lut_sample(uint(LUT_FRESNEL), 3, n, norm_view, l, h_norm, spot, tangent);
            if ((config0 & 4u) != 0u) diffuse_sum.a = fresnel;
            if ((config0 & 8u) != 0u) specular_sum.a = fresnel;
        }

        float ln = dot(l, n);
        float lit = ((light_config >> 1) & 1u) != 0u ? abs(ln) : max(ln, 0.0);
        float clamp_factor = clamp_highlights ? (ln <= 0.0 ? 0.0 : 1.0) : 1.0;
        if (((light_config >> 2) & 3u) != 0u) {
            float geo = h_len2 == 0.0 ? 0.0 : min(lit / h_len2, 1.0);
            if (((light_config >> 2) & 1u) != 0u) specular_0 *= geo;
            if (((light_config >> 3) & 1u) != 0u) specular_1 *= geo;
        }
        float atten = dist_atten * spot_atten;
        diffuse_sum.rgb += (diffuse * lit + ambient) * atten;
        specular_sum.rgb += (specular_0 + specular_1) * clamp_factor * atten;
    }
    diffuse_sum.rgb += vec3(SF(ST_L_GLOBAL), SF(ST_L_GLOBAL + 1), SF(ST_L_GLOBAL + 2));
    // Truncated to eight bits, as the console's conversion has it.
    primary = ivec4(clamp(diffuse_sum, 0.0, 1.0) * 255.0);
    secondary = ivec4(clamp(specular_sum, 0.0, 1.0) * 255.0);
}

// ---------------------------------------------------------------------------------------------
void main() {
    // The coordinates each unit samples, and their gradients, while all of the quad runs.
    vec2 tc[3] = vec2[3](v_tc01.xy, v_tc01.zw, v_tc2w.xy);
    vec2 tdx[3] = vec2[3](dFdx(tc[0]), dFdx(tc[1]), dFdx(tc[2]));
    vec2 tdy[3] = vec2[3](dFdy(tc[0]), dFdy(tc[1]), dFdy(tc[2]));

    ivec4 src_tab[16];
    for (int i = 0; i < 16; i++) src_tab[i] = ivec4(0);
    src_tab[0] = ivec4(clamp(v_color, 0.0, 1.0) * 255.0 + 0.5);
    ivec4 textures[3] = ivec4[3](ivec4(0), ivec4(0), ivec4(0));
    for (int u = 0; u < 3; u++) {
        uint cfg = S(ST_TEX + u * 4);
        if ((cfg & 1u) == 0u) continue;
        int coords = int((cfg >> 1) & 3u);
        textures[u] = sample_unit(u, tc[coords], tdx[coords], tdy[coords]);
        src_tab[3 + u] = textures[u];
    }
    uint flags = S(ST_FLAGS);
    if ((flags & ST_F_LIGHTING) != 0u) {
        ivec4 p, s;
        lighting(textures, p, s);
        src_tab[1] = p;
        src_tab[2] = s;
    }

    // The combiner. The buffer a stage reads lags two stages behind; "previous" for the first stage
    // is nothing.
    ivec4 next_buffer = unpack_rgba(S(ST_BUFFER_COLOR));
    ivec4 previous = ivec4(0);
    uint update = S(ST_UPDATE);
    for (int stage = 0; stage < 6; stage++) {
        uint srcs = S(ST_STAGE + stage * 4);
        uint mods = S(ST_STAGE + stage * 4 + 1);
        uint ops = S(ST_STAGE + stage * 4 + 2);
        src_tab[15] = previous;
        src_tab[14] = unpack_rgba(S(ST_STAGE + stage * 4 + 3));
        uint op_rgb = ops & 15u, op_a = (ops >> 4) & 15u;
        uint scale_rgb = (ops >> 8) & 3u, scale_a = (ops >> 12) & 3u;

        ivec3 m0 = modify_rgb(src_tab[srcs & 15u], mods & 15u);
        ivec3 m1 = modify_rgb(src_tab[(srcs >> 4) & 15u], (mods >> 4) & 15u);
        ivec3 m2 = modify_rgb(src_tab[(srcs >> 8) & 15u], (mods >> 8) & 15u);
        ivec3 rgb;
        int dot3v = 0;
        if (op_rgb == 6u || op_rgb == 7u) {
            ivec3 d = (m0 - 128) * (m1 - 128);
            dot3v = (d.x + d.y + d.z) * 4 / 255;
            rgb = ivec3(saturate1(dot3v, scale_rgb));
        } else {
            rgb = saturate3(combine3(op_rgb, m0, m1, m2), scale_rgb);
        }
        int a;
        if (op_rgb == 7u) {
            a = saturate1(dot3v, scale_a);
        } else {
            int a0 = modify_alpha(src_tab[(srcs >> 12) & 15u], (mods >> 12) & 15u);
            int a1 = modify_alpha(src_tab[(srcs >> 16) & 15u], (mods >> 16) & 15u);
            int a2 = modify_alpha(src_tab[(srcs >> 20) & 15u], (mods >> 20) & 15u);
            a = saturate1(combine1(op_a, a0, a1, a2), scale_a);
        }
        previous = ivec4(rgb, a);

        src_tab[13] = next_buffer;
        if (stage < 4) {
            if (((update >> (8 + stage)) & 1u) != 0u) next_buffer.rgb = previous.rgb;
            if (((update >> (12 + stage)) & 1u) != 0u) next_buffer.a = previous.a;
        }
    }

    uint alpha = S(ST_ALPHA);
    if ((alpha & 1u) != 0u && !compare_func((alpha >> 4) & 7u, previous.a, int((alpha >> 8) & 0xFFu))) {
        discard;
    }

    // The depth as the console has it: the depth range and offset applied to z over w, which the
    // rasteriser has interpolated in screen space, times w again for a W buffer.
    float depth = SF(ST_DEPTH_RANGE) * -gl_FragCoord.z + SF(ST_DEPTH_NEAR);
    if ((flags & ST_F_WBUFFER) != 0u) depth /= gl_FragCoord.w;
    gl_FragDepth = clamp(depth, 0.0, 1.0);

    o_color = vec4(previous) / 255.0;

    // Where this surface was a frame before, less where it is now, the jitter taken out.
    vec2 here = gl_FragCoord.xy - u_jitter.xy;
    vec2 before = here;
    if (v_prev.w > 1e-6) before = (v_prev.xy / v_prev.w + 1.0) * u_view.xy + u_view.zw;
    o_motion = before - here;
    o_depth = u_info.y != 0u ? 1.0 - gl_FragDepth : gl_FragDepth;
    o_reactive = u_jitter.z;
}

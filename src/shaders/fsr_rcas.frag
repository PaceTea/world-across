#version 450
// FSR 1, second pass: RCAS, AMD's robust contrast-adaptive sharpening, from the EASU result onto
// the window one pixel to one pixel. The code is AMD's (third_party/fsr1); this only feeds it.
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0, std140) uniform Rcas {
    vec4 params;                                         // where the picture starts in the window,
};                                                       // and the sharpness in stops (0 is most)

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
    ivec2 last = textureSize(u_texture, 0) - 1;
    return texelFetch(u_texture, clamp(ivec2(p), ivec2(0), last), 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main() {
    AU4 con;
    FsrRcasCon(con, params.z);
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(max(gl_FragCoord.xy - params.xy, vec2(0.0))), con);
    o_color = vec4(r, g, b, 1.0);
}

#version 450
// FSR 1, first pass: EASU, AMD's edge-adaptive spatial upsampling, from the game's screen at its
// own size to the size it is shown at. Drawn into a target exactly that size, so the fragment's
// position is the output pixel. The code is AMD's (third_party/fsr1); this only feeds it.
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// The screen being drawn, as SDL's GPU renderer binds it, and the sizes, as the window sets them.
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0, std140) uniform Easu {
    vec4 sizes;                                          // input width, height; output width, height
};

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(u_texture, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(u_texture, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(u_texture, p, 2); }
#include "ffx_fsr1.h"

void main() {
    AU4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3, sizes.x, sizes.y, sizes.x, sizes.y, sizes.z, sizes.w);
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
    o_color = vec4(c, 1.0);
}

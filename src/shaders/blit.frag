#version 450
// A display transfer, onto a screen: the target's pixel each of the screen's pixels comes from,
// by an affine map of the screen's coordinates (0 to 1, from the top left) into the target's.
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D u_source;
layout(set = 3, binding = 0) uniform Blit {
    vec4 u_size;                // the screen's size in pixels, and unused
    vec4 u_row0;                // source u = row0.x * u + row0.y * v + row0.z
    vec4 u_row1;                // source v = row1.x * u + row1.y * v + row1.z
};

void main() {
    vec2 uv = gl_FragCoord.xy / u_size.xy;
    vec2 src = vec2(dot(u_row0.xyz, vec3(uv, 1.0)), dot(u_row1.xyz, vec3(uv, 1.0)));
    o_color = vec4(texture(u_source, src).rgb, 1.0);
}

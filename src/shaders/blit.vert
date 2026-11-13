#version 450
// One triangle over the whole target, for copying a picture with its coordinates turned -- a display
// transfer onto a screen, a clear of part of a target.
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

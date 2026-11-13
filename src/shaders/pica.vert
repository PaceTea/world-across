#version 450
// The GPU renderer's vertex stage. The vertices arrive shaded -- the console's vertex program ran
// on the processor -- so all that is left is handing them on: position in the console's clip space,
// whose z runs from 0 at the near plane to -w at the far one, made into SDL's, whose z runs from 0
// to w and whose y points the other way (a target's rows are counted as the console counts them).
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec4 a_tc01;
layout(location = 3) in vec4 a_tc2w;
layout(location = 4) in vec4 a_quat;
layout(location = 5) in vec4 a_view;
layout(location = 6) in vec4 a_prev;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_tc01;
layout(location = 2) out vec4 v_tc2w;
layout(location = 3) out vec4 v_quat;
layout(location = 4) out vec4 v_view;
layout(location = 5) out vec4 v_prev;

void main() {
    gl_Position = vec4(a_pos.x, -a_pos.y, -a_pos.z, a_pos.w);
    v_color = a_color;
    v_tc01 = a_tc01;
    v_tc2w = a_tc2w;
    v_quat = a_quat;
    v_view = a_view;
    v_prev = a_prev;
}

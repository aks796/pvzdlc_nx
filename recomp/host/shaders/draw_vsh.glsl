#version 460
// gpu.h's vertices (images are stored top row first): guest pixel position (D3D7 pixel centres on integers), uv, colour as B,G,R,A bytes
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(std140, binding = 0) uniform V { vec4 size; };
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_col;
void main() {
    v_uv = a_uv;
    v_col = a_col.zyxw;
    vec2 ndc = (a_pos + 0.5) / size.xy * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0); // clip-space y points up: guest row 0 is image row 0
}

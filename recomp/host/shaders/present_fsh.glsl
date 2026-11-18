#version 460
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(binding = 0) uniform sampler2D image;
void main() { o = vec4(texture(image, uv).rgb, 1.0); }

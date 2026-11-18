#version 460
// Direct3D 7 stage 0 (D3DTOP / D3DTA), alpha test, DirectDraw source colour key: as host/gpu_gl.c
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(binding = 0) uniform sampler2D u_tex;
layout(std140, binding = 0) uniform F {
    ivec4 cop;   // op, arg1, arg2, texture on
    ivec4 aop;   // op, arg1, arg2, texture has alpha
    ivec4 test;  // alpha func (0 off), colour key on
    vec4 tf;     // texture factor
    vec4 ck_lo;  // colour key range, 0..255
    vec4 ck_hi;
    vec4 aref;   // alpha reference, 0..255
};
layout(location = 0) out vec4 o;
vec4 arg(int a, vec4 d, vec4 t) { int s = a & 15; return s == 2 ? t : s == 3 ? tf : d; }
vec4 op(int k, vec4 a, vec4 b) {
    if (k == 2) return a;
    if (k == 3) return b;
    if (k == 4) return a * b;
    if (k == 5) return min(a * b * 2.0, 1.0);
    if (k == 6) return min(a * b * 4.0, 1.0);
    if (k == 7) return min(a + b, 1.0);
    if (k == 8) return clamp(a + b - 0.5, 0.0, 1.0);
    if (k == 10) return max(a - b, 0.0);
    return a;
}
void main() {
    vec4 d = v_col, t = vec4(1.0);
    if (cop.w != 0) {
        t = texture(u_tex, v_uv);
        if (aop.w == 0) t.a = 1.0;
        if (test.y != 0) {
            vec3 c = floor(t.rgb * 255.0 + 0.5);
            if (all(greaterThanEqual(c, ck_lo.rgb)) && all(lessThanEqual(c, ck_hi.rgb))) discard;
        }
    }
    vec4 r = d;
    if (cop.x != 1) r.rgb = op(cop.x, arg(cop.y, d, t), arg(cop.z, d, t)).rgb;
    if (aop.x != 1) r.a = op(aop.x, arg(aop.y, d, t), arg(aop.z, d, t)).a;
    if (test.x != 0) {
        float a = floor(r.a * 255.0 + 0.5), ref = aref.x;
        int f = test.x;
        bool p = f == 1 ? false : f == 2 ? a < ref : f == 3 ? a == ref : f == 4 ? a <= ref :
                 f == 5 ? a > ref : f == 6 ? a != ref : f == 7 ? a >= ref : true;
        if (!p) discard;
    }
    o = r;
}

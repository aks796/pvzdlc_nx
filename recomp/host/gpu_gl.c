/* gpu_gl.c: the OpenGL 3.2 core backend of gpu.h (the Mac), and OpenGL ES 3.0 (Android).
 *
 * One program does everything the game asks of Direct3D 7 and DirectDraw:
 * stage-0 texture ops, alpha test and DirectDraw source colour keys; blending
 * is fixed-function. Draws are queued while their state stays the same and
 * go out as one glDrawArrays. Every texture has a framebuffer object so it
 * can be a render target. Rows are kept top-first: guest row 0 is GL row 0,
 * so render targets and images agree and readback needs no flip; only the
 * final present to the window flips.
 * OpenGL ES has no BGRA pixel formats: there textures keep R, G, B, A in their own order, and the game's B, G, R, A
 * pixels are swapped on the way in and out (tex_upload / tex_download). */
#if defined(__ANDROID__) || defined(RT_GLES)
#define GPU_GLES 1 /* (Android; Linux on ARM, which mostly has OpenGL ES only: make TARGET=linux-arm64) */
#endif
#ifdef GPU_GLES
#include <GLES3/gl3.h>
#define GLSL "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
#define PIX_FORMAT GL_RGBA, GL_UNSIGNED_BYTE
#elif defined(_WIN32) || defined(__linux__)
/* Windows: opengl32.dll has only OpenGL 1.1; the rest comes from the driver once there is a context (gl_load).
 * Linux the same way (libGL's newer functions through SDL from whichever driver the context is on) */
#define GPU_GL_LOADER 1
#include <SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#define GLSL "#version 150\n"
#define PIX_FORMAT GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_FUNCS(X) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLBINDSAMPLERPROC, glBindSampler) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) X(PFNGLDELETESYNCPROC, glDeleteSync) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLFENCESYNCPROC, glFenceSync) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLGENSAMPLERSPROC, glGenSamplers) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) X(PFNGLUNIFORM3FPROC, glUniform3f) X(PFNGLUNIFORM3IPROC, glUniform3i) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLWAITSYNCPROC, glWaitSync)
#define GL_PTR(t, n) static t p_##n;
GL_FUNCS(GL_PTR)
static void gl_load(void)
{
#define GL_GET(t, n) p_##n = (t)SDL_GL_GetProcAddress(#n);
    GL_FUNCS(GL_GET)
}
#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glBindBuffer p_glBindBuffer
#define glBindFragDataLocation p_glBindFragDataLocation
#define glBindFramebuffer p_glBindFramebuffer
#define glBindSampler p_glBindSampler
#define glBindVertexArray p_glBindVertexArray
#define glBlitFramebuffer p_glBlitFramebuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glCompileShader p_glCompileShader
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteSync p_glDeleteSync
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glFenceSync p_glFenceSync
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glGenBuffers p_glGenBuffers
#define glGenFramebuffers p_glGenFramebuffers
#define glGenSamplers p_glGenSamplers
#define glGenVertexArrays p_glGenVertexArrays
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glGetProgramiv p_glGetProgramiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glSamplerParameteri p_glSamplerParameteri
#define glShaderSource p_glShaderSource
#define glUniform1f p_glUniform1f
#define glUniform1i p_glUniform1i
#define glUniform2f p_glUniform2f
#define glUniform3f p_glUniform3f
#define glUniform3i p_glUniform3i
#define glUniform4f p_glUniform4f
#define glUseProgram p_glUseProgram
#define glVertexAttribPointer p_glVertexAttribPointer
#define glWaitSync p_glWaitSync
#else
#include <OpenGL/gl3.h>
#define GLSL "#version 150\n"
#define PIX_FORMAT GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/gpu.h"
#include "../runtime/rt.h"
#include "overlay.h"

struct GpuTex {
    GLuint tex, fbo;
    uint32_t w, h;
    struct GpuTex *next_free;
};

static GLuint prog, prog_show, vao, vbo, samplers[2][4][4];
static GLint u_size, u_tex_on, u_tex_alpha, u_cop, u_aop, u_tf, u_atest, u_aref, u_ck, u_cklo, u_ckhi;
static GLint us_tex, us_rect;
static pthread_t gl_thread;
static GpuTex *shown;

/* queued draws */
#define QMAX 65536
static GpuState qstate;
static GpuVtx qv[QMAX];
static uint32_t qn;

static pthread_mutex_t free_lock = PTHREAD_MUTEX_INITIALIZER;
static GpuTex *free_list;

static const char *vs_src =
    GLSL
    "in vec2 a_pos; in vec2 a_uv; in vec4 a_col;\n"
    "uniform vec2 u_size;\n"
    "out vec2 v_uv; out vec4 v_col;\n"
    "void main() {\n"
    "  v_uv = a_uv; v_col = a_col.zyxw;\n" /* ARGB in memory is B,G,R,A */
    "  gl_Position = vec4((a_pos + 0.5) / u_size * 2.0 - 1.0, 0.0, 1.0);\n" /* D3D7 pixel centres are integers */
    "}\n";

static const char *fs_src =
    GLSL
    "in vec2 v_uv; in vec4 v_col;\n"
    "uniform sampler2D u_tex; uniform int u_tex_on, u_tex_alpha;\n"
    "uniform ivec3 u_cop, u_aop;\n" /* D3DTOP, arg1, arg2 */
    "uniform vec4 u_tf;\n"
    "uniform int u_atest; uniform float u_aref;\n"
    "uniform int u_ck; uniform vec3 u_cklo, u_ckhi;\n"
    "out vec4 o;\n"
    "vec4 arg(int a, vec4 d, vec4 t) { int s = a & 15; return s == 2 ? t : s == 3 ? u_tf : d; }\n"
    "vec4 op(int k, vec4 a, vec4 b) {\n"
    "  if (k == 2) return a; if (k == 3) return b; if (k == 4) return a * b;\n"
    "  if (k == 5) return min(a * b * 2.0, 1.0); if (k == 6) return min(a * b * 4.0, 1.0);\n"
    "  if (k == 7) return min(a + b, 1.0); if (k == 8) return clamp(a + b - 0.5, 0.0, 1.0);\n"
    "  if (k == 10) return max(a - b, 0.0); return a;\n"
    "}\n"
    "void main() {\n"
    "  vec4 d = v_col, t = vec4(1.0);\n"
    "  if (u_tex_on != 0) {\n"
    "    t = texture(u_tex, v_uv);\n"
    "    if (u_tex_alpha == 0) t.a = 1.0;\n"
    "    if (u_ck != 0) {\n"
    "      vec3 c = floor(t.rgb * 255.0 + 0.5);\n"
    "      if (all(greaterThanEqual(c, u_cklo)) && all(lessThanEqual(c, u_ckhi))) discard;\n"
    "    }\n"
    "  }\n"
    "  vec4 r = d;\n"
    "  if (u_cop.x != 1) r.rgb = op(u_cop.x, arg(u_cop.y, d, t), arg(u_cop.z, d, t)).rgb;\n"
    "  if (u_aop.x != 1) r.a = op(u_aop.x, arg(u_aop.y, d, t), arg(u_aop.z, d, t)).a;\n"
    "  if (u_atest != 0) {\n"
    "    float a = floor(r.a * 255.0 + 0.5);\n"
    "    bool p = u_atest == 1 ? false : u_atest == 2 ? a < u_aref : u_atest == 3 ? a == u_aref :\n"
    "             u_atest == 4 ? a <= u_aref : u_atest == 5 ? a > u_aref : u_atest == 6 ? a != u_aref :\n"
    "             u_atest == 7 ? a >= u_aref : true;\n"
    "    if (!p) discard;\n"
    "  }\n"
    "  o = r;\n"
    "}\n";

/* present: the shown texture, top row at the top of the window */
static const char *vs_show =
    GLSL
    "uniform vec4 u_rect;\n" /* x, y, w, h in NDC */
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "  v_uv = vec2(p.x, 1.0 - p.y);\n"
    "  gl_Position = vec4(u_rect.xy + p * u_rect.zw, 0.0, 1.0);\n"
    "}\n";
static const char *fs_show =
    GLSL
    "in vec2 v_uv; uniform sampler2D u_tex; out vec4 o;\n"
    "void main() { o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_fatal("[gpu] shader: %s", log);
    }
    return s;
}
static GLuint program(const char *vs, const char *fs, int attribs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    if (attribs) {
        glBindAttribLocation(p, 0, "a_pos");
        glBindAttribLocation(p, 1, "a_uv");
        glBindAttribLocation(p, 2, "a_col");
    }
#ifndef GPU_GLES
    glBindFragDataLocation(p, 0, "o");
#endif
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        rt_fatal("[gpu] link: %s", log);
    }
    return p;
}

static void reap(void)
{
    pthread_mutex_lock(&free_lock);
    GpuTex *t = free_list;
    free_list = NULL;
    pthread_mutex_unlock(&free_lock);
    while (t) {
        GpuTex *n = t->next_free;
        if (t == shown) shown = NULL;
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->tex);
        free(t);
        t = n;
    }
}

static GLenum blend_factor(uint32_t f, int target_alpha)
{
    switch (f) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return target_alpha ? GL_DST_ALPHA : GL_ONE;
    case 8: return target_alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return target_alpha ? GL_SRC_ALPHA_SATURATE : GL_ZERO;
    default: return GL_ONE;
    }
}

static void bind_target(GpuTex *t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
}

static void flush(void)
{
    if (!qn) return;
    const GpuState *s = &qstate;
    bind_target(s->target);
    glEnable(GL_SCISSOR_TEST);
    glScissor(s->clip_x, s->clip_y, s->clip_w, s->clip_h);
    glUseProgram(prog);
    glUniform2f(u_size, (float)s->target->w, (float)s->target->h);
    int tex_on = s->tex != NULL && s->cop != 1;
    glUniform1i(u_tex_on, tex_on);
    glUniform1i(u_tex_alpha, s->tex_alpha);
    glUniform3i(u_cop, (GLint)s->cop, (GLint)s->ca1, (GLint)s->ca2);
    glUniform3i(u_aop, (GLint)s->aop, (GLint)s->aa1, (GLint)s->aa2);
    glUniform4f(u_tf, (float)((s->tfactor >> 16) & 255) / 255.f, (float)((s->tfactor >> 8) & 255) / 255.f,
                (float)(s->tfactor & 255) / 255.f, (float)(s->tfactor >> 24) / 255.f);
    glUniform1i(u_atest, s->alpha_test ? (GLint)s->alpha_func : 0);
    glUniform1f(u_aref, (float)(s->alpha_ref & 255));
    glUniform1i(u_ck, s->colorkey);
    glUniform3f(u_cklo, (float)((s->ck_lo >> 16) & 255), (float)((s->ck_lo >> 8) & 255), (float)(s->ck_lo & 255));
    glUniform3f(u_ckhi, (float)((s->ck_hi >> 16) & 255), (float)((s->ck_hi >> 8) & 255), (float)(s->ck_hi & 255));
    if (s->tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s->tex->tex);
        uint32_t au = s->address_u >= 1 && s->address_u <= 3 ? s->address_u : 3;
        uint32_t av = s->address_v >= 1 && s->address_v <= 3 ? s->address_v : 3;
        glBindSampler(0, samplers[s->linear ? 1 : 0][au][av]);
    }
    if (s->blend) {
        glEnable(GL_BLEND);
        uint32_t sb = s->src_blend, db = s->dst_blend;
        if (sb == 12) sb = 5, db = 6;      /* BOTHSRCALPHA */
        else if (sb == 13) sb = 6, db = 5; /* BOTHINVSRCALPHA */
        glBlendFunc(blend_factor(sb, s->target_alpha), blend_factor(db, s->target_alpha));
    } else {
        glDisable(GL_BLEND);
    }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(qn * sizeof(GpuVtx)), NULL, GL_STREAM_DRAW); /* orphan */
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(qn * sizeof(GpuVtx)), qv);
    glDrawArrays(s->lines ? GL_LINES : GL_TRIANGLES, 0, (GLsizei)qn);
    qn = 0;
}

static GpuTex *tex_new(uint32_t w, uint32_t h)
{
    GpuTex *t = calloc(1, sizeof *t);
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, PIX_FORMAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    return t;
}
static void tex_free(GpuTex *t)
{
    if (!t) return;
    pthread_mutex_lock(&free_lock);
    t->next_free = free_list;
    free_list = t;
    pthread_mutex_unlock(&free_lock);
}
#ifdef GPU_GLES
/* B, G, R, A <-> R, G, B, A, rows of w pixels pitch bytes apart */
static void swap_rb(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h, uint32_t dpitch, uint32_t spitch)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * spitch);
        uint32_t *d = (uint32_t *)(dst + (size_t)y * dpitch);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = s[x];
            d[x] = (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
        }
    }
}
static uint8_t *swap_buf;
static size_t swap_cap;
#endif
static void tex_upload(GpuTex *t, const void *pixels, uint32_t pitch)
{
    if (qn && (qstate.tex == t || qstate.target == t)) flush();
#ifdef GPU_GLES
    size_t need = (size_t)t->w * t->h * 4;
    if (need > swap_cap) {
        free(swap_buf);
        swap_buf = malloc(need);
        swap_cap = swap_buf ? need : 0;
        if (!swap_buf) return;
    }
    swap_rb(swap_buf, pixels, t->w, t->h, t->w * 4, pitch);
    pixels = swap_buf;
    pitch = t->w * 4;
#endif
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)t->w, (GLsizei)t->h, PIX_FORMAT, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}
static void tex_download(GpuTex *t, void *pixels, uint32_t pitch)
{
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(pitch / 4));
    glReadPixels(0, 0, (GLsizei)t->w, (GLsizei)t->h, PIX_FORMAT, pixels);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
#ifdef GPU_GLES
    swap_rb(pixels, pixels, t->w, t->h, pitch, pitch);
#endif
}
static void draw(const GpuState *s, const GpuVtx *v, uint32_t n)
{
    reap();
    if (qn && (memcmp(s, &qstate, sizeof *s) || qn + n > QMAX)) flush();
    if (n > QMAX) n = QMAX;
    qstate = *s;
    memcpy(qv + qn, v, n * sizeof *v);
    qn += n;
}
static void fill(GpuTex *t, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t argb)
{
    flush();
    bind_target(t);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor((float)((argb >> 16) & 255) / 255.f, (float)((argb >> 8) & 255) / 255.f, (float)(argb & 255) / 255.f,
                 (float)(argb >> 24) / 255.f);
    glClear(GL_COLOR_BUFFER_BIT);
}
static void show(GpuTex *t)
{
    flush();
    shown = t;
}
static int on_gpu_thread(void) { return pthread_equal(pthread_self(), gl_thread); }

static const GpuBackend backend = {"OpenGL", tex_new, tex_free, tex_upload, tex_download, draw, fill, show, on_gpu_thread};

/* The context is current on this thread (the platform made it) */
void gpu_gl_init(void)
{
#ifdef GPU_GL_LOADER
    gl_load();
#endif
    gl_thread = pthread_self();
    prog = program(vs_src, fs_src, 1);
    u_size = glGetUniformLocation(prog, "u_size");
    u_tex_on = glGetUniformLocation(prog, "u_tex_on");
    u_tex_alpha = glGetUniformLocation(prog, "u_tex_alpha");
    u_cop = glGetUniformLocation(prog, "u_cop");
    u_aop = glGetUniformLocation(prog, "u_aop");
    u_tf = glGetUniformLocation(prog, "u_tf");
    u_atest = glGetUniformLocation(prog, "u_atest");
    u_aref = glGetUniformLocation(prog, "u_aref");
    u_ck = glGetUniformLocation(prog, "u_ck");
    u_cklo = glGetUniformLocation(prog, "u_cklo");
    u_ckhi = glGetUniformLocation(prog, "u_ckhi");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    prog_show = program(vs_show, fs_show, 0);
    us_tex = glGetUniformLocation(prog_show, "u_tex");
    us_rect = glGetUniformLocation(prog_show, "u_rect");
    glUseProgram(prog_show);
    glUniform1i(us_tex, 0);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVtx), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVtx), (void *)8);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVtx), (void *)16);

    /* [linear][address u][address v]; D3DTADDRESS 1 wrap, 2 mirror, 3 clamp */
    static const GLenum wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE};
    for (int l = 0; l < 2; l++)
        for (int u = 0; u < 4; u++)
            for (int v = 0; v < 4; v++) {
                GLuint smp;
                glGenSamplers(1, &smp);
                glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, l ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, l ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, wrap[u]);
                glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, wrap[v]);
                samplers[l][u][v] = smp;
            }
    rt_gpu = &backend;
    rt_log("[gpu] OpenGL renderer: %s\n", (const char *)glGetString(GL_RENDERER));
}

/* Draw the shown texture into the window's default framebuffer, letterboxed; the caller swaps */
void gpu_gl_present(int win_w, int win_h)
{
    flush();
    reap();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!shown) return;
    float sx = (float)win_w / (float)shown->w, sy = (float)win_h / (float)shown->h, k = sx < sy ? sx : sy;
    float w = (float)shown->w * k / (float)win_w * 2.f, h = (float)shown->h * k / (float)win_h * 2.f;
    glUseProgram(prog_show);
    glUniform4f(us_rect, -w / 2.f, -h / 2.f, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, shown->tex);
    glBindSampler(0, samplers[1][3][3]);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Test screenshots (RT_CTL shot): the shown frame with the port's overlay, as ARGB rows; 0 if there is
 * no GPU frame or nothing to add (the caller uses the game's own surface then) */
int gpu_gl_snapshot(void **pixels, uint32_t *w, uint32_t *h)
{
    if (!shown || !ov_active()) return 0;
    GpuTex *t = ov_compose(shown, shown->w, shown->h, draw);
    flush();
    *w = t->w;
    *h = t->h;
    *pixels = malloc((size_t)t->w * t->h * 4);
    if (!*pixels) return 0;
    tex_download(t, *pixels, t->w * 4);
    return 1;
}

/* ---------------- presentation thread ----------------
 * The game thread copies each finished display frame into one of three
 * shared textures and moves on; a presenter thread (its own context in the
 * same share group) shows the newest one on every vsync. Only the presenter
 * ever waits for the display, so the game loop never stalls on a swap. */
enum { SLOT_FREE, SLOT_READY, SLOT_SHOWING };
static GLuint pres_tex[3], pres_fbo[3], pres_vao;
static GLsync pres_fence[3];
static int pres_state[3], pres_w, pres_h;
static pthread_mutex_t pres_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pres_cond = PTHREAD_COND_INITIALIZER;
static unsigned pres_frames;

/* game thread: publish the shown texture as the newest frame */
void gpu_gl_publish(void)
{
    if (!shown) return;
    GpuTex *game = shown;
    if (ov_active()) shown = ov_compose(shown, shown->w, shown->h, draw); /* the port's pictures over it */
    flush();
    reap();
    if (!pres_tex[0] || pres_w != (int)shown->w || pres_h != (int)shown->h) {
        pthread_mutex_lock(&pres_lock);
        for (int i = 0; i < 3; i++) {
            if (pres_state[i] == SLOT_SHOWING) continue; /* the presenter frees nothing; it'll get a new one */
            if (!pres_tex[i]) {
                glGenTextures(1, &pres_tex[i]);
                glGenFramebuffers(1, &pres_fbo[i]);
            }
            glBindTexture(GL_TEXTURE_2D, pres_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)shown->w, (GLsizei)shown->h, 0, PIX_FORMAT, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, pres_fbo[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pres_tex[i], 0);
        }
        pres_w = (int)shown->w;
        pres_h = (int)shown->h;
        pthread_mutex_unlock(&pres_lock);
    }
    pthread_mutex_lock(&pres_lock);
    int slot = -1;
    for (int i = 0; i < 3 && slot < 0; i++)
        if (pres_state[i] == SLOT_FREE) slot = i;
    if (slot < 0) /* the presenter hasn't taken the last one: replace it */
        for (int i = 0; i < 3 && slot < 0; i++)
            if (pres_state[i] == SLOT_READY) slot = i;
    if (pres_fence[slot]) {
        glDeleteSync(pres_fence[slot]);
        pres_fence[slot] = 0;
    }
    pres_state[slot] = SLOT_FREE;
    pthread_mutex_unlock(&pres_lock);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, shown->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pres_fbo[slot]);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)shown->w, (GLint)shown->h, 0, 0, (GLint)shown->w, (GLint)shown->h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush(); /* the other context only sees commands that were flushed */

    pthread_mutex_lock(&pres_lock);
    for (int i = 0; i < 3; i++)
        if (pres_state[i] == SLOT_READY) {
            pres_state[i] = SLOT_FREE;
            if (pres_fence[i]) {
                glDeleteSync(pres_fence[i]);
                pres_fence[i] = 0;
            }
        }
    pres_fence[slot] = f;
    pres_state[slot] = SLOT_READY;
    pthread_cond_broadcast(&pres_cond);
    pthread_mutex_unlock(&pres_lock);
    shown = game;
}

/* presenter thread: wait up to timeout_ms for a newer frame, draw the newest into the window's
 * framebuffer; returns 0 if there was nothing new (the caller doesn't swap) */
int gpu_gl_present_next(int win_w, int win_h, int timeout_ms)
{
    pthread_mutex_lock(&pres_lock);
    int ready = -1;
    for (int i = 0; i < 3; i++)
        if (pres_state[i] == SLOT_READY) ready = i;
    if (ready < 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)timeout_ms * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&pres_cond, &pres_lock, &ts);
        for (int i = 0; i < 3; i++)
            if (pres_state[i] == SLOT_READY) ready = i;
    }
    if (ready < 0) {
        pthread_mutex_unlock(&pres_lock);
        return 0;
    }
    for (int i = 0; i < 3; i++)
        if (pres_state[i] == SLOT_SHOWING) pres_state[i] = SLOT_FREE;
    pres_state[ready] = SLOT_SHOWING;
    GLsync f = pres_fence[ready];
    pres_fence[ready] = 0;
    GLuint tex = pres_tex[ready];
    int tw = pres_w, th = pres_h;
    pthread_mutex_unlock(&pres_lock);

    if (f) {
        glWaitSync(f, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(f);
    }
    if (!pres_vao) glGenVertexArrays(1, &pres_vao); /* VAOs aren't shared between contexts */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    float sx = (float)win_w / (float)tw, sy = (float)win_h / (float)th, k = sx < sy ? sx : sy;
    float w = (float)tw * k / (float)win_w * 2.f, h = (float)th * k / (float)win_h * 2.f;
    glUseProgram(prog_show);
    glUniform4f(us_rect, -w / 2.f, -h / 2.f, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, samplers[1][3][3]);
    glBindVertexArray(pres_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return 1;
}

/* presenter thread, after its swap: wake anyone waiting for the vertical blank */
void gpu_gl_swapped(void)
{
    pthread_mutex_lock(&pres_lock);
    pres_frames++;
    pthread_cond_broadcast(&pres_cond);
    pthread_mutex_unlock(&pres_lock);
}

/* game thread (DirectDraw WaitForVerticalBlank): wait for the presenter's next swap, at most timeout_ms */
void gpu_gl_wait_swap(int timeout_ms)
{
    pthread_mutex_lock(&pres_lock);
    unsigned n = pres_frames;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)timeout_ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    while (pres_frames == n)
        if (pthread_cond_timedwait(&pres_cond, &pres_lock, &ts)) break;
    pthread_mutex_unlock(&pres_lock);
}

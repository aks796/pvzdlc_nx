/* hle_ddraw.c: DirectDraw 7 (and the v1 views the framework asks for).
 *
 * Every surface is 32-bit X8R8G8B8 in guest memory, so the game's own
 * software renderer can Lock and draw into it. Blt/BltFast/Flip are done
 * here; whenever the visible image changes (blit to the primary, unlock of
 * the primary, Flip) the frame is handed to rt_present. Direct3D is not
 * offered, which keeps SexyApp on its software path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rt.h"
#include "rt_com.h"
#include "hle_ddraw.h"

#define DD_OK 0u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600ffu
#define DDERR_UNSUPPORTED 0x80004001u

#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PITCH 0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_LPSURFACE 0x800u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_CKSRCBLT 0x10000u

#define DDSCAPS_BACKBUFFER 0x4u
#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_SYSTEMMEMORY 0x800u
#define DDSCAPS_VIDEOMEMORY 0x4000u
#define DDSCAPS_LOCALVIDMEM 0x10000000u

#define DDBLT_COLORFILL 0x400u
#define DDBLT_KEYSRC 0x8000u
#define DDBLT_KEYSRCOVERRIDE 0x10000u
#define DDBLTFAST_SRCCOLORKEY 0x1u
#define DDCKEY_SRCBLT 0x8u

/* Platform hook: show w x h pixels (XRGB8888) from guest memory */
void rt_present(uint32_t bits, int w, int h, int pitch) __attribute__((weak));
void rt_present(uint32_t bits, int w, int h, int pitch) { (void)bits; (void)w; (void)h; (void)pitch; }
int rt_window_rect(uint32_t hwnd, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
extern uint32_t rt_main_hwnd;

typedef struct {
    uint32_t hwnd;
} Clip;

#define DDLOCK_READONLY 0x10u
static uint32_t vt_dd7, vt_dd1, vt_surf, vt_clip;
uint32_t d3d7_create_object(void);
static uint32_t dd7_obj, dd1_obj, d3d7_obj;
static uint32_t disp_w = 1280, disp_h = 720;
static int fullscreen;
Surf *primary;
int rt_widescreen;

static const uint8_t IID_IDirectDraw[16] = GUID_BYTES(0x6C14DB80, 0xA733, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
static const uint8_t IID_IDirectDraw2[16] = GUID_BYTES(0xB3A6F3E0, 0x2B43, 0x11CF, 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56);
static const uint8_t IID_IDirectDraw4[16] = GUID_BYTES(0x9c59509a, 0x39bd, 0x11d1, 0x8c, 0x4a, 0x00, 0xc0, 0x4f, 0xd9, 0x30, 0xc5);
static const uint8_t IID_IDirectDraw7[16] = GUID_BYTES(0x15e65ec0, 0x3b9c, 0x11d2, 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b);
static const uint8_t IID_IDirectDrawSurface[16] = GUID_BYTES(0x6C14DB81, 0xA733, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
static const uint8_t IID_IDirectDrawSurface2[16] = GUID_BYTES(0x57805885, 0x6eec, 0x11cf, 0x94, 0x41, 0xa8, 0x23, 0x03, 0xc1, 0x0e, 0x27);
static const uint8_t IID_IDirectDrawSurface4[16] = GUID_BYTES(0x0B2B8630, 0xAD35, 0x11D0, 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B);
static const uint8_t IID_IDirectDrawSurface7[16] = GUID_BYTES(0x06675a80, 0x3b9b, 0x11d2, 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b);
static const uint8_t IID_IDirect3D7[16] = GUID_BYTES(0xf5049e77, 0x4861, 0x11d2, 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8);
static const uint8_t IID_IUnknown[16] = GUID_BYTES(0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

static void init_vtables(void);

/* ---------------- pixel format / descriptions ---------------- */
static void put_pixfmt_a(uint32_t pf, int alpha)
{
    memset(G2H(pf), 0, 32);
    g_wr32(pf, 32);
    g_wr32(pf + 4, alpha ? 0x41 : 0x40);  /* DDPF_RGB (| DDPF_ALPHAPIXELS) */
    if (alpha) g_wr32(pf + 28, 0xff000000u);
    g_wr32(pf + 12, 32);
    g_wr32(pf + 16, 0x00ff0000);
    g_wr32(pf + 20, 0x0000ff00);
    g_wr32(pf + 24, 0x000000ff);
}

static void put_desc(Surf *s, uint32_t d, uint32_t surface_ptr)
{
    uint32_t size = g_rd32(d);
    if (size != 108 && size != 124) size = 124;
    memset(G2H(d + 4), 0, size - 4);
    g_wr32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT |
                      (surface_ptr ? DDSD_LPSURFACE : 0) | (s->has_ck ? DDSD_CKSRCBLT : 0));
    g_wr32(d + 8, s->h);
    g_wr32(d + 12, s->ox ? s->vw : s->w);
    g_wr32(d + 16, s->pitch);
    if (s->back) g_wr32(d + 20, 1);
    g_wr32(d + 36, surface_ptr);
    if (s->has_ck) {
        g_wr32(d + 64, s->ck_lo);
        g_wr32(d + 68, s->ck_hi);
    }
    put_pixfmt_a(d + 72, s->alpha);
    g_wr32(d + 104, s->caps);
}

/* ---------------- surfaces ---------------- */
const GpuBackend *rt_gpu;

int surf_gpu(Surf *s)
{
    if (!rt_gpu || !rt_gpu->on_gpu_thread()) return 0;
    if (!s->gtex) {
        s->gtex = rt_gpu->tex_new(s->w, s->h);
        s->gpu_ok = 0;
    }
    if (!s->gpu_ok) {
        rt_gpu->tex_upload(s->gtex, G2H(s->bits), s->pitch);
        s->gpu_ok = 1;
    }
    return 1;
}

void surf_cpu(Surf *s)
{
    if (s->cpu_ok) return;
    if (!rt_gpu || !s->gtex) {
        s->cpu_ok = 1;
        return;
    }
    if (!rt_gpu->on_gpu_thread()) {
        static int warned;
        if (!warned++) rt_log("[gpu] surface %ux%u read from another thread before the GPU finished it\n",
                              (unsigned)s->w, (unsigned)s->h);
        return;
    }
    rt_gpu->tex_download(s->gtex, G2H(s->bits), s->pitch);
    s->cpu_ok = 1;
}

Surf *surf_new(uint32_t w, uint32_t h, uint32_t caps)
{
    Surf *s = calloc(1, sizeof *s);
    s->cpu_ok = 1;
    s->w = w ? w : 1;
    s->h = h ? h : 1;
    s->pitch = s->w * 4;
    s->caps = caps;
    s->bits = rt_vm_alloc(0, s->pitch * s->h);
    s->refs = 1;
    s->obj = com_wrap(vt_surf, s);
    return s;
}
static void surf_free(Surf *s)
{
    if (s->back) surf_free(s->back);
    if (s->gtex) rt_gpu->tex_free(s->gtex);
    if (s->bits) rt_vm_free(s->bits);
    if (primary == s) primary = NULL;
    com_unwrap(s->obj);
    free(s);
}
Surf *S(uint32_t obj) { return com_impl(obj); }

static uint64_t now_us(void);
/* A whole frame reached the screen (the draw surface blitted to the primary, or a Flip): -v frame timing */
uint32_t rt_frames_shown; /* frames that reached the screen (blit of the draw surface or Flip) */
static void rt_frame_mark(void)
{
    rt_frames_shown++;
    if (!rt_verbose) return;
    static uint64_t last, t0, worst;
    static int n, slow;
    uint64_t t = now_us();
    if (last) {
        uint64_t dt = t - last;
        n++;
        if (dt > 17500) slow++;
        if (dt > worst) worst = dt;
        if (dt > 50000)
            rt_log("[frames] slow frame %.1f ms: %u draws %.1f ms, %u sleeps %.1f ms, lock waits %.1f ms, vblank %.1f ms, "
                   "other %.1f ms\n",
                   dt / 1000.0, (unsigned)rt_stat_draws, rt_stat_draw_us / 1000.0, (unsigned)rt_stat_sleeps,
                   rt_stat_sleep_us / 1000.0, rt_stat_lock_us / 1000.0, rt_stat_vblank_us / 1000.0,
                   ((double)dt - (double)(rt_stat_draw_us + rt_stat_sleep_us + rt_stat_lock_us + rt_stat_vblank_us)) /
                       1000.0);
    }
    rt_stat_draws = rt_stat_sleeps = 0;
    rt_stat_draw_us = rt_stat_sleep_us = rt_stat_max_sleep_us = rt_stat_lock_us = rt_stat_vblank_us = 0;
    last = t;
    if (!t0) t0 = t;
    if (t - t0 >= 5000000 && n) {
        rt_log("[frames] %.1f fps, %d of %d frames over 17.5 ms, worst %.1f ms\n", n * 1e6 / (double)(t - t0), slow, n,
               worst / 1000.0);
        uint64_t vm_now, vm_peak;
        rt_vm_usage(&vm_now, &vm_peak);
        rt_log("[mem] guest memory %.1f MB in use, peak %.1f MB\n", vm_now / 1048576.0, vm_peak / 1048576.0);
        rt_log("[frames] per frame: target locks %.2f, blits from target %.2f, blits/fills to target %.2f/%.2f, "
               "texture locks %.2f, blits to textures %.2f\n",
               st_lock_target / (double)n, st_blt_from_target / (double)n, st_blt_to_target / (double)n,
               st_fill_target / (double)n, st_lock_tex / (double)n, st_blt_to_tex / (double)n);
        st_lock_target = st_blt_from_target = st_blt_to_target = st_lock_tex = st_blt_to_tex = st_fill_target = 0;
        t0 = t;
        n = slow = 0;
        worst = 0;
    }
}

/* The display for screenshots: a CPU copy of the primary (rt_present got bits 0 for a GPU frame) */
uint32_t rt_screen_bits(void)
{
    if (!primary) return 0;
    surf_cpu(primary);
    return primary->bits;
}

static void present_primary(void)
{
    if (!primary) return;
    if (fullscreen && surf_gpu(primary)) {
        rt_gpu->show(primary->gtex);
        rt_present(0, (int)primary->w, (int)primary->h, (int)primary->pitch);
        return;
    }
    surf_cpu(primary);
    if (fullscreen) {
        rt_present(primary->bits, (int)primary->w, (int)primary->h, (int)primary->pitch);
        return;
    }
    int32_t x = 0, y = 0, w = (int32_t)primary->w, h = (int32_t)primary->h;
    rt_window_rect(rt_main_hwnd, &x, &y, &w, &h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (int32_t)primary->w) w = (int32_t)primary->w - x;
    if (y + h > (int32_t)primary->h) h = (int32_t)primary->h - y;
    if (w > 0 && h > 0) rt_present(primary->bits + (uint32_t)y * primary->pitch + (uint32_t)x * 4, w, h, (int)primary->pitch);
}

typedef struct { int32_t l, t, r, b; } Rc;
static Rc read_rect(uint32_t p, Surf *s)
{
    if (!p) return (Rc){0, 0, (int32_t)s->w, (int32_t)s->h};
    return (Rc){(int32_t)g_rd32(p) + s->ox, (int32_t)g_rd32(p + 4), (int32_t)g_rd32(p + 8) + s->ox, (int32_t)g_rd32(p + 12)};
}

static void fill(Surf *d, Rc r, uint32_t color)
{
    if (r.l < 0) r.l = 0;
    if (r.t < 0) r.t = 0;
    if (r.r > (int32_t)d->w) r.r = (int32_t)d->w;
    if (r.b > (int32_t)d->h) r.b = (int32_t)d->h;
    if (r.r <= r.l || r.b <= r.t) return;
    if (d->gtex && surf_gpu(d)) {
        rt_gpu->fill(d->gtex, r.l, r.t, r.r - r.l, r.b - r.t, d->alpha ? color : color | 0xff000000u);
        surf_gpu_wrote(d);
        return;
    }
    surf_cpu(d);
    d->gpu_ok = 0;
    for (int32_t y = r.t; y < r.b; y++) {
        uint32_t *row = (uint32_t *)G2H(d->bits + (uint32_t)y * d->pitch);
        for (int32_t x = r.l; x < r.r; x++) row[x] = color;
    }
}

/* Stretch with bilinear filtering, as a Windows driver's hardware stretch blit does. This is the
 * game's own 800x600 -> 960x720 pillarbox scale every frame, so its rows go to the worker threads.
 * Sample centres line up, edges clamp. */
typedef struct {
    Surf *d, *s;
    Rc dr, sr;
    int32_t xs, ys;
} StretchJob;

static void stretch_rows(void *ctx, int y0, int y1)
{
    const StretchJob *j = ctx;
    Surf *d = j->d, *s = j->s;
    Rc dr = j->dr, sr = j->sr;
    int32_t dw = dr.r - dr.l, sw = sr.r - sr.l, sh = sr.b - sr.t, xs = j->xs, ys = j->ys;
    const uint8_t *src = G2H(s->bits);
    for (int32_t y = y0; y < y1; y++) {
        int32_t dy = dr.t + y;
        if (dy < 0 || dy >= (int32_t)d->h) continue;
        int32_t fy = (y * ys) + ys / 2 - 0x8000;
        if (fy < 0) fy = 0;
        int32_t sy0 = fy >> 16, wy = (fy >> 8) & 0xff;
        int32_t sy1 = sy0 + 1 < sh ? sy0 + 1 : sh - 1;
        if (sy0 >= sh) sy0 = sy1 = sh - 1;
        const uint32_t *r0 = (const uint32_t *)(src + (uint32_t)(sr.t + sy0) * s->pitch) + sr.l;
        const uint32_t *r1 = (const uint32_t *)(src + (uint32_t)(sr.t + sy1) * s->pitch) + sr.l;
        uint32_t *drow = (uint32_t *)G2H(d->bits + (uint32_t)dy * d->pitch);
        for (int32_t x = 0; x < dw; x++) {
            int32_t dx = dr.l + x;
            if (dx < 0 || dx >= (int32_t)d->w) continue;
            int32_t fx = (x * xs) + xs / 2 - 0x8000;
            if (fx < 0) fx = 0;
            int32_t x0 = fx >> 16, wx = (fx >> 8) & 0xff;
            int32_t x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            if (x0 >= sw) x0 = x1 = sw - 1;
            uint32_t a = r0[x0], b = r0[x1], c = r1[x0], e = r1[x1], out = 0;
            if (a == b && a == c && a == e) {
                drow[dx] = a;
                continue;
            }
            for (int sh8 = 0; sh8 < 32; sh8 += 8) {
                int32_t ca = (a >> sh8) & 0xff, cb = (b >> sh8) & 0xff, cc = (c >> sh8) & 0xff, ce = (e >> sh8) & 0xff;
                int32_t top = ca * 256 + (cb - ca) * wx, bot = cc * 256 + (ce - cc) * wx;
                int32_t v = (top * 256 + (bot - top) * wy) >> 16;
                out |= (uint32_t)v << sh8;
            }
            drow[dx] = out;
        }
    }
}

static void blit_bilinear(Surf *d, Rc dr, Surf *s, Rc sr)
{
    StretchJob j = {d, s, dr, sr, 0, 0};
    int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    j.xs = (int32_t)(((int64_t)sw << 16) / dw);
    j.ys = (int32_t)(((int64_t)sh << 16) / dh);
    rt_parallel_rows(0, dh, stretch_rows, &j);
}

/* The blit as a textured quad on the GPU: point copy, bilinear when stretching (as the CPU path), the
 * source colour key in the shader. Pixel-aligned rects cover pixel centres l..r-1. */
static int gpu_blit(Surf *d, Rc dr, Surf *s, Rc sr, int key, uint32_t lo, uint32_t hi)
{
    if (s == d || !surf_gpu(s) || !surf_gpu(d)) return 0;
    int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    GpuState st;
    memset(&st, 0, sizeof st);
    st.target = d->gtex;
    st.tex = s->gtex;
    st.tex_alpha = s->alpha;
    st.linear = !key && (sw != dw || sh != dh);
    st.address_u = st.address_v = 3;
    st.cop = 2; st.ca1 = 2; st.ca2 = 0; /* SELECTARG1(TEXTURE) */
    st.aop = 2; st.aa1 = 2; st.aa2 = 0;
    st.target_alpha = d->alpha;
    st.colorkey = key;
    st.ck_lo = lo;
    st.ck_hi = hi;
    st.clip_w = (int32_t)d->w;
    st.clip_h = (int32_t)d->h;
    float x0 = (float)dr.l - 0.5f, x1 = (float)dr.r - 0.5f, y0 = (float)dr.t - 0.5f, y1 = (float)dr.b - 0.5f;
    float u0 = (float)sr.l / (float)s->w, u1 = (float)sr.r / (float)s->w;
    float v0 = (float)sr.t / (float)s->h, v1 = (float)sr.b / (float)s->h;
    GpuVtx q[6] = {{x0, y0, u0, v0, 0xffffffffu}, {x1, y0, u1, v0, 0xffffffffu}, {x0, y1, u0, v1, 0xffffffffu},
                   {x1, y0, u1, v0, 0xffffffffu}, {x1, y1, u1, v1, 0xffffffffu}, {x0, y1, u0, v1, 0xffffffffu}};
    rt_gpu->draw(&st, q, 6);
    surf_gpu_wrote(d);
    return 1;
}

/* Copy (optionally stretched, optionally colour-keyed) from s:sr to d:dr */
static void blit(Surf *d, Rc dr, Surf *s, Rc sr, int key, uint32_t lo, uint32_t hi)
{
    int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    /* on the GPU when the destination lives there, or the source is only current there */
    if (rt_gpu && (d->gtex || (s->gtex && !s->cpu_ok)) && gpu_blit(d, dr, s, sr, key, lo, hi)) return;
    surf_cpu(s);
    surf_cpu(d);
    d->gpu_ok = 0;
    if (!key && s != d && (sw != dw || sh != dh) && sr.l >= 0 && sr.t >= 0 && sr.r <= (int32_t)s->w &&
        sr.b <= (int32_t)s->h) {
        blit_bilinear(d, dr, s, sr);
        return;
    }
    uint32_t *tmp = NULL;
    const uint8_t *src_base = G2H(s->bits);
    uint32_t src_pitch = s->pitch;
    if (s == d) { /* overlapping self-blit: copy the source first */
        tmp = malloc((size_t)sw * (size_t)sh * 4);
        for (int32_t y = 0; y < sh; y++)
            memcpy(tmp + (size_t)y * (size_t)sw, src_base + (uint32_t)(sr.t + y) * src_pitch + (uint32_t)sr.l * 4, (size_t)sw * 4);
        src_base = (const uint8_t *)tmp;
        src_pitch = (uint32_t)sw * 4;
        sr.r -= sr.l; sr.b -= sr.t; sr.l = 0; sr.t = 0;
    }
    for (int32_t y = 0; y < dh; y++) {
        int32_t dy = dr.t + y;
        if (dy < 0 || dy >= (int32_t)d->h) continue;
        int32_t sy = sr.t + (int32_t)((int64_t)y * sh / dh);
        if (sy < 0 || sy >= (int32_t)(tmp ? (uint32_t)sh : s->h)) continue;
        const uint32_t *srow = (const uint32_t *)(src_base + (uint32_t)sy * src_pitch);
        uint32_t *drow = (uint32_t *)G2H(d->bits + (uint32_t)dy * d->pitch);
        if (!key && sw == dw && dr.l >= 0 && dr.r <= (int32_t)d->w && sr.l >= 0) {
            memcpy(drow + dr.l, srow + sr.l, (size_t)dw * 4);
            continue;
        }
        for (int32_t x = 0; x < dw; x++) {
            int32_t dx = dr.l + x;
            if (dx < 0 || dx >= (int32_t)d->w) continue;
            int32_t sx = sr.l + (sw == dw ? x : (int32_t)((int64_t)x * sw / dw));
            uint32_t px = srow[sx];
            if (key) {
                uint32_t k = px & 0xffffff;
                if (k >= lo && k <= hi) continue;
            }
            drow[dx] = px;
        }
    }
    free(tmp);
}

/* ---------------- IDirectDrawSurface7 ---------------- */
HLE(surf_QueryInterface)
{
    uint32_t riid = ARG(1), out = ARG(2);
    if (com_guid_eq(riid, IID_IDirectDrawSurface) || com_guid_eq(riid, IID_IDirectDrawSurface2) ||
        com_guid_eq(riid, IID_IDirectDrawSurface4) || com_guid_eq(riid, IID_IDirectDrawSurface7) ||
        com_guid_eq(riid, IID_IUnknown)) {
        S(ARG(0))->refs++;
        g_wr32(out, ARG(0));
        RET(3, S_OK);
    }
    char g[64];
    com_guid_str(riid, g, sizeof g);
    rt_log("[ddraw] surface QueryInterface %s: no\n", g);
    g_wr32(out, 0);
    RET(3, E_NOINTERFACE);
}
HLE(surf_AddRef) { RET(1, ++S(ARG(0))->refs); }
HLE(surf_Release)
{
    Surf *s = S(ARG(0));
    if (!s) RET(1, 0);
    int r = --s->refs;
    if (r <= 0) {
        surf_free(s);
        r = 0;
    }
    RET(1, (uint32_t)r);
}
HLE(surf_AddAttachedSurface) { RET(2, DD_OK); }
uint32_t st_lock_target, st_blt_from_target, st_blt_to_target, st_lock_tex, st_blt_to_tex, st_fill_target;
static void count_blt(Surf *d, Surf *s, int fill)
{
    if (fill && d->d3d_target) st_fill_target++;
    if (s && s->d3d_target && d != primary) st_blt_from_target++;
    if (!fill && d->d3d_target) st_blt_to_target++;
    if (d->d3d_texture) st_blt_to_tex++;
}
HLE(surf_Blt)
{
    Surf *d = S(ARG(0)), *s = S(ARG(2));
    uint32_t flags = ARG(4), fx = ARG(5);
    if (rt_verbose) count_blt(d, s, (flags & DDBLT_COLORFILL) != 0);
    Rc dr = read_rect(ARG(1), d);
    if (flags & DDBLT_COLORFILL) {
        fill(d, dr, g_rd32(fx + 80));
    } else if (s) {
        Rc sr = read_rect(ARG(3), s);
        int key = 0;
        uint32_t lo = 0, hi = 0;
        /* widescreen: the game's own scale of its draw surface onto the 1280x720 display (a 960x720
         * pillarbox) takes the whole wide surface onto the whole display */
        if (s->ox && primary && (d == primary || d == primary->back)) {
            sr = (Rc){0, 0, (int32_t)s->w, (int32_t)s->h};
            dr = (Rc){0, 0, (int32_t)d->w, (int32_t)d->h};
            extern void rt_wide_keep_strips(Surf *s);
            rt_wide_keep_strips(s); /* a finished frame: the menu's strips, if it drew */
        }
        if (flags & DDBLT_KEYSRCOVERRIDE) { key = 1; lo = g_rd32(fx + 92); hi = g_rd32(fx + 96); }
        else if ((flags & DDBLT_KEYSRC) && s->has_ck) { key = 1; lo = s->ck_lo; hi = s->ck_hi; }
        blit(d, dr, s, sr, key, lo & 0xffffff, hi & 0xffffff);
    }
    if (d == primary) {
        if (s && s != primary && !(flags & DDBLT_COLORFILL)) rt_frame_mark();
        present_primary();
    }
    RET(6, DD_OK);
}
HLE(surf_BltFast)
{
    Surf *d = S(ARG(0)), *s = S(ARG(3));
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    if (!s) RET(6, DDERR_INVALIDPARAMS);
    Rc sr = read_rect(ARG(4), s);
    Rc dr = {x + d->ox, y, x + d->ox + (sr.r - sr.l), y + (sr.b - sr.t)};
    int key = (ARG(5) & DDBLTFAST_SRCCOLORKEY) && s->has_ck;
    if (rt_verbose) count_blt(d, s, 0);
    blit(d, dr, s, sr, key, s->ck_lo & 0xffffff, s->ck_hi & 0xffffff);
    if (d == primary) present_primary();
    RET(6, DD_OK);
}
/* the host's per-frame overlay pieces: the frame just drawn is the one to show (overlay.c) */
__attribute__((weak)) void rt_frame_end(void) {}
HLE(surf_Flip)
{
    rt_frame_end();
    Surf *s = S(ARG(0));
    if (s->back) {
        Surf *b = s->back;
        uint32_t t = s->bits;
        s->bits = b->bits;
        b->bits = t;
        GpuTex *g = s->gtex;
        s->gtex = b->gtex;
        b->gtex = g;
        int c = s->cpu_ok, k = s->gpu_ok;
        s->cpu_ok = b->cpu_ok;
        s->gpu_ok = b->gpu_ok;
        b->cpu_ok = c;
        b->gpu_ok = k;
    }
    if (s == primary) {
        rt_frame_mark();
        present_primary();
    }
    RET(3, DD_OK);
}
HLE(surf_GetAttachedSurface)
{
    Surf *s = S(ARG(0));
    if (!s->back) RET(3, DDERR_NOTFOUND);
    s->back->refs++;
    g_wr32(ARG(2), s->back->obj);
    RET(3, DD_OK);
}
HLE(surf_GetBltStatus) { RET(2, DD_OK); }
HLE(surf_GetFlipStatus) { RET(2, DD_OK); }
HLE(surf_GetCaps)
{
    g_wr32(ARG(1), S(ARG(0))->caps);
    RET(2, DD_OK);
}
HLE(surf_GetClipper)
{
    Surf *s = S(ARG(0));
    g_wr32(ARG(1), s->clipper);
    RET(2, s->clipper ? DD_OK : 0x887600ccu); /* DDERR_NOCLIPPERATTACHED */
}
HLE(surf_SetClipper)
{
    S(ARG(0))->clipper = ARG(1);
    RET(2, DD_OK);
}
HLE(surf_GetColorKey)
{
    Surf *s = S(ARG(0));
    if (!s->has_ck) RET(3, 0x887600d6u); /* DDERR_NOCOLORKEY */
    g_wr32(ARG(2), s->ck_lo);
    g_wr32(ARG(2) + 4, s->ck_hi);
    RET(3, DD_OK);
}
HLE(surf_SetColorKey)
{
    Surf *s = S(ARG(0));
    if (ARG(1) & DDCKEY_SRCBLT) {
        if (ARG(2)) {
            s->has_ck = 1;
            s->ck_lo = g_rd32(ARG(2));
            s->ck_hi = g_rd32(ARG(2) + 4);
        } else {
            s->has_ck = 0;
        }
    }
    RET(3, DD_OK);
}
HLE(surf_GetDC)
{
    g_wr32(ARG(1), 0x00040001u);
    RET(2, DD_OK);
}
HLE(surf_ReleaseDC) { RET(2, DD_OK); }
HLE(surf_GetPixelFormat)
{
    put_pixfmt_a(ARG(1), S(ARG(0))->alpha);
    RET(2, DD_OK);
}
HLE(surf_GetSurfaceDesc)
{
    put_desc(S(ARG(0)), ARG(1), 0);
    RET(2, DD_OK);
}
HLE(surf_IsLost) { RET(1, DD_OK); }
HLE(surf_Restore) { RET(1, DD_OK); }
HLE(surf_Lock)
{
    Surf *s = S(ARG(0));
    uint32_t off = 0;
    if (rt_verbose) {
        if (s->d3d_target && s != primary) st_lock_target++;
        if (s->d3d_texture) st_lock_tex++;
    }
    /* DDInterface locks its draw surface read-only every frame and unlocks it straight away, only to
     * wait for the 3D hardware (0x00a2d470); it reads nothing, so no download for that one. */
    int flush_idiom = (ARG(3) & DDLOCK_READONLY) && g_rd32(c->esp) == 0x00a2d623u;
    if (!flush_idiom) surf_cpu(s);
    if (!(ARG(3) & DDLOCK_READONLY)) s->gpu_ok = 0; /* the CPU may write until Unlock */
    if (ARG(1)) {
        Rc r = read_rect(ARG(1), s);
        off = (uint32_t)r.t * s->pitch + (uint32_t)r.l * 4;
    } else {
        off = (uint32_t)s->ox * 4;
    }
    put_desc(s, ARG(2), s->bits + off);
    RET(5, DD_OK);
}
HLE(surf_Unlock)
{
    if (S(ARG(0)) == primary) present_primary();
    RET(2, DD_OK);
}
HLE(surf_GetDDInterface)
{
    g_wr32(ARG(1), dd7_obj);
    RET(2, DD_OK);
}
HLE(surf_PageLock) { RET(2, DD_OK); }
HLE(surf_PageUnlock) { RET(2, DD_OK); }
static uint32_t uniq = 1;
HLE(surf_GetUniquenessValue)
{
    g_wr32(ARG(1), uniq);
    RET(2, DD_OK);
}
HLE(surf_ChangeUniquenessValue)
{
    uniq++;
    RET(1, DD_OK);
}

/* ---------------- IDirectDrawClipper ---------------- */
HLE(clip_QueryInterface) { RET(3, E_NOINTERFACE); }
HLE(clip_AddRef) { RET(1, 1); }
HLE(clip_Release) { RET(1, 0); }
HLE(clip_SetHWnd)
{
    Clip *k = com_impl(ARG(0));
    k->hwnd = ARG(2);
    RET(3, DD_OK);
}
HLE(clip_GetHWnd)
{
    Clip *k = com_impl(ARG(0));
    g_wr32(ARG(1), k->hwnd);
    RET(2, DD_OK);
}
HLE(clip_IsClipListChanged)
{
    g_wr32(ARG(1), 0);
    RET(2, DD_OK);
}

/* ---------------- IDirectDraw7 / IDirectDraw ---------------- */
HLE(dd_QueryInterface)
{
    uint32_t riid = ARG(1), out = ARG(2);
    if (com_guid_eq(riid, IID_IDirectDraw7) || com_guid_eq(riid, IID_IDirectDraw4)) {
        g_wr32(out, dd7_obj);
        RET(3, S_OK);
    }
    if (com_guid_eq(riid, IID_IDirectDraw) || com_guid_eq(riid, IID_IDirectDraw2) || com_guid_eq(riid, IID_IUnknown)) {
        g_wr32(out, dd1_obj);
        RET(3, S_OK);
    }
    if (com_guid_eq(riid, IID_IDirect3D7)) {
        rt_log("[ddraw] QueryInterface IDirect3D7\n");
        g_wr32(out, d3d7_obj);
        RET(3, S_OK);
    }
    char g[64];
    com_guid_str(riid, g, sizeof g);
    rt_log("[ddraw] QueryInterface %s: not offered (no Direct3D)\n", g);
    g_wr32(out, 0);
    RET(3, E_NOINTERFACE);
}
HLE(dd_AddRef) { RET(1, 2); }
HLE(dd_Release) { RET(1, 1); } /* the DirectDraw object lives for the whole run */
HLE(dd_Compact) { RET(1, DD_OK); }
HLE(dd_CreateClipper)
{
    Clip *k = calloc(1, sizeof *k);
    g_wr32(ARG(2), com_wrap(vt_clip, k));
    RET(4, DD_OK);
}
HLE(dd_CreateSurface)
{
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = g_rd32(d + 4), caps = (flags & DDSD_CAPS) ? g_rd32(d + 104) : DDSCAPS_OFFSCREENPLAIN;
    uint32_t w = (flags & DDSD_WIDTH) ? g_rd32(d + 12) : 0, h = (flags & DDSD_HEIGHT) ? g_rd32(d + 8) : 0;
    Surf *s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s = surf_new(disp_w, disp_h, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY);
        s->primary = 1;
        primary = s;
        if ((caps & DDSCAPS_FLIP) && (flags & DDSD_BACKBUFFERCOUNT) && g_rd32(d + 20)) {
            s->back = surf_new(disp_w, disp_h, DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY);
        }
        rt_log("[ddraw] primary surface %ux%u%s\n", (unsigned)disp_w, (unsigned)disp_h, s->back ? " + back buffer" : "");
    } else {
        if (!w || !h) RET(4, DDERR_INVALIDPARAMS);
        if ((flags & DDSD_PIXELFORMAT) && g_rd32(d + 72 + 12) != 32)
            rt_log("[ddraw] %ux%u surface asked for %u bpp; giving 32\n", (unsigned)w, (unsigned)h, (unsigned)g_rd32(d + 72 + 12));
        static int wide_made;
        if (rt_widescreen && !wide_made && w == 800 && h == 600 && (caps & 0x2000u)) { /* DDSCAPS_3DDEVICE: the draw surface */
            wide_made = 1;
            s = surf_new(WIDE_W, h, caps);
            s->ox = WIDE_OX;
            s->vw = w;
            rt_log("[ddraw] widescreen: draw surface %ux%u, the game's 800x600 at x %d\n", (unsigned)WIDE_W, (unsigned)h, WIDE_OX);
        } else {
            s = surf_new(w, h, caps);
        }
        if ((flags & DDSD_PIXELFORMAT) && (g_rd32(d + 72 + 4) & 1)) s->alpha = 1;
        if (rt_verbose) rt_log("[ddraw] surface %ux%u caps %x\n", (unsigned)w, (unsigned)h, (unsigned)caps);
    }
    if (flags & DDSD_CKSRCBLT) {
        s->has_ck = 1;
        s->ck_lo = g_rd32(d + 64);
        s->ck_hi = g_rd32(d + 68);
    }
    g_wr32(out, s->obj);
    RET(4, DD_OK);
}
static uint32_t mode_list[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}};
HLE(dd_EnumDisplayModes)
{
    uint32_t ctx = ARG(3), cb = ARG(4);
    uint32_t d = rt_heap_alloc(124, 1);
    for (int i = 0; i < 4; i++) {
        Surf tmp = {0};
        tmp.w = mode_list[i][0];
        tmp.h = mode_list[i][1];
        tmp.pitch = tmp.w * 4;
        g_wr32(d, 124);
        put_desc(&tmp, d, 0);
        g_wr32(d + 24, 60);
        uint32_t args[2] = {d, ctx};
        if (!rt_guest_call(c, cb, 2, args)) break;
    }
    rt_heap_free(d);
    RET(5, DD_OK);
}
HLE(dd_FlipToGDISurface) { RET(1, DD_OK); }
HLE(dd_GetCaps)
{
    for (int k = 1; k <= 2; k++) {
        uint32_t p = ARG(k);
        if (!p) continue;
        uint32_t size = g_rd32(p);
        if (size < 8 || size > 1024) size = 380;
        memset(G2H(p + 4), 0, size - 4);
        g_wr32(p + 4, 0x40u | 0x200u | 0x400000u | 0x4000000u); /* BLT, BLTSTRETCH, COLORKEY, BLTCOLORFILL */
    }
    RET(3, DD_OK);
}
HLE(dd_GetDisplayMode)
{
    Surf tmp = {0};
    tmp.w = disp_w;
    tmp.h = disp_h;
    tmp.pitch = disp_w * 4;
    put_desc(&tmp, ARG(1), 0);
    g_wr32(ARG(1) + 24, 60);
    RET(2, DD_OK);
}
HLE(dd_GetMonitorFrequency)
{
    g_wr32(ARG(1), 60);
    RET(2, DD_OK);
}
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}
/* No scan line: SexyApp's fullscreen Redraw then stops splitting its blit into two halves timed
 * against the beam, and uses WaitForVerticalBlank + one whole-frame blit. */
#define DDERR_UNSUPPORTED 0x80004001u
HLE(dd_GetScanLine) { RET(2, DDERR_UNSUPPORTED); }
HLE(dd_GetVerticalBlankStatus)
{
    g_wr32(ARG(1), now_us() % 16667 > 15500);
    RET(2, DD_OK);
}
HLE(dd_RestoreDisplayMode) { RET(1, DD_OK); }
HLE(dd_SetCooperativeLevel)
{
    fullscreen = (ARG(2) & 0x1) != 0; /* DDSCL_FULLSCREEN */
    rt_log("[ddraw] SetCooperativeLevel(%s)\n", fullscreen ? "fullscreen" : "windowed");
    RET(3, DD_OK);
}
static void set_mode(uint32_t w, uint32_t h, uint32_t bpp)
{
    rt_log("[ddraw] SetDisplayMode(%u x %u x %u)\n", (unsigned)w, (unsigned)h, (unsigned)bpp);
    disp_w = w;
    disp_h = h;
}
HLE(dd1_SetDisplayMode)
{
    set_mode(ARG(1), ARG(2), ARG(3));
    RET(4, DD_OK);
}
HLE(dd7_SetDisplayMode)
{
    set_mode(ARG(1), ARG(2), ARG(3));
    RET(6, DD_OK);
}
/* The platform presents the last frame on the display's real vsync, when it can (host window) */
int rt_wait_vblank(void) __attribute__((weak));
int rt_wait_vblank(void) { return 0; }
HLE(dd_WaitForVerticalBlank)
{
    uint64_t t0 = now_us();
    if (!rt_wait_vblank()) {
        uint64_t t = now_us(), next = (t / 16667 + 1) * 16667;
        usleep((useconds_t)(next - t));
    }
    rt_stat_vblank_us += now_us() - t0;
    RET(3, DD_OK);
}
HLE(dd_GetAvailableVidMem)
{
    if (ARG(2)) g_wr32(ARG(2), 256u << 20);
    if (ARG(3)) g_wr32(ARG(3), 256u << 20);
    RET(4, DD_OK);
}
HLE(dd_TestCooperativeLevel) { RET(1, DD_OK); }
HLE(dd_RestoreAllSurfaces) { RET(1, DD_OK); }
HLE(dd_GetDeviceIdentifier)
{
    memset(G2H(ARG(1)), 0, 1072);
    rt_u2a("PvZ DLC Switch software renderer", ARG(1) + 512, 512);
    RET(3, DD_OK);
}

static void init_vtables(void)
{
    if (vt_dd7) return;
#define M(n, f) {n, f}
    static const ComMethod dd[30] = {
        M("QueryInterface", dd_QueryInterface), M("AddRef", dd_AddRef), M("Release", dd_Release),
        M("Compact", dd_Compact), M("CreateClipper", dd_CreateClipper), M("CreatePalette", NULL),
        M("CreateSurface", dd_CreateSurface), M("DuplicateSurface", NULL), M("EnumDisplayModes", dd_EnumDisplayModes),
        M("EnumSurfaces", NULL), M("FlipToGDISurface", dd_FlipToGDISurface), M("GetCaps", dd_GetCaps),
        M("GetDisplayMode", dd_GetDisplayMode), M("GetFourCCCodes", NULL), M("GetGDISurface", NULL),
        M("GetMonitorFrequency", dd_GetMonitorFrequency), M("GetScanLine", dd_GetScanLine),
        M("GetVerticalBlankStatus", dd_GetVerticalBlankStatus), M("Initialize", NULL),
        M("RestoreDisplayMode", dd_RestoreDisplayMode), M("SetCooperativeLevel", dd_SetCooperativeLevel),
        M("SetDisplayMode", dd7_SetDisplayMode), M("WaitForVerticalBlank", dd_WaitForVerticalBlank),
        M("GetAvailableVidMem", dd_GetAvailableVidMem), M("GetSurfaceFromDC", NULL),
        M("RestoreAllSurfaces", dd_RestoreAllSurfaces), M("TestCooperativeLevel", dd_TestCooperativeLevel),
        M("GetDeviceIdentifier", dd_GetDeviceIdentifier), M("StartModeTest", NULL), M("EvaluateMode", NULL)};
    vt_dd7 = com_vtable("IDirectDraw7", dd, 30);
    ComMethod dd1[23];
    memcpy(dd1, dd, sizeof dd1);
    dd1[21].fn = dd1_SetDisplayMode;
    vt_dd1 = com_vtable("IDirectDraw", dd1, 23);
    static const ComMethod surf[49] = {
        M("QueryInterface", surf_QueryInterface), M("AddRef", surf_AddRef), M("Release", surf_Release),
        M("AddAttachedSurface", surf_AddAttachedSurface), M("AddOverlayDirtyRect", NULL), M("Blt", surf_Blt),
        M("BltBatch", NULL), M("BltFast", surf_BltFast), M("DeleteAttachedSurface", NULL),
        M("EnumAttachedSurfaces", NULL), M("EnumOverlayZOrders", NULL), M("Flip", surf_Flip),
        M("GetAttachedSurface", surf_GetAttachedSurface), M("GetBltStatus", surf_GetBltStatus),
        M("GetCaps", surf_GetCaps), M("GetClipper", surf_GetClipper), M("GetColorKey", surf_GetColorKey),
        M("GetDC", surf_GetDC), M("GetFlipStatus", surf_GetFlipStatus), M("GetOverlayPosition", NULL),
        M("GetPalette", NULL), M("GetPixelFormat", surf_GetPixelFormat), M("GetSurfaceDesc", surf_GetSurfaceDesc),
        M("Initialize", NULL), M("IsLost", surf_IsLost), M("Lock", surf_Lock), M("ReleaseDC", surf_ReleaseDC),
        M("Restore", surf_Restore), M("SetClipper", surf_SetClipper), M("SetColorKey", surf_SetColorKey),
        M("SetOverlayPosition", NULL), M("SetPalette", NULL), M("Unlock", surf_Unlock), M("UpdateOverlay", NULL),
        M("UpdateOverlayDisplay", NULL), M("UpdateOverlayZOrder", NULL), M("GetDDInterface", surf_GetDDInterface),
        M("PageLock", surf_PageLock), M("PageUnlock", surf_PageUnlock), M("SetSurfaceDesc", NULL),
        M("SetPrivateData", NULL), M("GetPrivateData", NULL), M("FreePrivateData", NULL),
        M("GetUniquenessValue", surf_GetUniquenessValue), M("ChangeUniquenessValue", surf_ChangeUniquenessValue),
        M("SetPriority", NULL), M("GetPriority", NULL), M("SetLOD", NULL), M("GetLOD", NULL)};
    vt_surf = com_vtable("IDirectDrawSurface7", surf, 49);
    static const ComMethod clip[9] = {
        M("QueryInterface", clip_QueryInterface), M("AddRef", clip_AddRef), M("Release", clip_Release),
        M("GetClipList", NULL), M("GetHWnd", clip_GetHWnd), M("Initialize", NULL),
        M("IsClipListChanged", clip_IsClipListChanged), M("SetClipList", NULL), M("SetHWnd", clip_SetHWnd)};
    vt_clip = com_vtable("IDirectDrawClipper", clip, 9);
#undef M
    static int impl;
    dd7_obj = com_wrap(vt_dd7, &impl);
    dd1_obj = com_wrap(vt_dd1, &impl);
    d3d7_obj = d3d7_create_object();
}

/* ---------------- ddraw.dll exports ---------------- */
/* The host starts its GPU backend here (on the game's main thread, which owns it) unless RT_SOFT=1 */
void rt_gpu_start(void) __attribute__((weak));
void rt_gpu_start(void) {}

HLE(DirectDrawCreateEx)
{
    init_vtables();
    rt_gpu_start();
    rt_log("[ddraw] DirectDrawCreateEx\n");
    g_wr32(ARG(1), dd7_obj);
    RET(4, DD_OK);
}
HLE(DirectDrawCreate)
{
    init_vtables();
    rt_gpu_start();
    rt_log("[ddraw] DirectDrawCreate\n");
    g_wr32(ARG(1), dd1_obj);
    RET(3, DD_OK);
}

const HleEntry hle_ddraw[] = {
    {"ddraw.dll", "DirectDrawCreateEx", DirectDrawCreateEx},
    {"ddraw.dll", "DirectDrawCreate", DirectDrawCreate},
    {NULL, NULL, NULL}};

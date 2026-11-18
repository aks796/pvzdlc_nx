/* hle_d3d7.c: IDirect3D7 + IDirect3DDevice7 with a software rasterizer.
 *
 * The game only draws pre-transformed (TLVERTEX-style) primitives with one
 * texture stage and alpha blending, so this rasterizes screen-space
 * triangles and lines straight into the 32-bit render-target surface.
 * The Switch build will route the same calls to the GPU. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle_ddraw.h"
#include "rt_com.h"

#define D3D_OK 0u

/* render states */
#define RS_TEXTUREFACTOR 60
#define RS_ALPHABLENDENABLE 27
#define RS_SRCBLEND 19
#define RS_DESTBLEND 20
#define RS_ALPHATESTENABLE 15
#define RS_ALPHAREF 24
#define RS_ALPHAFUNC 25
/* texture stage states */
#define TSS_COLOROP 1
#define TSS_COLORARG1 2
#define TSS_COLORARG2 3
#define TSS_ALPHAOP 4
#define TSS_ALPHAARG1 5
#define TSS_ALPHAARG2 6
#define TSS_ADDRESSU 13
#define TSS_ADDRESSV 14
#define TSS_MAGFILTER 16
#define TSS_MINFILTER 17

typedef struct {
    Surf *target;
    Surf *tex;
    uint32_t rs[256];
    uint32_t tss[32];
    int32_t vx, vy, vw, vh;
} Dev;

uint32_t vt_d3ddev;
static uint32_t vt_d3d7;

static Dev *D(uint32_t obj) { return com_impl(obj); }

/* ---------------- pixel pipeline ----------------
 * Render state is decoded into a Pipe once per draw call; pixels are integer
 * 0..255 with exact /255 rounding. The DLC draws every sprite as a textured
 * quad, so the common blends (normal alpha, additive, opaque) have their own
 * paths and fully transparent texels are skipped. */
typedef struct { int r, g, b, a; } Col; /* 0..255 */

typedef struct {
    float x, y, u, v;
    Col c;
} Vtx;

enum { BL_NONE, BL_ALPHA, BL_ADD, BL_GENERIC };

typedef struct {
    Surf *target;
    int textured, linear, talpha, tw, th, tmask_w, tmask_h;
    const uint8_t *tbits;
    uint32_t tpitch, au, av;
    uint32_t cop, ca1, ca2, aop, aa1, aa2;
    Col tf;
    int blend, dst_alpha;
    uint32_t sb, db;
    int atest, aref;
    uint32_t afn;
    int tex_alpha_mod; /* alpha = texture alpha * something, normal alpha blend: a=0 texels draw nothing */
} Pipe;

static inline Col unpack(uint32_t c)
{
    return (Col){(int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255), (int)(c >> 24)};
}
static inline uint32_t pack(Col c)
{
#define CL(v) ((uint32_t)((v) < 0 ? 0 : (v) > 255 ? 255 : (v)))
    return (CL(c.a) << 24) | (CL(c.r) << 16) | (CL(c.g) << 8) | CL(c.b);
#undef CL
}
/* a*b/255, rounded */
static inline int mul255(int a, int b)
{
    int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

static void pipe_setup(Dev *d, Pipe *p)
{
    memset(p, 0, sizeof *p);
    p->target = d->target;
    p->cop = d->tss[TSS_COLOROP];
    p->ca1 = d->tss[TSS_COLORARG1];
    p->ca2 = d->tss[TSS_COLORARG2];
    p->aop = d->tss[TSS_ALPHAOP];
    p->aa1 = d->tss[TSS_ALPHAARG1];
    p->aa2 = d->tss[TSS_ALPHAARG2];
    p->tf = unpack(d->rs[RS_TEXTUREFACTOR]);
    Surf *t = d->tex;
    p->textured = t && p->cop != 1;
    if (t) {
        p->tbits = G2H(t->bits);
        p->tpitch = t->pitch;
        p->tw = (int)t->w;
        p->th = (int)t->h;
        p->talpha = t->alpha;
        p->au = d->tss[TSS_ADDRESSU];
        p->av = d->tss[TSS_ADDRESSV];
        p->linear = d->tss[TSS_MAGFILTER] == 2 || d->tss[TSS_MINFILTER] == 2;
        /* power-of-two WRAP is a mask */
        p->tmask_w = (p->au == 1 && !(p->tw & (p->tw - 1))) ? p->tw - 1 : -1;
        p->tmask_h = (p->av == 1 && !(p->th & (p->th - 1))) ? p->th - 1 : -1;
    }
    p->dst_alpha = d->target->alpha;
    p->sb = d->rs[RS_SRCBLEND];
    p->db = d->rs[RS_DESTBLEND];
    if (!d->rs[RS_ALPHABLENDENABLE] || (p->sb == 2 && p->db == 1)) p->blend = BL_NONE; /* ONE, ZERO */
    else if (p->sb == 5 && p->db == 6) p->blend = BL_ALPHA;                            /* SRCALPHA, INVSRCALPHA */
    else if (p->sb == 5 && p->db == 2) p->blend = BL_ADD;                              /* SRCALPHA, ONE */
    else p->blend = BL_GENERIC;
    p->atest = d->rs[RS_ALPHATESTENABLE] != 0;
    p->aref = (int)(d->rs[RS_ALPHAREF] & 255);
    p->afn = d->rs[RS_ALPHAFUNC];
    p->tex_alpha_mod = p->textured && p->blend == BL_ALPHA &&
                       ((p->aop == 4 && ((p->aa1 & 0xf) == 2 || (p->aa2 & 0xf) == 2)) ||
                        (p->aop == 2 && (p->aa1 & 0xf) == 2) || (p->aop == 3 && (p->aa2 & 0xf) == 2));
}

static inline int wrap_coord(int v, int n, uint32_t mode)
{
    if (mode == 1) { /* WRAP */
        v %= n;
        return v < 0 ? v + n : v;
    }
    if (mode == 2) { /* MIRROR */
        int p = 2 * n;
        v %= p;
        if (v < 0) v += p;
        return v < n ? v : p - 1 - v;
    }
    return v < 0 ? 0 : v >= n ? n - 1 : v; /* CLAMP */
}
static inline int tx(const Pipe *p, int x) { return p->tmask_w >= 0 ? x & p->tmask_w : wrap_coord(x, p->tw, p->au); }
static inline int ty(const Pipe *p, int y) { return p->tmask_h >= 0 ? y & p->tmask_h : wrap_coord(y, p->th, p->av); }
static inline uint32_t texel(const Pipe *p, int x, int y)
{
    uint32_t c;
    memcpy(&c, p->tbits + (uint32_t)y * p->tpitch + (uint32_t)x * 4, 4);
    return p->talpha ? c : c | 0xff000000u;
}
static inline int ffloor(float f)
{
    int i = (int)f;
    return i - (f < (float)i);
}

static inline uint32_t sample(const Pipe *p, float u, float v)
{
    float fx = u * (float)p->tw, fy = v * (float)p->th;
    if (!p->linear) return texel(p, tx(p, ffloor(fx)), ty(p, ffloor(fy)));
    fx -= 0.5f;
    fy -= 0.5f;
    int x0 = ffloor(fx), y0 = ffloor(fy);
    int wx = (int)((fx - (float)x0) * 256.f), wy = (int)((fy - (float)y0) * 256.f);
    int x1 = tx(p, x0 + 1), y1 = ty(p, y0 + 1);
    x0 = tx(p, x0);
    y0 = ty(p, y0);
    uint32_t c00 = texel(p, x0, y0), c10 = texel(p, x1, y0), c01 = texel(p, x0, y1), c11 = texel(p, x1, y1);
    if (c00 == c10 && c00 == c01 && c00 == c11) return c00;
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        int a = (c00 >> s) & 255, b = (c10 >> s) & 255, c = (c01 >> s) & 255, e = (c11 >> s) & 255;
        int top = a * 256 + (b - a) * wx, bot = c * 256 + (e - c) * wx;
        out |= (uint32_t)((top * 256 + (bot - top) * wy + 32768) >> 16) << s;
    }
    return out;
}

static inline Col pick(uint32_t a, Col diff, Col tex, Col tf)
{
    switch (a & 0xf) {
    case 2: return tex;
    case 3: return tf;
    default: return diff; /* DIFFUSE, CURRENT */
    }
}
static inline int op(uint32_t o, int a1, int a2)
{
    int v;
    switch (o) {
    case 2: return a1;                                             /* SELECTARG1 */
    case 3: return a2;                                             /* SELECTARG2 */
    case 4: return mul255(a1, a2);                                 /* MODULATE */
    case 5: v = mul255(a1, a2) * 2; return v > 255 ? 255 : v;      /* MODULATE2X */
    case 6: v = mul255(a1, a2) * 4; return v > 255 ? 255 : v;      /* MODULATE4X */
    case 7: v = a1 + a2; return v > 255 ? 255 : v;                 /* ADD */
    case 8: v = a1 + a2 - 128; return v < 0 ? 0 : v > 255 ? 255 : v; /* ADDSIGNED */
    case 10: v = a1 - a2; return v < 0 ? 0 : v;                    /* SUBTRACT */
    default: return a1;
    }
}
static inline Col combine(const Pipe *p, Col diff, Col tex)
{
    Col out = diff;
    if (p->cop == 4 && (p->ca1 & 0xf) == 2 && (p->ca2 & 0xf) <= 1) { /* TEXTURE * DIFFUSE, the usual sprite setup */
        out.r = mul255(tex.r, diff.r);
        out.g = mul255(tex.g, diff.g);
        out.b = mul255(tex.b, diff.b);
    } else if (p->cop != 1) { /* DISABLE */
        Col a1 = pick(p->ca1, diff, tex, p->tf), a2 = pick(p->ca2, diff, tex, p->tf);
        out.r = op(p->cop, a1.r, a2.r);
        out.g = op(p->cop, a1.g, a2.g);
        out.b = op(p->cop, a1.b, a2.b);
    }
    if (p->aop != 1) {
        Col a1 = pick(p->aa1, diff, tex, p->tf), a2 = pick(p->aa2, diff, tex, p->tf);
        out.a = op(p->aop, a1.a, a2.a);
    }
    return out;
}

/* D3DBLEND factor for one channel, 0..255 */
static inline int factor(uint32_t f, int sc, int sa, int dc, int da)
{
    switch (f) {
    case 1: return 0;
    case 2: return 255;
    case 3: return sc;
    case 4: return 255 - sc;
    case 5: return sa;
    case 6: return 255 - sa;
    case 7: return da;
    case 8: return 255 - da;
    case 9: return dc;
    case 10: return 255 - dc;
    case 11: return sa < 255 - da ? sa : 255 - da;
    default: return 255;
    }
}

static inline void shade(const Pipe *p, uint32_t *px, Col diff, float u, float v)
{
    Col tex = {255, 255, 255, 255};
    if (p->textured) {
        uint32_t t = sample(p, u, v);
        /* transparent texel under a modulated alpha blend: nothing to draw */
        if (!(t >> 24) && p->tex_alpha_mod) return;
        tex = unpack(t);
    }
    Col c = combine(p, diff, tex);
    if (p->atest) {
        int a = c.a, r = p->aref, pass;
        switch (p->afn) {
        case 1: pass = 0; break;
        case 2: pass = a < r; break;
        case 3: pass = a == r; break;
        case 4: pass = a <= r; break;
        case 5: pass = a > r; break;
        case 6: pass = a != r; break;
        case 7: pass = a >= r; break;
        default: pass = 1;
        }
        if (!pass) return;
    }
    switch (p->blend) {
    case BL_NONE:
        *px = pack(c);
        return;
    case BL_ALPHA: {
        int a = c.a < 0 ? 0 : c.a > 255 ? 255 : c.a;
        if (!a) return;
        if (a == 255) {
            *px = pack(c);
            return;
        }
        Col dst = unpack(*px);
        if (!p->dst_alpha) dst.a = 255;
        int ia = 255 - a;
        dst.r = mul255(c.r, a) + mul255(dst.r, ia);
        dst.g = mul255(c.g, a) + mul255(dst.g, ia);
        dst.b = mul255(c.b, a) + mul255(dst.b, ia);
        dst.a = mul255(a, a) + mul255(dst.a, ia);
        *px = pack(dst);
        return;
    }
    case BL_ADD: {
        int a = c.a < 0 ? 0 : c.a > 255 ? 255 : c.a;
        if (!a) return;
        Col dst = unpack(*px);
        if (!p->dst_alpha) dst.a = 255;
        dst.r += mul255(c.r, a);
        dst.g += mul255(c.g, a);
        dst.b += mul255(c.b, a);
        dst.a += mul255(a, a);
        *px = pack(dst);
        return;
    }
    default: {
        Col dst = unpack(*px);
        if (!p->dst_alpha) dst.a = 255;
        Col o;
        o.r = mul255(c.r, factor(p->sb, c.r, c.a, dst.r, dst.a)) + mul255(dst.r, factor(p->db, c.r, c.a, dst.r, dst.a));
        o.g = mul255(c.g, factor(p->sb, c.g, c.a, dst.g, dst.a)) + mul255(dst.g, factor(p->db, c.g, c.a, dst.g, dst.a));
        o.b = mul255(c.b, factor(p->sb, c.b, c.a, dst.b, dst.a)) + mul255(dst.b, factor(p->db, c.b, c.a, dst.b, dst.a));
        o.a = mul255(c.a, factor(p->sb, c.a, c.a, dst.a, dst.a)) + mul255(dst.a, factor(p->db, c.a, c.a, dst.a, dst.a));
        *px = pack(o);
    }
    }
}

/* -v statistics: covered pixels per pipe kind, logged every 600 draws of the primary-sized target */
static uint64_t st_px[4][2][2], st_tris, st_draws;

/* the viewport on the target: on the widescreen draw surface the game's full viewport is the whole
 * surface, any other moves in by ox */
static void view(Dev *d, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    Surf *s = d->target;
    *x0 = d->vx, *y0 = d->vy, *x1 = d->vx + d->vw, *y1 = d->vy + d->vh;
    if (!s->ox) return;
    if (d->vx == 0 && d->vy == 0 && d->vw == (int32_t)s->vw && d->vh == (int32_t)s->h) {
        *x1 = (int32_t)s->w;
        return;
    }
    *x0 += s->ox, *x1 += s->ox;
}

static void clip_rect(Dev *d, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    int32_t l, t, r, b;
    view(d, &l, &t, &r, &b);
    if (r > (int32_t)d->target->w) r = (int32_t)d->target->w;
    if (b > (int32_t)d->target->h) b = (int32_t)d->target->h;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (*x0 < l) *x0 = l;
    if (*y0 < t) *y0 = t;
    if (*x1 > r) *x1 = r;
    if (*y1 > b) *y1 = b;
}

/* The exact coverage test (edge functions + top-left rule) */
static inline int inside(const Vtx *a, const Vtx *b, const Vtx *c, int tl_a, int tl_b, int tl_c, float px, float py)
{
    float wa = ((c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x));
    float wb = ((a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x));
    float wc = ((b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x));
    if (wa < 0 || wb < 0 || wc < 0) return 0;
    return !((wa == 0 && !tl_a) || (wb == 0 && !tl_b) || (wc == 0 && !tl_c));
}

/* The usual sprite: texture (point or bilinear) times one flat vertex colour, normal alpha blend, no
 * alpha test. Same result as shade(), without its general combine/blend. */
static inline int sprite_pipe(const Pipe *p)
{
    return p->textured && p->blend == BL_ALPHA && !p->atest && p->cop == 4 &&
           (p->ca1 & 0xf) == 2 && (p->ca2 & 0xf) <= 1 && p->aop == 4 &&
           (((p->aa1 & 0xf) == 2 && (p->aa2 & 0xf) <= 1) || ((p->aa2 & 0xf) == 2 && (p->aa1 & 0xf) <= 1));
}
/* lerp two ARGB pixels by w/256, two channels per multiply */
static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t w)
{
    uint32_t rb = ((a & 0xff00ffu) * (256 - w) + (b & 0xff00ffu) * w) >> 8;
    uint32_t ag = (((a >> 8) & 0xff00ffu) * (256 - w) + ((b >> 8) & 0xff00ffu) * w);
    return (rb & 0xff00ffu) | (ag & 0xff00ff00u);
}
/* Bilinear texel at 16.16 fixed-point texel coordinates (already offset by half a texel) */
static inline uint32_t sample_fx(const Pipe *p, int32_t fu, int32_t fv)
{
    int x0 = fu >> 16, y0 = fv >> 16;
    uint32_t wx = ((uint32_t)fu >> 8) & 0xff, wy = ((uint32_t)fv >> 8) & 0xff;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0 || x1 >= p->tw) { x1 = tx(p, x1); x0 = tx(p, x0); }
    if (y0 < 0 || y1 >= p->th) { y1 = ty(p, y1); y0 = ty(p, y0); }
    const uint8_t *r0 = p->tbits + (uint32_t)y0 * p->tpitch, *r1 = p->tbits + (uint32_t)y1 * p->tpitch;
    uint32_t c00, c10, c01, c11;
    memcpy(&c00, r0 + (uint32_t)x0 * 4, 4);
    memcpy(&c10, r0 + (uint32_t)x1 * 4, 4);
    memcpy(&c01, r1 + (uint32_t)x0 * 4, 4);
    memcpy(&c11, r1 + (uint32_t)x1 * 4, 4);
    if (!p->talpha) {
        c00 |= 0xff000000u;
        c10 |= 0xff000000u;
        c01 |= 0xff000000u;
        c11 |= 0xff000000u;
    }
    return lerp_px(lerp_px(c00, c10, wx), lerp_px(c01, c11, wx), wy);
}

static void sprite_span(const Pipe *p, uint32_t *row, int32_t x, int32_t xe, float u, float v, float du, float dv,
                        Col k)
{
    int white = k.r == 255 && k.g == 255 && k.b == 255;
    float fw = (float)p->tw, fh = (float)p->th;
    /* bilinear: 16.16 texel coordinates stepped per pixel */
    int32_t fu = 0, fv = 0, dfu = 0, dfv = 0;
    if (p->linear) {
        fu = (int32_t)((u * fw - 0.5f) * 65536.f);
        fv = (int32_t)((v * fh - 0.5f) * 65536.f);
        dfu = (int32_t)(du * fw * 65536.f);
        dfv = (int32_t)(dv * fh * 65536.f);
    }
    for (; x < xe; x++, u += du, v += dv, fu += dfu, fv += dfv) {
        uint32_t t = p->linear ? sample_fx(p, fu, fv) : texel(p, tx(p, ffloor(u * fw)), ty(p, ffloor(v * fh)));
        int a = (int)(t >> 24);
        if (k.a != 255) a = mul255(a, k.a);
        if (!a) continue;
        int r = (int)((t >> 16) & 255), g = (int)((t >> 8) & 255), b = (int)(t & 255);
        if (!white) {
            r = mul255(r, k.r);
            g = mul255(g, k.g);
            b = mul255(b, k.b);
        }
        if (a == 255) {
            row[x] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            continue;
        }
        uint32_t dpx = row[x];
        int ia = 255 - a;
        int da = p->dst_alpha ? (int)(dpx >> 24) : 255;
        r = mul255(r, a) + mul255((int)((dpx >> 16) & 255), ia);
        g = mul255(g, a) + mul255((int)((dpx >> 8) & 255), ia);
        b = mul255(b, a) + mul255((int)(dpx & 255), ia);
        da = mul255(a, a) + mul255(da, ia);
        row[x] = ((uint32_t)(da > 255 ? 255 : da) << 24) | ((uint32_t)(r > 255 ? 255 : r) << 16) |
                 ((uint32_t)(g > 255 ? 255 : g) << 8) | (uint32_t)(b > 255 ? 255 : b);
    }
}

/* Edge-function rasterizer, D3D pixel centres at integer coordinates, top-left rule. Each row's span
 * comes from the edge equations, then is nudged until it matches the exact per-pixel test (quads
 * share a diagonal and must not blend it twice); attributes step along x from per-triangle gradients.
 * Big triangles split their rows between the worker threads. */
typedef struct {
    const Pipe *p;
    const Vtx *a, *b, *c;
    int tl_a, tl_b, tl_c, flat, sprite;
    int32_t x0, x1;
    float inv, du, dv, dr, dg, db, da;
} TriJob;

#define EDGE(v0, v1, px, py) (((v1)->x - (v0)->x) * ((py) - (v0)->y) - ((v1)->y - (v0)->y) * ((px) - (v0)->x))

static void tri_rows(void *ctx, int y0, int y1)
{
    const TriJob *j = ctx;
    const Pipe *p = j->p;
    const Vtx *a = j->a, *b = j->b, *c = j->c;
    int32_t x0 = j->x0, x1 = j->x1;
    const Vtx *ev[3][2] = {{b, c}, {c, a}, {a, b}};
    uint64_t covered = 0;
    #define INSIDE(px, py) inside(a, b, c, j->tl_a, j->tl_b, j->tl_c, (float)(px), py)
    for (int32_t y = y0; y < y1; y++) {
        float py = (float)y;
        /* analytic span: each edge bounds x from one side */
        float lo = (float)x0, hi = (float)(x1 - 1);
        int empty = 0;
        for (int k = 0; k < 3; k++) {
            const Vtx *v0 = ev[k][0], *v1 = ev[k][1];
            float dy = v1->y - v0->y, cc = (v1->x - v0->x) * (py - v0->y) + dy * v0->x;
            if (dy > 0) hi = fminf(hi, cc / dy);
            else if (dy < 0) lo = fmaxf(lo, cc / dy);
            else if (cc < 0) empty = 1;
        }
        if (empty || lo > hi + 1.f) continue;
        int32_t xs = (int32_t)ceilf(lo), xe = (int32_t)floorf(hi);
        if (xs < x0) xs = x0;
        if (xe > x1 - 1) xe = x1 - 1;
        /* match the exact test at both ends */
        while (xs <= xe && !INSIDE(xs, py)) xs++;
        while (xs > x0 && INSIDE(xs - 1, py)) xs--;
        while (xe >= xs && !INSIDE(xe, py)) xe--;
        while (xe < x1 - 1 && xe >= xs && INSIDE(xe + 1, py)) xe++;
        if (xs > xe) continue;
        float inv = j->inv;
        float wa = EDGE(b, c, (float)xs, py) * inv, wb = EDGE(c, a, (float)xs, py) * inv, wc = EDGE(a, b, (float)xs, py) * inv;
        float u = a->u * wa + b->u * wb + c->u * wc, v = a->v * wa + b->v * wb + c->v * wc;
        uint32_t *row = (uint32_t *)G2H(p->target->bits + (uint32_t)y * p->target->pitch);
        covered += (uint64_t)(xe + 1 - xs);
        if (j->sprite) {
            sprite_span(p, row, xs, xe + 1, u, v, j->du, j->dv, a->c);
            continue;
        }
        float r = a->c.r * wa + b->c.r * wb + c->c.r * wc, g = a->c.g * wa + b->c.g * wb + c->c.g * wc;
        float bl = a->c.b * wa + b->c.b * wb + c->c.b * wc, al = a->c.a * wa + b->c.a * wb + c->c.a * wc;
        for (int32_t x = xs; x <= xe; x++) {
            Col diff = j->flat ? a->c : (Col){(int)(r + 0.5f), (int)(g + 0.5f), (int)(bl + 0.5f), (int)(al + 0.5f)};
            shade(p, &row[x], diff, u, v);
            u += j->du;
            v += j->dv;
            if (!j->flat) {
                r += j->dr;
                g += j->dg;
                bl += j->db;
                al += j->da;
            }
        }
    }
    #undef INSIDE
    if (rt_verbose) __atomic_fetch_add(&st_px[p->blend][p->textured][p->linear], covered, __ATOMIC_RELAXED);
}

static void tri(Dev *d, const Pipe *p, const Vtx *a, const Vtx *b, const Vtx *c)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-6f) return;
    if (area < 0) { const Vtx *t = b; b = c; c = t; area = -area; }
    int32_t x0 = (int32_t)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int32_t)ceilf(fmaxf(a->x, fmaxf(b->x, c->x))) + 1;
    int32_t y0 = (int32_t)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int32_t)ceilf(fmaxf(a->y, fmaxf(b->y, c->y))) + 1;
    clip_rect(d, &x0, &y0, &x1, &y1);
    if (x0 >= x1 || y0 >= y1) return;
    TriJob j;
    j.p = p;
    j.a = a;
    j.b = b;
    j.c = c;
    j.x0 = x0;
    j.x1 = x1;
    j.inv = 1.f / area;
    #define TOPLEFT(v0, v1) (((v1)->y == (v0)->y && (v1)->x < (v0)->x) || (v1)->y < (v0)->y)
    j.tl_a = TOPLEFT(b, c);
    j.tl_b = TOPLEFT(c, a);
    j.tl_c = TOPLEFT(a, b);
    #undef TOPLEFT
    /* d(weight)/dx for each vertex's weight */
    float dwa = -(c->y - b->y) * j.inv, dwb = -(a->y - c->y) * j.inv, dwc = -(b->y - a->y) * j.inv;
    #define DX(f) (a->f * dwa + b->f * dwb + c->f * dwc)
    j.du = DX(u);
    j.dv = DX(v);
    j.dr = DX(c.r);
    j.dg = DX(c.g);
    j.db = DX(c.b);
    j.da = DX(c.a);
    #undef DX
    j.flat = a->c.r == b->c.r && a->c.r == c->c.r && a->c.g == b->c.g && a->c.g == c->c.g &&
             a->c.b == b->c.b && a->c.b == c->c.b && a->c.a == b->c.a && a->c.a == c->c.a;
    j.sprite = j.flat && sprite_pipe(p);
    /* about a 128x128 sprite's worth of pixels before threads pay off */
    if ((int64_t)(x1 - x0) * (y1 - y0) >= 32768) rt_parallel_rows(y0, y1, tri_rows, &j);
    else tri_rows(&j, y0, y1);
}
#undef EDGE

static void line(Dev *d, const Pipe *p, const Vtx *a, const Vtx *b)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy));
    if (n < 1) n = 1;
    Pipe q = *p;
    q.textured = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        int32_t x = (int32_t)floorf(a->x + dx * t + 0.5f), y = (int32_t)floorf(a->y + dy * t + 0.5f);
        int32_t x0 = x, y0 = y, x1 = x + 1, y1 = y + 1;
        clip_rect(d, &x0, &y0, &x1, &y1);
        if (x0 >= x1 || y0 >= y1) continue;
        Col c = {(int)(a->c.r + (b->c.r - a->c.r) * t), (int)(a->c.g + (b->c.g - a->c.g) * t),
                 (int)(a->c.b + (b->c.b - a->c.b) * t), (int)(a->c.a + (b->c.a - a->c.a) * t)};
        shade(&q, (uint32_t *)G2H(p->target->bits + (uint32_t)y * p->target->pitch + (uint32_t)x * 4), c, 0, 0);
    }
}

/* Read a vertex in the given FVF (needs XYZRHW or XYZ; diffuse optional; tex0 optional) */
static int fvf_layout(uint32_t fvf, uint32_t *stride, int *diff_off, int *tex_off)
{
    uint32_t pos = fvf & 0xe;
    uint32_t off = pos == 0x4 ? 16 : pos == 0x2 ? 12 : 0;
    if (!off) return 0;
    if (fvf & 0x10) off += 12; /* NORMAL */
    if (fvf & 0x20) off += 4;  /* RESERVED1 */
    *diff_off = (fvf & 0x40) ? (int)off : -1;
    if (fvf & 0x40) off += 4;
    if (fvf & 0x80) off += 4;  /* SPECULAR */
    uint32_t ntex = (fvf >> 8) & 0xf;
    *tex_off = ntex ? (int)off : -1;
    off += 8 * ntex;
    *stride = off;
    return 1;
}

static float vtx_ox; /* the target's widescreen offset, for this draw */
static Vtx read_vtx(uint32_t p, int diff_off, int tex_off)
{
    Vtx v;
    v.x = g_rdf32(p) + vtx_ox;
    v.y = g_rdf32(p + 4);
    v.c = diff_off >= 0 ? unpack(g_rd32(p + (uint32_t)diff_off)) : (Col){255, 255, 255, 255};
    v.u = tex_off >= 0 ? g_rdf32(p + (uint32_t)tex_off) : 0;
    v.v = tex_off >= 0 ? g_rdf32(p + (uint32_t)tex_off + 4) : 0;
    return v;
}

/* ---------------- GPU path ----------------
 * The same draws as below, handed to rt_gpu as triangle/line lists. */
static GpuVtx *gv;
static uint32_t gv_cap;

static void gpu_state(Dev *d, GpuState *st, int lines)
{
    memset(st, 0, sizeof *st);
    st->target = d->target->gtex;
    st->cop = d->tss[TSS_COLOROP];
    st->ca1 = d->tss[TSS_COLORARG1];
    st->ca2 = d->tss[TSS_COLORARG2];
    st->aop = d->tss[TSS_ALPHAOP];
    st->aa1 = d->tss[TSS_ALPHAARG1];
    st->aa2 = d->tss[TSS_ALPHAARG2];
    st->tfactor = d->rs[RS_TEXTUREFACTOR];
    if (d->tex && st->cop != 1 && surf_gpu(d->tex)) {
        st->tex = d->tex->gtex;
        st->tex_alpha = d->tex->alpha;
        st->linear = d->tss[TSS_MAGFILTER] == 2 || d->tss[TSS_MINFILTER] == 2;
        st->address_u = d->tss[TSS_ADDRESSU];
        st->address_v = d->tss[TSS_ADDRESSV];
    }
    st->blend = d->rs[RS_ALPHABLENDENABLE] != 0;
    st->src_blend = d->rs[RS_SRCBLEND];
    st->dst_blend = d->rs[RS_DESTBLEND];
    st->target_alpha = d->target->alpha;
    st->alpha_test = d->rs[RS_ALPHATESTENABLE] != 0;
    st->alpha_func = d->rs[RS_ALPHAFUNC];
    st->alpha_ref = d->rs[RS_ALPHAREF];
    int32_t x0, y0, x1, y1;
    view(d, &x0, &y0, &x1, &y1);
    clip_rect(d, &x0, &y0, &x1, &y1);
    st->clip_x = x0;
    st->clip_y = y0;
    st->clip_w = x1 > x0 ? x1 - x0 : 0;
    st->clip_h = y1 > y0 ? y1 - y0 : 0;
    st->lines = lines;
}

static int gpu_draw(Dev *d, uint32_t type, uint32_t verts, uint32_t stride, int doff, int toff, uint32_t count,
                    uint32_t indices)
{
    if (!surf_gpu(d->target)) return 0;
    int lines = type == 1 || type == 2 || type == 3;
    uint32_t need = count * 3 + 6;
    if (need > gv_cap) {
        gv_cap = need * 2;
        gv = realloc(gv, gv_cap * sizeof *gv);
    }
    uint32_t n = 0;
#define IDX(i) (indices ? g_rd16(indices + 2 * (i)) : (i))
#define PUT(i)                                                                            \
    do {                                                                                  \
        uint32_t p_ = verts + stride * IDX(i);                                            \
        gv[n].x = g_rdf32(p_) + vtx_ox;                                                        \
        gv[n].y = g_rdf32(p_ + 4);                                                        \
        gv[n].argb = doff >= 0 ? g_rd32(p_ + (uint32_t)doff) : 0xffffffffu;               \
        gv[n].u = toff >= 0 ? g_rdf32(p_ + (uint32_t)toff) : 0;                           \
        gv[n].v = toff >= 0 ? g_rdf32(p_ + (uint32_t)toff + 4) : 0;                       \
        n++;                                                                              \
    } while (0)
    switch (type) {
    case 4: for (uint32_t i = 0; i + 2 < count; i += 3) { PUT(i); PUT(i + 1); PUT(i + 2); } break;
    case 5: for (uint32_t i = 0; i + 2 < count; i++) { PUT(i); PUT(i + 1); PUT(i + 2); } break;
    case 6: for (uint32_t i = 1; i + 1 < count; i++) { PUT(0); PUT(i); PUT(i + 1); } break;
    case 2: for (uint32_t i = 0; i + 1 < count; i += 2) { PUT(i); PUT(i + 1); } break;
    case 3: for (uint32_t i = 0; i + 1 < count; i++) { PUT(i); PUT(i + 1); } break;
    case 1: for (uint32_t i = 0; i < count; i++) { PUT(i); PUT(i); gv[n - 1].x += 1.f; } break;
    default: rt_log("[d3d] primitive type %u not handled\n", (unsigned)type); return 1;
    }
#undef PUT
#undef IDX
    GpuState st;
    gpu_state(d, &st, lines);
    /* a draw whose texture coordinates stay inside the texture (a picture or one piece of a big one, as
     * SexyApp's TextureData splits them at 2048) is clamped whatever the game set: filtered, a WRAP edge
     * blends in the opposite edge's row, a line across the screen at every piece's seam (the credits page) */
    if (st.tex && (st.address_u == 1 || st.address_v == 1) && n) {
        float u0 = gv[0].u, u1 = gv[0].u, v0 = gv[0].v, v1 = gv[0].v;
        for (uint32_t i = 1; i < n; i++) {
            u0 = gv[i].u < u0 ? gv[i].u : u0, u1 = gv[i].u > u1 ? gv[i].u : u1;
            v0 = gv[i].v < v0 ? gv[i].v : v0, v1 = gv[i].v > v1 ? gv[i].v : v1;
        }
        if (u0 >= -0.001f && u1 <= 1.001f) st.address_u = 3;
        if (v0 >= -0.001f && v1 <= 1.001f) st.address_v = 3;
    }
    if (n && st.clip_w && st.clip_h) rt_gpu->draw(&st, gv, n);
    surf_gpu_wrote(d->target);
    return 1;
}

static void draw(Dev *d, uint32_t type, uint32_t fvf, uint32_t verts, uint32_t n, uint32_t indices, uint32_t nidx)
{
    uint32_t stride;
    int doff, toff;
    vtx_ox = d->target ? (float)d->target->ox : 0.f;
    if (!d->target || !fvf_layout(fvf, &stride, &doff, &toff)) {
        rt_log("[d3d] unsupported FVF %x\n", (unsigned)fvf);
        return;
    }
    uint32_t count = indices ? nidx : n;
    d->target->d3d_target = 1;
    if (d->tex) d->tex->d3d_texture = 1;
    if (rt_gpu && gpu_draw(d, type, verts, stride, doff, toff, count, indices)) return;
    surf_cpu(d->target);
    if (d->tex) surf_cpu(d->tex);
    d->target->gpu_ok = 0;
    Pipe pp;
    pipe_setup(d, &pp);
    const Pipe *p = &pp;
    if (rt_verbose) {
        st_tris += count / 2;
        if (++st_draws % 20000 == 0) {
            static const char *bn[4] = {"opaque", "alpha", "add", "generic"};
            rt_log("[d3d] last 20000 draws (~%llu tris), pixels:", (unsigned long long)st_tris);
            for (int b = 0; b < 4; b++)
                for (int t = 0; t < 2; t++)
                    for (int l = 0; l < 2; l++)
                        if (st_px[b][t][l])
                            rt_log(" %s%s%s=%llu", bn[b], t ? "+tex" : "", l ? "(lin)" : "", (unsigned long long)st_px[b][t][l]);
            rt_log("\n");
            memset(st_px, 0, sizeof st_px);
            st_tris = 0;
        }
    }
#define V(i) read_vtx(verts + stride * (indices ? g_rd16(indices + 2 * (i)) : (i)), doff, toff)
    switch (type) {
    case 4: /* TRIANGLELIST */
        for (uint32_t i = 0; i + 2 < count; i += 3) { Vtx a = V(i), b = V(i + 1), c = V(i + 2); tri(d, p, &a, &b, &c); }
        break;
    case 5: /* TRIANGLESTRIP */
        for (uint32_t i = 0; i + 2 < count; i++) { Vtx a = V(i), b = V(i + 1), c = V(i + 2); tri(d, p, &a, &b, &c); }
        break;
    case 6: /* TRIANGLEFAN */
        for (uint32_t i = 1; i + 1 < count; i++) { Vtx a = V(0), b = V(i), c = V(i + 1); tri(d, p, &a, &b, &c); }
        break;
    case 2: /* LINELIST */
        for (uint32_t i = 0; i + 1 < count; i += 2) { Vtx a = V(i), b = V(i + 1); line(d, p, &a, &b); }
        break;
    case 3: /* LINESTRIP */
        for (uint32_t i = 0; i + 1 < count; i++) { Vtx a = V(i), b = V(i + 1); line(d, p, &a, &b); }
        break;
    case 1: /* POINTLIST */
        for (uint32_t i = 0; i < count; i++) { Vtx a = V(i); line(d, p, &a, &a); }
        break;
    default:
        rt_log("[d3d] primitive type %u not handled\n", (unsigned)type);
    }
#undef V
}

/* ---------------- IDirect3DDevice7 ---------------- */
HLE(dev_QueryInterface)
{
    g_wr32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}
HLE(dev_AddRef) { RET(1, 2); }
HLE(dev_Release) { RET(1, 1); }
HLE(dev_GetCaps)
{
    uint32_t p = ARG(1);
    memset(G2H(p), 0, 236);
    g_wr32(p, 0x1u | 0x10u | 0x40u | 0x200u); /* FLOATTLVERTEX | EXECUTESYSTEMMEMORY | TLVERTEXSYSTEMMEMORY | DRAWPRIMTLVERTEX */
    for (uint32_t k = 4; k <= 60; k += 56) {
        g_wr32(k + p, 56);
        g_wr32(k + p + 16, 0x1fff);       /* src blend caps */
        g_wr32(k + p + 20, 0x1fff);       /* dest blend caps */
        g_wr32(k + p + 24, 0xff);         /* alpha compare */
        g_wr32(k + p + 28, 0x7ffff);      /* shade caps */
        g_wr32(k + p + 32, 0x1u | 0x4u | 0x100u); /* PERSPECTIVE | ALPHA | NONPOW2CONDITIONAL */
        g_wr32(k + p + 36, 0x3ff);        /* filter caps */
        g_wr32(k + p + 44, 0x1f);         /* address caps */
    }
    g_wr32(p + 116, 0x400);               /* DDBD_32 render depth */
    g_wr32(p + 124, 1);
    g_wr32(p + 128, 1);
    g_wr32(p + 132, 2048);
    g_wr32(p + 136, 2048);
    g_wr32(p + 140, 2048);
    g_wr32(p + 144, 0);
    g_wr32(p + 180, 0xffffff);            /* texture op caps */
    g_wr16(p + 184, 1);
    g_wr16(p + 186, 1);
    RET(2, D3D_OK);
}
HLE(dev_EnumTextureFormats)
{
    uint32_t cb = ARG(1), ctx = ARG(2);
    uint32_t pf = rt_heap_alloc(32, 1);
    g_wr32(pf, 32);
    g_wr32(pf + 4, 0x41);   /* RGB | ALPHAPIXELS */
    g_wr32(pf + 12, 32);
    g_wr32(pf + 16, 0x00ff0000);
    g_wr32(pf + 20, 0x0000ff00);
    g_wr32(pf + 24, 0x000000ff);
    g_wr32(pf + 28, 0xff000000u);
    uint32_t args[2] = {pf, ctx};
    if (rt_guest_call(c, cb, 2, args)) {
        g_wr32(pf + 4, 0x40); /* X8R8G8B8 too */
        g_wr32(pf + 28, 0);
        rt_guest_call(c, cb, 2, args);
    }
    rt_heap_free(pf);
    RET(3, D3D_OK);
}
/* the host's per-frame overlay pieces (pictures placed while the game draws) start over each frame */
__attribute__((weak)) void rt_frame_begin(void) {}
/* widescreen: the strips beside the game's 800 start each frame black (screens that draw nothing there
 * would keep old frames), or, under a 4:3 window screen that replaced the menu (rt_wide_backdrop, set
 * by the host when such a screen drew last frame), with the menu's strips as it was last drawn
 * (kept by rt_wide_keep_strips at the end of each menu frame) */
int rt_wide_backdrop, rt_wide_menu_drawn;
static Surf *strips; /* the menu's two strips, side by side */
void rt_wide_keep_strips(Surf *s)
{
    if (!rt_wide_menu_drawn || !rt_gpu || !surf_gpu(s)) return;
    rt_wide_menu_drawn = 0;
    int32_t w = s->ox, r = (int32_t)s->w - s->ox - (int32_t)s->vw;
    if (!strips) strips = surf_new((uint32_t)(w + r), s->h, 0);
    if (!strips || !surf_gpu(strips)) return;
    GpuState st;
    memset(&st, 0, sizeof st);
    st.target = strips->gtex;
    st.tex = s->gtex;
    st.address_u = st.address_v = 3;
    st.cop = 2, st.ca1 = 2, st.aop = 2, st.aa1 = 2; /* SELECTARG1(TEXTURE) */
    st.clip_w = w + r, st.clip_h = (int32_t)s->h;
    float H = (float)s->h, W = (float)s->w, xr = (float)(s->ox + (int32_t)s->vw);
    GpuVtx q[12] = {
        {-.5f, -.5f, 0, 0, ~0u}, {(float)w - .5f, -.5f, (float)w / W, 0, ~0u}, {-.5f, H - .5f, 0, 1, ~0u},
        {(float)w - .5f, -.5f, (float)w / W, 0, ~0u}, {(float)w - .5f, H - .5f, (float)w / W, 1, ~0u}, {-.5f, H - .5f, 0, 1, ~0u},
        {(float)w - .5f, -.5f, xr / W, 0, ~0u}, {(float)(w + r) - .5f, -.5f, 1, 0, ~0u}, {(float)w - .5f, H - .5f, xr / W, 1, ~0u},
        {(float)(w + r) - .5f, -.5f, 1, 0, ~0u}, {(float)(w + r) - .5f, H - .5f, 1, 1, ~0u}, {(float)w - .5f, H - .5f, xr / W, 1, ~0u},
    };
    rt_gpu->draw(&st, q, 12);
    surf_gpu_wrote(strips);
}
static int put_strips(Surf *t)
{
    if (!strips || !rt_gpu || !surf_gpu(t) || !surf_gpu(strips)) return 0;
    int32_t w = t->ox, r = (int32_t)t->w - t->ox - (int32_t)t->vw;
    GpuState st;
    memset(&st, 0, sizeof st);
    st.target = t->gtex;
    st.tex = strips->gtex;
    st.address_u = st.address_v = 3;
    st.cop = 2, st.ca1 = 2, st.aop = 2, st.aa1 = 2;
    st.clip_w = (int32_t)t->w, st.clip_h = (int32_t)t->h;
    float H = (float)t->h, SW = (float)(w + r), us = (float)w / SW, xr = (float)(t->ox + (int32_t)t->vw);
    GpuVtx q[12] = {
        {-.5f, -.5f, 0, 0, ~0u}, {(float)w - .5f, -.5f, us, 0, ~0u}, {-.5f, H - .5f, 0, 1, ~0u},
        {(float)w - .5f, -.5f, us, 0, ~0u}, {(float)w - .5f, H - .5f, us, 1, ~0u}, {-.5f, H - .5f, 0, 1, ~0u},
        {xr - .5f, -.5f, us, 0, ~0u}, {(float)t->w - .5f, -.5f, 1, 0, ~0u}, {xr - .5f, H - .5f, us, 1, ~0u},
        {(float)t->w - .5f, -.5f, 1, 0, ~0u}, {(float)t->w - .5f, H - .5f, 1, 1, ~0u}, {xr - .5f, H - .5f, us, 1, ~0u},
    };
    rt_gpu->draw(&st, q, 12);
    surf_gpu_wrote(t);
    return 1;
}
static void clear_sides(Surf *t)
{
    int backdrop = rt_wide_backdrop;
    rt_wide_backdrop = 0;
    if (backdrop && put_strips(t)) return;
    int32_t xs[2] = {0, t->ox + (int32_t)t->vw}, ws[2] = {t->ox, (int32_t)t->w - t->ox - (int32_t)t->vw};
    for (int i = 0; i < 2; i++) {
        if (ws[i] <= 0) continue;
        if (rt_gpu && surf_gpu(t)) {
            rt_gpu->fill(t->gtex, xs[i], 0, ws[i], (int32_t)t->h, 0xff000000u);
            surf_gpu_wrote(t);
            continue;
        }
        surf_cpu(t);
        t->gpu_ok = 0;
        for (uint32_t y = 0; y < t->h; y++) {
            uint32_t *row = (uint32_t *)G2H(t->bits + y * t->pitch);
            for (int32_t x = xs[i]; x < xs[i] + ws[i]; x++) row[x] = 0xff000000u;
        }
    }
}
/* a 4:3 window screen starting to draw over the menu: its strips at once (rt_wide_backdrop only gives them
 * from the next frame: the screen's first frame showed black or the menu's own 4:3 drawing at the sides) */
static Surf *scene_target;
void rt_wide_backdrop_now(void)
{
    if (scene_target && scene_target->ox) put_strips(scene_target);
}
HLE(dev_BeginScene)
{
    rt_frame_begin();
    Dev *d = D(ARG(0));
    scene_target = d->target;
    /* the DLC begins several scenes per frame: only the frame's first starts the strips over */
    extern uint32_t rt_frames_shown;
    static uint32_t cleared_at = ~0u;
    if (d->target && d->target->ox && cleared_at != rt_frames_shown) {
        cleared_at = rt_frames_shown;
        clear_sides(d->target);
    }
    RET(1, D3D_OK);
}
HLE(dev_EndScene) { RET(1, D3D_OK); }
HLE(dev_SetRenderTarget)
{
    Dev *d = D(ARG(0));
    Surf *s = S(ARG(1));
    if (s) {
        d->target = s;
        d->vx = d->vy = 0;
        d->vw = (int32_t)(s->ox ? s->vw : s->w);
        d->vh = (int32_t)s->h;
    }
    RET(3, D3D_OK);
}
HLE(dev_GetRenderTarget)
{
    Dev *d = D(ARG(0));
    d->target->refs++;
    g_wr32(ARG(1), d->target->obj);
    RET(2, D3D_OK);
}
HLE(dev_Clear)
{
    Dev *d = D(ARG(0));
    uint32_t count = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4);
    if (rt_verbose && d->target && d->target->ox) {
        static int logged;
        if (logged++ < 30) rt_log("[d3d] clear flags %x count %u rect %d,%d,%d,%d colour %08x\n", (unsigned)flags, (unsigned)count,
                                  rects ? (int)g_rd32(rects) : -1, rects ? (int)g_rd32(rects + 4) : -1, rects ? (int)g_rd32(rects + 8) : -1,
                                  rects ? (int)g_rd32(rects + 12) : -1, (unsigned)color);
    }
    if ((flags & 1) && d->target) {
        int n = rects ? (int)count : 1;
        for (int i = 0; i < n; i++) {
            int32_t x0, y0, x1, y1;
            view(d, &x0, &y0, &x1, &y1);
            if (rects) {
                uint32_t r = rects + 16 * (uint32_t)i;
                x0 = (int32_t)g_rd32(r); y0 = (int32_t)g_rd32(r + 4); x1 = (int32_t)g_rd32(r + 8); y1 = (int32_t)g_rd32(r + 12);
                x0 += d->target->ox, x1 += d->target->ox;
            }
            clip_rect(d, &x0, &y0, &x1, &y1);
            if (x1 <= x0 || y1 <= y0) continue;
            if (rt_gpu && surf_gpu(d->target)) {
                rt_gpu->fill(d->target->gtex, x0, y0, x1 - x0, y1 - y0, color);
                surf_gpu_wrote(d->target);
                continue;
            }
            surf_cpu(d->target);
            d->target->gpu_ok = 0;
            for (int32_t y = y0; y < y1; y++) {
                uint32_t *row = (uint32_t *)G2H(d->target->bits + (uint32_t)y * d->target->pitch);
                for (int32_t x = x0; x < x1; x++) row[x] = color;
            }
        }
    }
    RET(7, D3D_OK);
}
HLE(dev_SetTransform) { RET(3, D3D_OK); }
HLE(dev_SetViewport)
{
    Dev *d = D(ARG(0));
    uint32_t v = ARG(1);
    d->vx = (int32_t)g_rd32(v);
    d->vy = (int32_t)g_rd32(v + 4);
    d->vw = (int32_t)g_rd32(v + 8);
    d->vh = (int32_t)g_rd32(v + 12);
    static int logged;
    if (rt_verbose > 1 || (rt_verbose && logged < 20 && (d->vw != 800 || d->vh != 600))) {
        logged++;
        rt_log("[d3d] viewport %d,%d %dx%d\n", d->vx, d->vy, d->vw, d->vh);
    }
    RET(2, D3D_OK);
}
HLE(dev_GetViewport)
{
    Dev *d = D(ARG(0));
    uint32_t v = ARG(1);
    g_wr32(v, (uint32_t)d->vx);
    g_wr32(v + 4, (uint32_t)d->vy);
    g_wr32(v + 8, (uint32_t)d->vw);
    g_wr32(v + 12, (uint32_t)d->vh);
    float z0 = 0.f, z1 = 1.f;
    memcpy(G2H(v + 16), &z0, 4);
    memcpy(G2H(v + 20), &z1, 4);
    RET(2, D3D_OK);
}
HLE(dev_SetRenderState)
{
    Dev *d = D(ARG(0));
    if (ARG(1) < 256) d->rs[ARG(1)] = ARG(2);
    RET(3, D3D_OK);
}
HLE(dev_GetRenderState)
{
    Dev *d = D(ARG(0));
    g_wr32(ARG(2), ARG(1) < 256 ? d->rs[ARG(1)] : 0);
    RET(3, D3D_OK);
}
HLE(dev_PreLoad) { RET(2, D3D_OK); }
HLE(dev_DrawPrimitive)
{
    struct timespec t0, t1;
    if (rt_verbose) clock_gettime(CLOCK_MONOTONIC, &t0);
    draw(D(ARG(0)), ARG(1), ARG(2), ARG(3), ARG(4), 0, 0);
    if (rt_verbose) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        rt_stat_draw_us += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
        rt_stat_draws++;
    }
    RET(6, D3D_OK);
}
HLE(dev_DrawIndexedPrimitive)
{
    draw(D(ARG(0)), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
    RET(8, D3D_OK);
}
HLE(dev_GetTexture)
{
    Dev *d = D(ARG(0));
    if (d->tex) d->tex->refs++;
    g_wr32(ARG(2), d->tex ? d->tex->obj : 0);
    RET(3, D3D_OK);
}
HLE(dev_SetTexture)
{
    Dev *d = D(ARG(0));
    if (ARG(1) == 0) d->tex = ARG(2) ? S(ARG(2)) : NULL;
    if (d->tex) d->tex->d3d_texture = 1;
    RET(3, D3D_OK);
}
HLE(dev_GetTextureStageState)
{
    Dev *d = D(ARG(0));
    g_wr32(ARG(3), ARG(1) == 0 && ARG(2) < 32 ? d->tss[ARG(2)] : 0);
    RET(4, D3D_OK);
}
HLE(dev_SetTextureStageState)
{
    Dev *d = D(ARG(0));
    if (ARG(1) == 0 && ARG(2) < 32) d->tss[ARG(2)] = ARG(3);
    RET(4, D3D_OK);
}
HLE(dev_ValidateDevice)
{
    if (ARG(1)) g_wr32(ARG(1), 1);
    RET(2, D3D_OK);
}

uint32_t d3d_device_create(Surf *target)
{
    Dev *d = calloc(1, sizeof *d);
    d->target = target;
    d->vw = (int32_t)(target->ox ? target->vw : target->w);
    d->vh = (int32_t)target->h;
    /* D3D7 defaults for what we model */
    d->rs[RS_SRCBLEND] = 2;
    d->rs[RS_DESTBLEND] = 1;
    d->rs[RS_TEXTUREFACTOR] = 0xffffffffu;
    d->tss[TSS_COLOROP] = 4;
    d->tss[TSS_COLORARG1] = 2;
    d->tss[TSS_COLORARG2] = 1;
    d->tss[TSS_ALPHAOP] = 2;
    d->tss[TSS_ALPHAARG1] = 2;
    d->tss[TSS_ALPHAARG2] = 1;
    d->tss[TSS_ADDRESSU] = d->tss[TSS_ADDRESSV] = 1;
    d->tss[TSS_MAGFILTER] = d->tss[TSS_MINFILTER] = 1;
    return com_wrap(vt_d3ddev, d);
}

/* ---------------- IDirect3D7 ---------------- */
HLE(d3d_QueryInterface)
{
    g_wr32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}
HLE(d3d_AddRef) { RET(1, 2); }
HLE(d3d_Release) { RET(1, 1); }
HLE(d3d_CreateDevice)
{
    Surf *t = S(ARG(2));
    if (!t) RET(4, 0x80070057u);
    rt_log("[d3d] CreateDevice on a %ux%u target\n", (unsigned)t->w, (unsigned)t->h);
    g_wr32(ARG(3), d3d_device_create(t));
    RET(4, D3D_OK);
}
HLE(d3d_EnumZBufferFormats)
{
    uint32_t cb = ARG(2), ctx = ARG(3);
    uint32_t pf = rt_heap_alloc(32, 1);
    g_wr32(pf, 32);
    g_wr32(pf + 4, 0x400); /* DDPF_ZBUFFER */
    g_wr32(pf + 12, 16);
    g_wr32(pf + 20, 0xffff);
    uint32_t args[2] = {pf, ctx};
    rt_guest_call(c, cb, 2, args);
    rt_heap_free(pf);
    RET(4, D3D_OK);
}
HLE(d3d_EvictManagedTextures) { RET(1, D3D_OK); }

uint32_t d3d7_create_object(void)
{
#define M(n, f) {n, f}
    static const ComMethod dev[49] = {
        M("QueryInterface", dev_QueryInterface), M("AddRef", dev_AddRef), M("Release", dev_Release),
        M("GetCaps", dev_GetCaps), M("EnumTextureFormats", dev_EnumTextureFormats), M("BeginScene", dev_BeginScene),
        M("EndScene", dev_EndScene), M("GetDirect3D", NULL), M("SetRenderTarget", dev_SetRenderTarget),
        M("GetRenderTarget", dev_GetRenderTarget), M("Clear", dev_Clear), M("SetTransform", dev_SetTransform),
        M("GetTransform", NULL), M("SetViewport", dev_SetViewport), M("MultiplyTransform", NULL),
        M("GetViewport", dev_GetViewport), M("SetMaterial", NULL), M("GetMaterial", NULL), M("SetLight", NULL),
        M("GetLight", NULL), M("SetRenderState", dev_SetRenderState), M("GetRenderState", dev_GetRenderState),
        M("BeginStateBlock", NULL), M("EndStateBlock", NULL), M("PreLoad", dev_PreLoad),
        M("DrawPrimitive", dev_DrawPrimitive), M("DrawIndexedPrimitive", dev_DrawIndexedPrimitive),
        M("SetClipStatus", NULL), M("GetClipStatus", NULL), M("DrawPrimitiveStrided", NULL),
        M("DrawIndexedPrimitiveStrided", NULL), M("DrawPrimitiveVB", NULL), M("DrawIndexedPrimitiveVB", NULL),
        M("ComputeSphereVisibility", NULL), M("GetTexture", dev_GetTexture), M("SetTexture", dev_SetTexture),
        M("GetTextureStageState", dev_GetTextureStageState), M("SetTextureStageState", dev_SetTextureStageState),
        M("ValidateDevice", dev_ValidateDevice), M("ApplyStateBlock", NULL), M("CaptureStateBlock", NULL),
        M("DeleteStateBlock", NULL), M("CreateStateBlock", NULL), M("Load", NULL), M("LightEnable", NULL),
        M("GetLightEnable", NULL), M("SetClipPlane", NULL), M("GetClipPlane", NULL), M("GetInfo", NULL)};
    vt_d3ddev = com_vtable("IDirect3DDevice7", dev, 49);
    static const ComMethod d3d[8] = {
        M("QueryInterface", d3d_QueryInterface), M("AddRef", d3d_AddRef), M("Release", d3d_Release),
        M("EnumDevices", NULL), M("CreateDevice", d3d_CreateDevice), M("CreateVertexBuffer", NULL),
        M("EnumZBufferFormats", d3d_EnumZBufferFormats), M("EvictManagedTextures", d3d_EvictManagedTextures)};
    vt_d3d7 = com_vtable("IDirect3D7", d3d, 8);
#undef M
    static int impl;
    return com_wrap(vt_d3d7, &impl);
}

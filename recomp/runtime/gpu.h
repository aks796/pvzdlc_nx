/* gpu.h: the GPU backend behind DirectDraw/Direct3D 7.
 *
 * hle_ddraw.c and hle_d3d7.c decide what runs where (surface sync, which
 * blits go to the GPU); a backend only draws. host/gpu_gl.c is the OpenGL one
 * (desktop test host, through the SDL window); the Switch gets a deko3d one
 * with the same interface. rt_gpu is NULL when there is no backend and the
 * software paths are used (RT_SOFT=1 forces that).
 *
 * Coordinates are guest pixels with D3D7's pixel centres at integers. Texture
 * rows are stored top row first, so v=0 is the top of an image everywhere.
 * Every call comes from the thread that owns the GPU context (the game's main
 * loop) except where noted. */
#ifndef GPU_H
#define GPU_H

#include <stdint.h>

typedef struct GpuTex GpuTex;

typedef struct {
    float x, y, u, v;
    uint32_t argb;
} GpuVtx;

/* Everything about a draw except its vertices; draws with equal state are batched */
typedef struct {
    GpuTex *target;
    GpuTex *tex;              /* NULL: no texture */
    int tex_alpha;            /* texture has alpha (else alpha reads 1) */
    int linear;               /* bilinear filtering */
    uint32_t address_u, address_v; /* D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp */
    uint32_t cop, ca1, ca2, aop, aa1, aa2; /* stage 0 D3DTOP / D3DTA */
    uint32_t tfactor;         /* ARGB */
    int blend;                /* alpha blending on */
    uint32_t src_blend, dst_blend; /* D3DBLEND */
    int target_alpha;         /* render target has alpha (else DESTALPHA reads 1) */
    int alpha_test;
    uint32_t alpha_func, alpha_ref;
    int colorkey;             /* DirectDraw source colour key: texels in [ck_lo, ck_hi] (RGB) are dropped */
    uint32_t ck_lo, ck_hi;
    int32_t clip_x, clip_y, clip_w, clip_h; /* viewport, as a scissor */
    int lines;                /* line list instead of triangle list */
} GpuState;

typedef struct {
    const char *name;
    GpuTex *(*tex_new)(uint32_t w, uint32_t h);
    void (*tex_free)(GpuTex *t);               /* any thread: freed later on the GPU thread */
    void (*tex_upload)(GpuTex *t, const void *pixels, uint32_t pitch);  /* whole texture, ARGB */
    void (*tex_download)(GpuTex *t, void *pixels, uint32_t pitch);      /* whole texture; waits */
    /* triangle list (or line list), 3 (2) vertices each */
    void (*draw)(const GpuState *s, const GpuVtx *v, uint32_t n);
    void (*fill)(GpuTex *t, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t argb);
    /* show this texture (the game's 1280x720 display) on the next present */
    void (*show)(GpuTex *t);
    int (*on_gpu_thread)(void);
} GpuBackend;

extern const GpuBackend *rt_gpu;

#endif

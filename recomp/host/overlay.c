/* overlay.c: pictures the port draws over the game's display, on top of everything the game drew:
 * button pictures (the Xbox 360 edition's, relabelled for the Switch by PvZ Touch), the dig meter (the TV edition's shovel meter, from the
 * user's files). They are built in (port_art.c, tools/embed_art.py); a file in the game folder's port/
 * (SD: /switch/pvzdlc/port/) replaces one.
 *
 * The controller code (pad_input.c, on the game thread) rebuilds the list every poll; each present
 * copies the game's display into a texture of its own, draws the list over it and shows that, so
 * nothing is ever drawn into the game's surfaces (whose unpainted parts would keep old pictures). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/gpu.h"
#include "overlay.h"
#include "../runtime/rt.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

extern const struct { const char *name; const unsigned char *data; size_t size; } ov_embedded[];

static struct {
    const char *file;
    GpuTex *tex;
    int w, h, tried;
} imgs[OV_IMAGES] = {
    [OV_HELP] = {"help_buttons.png"},
    [OV_HELP_SMALL] = {"help_buttons_small.png"},
    [OV_METER_EMPTY] = {"shovelmeter_empty.png"},
    [OV_METER_FULL] = {"shovelmeter_full.png"},
    [OV_WHITE] = {NULL},
    [OV_PAD] = {"pad_icon.png"},
};

#define MAX_VERTS 3072
#define MAX_BATCHES 128
/* lists: [0] the controller code's (rebuilt every poll); [2] pictures placed while the game draws a frame
 * (button icons in its text; started over at its BeginScene), which becomes [1], the one shown, at the
 * frame's Flip. The Switch presents at the game's vertical-blank wait, which can come after the next
 * frame's BeginScene. */
typedef struct {
    GpuVtx verts[MAX_VERTS];
    struct { int img, add; uint32_t first, count; } batches[MAX_BATCHES];
    int n_verts, n_batches;
} OvList;
static OvList lists[3];
static OvList *cur_list = &lists[0];
static int additive; /* ov_additive: what comes next adds light */

void ov_additive(int on) { additive = on; }

void ov_clear(void)
{
    lists[0].n_verts = lists[0].n_batches = 0;
}

/* The DLC begins more than one scene per frame: the frame list starts over at the first after a Flip */
static int frame_begun;

void rt_frame_begin(void)
{
    if (!frame_begun) lists[2].n_verts = lists[2].n_batches = 0;
    frame_begun = 1;
    extern void game_frame_begin(void);
    game_frame_begin();
}

void rt_frame_end(void)
{
    frame_begun = 0;
    extern void wide_frame_end(void);
    wide_frame_end();
    lists[1].n_verts = lists[2].n_verts;
    lists[1].n_batches = lists[2].n_batches;
    memcpy(lists[1].verts, lists[2].verts, sizeof(GpuVtx) * (size_t)lists[2].n_verts);
    memcpy(lists[1].batches, lists[2].batches, sizeof lists[1].batches[0] * (size_t)lists[2].n_batches);
}

void ov_use_frame_list(int on)
{
    cur_list = &lists[on ? 2 : 0];
}

int ov_active(void)
{
    return lists[0].n_batches > 0 || lists[1].n_batches > 0;
}

/* pictures made at run time (focus glows), after the fixed ones */
#define MAX_DYN 96
static struct { uint32_t key; GpuTex *tex; int w, h; } dyn[MAX_DYN];
static int n_dyn;

int ov_dynamic_find(uint32_t key, int *w, int *h)
{
    for (int i = 0; i < n_dyn; i++)
        if (dyn[i].key == key) {
            *w = dyn[i].w;
            *h = dyn[i].h;
            return OV_IMAGES + i;
        }
    return -1;
}

int ov_dynamic(uint32_t key, int w, int h, const uint32_t *argb)
{
    int ww, hh, i = ov_dynamic_find(key, &ww, &hh);
    if (i >= 0) return i;
    if (!rt_gpu || w <= 0 || h <= 0) return -1;
    if (n_dyn == MAX_DYN) { /* full: the oldest goes */
        rt_gpu->tex_free(dyn[0].tex);
        memmove(dyn, dyn + 1, sizeof dyn[0] * (MAX_DYN - 1));
        n_dyn--;
    }
    GpuTex *t = rt_gpu->tex_new((uint32_t)w, (uint32_t)h);
    if (!t) return -1;
    rt_gpu->tex_upload(t, argb, (uint32_t)w * 4);
    dyn[n_dyn].key = key, dyn[n_dyn].tex = t, dyn[n_dyn].w = w, dyn[n_dyn].h = h;
    return OV_IMAGES + n_dyn++;
}

/* port_art/<file> as RGBA bytes (free()), a file in the game folder's port/ winning over the copy built in
 * (port_art.c); NULL when there's neither */
uint8_t *ov_art_rgba(const char *file, int *w, int *h)
{
    char path[600];
    snprintf(path, sizeof path, "%s/port/%s", rt_game_dir, file);
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n = 0;
    int ok = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = malloc((size_t)n);
        ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
        fclose(f);
    }
    const uint8_t *src = buf;
    if (!ok)
        for (int k = 0; ov_embedded[k].name; k++)
            if (!strcmp(ov_embedded[k].name, file)) {
                src = ov_embedded[k].data;
                n = (long)ov_embedded[k].size;
                ok = 1;
            }
    if (!ok) rt_log("[ov] %s: no picture\n", file);
    int comp = 0;
    uint8_t *px = ok ? stbi_load_from_memory(src, (int)n, w, h, &comp, 4) : NULL;
    free(buf);
    if (ok && !px) rt_log("[ov] %s: not a PNG\n", file);
    return px;
}

/* the picture as a texture, loaded on first use (the GPU's thread: the game's) */
static GpuTex *image(int i, int *w, int *h)
{
    if (i >= OV_IMAGES && i < OV_IMAGES + n_dyn) {
        *w = dyn[i - OV_IMAGES].w;
        *h = dyn[i - OV_IMAGES].h;
        return dyn[i - OV_IMAGES].tex;
    }
    if (i < 0 || i >= OV_IMAGES || !rt_gpu) return NULL;
    if (!imgs[i].tried && !imgs[i].file) {
        imgs[i].tried = 1;
        uint32_t px[16];
        for (int k = 0; k < 16; k++) px[k] = 0xffffffffu;
        imgs[i].tex = rt_gpu->tex_new(4, 4);
        if (imgs[i].tex) rt_gpu->tex_upload(imgs[i].tex, px, 16);
        imgs[i].w = imgs[i].h = 4;
    }
    if (!imgs[i].tried) {
        imgs[i].tried = 1;
        int pw = 0, ph = 0;
        uint8_t *px = ov_art_rgba(imgs[i].file, &pw, &ph);
        if (!px) return NULL;
        /* RGBA bytes -> the backends' ARGB words */
        uint32_t *argb = (uint32_t *)px;
        for (int k = 0; k < pw * ph; k++) {
            uint8_t *p = px + 4 * k;
            argb[k] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
        imgs[i].tex = rt_gpu->tex_new((uint32_t)pw, (uint32_t)ph);
        if (imgs[i].tex) rt_gpu->tex_upload(imgs[i].tex, argb, (uint32_t)pw * 4);
        imgs[i].w = pw;
        imgs[i].h = ph;
        stbi_image_free(px);
    }
    *w = imgs[i].w;
    *h = imgs[i].h;
    return imgs[i].tex;
}

static GpuVtx *add(int img, int n)
{
    OvList *l = cur_list;
    if (l->n_verts + n > MAX_VERTS) return NULL;
    if (!l->n_batches || l->batches[l->n_batches - 1].img != img || l->batches[l->n_batches - 1].add != additive) {
        if (l->n_batches == MAX_BATCHES) return NULL;
        l->batches[l->n_batches].img = img;
        l->batches[l->n_batches].add = additive;
        l->batches[l->n_batches].first = (uint32_t)l->n_verts;
        l->batches[l->n_batches].count = 0;
        l->n_batches++;
    }
    l->batches[l->n_batches - 1].count += (uint32_t)n;
    GpuVtx *v = &l->verts[l->n_verts];
    l->n_verts += n;
    return v;
}

/* The display is 1280x720 with the game's 800x600 centred at 1.2x: game point -> display point */
static float dx(float x) { return 160.f + x * 1.2f; }
static float dy(float y) { return y * 1.2f; }

void ov_image(int img, int sx, int sy, int sw, int sh, float x, float y, float w, float h, uint32_t argb)
{
    int iw, ih;
    if (!image(img, &iw, &ih) || iw <= 0 || ih <= 0) return;
    GpuVtx *v = add(img, 6);
    if (!v) return;
    float x0 = dx(x) - .5f, y0 = dy(y) - .5f, x1 = dx(x + w) - .5f, y1 = dy(y + h) - .5f;
    float u0 = (float)sx / (float)iw, v0 = (float)sy / (float)ih, u1 = (float)(sx + sw) / (float)iw,
          v1 = (float)(sy + sh) / (float)ih;
    const GpuVtx q[4] = {{x0, y0, u0, v0, argb}, {x1, y0, u1, v0, argb}, {x0, y1, u0, v1, argb}, {x1, y1, u1, v1, argb}};
    v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[2], v[4] = q[1], v[5] = q[3];
}

void ov_image_rot(int img, int sx, int sy, int sw, int sh, float cx, float cy, float w, float h, float deg, uint32_t argb)
{
    int iw, ih;
    if (!image(img, &iw, &ih) || iw <= 0 || ih <= 0) return;
    GpuVtx *v = add(img, 6);
    if (!v) return;
    float a = deg * 3.14159265f / 180.f, cs = cosf(a), sn = sinf(a);
    float u0 = (float)sx / (float)iw, v0 = (float)sy / (float)ih, u1 = (float)(sx + sw) / (float)iw,
          v1 = (float)(sy + sh) / (float)ih;
    const float px[4] = {-w / 2, w / 2, -w / 2, w / 2}, py[4] = {-h / 2, -h / 2, h / 2, h / 2};
    const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
    GpuVtx q[4];
    for (int i = 0; i < 4; i++) {
        float x = cx + px[i] * cs - py[i] * sn, y = cy + px[i] * sn + py[i] * cs;
        q[i] = (GpuVtx){dx(x) - .5f, dy(y) - .5f, us[i], vs[i], argb};
    }
    v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[2], v[4] = q[1], v[5] = q[3];
}

void ov_rect(float x, float y, float w, float h, uint32_t argb)
{
    ov_image(OV_WHITE, 1, 1, 2, 2, x, y, w, h, argb);
}

void ov_frame(float x, float y, float w, float h, float t, uint32_t argb)
{
    ov_rect(x - t, y - t, w + 2 * t, t, argb);
    ov_rect(x - t, y + h, w + 2 * t, t, argb);
    ov_rect(x - t, y, t, h, argb);
    ov_rect(x + w, y, t, h, argb);
}

/* a disc's share `frac` (0..1) clockwise from 3 o'clock, centre (cx, cy), radius r (game points), the
 * whole picture mapped onto the disc's square: the dig meter's ring filling up */
void ov_pie(int img, float cx, float cy, float r, float frac, uint32_t argb)
{
    int iw, ih;
    if (!image(img, &iw, &ih)) return;
    int wedges = (int)(frac * 100.f + .5f);
    if (wedges <= 0) return;
    if (wedges > 100) wedges = 100;
    GpuVtx *v = add(img, 3 * wedges);
    if (!v) return;
    for (int i = 0; i < wedges; i++) {
        float a0 = (float)i * 6.2831853f / 100.f, a1 = (float)(i + 1) * 6.2831853f / 100.f;
        float pts[3][2] = {{0, 0}, {cosf(a0), sinf(a0)}, {cosf(a1), sinf(a1)}};
        for (int k = 0; k < 3; k++) {
            GpuVtx *p = &v[3 * i + k];
            p->x = dx(cx + pts[k][0] * r) - .5f;
            p->y = dy(cy + pts[k][1] * r) - .5f;
            p->u = .5f + .478f * pts[k][0];
            p->v = .5f + .478f * pts[k][1];
            p->argb = argb;
        }
    }
}

/* Present: the game's display with the list over it, in a texture of the overlay's own; returns it
 * (or src when there is nothing to draw) */
GpuTex *ov_compose(GpuTex *src, uint32_t w, uint32_t h, void (*draw)(const GpuState *, const GpuVtx *, uint32_t))
{
    static GpuTex *comp;
    static uint32_t cw, ch;
    if (!src || !ov_active() || !rt_gpu) return src;
    if (!comp || cw != w || ch != h) {
        if (comp) rt_gpu->tex_free(comp);
        comp = rt_gpu->tex_new(w, h);
        cw = w;
        ch = h;
        if (!comp) return src;
    }
    GpuState st;
    memset(&st, 0, sizeof st);
    st.target = comp;
    st.tex = src;
    st.cop = 2, st.ca1 = 2; /* SELECTARG1(TEXTURE) */
    st.aop = 2, st.aa1 = 2;
    st.address_u = st.address_v = 3;
    st.target_alpha = 1;
    st.clip_w = (int32_t)w, st.clip_h = (int32_t)h;
    float x1 = (float)w - .5f, y1 = (float)h - .5f;
    const GpuVtx q[6] = {{-.5f, -.5f, 0, 0, ~0u}, {x1, -.5f, 1, 0, ~0u}, {-.5f, y1, 0, 1, ~0u},
                         {-.5f, y1, 0, 1, ~0u}, {x1, -.5f, 1, 0, ~0u}, {x1, y1, 1, 1, ~0u}};
    draw(&st, q, 6);
    for (int li = 1; li >= 0; li--)
    for (int b = 0; b < lists[li].n_batches; b++) {
        int iw, ih;
        GpuTex *t = image(lists[li].batches[b].img, &iw, &ih);
        if (!t) continue;
        st.tex = t;
        st.tex_alpha = 1;
        st.linear = 1;
        st.cop = 4, st.ca1 = 2, st.ca2 = 0; /* MODULATE(TEXTURE, DIFFUSE) */
        st.aop = 4, st.aa1 = 2, st.aa2 = 0;
        st.blend = 1, st.src_blend = 5, st.dst_blend = lists[li].batches[b].add ? 2 : 6; /* SRCALPHA, ONE / INVSRCALPHA */
        if (lists[li].batches[b].add == 2) st.src_blend = 1, st.dst_blend = 3; /* ZERO, SRCCOLOR: multiply */
        draw(&st, &lists[li].verts[lists[li].batches[b].first], lists[li].batches[b].count);
    }
    return comp;
}

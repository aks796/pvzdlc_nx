/* gpu_dk.c: the deko3d backend of gpu.h (Switch). Same behaviour as host/gpu_gl.c.
 *
 *  - Textures are block-linear BGRA8 images (B,G,R,A in memory, as the game's ARGB) carved from 64 MiB
 *    image memory blocks, each also usable as a render target. A texture's image descriptor slot and
 *    memory are reused only SLOTS + 1 frames after it was freed (the GPU may still read it).
 *  - SLOTS (3) frame slots, each with command memory, a vertex ring and a staging area for uploads and
 *    readbacks; a slot is reused once its fence (SLOTS frames back) has passed.
 *  - Draws with the same state are queued and go out as one dkCmdBufDraw; the per-draw state is two
 *    small uniform blocks pushed through the command stream.
 *  - Rows are top-first everywhere (deko3d's upper-left origin), so the guest's row 0 is row 0.
 *  - Present (gpu_dk_present, at the game's vertical-blank wait): the shown texture onto the
 *    swapchain image, plus the controller cursor, then dkQueuePresentImage (interval 1 = vsync). */
#include <deko3d.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "../runtime/gpu.h"
#include "../runtime/rt.h"
#include "overlay.h"
#include "saltynx.h"

/* embedded DKSH (Makefile: uam + bin2s) */
extern const uint8_t draw_vsh_dksh[], draw_fsh_dksh[], present_vsh_dksh[], present_fsh_dksh[];
extern const uint32_t draw_vsh_dksh_size, draw_fsh_dksh_size, present_vsh_dksh_size, present_fsh_dksh_size;

#define ALIGN(v, a) (((v) + (a) - 1) & ~(uint64_t)((a) - 1))

/* ---------------- memory ---------------- */
#define IMAGE_BLOCK (64u << 20)
#define MAX_IMAGE_BLOCKS 16
#define MAX_RANGES 2048

typedef struct {
    DkMemBlock block;
    uint32_t nfree;
    struct { uint32_t off, size; } free[MAX_RANGES]; /* sorted by offset */
} ImgBlock;
static ImgBlock *img_blocks[MAX_IMAGE_BLOCKS];
static int n_img_blocks;

static int img_alloc_in(ImgBlock *b, uint32_t size, uint32_t align, uint32_t *off)
{
    for (uint32_t i = 0; i < b->nfree; i++) {
        uint32_t s = (uint32_t)ALIGN(b->free[i].off, align), end = b->free[i].off + b->free[i].size;
        if (s + size > end) continue;
        uint32_t from = b->free[i].off;
        /* split: [from, s) stays free, [s+size, end) stays free */
        if (s > from && s + size < end) {
            if (b->nfree == MAX_RANGES) return 0;
            memmove(&b->free[i + 2], &b->free[i + 1], (b->nfree - i - 1) * sizeof b->free[0]);
            b->nfree++;
            b->free[i].size = s - from;
            b->free[i + 1].off = s + size;
            b->free[i + 1].size = end - s - size;
        } else if (s > from) {
            b->free[i].size = s - from;
        } else if (s + size < end) {
            b->free[i].off = s + size;
            b->free[i].size = end - s - size;
        } else {
            memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof b->free[0]);
            b->nfree--;
        }
        *off = s;
        return 1;
    }
    return 0;
}

static void img_free_in(ImgBlock *b, uint32_t off, uint32_t size)
{
    uint32_t i = 0;
    while (i < b->nfree && b->free[i].off < off) i++;
    int merge_prev = i > 0 && b->free[i - 1].off + b->free[i - 1].size == off;
    int merge_next = i < b->nfree && off + size == b->free[i].off;
    if (merge_prev && merge_next) {
        b->free[i - 1].size += size + b->free[i].size;
        memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof b->free[0]);
        b->nfree--;
    } else if (merge_prev) {
        b->free[i - 1].size += size;
    } else if (merge_next) {
        b->free[i].off = off;
        b->free[i].size += size;
    } else if (b->nfree < MAX_RANGES) {
        memmove(&b->free[i + 1], &b->free[i], (b->nfree - i) * sizeof b->free[0]);
        b->free[i].off = off;
        b->free[i].size = size;
        b->nfree++;
    }
}

static DkDevice device;
static DkQueue queue;

static int img_alloc(uint32_t size, uint32_t align, int *blk, uint32_t *off)
{
    for (int i = 0; i < n_img_blocks; i++)
        if (img_alloc_in(img_blocks[i], size, align, off)) { *blk = i; return 1; }
    if (n_img_blocks == MAX_IMAGE_BLOCKS) return 0;
    uint32_t bsize = size > IMAGE_BLOCK ? (uint32_t)ALIGN(size, DK_MEMBLOCK_ALIGNMENT) : IMAGE_BLOCK;
    DkMemBlockMaker mk;
    dkMemBlockMakerDefaults(&mk, device, bsize);
    mk.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock mb = dkMemBlockCreate(&mk);
    if (!mb) return 0;
    ImgBlock *b = calloc(1, sizeof *b);
    b->block = mb;
    b->nfree = 1;
    b->free[0].off = 0;
    b->free[0].size = bsize;
    img_blocks[n_img_blocks] = b;
    rt_log("[gpu] image block %d: %u MiB\n", n_img_blocks, bsize >> 20);
    if (!img_alloc_in(b, size, align, off)) return 0;
    *blk = n_img_blocks++;
    return 1;
}

/* data memory: CPU-written, GPU-read; carved once at start. Three frame slots (and three swapchain images): the
 * CPU may run two frames ahead of the GPU, so a frame that takes the GPU a little over a vblank doesn't make the
 * next one wait (the menu's page turns stuttered at 2 slots: BeginScene's fence wait was ~40% of a frame) */
#define SLOTS 3
#define NFB 3
#define CMD_BYTES (4u << 20)
#define VTX_BYTES (8u << 20)
#define STAGE_BYTES (24u << 20)
#define MAX_TEX 8192
#define MAX_SAMPLERS 32

static DkMemBlock data_block, code_block;
static uint8_t *data_cpu;
static DkGpuAddr data_gpu;
static uint32_t data_used;

static uint32_t data_carve(uint32_t size, uint32_t align)
{
    uint32_t o = (uint32_t)ALIGN(data_used, align);
    data_used = o + size;
    return o;
}

/* ---------------- textures ---------------- */
struct GpuTex {
    DkImage image;
    DkImageView view;
    uint32_t w, h, slot;
    int blk;
    uint32_t off, size;
    uint64_t freed_frame;
    struct GpuTex *next;
};

static DkImageDescriptor *img_desc;
static DkSamplerDescriptor *smp_desc;
static uint32_t img_desc_off, smp_desc_off;
static uint32_t next_slot = 1;
static uint32_t free_slots[MAX_TEX];
static int n_free_slots;
static int descriptors_dirty = 1;

static pthread_mutex_t free_lock = PTHREAD_MUTEX_INITIALIZER;
static GpuTex *pending_free; /* tex_free from any thread */
static GpuTex *retiring;     /* waiting SLOTS + 1 frames */
static uint64_t frame_no;
static pthread_t gpu_thread;

/* ---------------- frames ---------------- */
typedef struct {
    uint32_t cmd_off, vtx_off, stage_off;
    uint32_t vtx_used, stage_used;
    DkFence fence;
    int submitted;
} Slot;
static Slot slots[SLOTS];
static int cur;
static int frame_open;
static DkCmdBuf cmd;

static DkShader sh_draw_v, sh_draw_f, sh_present_v, sh_present_f;
static uint32_t ubo_v_off, ubo_f_off;

/* queued draws */
static GpuState qstate;
static uint32_t q_first, q_count; /* vertices in the current slot's ring */
static GpuTex *bound_target;

static void on_cmd_full(void *user, DkCmdBuf cb, size_t min)
{
    (void)user;
    (void)cb;
    rt_fatal("[gpu] command memory full (%u bytes more wanted)", (unsigned)min);
}

static void common_state(void)
{
    DkShader const *shaders[] = {&sh_draw_v, &sh_draw_f};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &rs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(cmd, &cw);
    DkVtxAttribState attribs[3] = {
        {.bufferId = 0, .isFixed = 0, .offset = 0, .size = DkVtxAttribSize_2x32, .type = DkVtxAttribType_Float},
        {.bufferId = 0, .isFixed = 0, .offset = 8, .size = DkVtxAttribSize_2x32, .type = DkVtxAttribType_Float},
        {.bufferId = 0, .isFixed = 0, .offset = 16, .size = DkVtxAttribSize_4x8, .type = DkVtxAttribType_Unorm},
    };
    dkCmdBufBindVtxAttribState(cmd, attribs, 3);
    DkVtxBufferState vb = {sizeof(GpuVtx), 0};
    dkCmdBufBindVtxBufferState(cmd, &vb, 1);
    dkCmdBufBindImageDescriptorSet(cmd, data_gpu + img_desc_off, MAX_TEX);
    dkCmdBufBindSamplerDescriptorSet(cmd, data_gpu + smp_desc_off, MAX_SAMPLERS);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, data_gpu + ubo_v_off, DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, data_gpu + ubo_f_off, DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors | DkInvalidateFlags_Image);
    descriptors_dirty = 0;
    bound_target = NULL;
}

static void reap(void)
{
    pthread_mutex_lock(&free_lock);
    GpuTex *t = pending_free;
    pending_free = NULL;
    pthread_mutex_unlock(&free_lock);
    while (t) {
        GpuTex *n = t->next;
        t->freed_frame = frame_no;
        t->next = retiring;
        retiring = t;
        t = n;
    }
    GpuTex **pp = &retiring;
    while (*pp) {
        GpuTex *r = *pp;
        if (frame_no - r->freed_frame >= SLOTS + 1) {
            *pp = r->next;
            img_free_in(img_blocks[r->blk], r->off, r->size);
            free_slots[n_free_slots++] = r->slot;
            free(r);
        } else {
            pp = &r->next;
        }
    }
}

/* the GPU state flush() last set; forgotten whenever someone else sets state (a new command list, the
 * present) */
static int have_last;

static void begin(void)
{
    if (frame_open) return;
    Slot *s = &slots[cur];
    if (s->submitted) dkFenceWait(&s->fence, -1);
    s->submitted = 0;
    s->vtx_used = s->stage_used = 0;
    dkCmdBufClear(cmd);
    dkCmdBufAddMemory(cmd, data_block, s->cmd_off, CMD_BYTES);
    frame_open = 1;
    q_count = 0;
    reap();
    common_state();
    have_last = 0;
}

static void submit(void)
{
    if (!frame_open) return;
    Slot *s = &slots[cur];
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmd));
    dkQueueSignalFence(queue, &s->fence, true);
    dkQueueFlush(queue);
    s->submitted = 1;
    frame_open = 0;
    cur = (cur + 1) % SLOTS;
    frame_no++;
}

/* ---------------- draws ---------------- */
static uint32_t sampler_index(int linear, uint32_t au, uint32_t av)
{
    /* [linear][u][v], D3DTADDRESS 1 wrap, 2 mirror, 3 clamp -> 0..2 */
    au = au >= 1 && au <= 3 ? au - 1 : 2;
    av = av >= 1 && av <= 3 ? av - 1 : 2;
    return (uint32_t)linear * 9 + au * 3 + av;
}

static void bind_target(GpuTex *t)
{
    if (bound_target == t) return;
    /* whatever was drawn before may be sampled next */
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    dkCmdBufBindRenderTarget(cmd, &t->view, NULL);
    DkViewport vp = {0, 0, (float)t->w, (float)t->h, 0, 1};
    dkCmdBufSetViewports(cmd, 0, &vp, 1);
    bound_target = t;
}

static void flush(void)
{
    if (!q_count) return;
    const GpuState *st = &qstate;
    if (descriptors_dirty) {
        dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        descriptors_dirty = 0;
    }
    bind_target(st->target);
    /* only what changed since the last draw goes into the command buffer: the game changes texture on
     * nearly every draw (each glyph, each card) and little else, and re-sending the whole state each time
     * was most of a draw's cost on the console */
    static GpuState last;
    static DkResHandle last_tex;
    if (!have_last || last.clip_x != st->clip_x || last.clip_y != st->clip_y || last.clip_w != st->clip_w ||
        last.clip_h != st->clip_h) {
        DkScissor sc = {(uint32_t)st->clip_x, (uint32_t)st->clip_y, (uint32_t)st->clip_w, (uint32_t)st->clip_h};
        dkCmdBufSetScissors(cmd, 0, &sc, 1);
    }
    int tex_on = st->tex && st->cop != 1;
    if (st->tex) {
        DkResHandle h = dkMakeTextureHandle(st->tex->slot, sampler_index(st->linear, st->address_u, st->address_v));
        if (!have_last || h != last_tex) dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &h, 1);
        last_tex = h;
    }
    int blend_same = have_last && last.blend == st->blend && last.src_blend == st->src_blend &&
                     last.dst_blend == st->dst_blend && last.target_alpha == st->target_alpha;
    if (!have_last || last.blend != st->blend) {
        DkColorState cs;
        dkColorStateDefaults(&cs);
        dkColorStateSetBlendEnable(&cs, 0, st->blend != 0);
        dkCmdBufBindColorState(cmd, &cs);
    }
    if (st->blend && !blend_same) {
        uint32_t sb = st->src_blend, db = st->dst_blend;
        if (sb == 12) sb = 5, db = 6;      /* BOTHSRCALPHA */
        else if (sb == 13) sb = 6, db = 5; /* BOTHINVSRCALPHA */
        /* D3DBLEND numbers are DkBlendFactor's; the destination's alpha reads 1 on X8R8G8B8 */
        if (!st->target_alpha) {
            if (sb == 7) sb = 2; else if (sb == 8) sb = 1; else if (sb == 11) sb = 1;
            if (db == 7) db = 2; else if (db == 8) db = 1; else if (db == 11) db = 1;
        }
        if (sb < 1 || sb > 11) sb = 2;
        if (db < 1 || db > 11) db = 2;
        DkBlendState bs;
        dkBlendStateDefaults(&bs);
        dkBlendStateSetFactors(&bs, (DkBlendFactor)sb, (DkBlendFactor)db, (DkBlendFactor)sb, (DkBlendFactor)db);
        dkCmdBufBindBlendStates(cmd, 0, &bs, 1);
    }
    float v[4] = {(float)st->target->w, (float)st->target->h, 0, 0};
    static float last_v[4];
    if (!have_last || memcmp(v, last_v, sizeof v)) {
        dkCmdBufPushConstants(cmd, data_gpu + ubo_v_off, DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof v, v);
        memcpy(last_v, v, sizeof v);
    }
    struct {
        int32_t cop[4], aop[4], test[4];
        float tf[4], ck_lo[4], ck_hi[4], aref[4];
    } f = {
        {(int32_t)st->cop, (int32_t)st->ca1, (int32_t)st->ca2, tex_on},
        {(int32_t)st->aop, (int32_t)st->aa1, (int32_t)st->aa2, st->tex_alpha},
        {st->alpha_test ? (int32_t)st->alpha_func : 0, st->colorkey, 0, 0},
        {(float)((st->tfactor >> 16) & 255) / 255.f, (float)((st->tfactor >> 8) & 255) / 255.f,
         (float)(st->tfactor & 255) / 255.f, (float)(st->tfactor >> 24) / 255.f},
        {(float)((st->ck_lo >> 16) & 255), (float)((st->ck_lo >> 8) & 255), (float)(st->ck_lo & 255), 0},
        {(float)((st->ck_hi >> 16) & 255), (float)((st->ck_hi >> 8) & 255), (float)(st->ck_hi & 255), 0},
        {(float)(st->alpha_ref & 255), 0, 0, 0},
    };
    static uint8_t last_f[sizeof f];
    if (!have_last || memcmp(&f, last_f, sizeof f)) {
        dkCmdBufPushConstants(cmd, data_gpu + ubo_f_off, DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof f, &f);
        memcpy(last_f, &f, sizeof f);
    }
    last = *st;
    have_last = 1;
    Slot *s = &slots[cur];
    dkCmdBufBindVtxBuffer(cmd, 0, data_gpu + s->vtx_off + q_first * sizeof(GpuVtx), q_count * sizeof(GpuVtx));
    dkCmdBufDraw(cmd, st->lines ? DkPrimitive_Lines : DkPrimitive_Triangles, q_count, 1, 0, 0);
    q_first += q_count;
    q_count = 0;
}

static void draw(const GpuState *st, const GpuVtx *v, uint32_t n)
{
    begin();
    Slot *s = &slots[cur];
    uint32_t cap = VTX_BYTES / sizeof(GpuVtx);
    if (s->vtx_used + n > cap) {
        /* a frame with more vertices than the ring holds: send what we have and start over */
        flush();
        submit();
        begin();
        s = &slots[cur];
    }
    if (q_count && (memcmp(st, &qstate, sizeof *st) || q_first + q_count != s->vtx_used)) flush();
    if (!q_count) {
        qstate = *st;
        q_first = s->vtx_used;
    }
    memcpy(data_cpu + s->vtx_off + s->vtx_used * sizeof(GpuVtx), v, n * sizeof(GpuVtx));
    s->vtx_used += n;
    q_count += n;
}

static void fill(GpuTex *t, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t argb)
{
    GpuState st;
    memset(&st, 0, sizeof st);
    st.target = t;
    st.cop = 2; st.ca1 = 0; st.ca2 = 0; /* SELECTARG1(DIFFUSE) */
    st.aop = 2; st.aa1 = 0; st.aa2 = 0;
    st.target_alpha = 1;
    st.clip_x = x; st.clip_y = y; st.clip_w = w; st.clip_h = h;
    float x0 = (float)x - 0.5f, y0 = (float)y - 0.5f, x1 = (float)(x + w) - 0.5f, y1 = (float)(y + h) - 0.5f;
    GpuVtx q[6] = {{x0, y0, 0, 0, argb}, {x1, y0, 0, 0, argb}, {x0, y1, 0, 0, argb},
                   {x1, y0, 0, 0, argb}, {x1, y1, 0, 0, argb}, {x0, y1, 0, 0, argb}};
    draw(&st, q, 6);
}

/* ---------------- textures ---------------- */
static GpuTex *tex_new(uint32_t w, uint32_t h)
{
    GpuTex *t = calloc(1, sizeof *t);
    t->w = w;
    t->h = h;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_BGRA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    t->size = (uint32_t)ALIGN(dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));
    if (!img_alloc(t->size, dkImageLayoutGetAlignment(&layout), &t->blk, &t->off))
        rt_fatal("[gpu] out of image memory (%ux%u)", (unsigned)w, (unsigned)h);
    dkImageInitialize(&t->image, &layout, img_blocks[t->blk]->block, t->off);
    dkImageViewDefaults(&t->view, &t->image);
    if (n_free_slots) t->slot = free_slots[--n_free_slots];
    else if (next_slot < MAX_TEX) t->slot = next_slot++;
    else rt_fatal("[gpu] more than %d textures", MAX_TEX);
    dkImageDescriptorInitialize(&img_desc[t->slot], &t->view, false, false);
    descriptors_dirty = 1;
    return t;
}

static void tex_free(GpuTex *t)
{
    if (!t) return;
    pthread_mutex_lock(&free_lock);
    t->next = pending_free;
    pending_free = t;
    pthread_mutex_unlock(&free_lock);
}

/* rows [y0, y1) through the current slot's staging area */
static void upload_rows(GpuTex *t, const uint8_t *pixels, uint32_t pitch, uint32_t y0, uint32_t y1)
{
    Slot *s = &slots[cur];
    uint32_t row = t->w * 4, n = y1 - y0;
    uint32_t at = (uint32_t)ALIGN(s->stage_used, 256);
    uint8_t *dst = data_cpu + s->stage_off + at;
    for (uint32_t y = 0; y < n; y++) memcpy(dst + (size_t)y * row, pixels + (size_t)(y0 + y) * pitch, row);
    s->stage_used = at + n * row;
    DkCopyBuf src = {data_gpu + s->stage_off + at, row, n};
    DkImageRect r = {0, y0, 0, t->w, n, 1};
    dkCmdBufCopyBufferToImage(cmd, &src, &t->view, &r, 0);
}

static void tex_upload(GpuTex *t, const void *pixels, uint32_t pitch)
{
    begin();
    flush();
    /* draws before this read the old contents */
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    uint32_t row = t->w * 4, y = 0;
    while (y < t->h) {
        Slot *s = &slots[cur];
        uint32_t room = STAGE_BYTES - (uint32_t)ALIGN(s->stage_used, 256);
        uint32_t rows = room / row;
        if (!rows) {
            submit();
            begin();
            dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
            continue;
        }
        if (rows > t->h - y) rows = t->h - y;
        upload_rows(t, pixels, pitch, y, y + rows);
        y += rows;
    }
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
}

static void tex_download(GpuTex *t, void *pixels, uint32_t pitch)
{
    begin();
    flush();
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    uint32_t row = t->w * 4, y = 0;
    while (y < t->h) {
        Slot *s = &slots[cur];
        uint32_t at = (uint32_t)ALIGN(s->stage_used, 256);
        uint32_t rows = (STAGE_BYTES - at) / row;
        if (!rows) {
            submit();
            begin();
            continue;
        }
        if (rows > t->h - y) rows = t->h - y;
        DkImageRect r = {0, y, 0, t->w, rows, 1};
        DkCopyBuf dst = {data_gpu + s->stage_off + at, row, rows};
        dkCmdBufCopyImageToBuffer(cmd, &t->view, &r, &dst, 0);
        s->stage_used = at + rows * row;
        submit();
        dkQueueWaitIdle(queue);
        const uint8_t *src = data_cpu + slots[(cur + SLOTS - 1) % SLOTS].stage_off + at;
        for (uint32_t k = 0; k < rows; k++) memcpy((uint8_t *)pixels + (size_t)(y + k) * pitch, src + (size_t)k * row, row);
        y += rows;
        begin();
    }
    static int logged;
    if (rt_verbose && logged < 12) {
        logged++;
        const uint32_t *px = pixels;
        rt_log("[gpu] readback %ux%u: (0,0) %08x (5,5) %08x (w/2,h/2) %08x\n", (unsigned)t->w, (unsigned)t->h,
               (unsigned)px[0], (unsigned)((const uint32_t *)((const uint8_t *)pixels + 5 * pitch))[5],
               (unsigned)((const uint32_t *)((const uint8_t *)pixels + (t->h / 2) * pitch))[t->w / 2]);
    }
}

/* ---------------- present ---------------- */
static GpuTex *shown;
static DkMemBlock fb_block;
static DkImage fb_images[NFB];
static DkImageView fb_views[NFB];
static DkSwapchain swapchain;
static GpuTex *white;

static void show(GpuTex *t) { shown = t; }
static int on_gpu_thread(void) { return pthread_equal(pthread_self(), gpu_thread); }

static const GpuBackend backend = {"deko3d", tex_new, tex_free, tex_upload, tex_download, draw, fill, show, on_gpu_thread};

/* the controller cursor: an arrow, black outline then white */
static void cursor_tris(GpuVtx *v, int *n, float x, float y, float k, uint32_t c)
{
    static const float a[][2] = {{0, 0}, {0, 16}, {4, 12}, {7, 18}, {9, 17}, {6, 11}, {11, 11}};
    static const int tris[][3] = {{0, 1, 2}, {0, 2, 5}, {0, 5, 6}, {2, 3, 4}, {2, 4, 5}};
    for (unsigned i = 0; i < sizeof tris / sizeof *tris; i++)
        for (int j = 0; j < 3; j++) {
            GpuVtx *p = &v[(*n)++];
            p->x = x + a[tris[i][j]][0] * k;
            p->y = y + a[tris[i][j]][1] * k;
            p->u = p->v = 0;
            p->argb = c;
        }
}

/* test aid: with a file "shot" in the game folder, the display 15 s after it opens goes to shot.bmp */
static void debug_shot(GpuTex *t)
{
    static int checked, want, n;
    if (!checked) {
        checked = 1;
        char p[600];
        snprintf(p, sizeof p, "%s/shot", rt_game_dir);
        FILE *f = fopen(p, "rb");
        want = f != NULL;
        if (f) fclose(f);
    }
    if (!want || !t || ++n != 900) return;
    uint32_t w = t->w, h = t->h;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    tex_download(t, px, w * 4);
    char p[600];
    snprintf(p, sizeof p, "%s/shot.bmp", rt_game_dir);
    FILE *f = fopen(p, "wb");
    if (f) {
        uint8_t hd[54] = {'B', 'M'};
        uint32_t sz = 54 + w * h * 4;
        memcpy(hd + 2, &sz, 4);
        hd[10] = 54, hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        int32_t nh = -(int32_t)h;
        memcpy(hd + 22, &nh, 4);
        hd[26] = 1, hd[28] = 32;
        fwrite(hd, 1, 54, f);
        fwrite(px, 4, (size_t)w * h, f);
        fclose(f);
        rt_log("[gpu] debug shot written\n");
    }
    free(px);
}

void gpu_dk_present(int cursor, int cx, int cy)
{
    begin();
    GpuTex *game = shown;
    debug_shot(shown);
    /* (what follows sets its own scissor, shaders and constants) */
    if (shown && ov_active()) shown = ov_compose(shown, shown->w, shown->h, draw); /* the port's pictures over it */
    flush();
    int img = dkQueueAcquireImage(queue, swapchain);
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    dkCmdBufBindRenderTarget(cmd, &fb_views[img], NULL);
    bound_target = NULL;
    DkScissor sc = {0, 0, 1280, 720};
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkCmdBufBindColorState(cmd, &cs);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    float k = 1, ox = 0, oy = 0;
    if (shown) {
        /* the display at its own aspect ratio, centred (pillar- or letterboxed) */
        k = 1280.f / (float)shown->w < 720.f / (float)shown->h ? 1280.f / (float)shown->w : 720.f / (float)shown->h;
        float w = (float)shown->w * k, h = (float)shown->h * k;
        ox = (1280.f - w) / 2;
        oy = (720.f - h) / 2;
        DkViewport vp = {ox, oy, w, h, 0, 1};
        dkCmdBufSetViewports(cmd, 0, &vp, 1);
        DkShader const *ps[] = {&sh_present_v, &sh_present_f};
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, ps, 2);
        DkResHandle th = dkMakeTextureHandle(shown->slot, sampler_index(1, 3, 3));
        dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &th, 1);
        dkCmdBufDraw(cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
    }
    if (cursor) {
        /* the controller's pointer, over the picture (never into the game's own surfaces: their unpainted
         * parts, such as the bars beside a 4:3 picture, would keep old arrows) */
        GpuVtx v[60];
        int n = 0;
        float sx = ox + (float)cx * k, sy = oy + (float)cy * k;
        cursor_tris(v, &n, sx - 1.5f, sy - 1.5f, 1.35f, 0xff000000u);
        cursor_tris(v, &n, sx, sy, 1.0f, 0xffffffffu);
        Slot *s = &slots[cur];
        if ((s->vtx_used + (uint32_t)n) * sizeof(GpuVtx) <= VTX_BYTES) {
            memcpy(data_cpu + s->vtx_off + s->vtx_used * sizeof(GpuVtx), v, (size_t)n * sizeof(GpuVtx));
            DkViewport vp = {0, 0, 1280, 720, 0, 1};
            dkCmdBufSetViewports(cmd, 0, &vp, 1);
            DkShader const *ds[] = {&sh_draw_v, &sh_draw_f};
            dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, ds, 2);
            float vs[4] = {1280.f, 720.f, 0, 0};
            dkCmdBufPushConstants(cmd, data_gpu + ubo_v_off, DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof vs, vs);
            int32_t fs[28] = {2, 0, 0, 0, 2, 0, 0, 0}; /* SELECTARG1(DIFFUSE) for colour and alpha, no texture */
            dkCmdBufPushConstants(cmd, data_gpu + ubo_f_off, DK_UNIFORM_BUF_ALIGNMENT, 0, sizeof fs, fs);
            dkCmdBufBindVtxBuffer(cmd, 0, data_gpu + s->vtx_off + s->vtx_used * sizeof(GpuVtx), (uint32_t)n * sizeof(GpuVtx));
            dkCmdBufDraw(cmd, DkPrimitive_Triangles, (uint32_t)n, 1, 0, 0);
            s->vtx_used += (uint32_t)n;
        }
    }
    submit();
    dkQueuePresentImage(queue, swapchain, img);
    if (shown) saltynx_present(shown->w, shown->h); /* (the overlays' FPS and resolution: the game's picture) */
    shown = game;
    have_last = 0;
}

static void load_shader(DkShader *sh, const uint8_t *code, uint32_t size, uint32_t *code_used)
{
    uint8_t *dst = (uint8_t *)dkMemBlockGetCpuAddr(code_block) + *code_used;
    memcpy(dst, code, size);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, code_block, *code_used);
    dkShaderInitialize(sh, &m);
    *code_used = (uint32_t)ALIGN(*code_used + size, DK_SHADER_CODE_ALIGNMENT);
}

/* On the game's main thread, which owns the GPU from then on */
void gpu_dk_init(void)
{
    gpu_thread = pthread_self();
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    device = dkDeviceCreate(&dm);

    /* data: command memory, vertex rings, staging, descriptors, uniform buffers */
    uint32_t data_size = (uint32_t)ALIGN(SLOTS * (CMD_BYTES + VTX_BYTES + STAGE_BYTES) + (1u << 20), DK_MEMBLOCK_ALIGNMENT);
    DkMemBlockMaker mk;
    dkMemBlockMakerDefaults(&mk, device, data_size);
    mk.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    data_block = dkMemBlockCreate(&mk);
    data_cpu = dkMemBlockGetCpuAddr(data_block);
    data_gpu = dkMemBlockGetGpuAddr(data_block);
    for (int i = 0; i < SLOTS; i++) {
        slots[i].cmd_off = data_carve(CMD_BYTES, DK_CMDMEM_ALIGNMENT);
        slots[i].vtx_off = data_carve(VTX_BYTES, 256);
        slots[i].stage_off = data_carve(STAGE_BYTES, 256);
    }
    img_desc_off = data_carve(MAX_TEX * sizeof(DkImageDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    smp_desc_off = data_carve(MAX_SAMPLERS * sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
    ubo_v_off = data_carve(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
    ubo_f_off = data_carve(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
    img_desc = (DkImageDescriptor *)(data_cpu + img_desc_off);
    smp_desc = (DkSamplerDescriptor *)(data_cpu + smp_desc_off);

    /* samplers [linear][u][v] */
    static const DkWrapMode wrap[3] = {DkWrapMode_Repeat, DkWrapMode_MirroredRepeat, DkWrapMode_ClampToEdge};
    for (int l = 0; l < 2; l++)
        for (int u = 0; u < 3; u++)
            for (int v = 0; v < 3; v++) {
                DkSampler s;
                dkSamplerDefaults(&s);
                s.minFilter = s.magFilter = l ? DkFilter_Linear : DkFilter_Nearest;
                s.wrapMode[0] = wrap[u];
                s.wrapMode[1] = wrap[v];
                s.wrapMode[2] = DkWrapMode_ClampToEdge;
                dkSamplerDescriptorInitialize(&smp_desc[l * 9 + u * 3 + v], &s);
            }

    /* shaders */
    dkMemBlockMakerDefaults(&mk, device, 64u << 10);
    mk.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    code_block = dkMemBlockCreate(&mk);
    uint32_t code_used = 0;
    load_shader(&sh_draw_v, draw_vsh_dksh, draw_vsh_dksh_size, &code_used);
    load_shader(&sh_draw_f, draw_fsh_dksh, draw_fsh_dksh_size, &code_used);
    load_shader(&sh_present_v, present_vsh_dksh, present_vsh_dksh_size, &code_used);
    load_shader(&sh_present_f, present_fsh_dksh, present_fsh_dksh_size, &code_used);

    /* queue and command buffer */
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    qm.flags = DkQueueFlags_Graphics;
    queue = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device);
    cm.cbAddMem = on_cmd_full;
    cmd = dkCmdBufCreate(&cm);

    /* swapchain: NFB 1280x720 RGBA8 images */
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = 1280;
    lm.dimensions[1] = 720;
    DkImageLayout fl;
    dkImageLayoutInitialize(&fl, &lm);
    uint32_t fsize = (uint32_t)ALIGN(dkImageLayoutGetSize(&fl), dkImageLayoutGetAlignment(&fl));
    dkMemBlockMakerDefaults(&mk, device, (uint32_t)ALIGN(NFB * fsize, DK_MEMBLOCK_ALIGNMENT));
    mk.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    fb_block = dkMemBlockCreate(&mk);
    DkImage const *fbs[NFB];
    for (int i = 0; i < NFB; i++) {
        dkImageInitialize(&fb_images[i], &fl, fb_block, (uint32_t)i * fsize);
        dkImageViewDefaults(&fb_views[i], &fb_images[i]);
        fbs[i] = &fb_images[i];
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device, nwindowGetDefault(), fbs, NFB);
    swapchain = dkSwapchainCreate(&sm);
    dkSwapchainSetSwapInterval(swapchain, 1);

    rt_gpu = &backend;
    (void)white;
    rt_log("[gpu] deko3d renderer ready\n");
}

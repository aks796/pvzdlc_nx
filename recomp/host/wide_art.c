/* Widescreen pictures (rt_widescreen): what fills the 133 px beside the game's 4:3 screens.
 *
 * Pictures come from files in the game folder's wide/ (loaded once through the game's own
 * SexyAppBase::GetImage, so they are ordinary game images) and are drawn with the game's own calls into
 * the screen's Graphics, before or between its own drawing, so they sit under what the screen draws
 * (clouds, buttons, the logo).
 *
 * Title screen: the game's titlescreen.png zoomed 4/3 and cropped as the 16:9 key art is (top 150 px of
 * the zoomed picture off), with PvZ Touch's blinking Potato Mine light (its titlescreen_ball / _glow and
 * its timing: TitleScreen::Draw in libGameMain.so). */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"
int game_rtti_is(uint32_t obj, const char *want);

extern uint32_t rt_img_delta;
#define GFN(va) ((va) + rt_img_delta)
#define APP_GET_IMAGE 0xa91ef0u   /* SexyAppBase::GetImage(const std::string &, bool commit) -> DDImage * */
#define DRAW_SCALED_F 0x9c9640u   /* TodDrawImageScaledF(Graphics *, Image *, x, y, sx, sy), cdecl */
#define STR_CTOR 0x72a830u        /* std::string(const char *) */
#define STR_DTOR 0x72cd50u
#define IMG_TITLESCREEN 0x1093f70u /* Image *IMAGE_TITLESCREEN */

static uint32_t call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args)
{
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx;
    c->ecx = self;
    uint32_t r = rt_guest_call(c, GFN(fn), n, args);
    c->eax = eax, c->ecx = ecx, c->edx = edx;
    return r;
}

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* wide/<name>.png as a game image, 0 when it isn't there (looked for once) */
typedef struct {
    const char *name;
    uint32_t img;
    int tried;
} Pic;

static uint32_t pic(CPU *c, Pic *p)
{
    if (p->tried) return p->img;
    p->tried = 1;
    uint32_t app = rt_app_object();
    if (!app) return 0;
    uint32_t save = c->esp, base = (c->esp - 0x400) & ~15u;
    char path[96];
    int n = snprintf(path, sizeof path, strchr(p->name, '.') ? "wide/%s" : "wide/%s.png", p->name);
    memcpy(G2H(base + 32), path, (size_t)n + 1);
    c->esp = base; /* the calls use the stack below the scratch string */
    uint32_t text = base + 32;
    call(c, STR_CTOR, base, 1, &text);
    uint32_t args[2] = {base, 1};
    p->img = call(c, APP_GET_IMAGE, app, 2, args);
    call(c, STR_DTOR, base, 0, NULL);
    c->esp = save;
    rt_log("[wide] %s: %s %dx%d\n", path, p->img ? "loaded" : "missing", p->img ? (int)g_rd32(p->img + 0x24) : 0, p->img ? (int)g_rd32(p->img + 0x28) : 0);
    return p->img;
}

/* one of the port's pictures (wide/NAME.png) as a game image, for pad_input */
uint32_t wide_image(const char *name)
{
    static Pic pics[4];
    RtThread *t = rt_thread_self();
    if (!t) return 0;
    for (int i = 0; i < 4; i++) {
        if (!pics[i].name) pics[i].name = name;
        if (!strcmp(pics[i].name, name)) return pic(&t->cpu, &pics[i]);
    }
    return 0;
}

static void draw_scaled(CPU *c, uint32_t g, uint32_t img, float x, float y, float sx, float sy)
{
    if (!img) return;
    uint32_t args[6] = {g, img, f2u(x), f2u(y), f2u(sx), f2u(sy)};
    call(c, DRAW_SCALED_F, c->ecx, 6, args);
}

/* Graphics: +0x30 mColor (r, g, b, a as ints), +0x48 mColorizeImages */
static void draw_alpha(CPU *c, uint32_t g, uint32_t img, float x, float y, float s, int alpha)
{
    uint32_t col[4], colorize = *(uint8_t *)G2H(g + 0x48);
    for (int i = 0; i < 4; i++) col[i] = g_rd32(g + 0x30 + 4 * i);
    g_wr32(g + 0x30, 255), g_wr32(g + 0x34, 255), g_wr32(g + 0x38, 255), g_wr32(g + 0x3c, (uint32_t)alpha);
    *(uint8_t *)G2H(g + 0x48) = 1;
    draw_scaled(c, g, img, x, y, s, s);
    for (int i = 0; i < 4; i++) g_wr32(g + 0x30 + 4 * i, col[i]);
    *(uint8_t *)G2H(g + 0x48) = (uint8_t)colorize;
}

/* ---------------- title screen ---------------- */
#define TITLE_ZOOM (4.f / 3.f)
#define TITLE_X (-400.f / 3.f) /* the zoomed picture's left edge: centred on the 800 */
#define TITLE_Y (-150.f)       /* and its top: the key art's crop */
static Pic title_ball = {"titlescreen_ball", 0, 0}, title_glow = {"titlescreen_glow", 0, 0};

/* TitleScreen::Draw about to draw its background (esi = the Graphics): draw it zoomed, then the light.
 * Returns 1 to skip the game's own background draw (both its plain and its scaled one). */
int game_hook_title_bg(CPU *c)
{
    if (!rt_widescreen) return 0;
    uint32_t g = c->esi, bg = g_rd32(GFN(IMG_TITLESCREEN));
    if (!bg) return 0;
    draw_scaled(c, g, bg, TITLE_X, TITLE_Y, TITLE_ZOOM, TITLE_ZOOM);
    /* PvZ Touch's light: v = 0.5 + 0.5 sin(age * 0.035) with age in 1/100 s; lit while v > 0.7, the ball
     * opaque and the glow at alpha (v - 0.5) * 255. On this picture (PvZ Touch's own measure of the
     * mine's knob) the ball sits at (422, 271) and the glow at (333, 178) at 0.834 of their size. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double age = (double)ts.tv_sec * 100.0 + (double)ts.tv_nsec / 1e7;
    float v = (float)(0.5 + 0.5 * sin(fmod(age * 0.035, 2.0 * M_PI)));
    if (v > 0.7f) {
        uint32_t ball = pic(c, &title_ball), glow = pic(c, &title_glow);
        float s = 0.834f * TITLE_ZOOM;
        draw_scaled(c, g, ball, TITLE_X + 422.f * TITLE_ZOOM, TITLE_Y + 271.f * TITLE_ZOOM, s, s);
        draw_alpha(c, g, glow, TITLE_X + 333.f * TITLE_ZOOM, TITLE_Y + 178.f * TITLE_ZOOM, s, (int)((v - 0.5f) * 255.f));
    }
    return 1;
}

/* ---------------- main menu ----------------
 * Page 1 is the menu reanim's pieces, drawn through DDImage::BltMatrix: the sky (100x75 at 8x), the tree
 * (SelectorScreen_BG_Left, 476x680) and the tombstone hill (SelectorScreen_BG_Right, 730x560, ending at
 * x 800 where page 2 begins). Each draw is caught as it happens, so what is added follows the piece
 * exactly (its intro slide, the page slides):
 *  - the sky gets a mirrored copy on its left (seamless at the edge), drawn before it, so the clouds
 *    still pass over;
 *  - the tree gets the widescreen mod's whole tree (SelectorScreen_BG_Left, 849x860: the DLC's piece is
 *    its right part, at 0.9925 scale, 370,56 in);
 *  - the hill is remembered for the junction with page 2 (below).
 * Page 2 (SideScreen) gets its own right edge continued (wide/menu_page2_right.png, tools/make_page2_strip.py:
 * the dead tree mirrored, the wall carried on towards the viewer, the mail pot covered with its own grass).
 * The junction is in day light (the mod's whole hill, SelectorScreen_BG_Right 1169x925: the DLC's piece at
 * 0.9875, 102,72 in, and the mirrored day sky, over page 2's left edge); on page 2 that light fades into
 * page 2's night across the edge. */
#define DDIMAGE_BLT_MATRIX 0x9ed400u /* DDImage::BltMatrix(Image *, x, y, const SexyMatrix3 &, const Rect &clip,
                                      * const Color &, int mode, const Rect &src, bool blend) */
static Pic menu_tree = {"hj_SelectorScreen_BG_Left", 0, 0}, page2_right = {"menu_page2_right", 0, 0},
           page2_day = {"menu_page2_day", 0, 0}, page2_mound = {"menu_page2_mound", 0, 0},
           page2_brick = {"menu_page2_brick", 0, 0}, /* its first brick moved to meet the tombstone's lines */
           menu_hill = {"menu_hill", 0, 0}, menu_leaves = {"menu_leaves", 0, 0};
static uint32_t frame_no, menu_frame, page2_frame, board_frame;
static float page2_x; /* page 2's left edge on the screen this frame (800 on page 1, 0 on page 2) */
static int in_blt;
typedef struct {
    uint32_t frame, img, dest, x, y, mode, blend;
    float m[9];
    int32_t clip[4], col[4];
} Rec;
static Rec rec_sky, rec_hill;

/* a frame was shown (Flip): what follows belongs to the next (the DLC begins several scenes per frame) */
void wide_frame_end(void) { frame_no++; }

/* GameSelector::Draw: the menu is on screen this frame (it draws the sky; its pieces come later) */
/* The DLC's own plants (the plant select's later pages) build their reanim atlases the first time their
 * packets are drawn: 20-110 ms each, so the first ZR there stalled about a second and a half. They are
 * built while the main menu sits idle instead, one every few frames (ReanimationPreload 0x9c2ef0). */
static const uint8_t dlc_plant_reanims[] = {150, 152, 153, 155, 156, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 171,
                                            173, 176, 177, 180, 181, 183, 186, 187, 189, 190, 191, 192, 193, 194, 212, 223,
                                            224, 225, 226, 227, 228, 231};
static void preload_step(CPU *c)
{
    static unsigned next, wait = 150;
    if (next >= sizeof dlc_plant_reanims) return;
    if (wait) { wait--; return; }
    wait = 5;
    uint32_t type = dlc_plant_reanims[next++];
    call(c, 0x9c2ef0u, 0, 1, &type);
}

void game_hook_menu_draw(CPU *c)
{
    extern void pad_menu_drawn(uint32_t sel);
    pad_menu_drawn(c->ecx);
    preload_step(c);
    menu_frame = frame_no;
}

/* r = m . a (3x3, row major; points are columns) */
static void mat_mul(float *r, const float *m, const float *a)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[3 * i + j] = m[3 * i] * a[j] + m[3 * i + 1] * a[3 + j] + m[3 * i + 2] * a[6 + j];
}

/* a piece's draw with another picture and transform: scratch below the stack (matrix, clip, colour, source) */
static void blt(CPU *c, const Rec *r, uint32_t img, const float *a, const int32_t *clip, int alpha)
{
    if (!img) return;
    float m[9];
    mat_mul(m, r->m, a);
    uint32_t save = c->esp, base = (c->esp - 0x200) & ~15u;
    /* alpha < 0: drawn opaque white whatever the call's colour (the hill's call is a faint overlay pass) */
    int32_t col[4] = {r->col[0], r->col[1], r->col[2], r->col[3] * alpha / 255};
    if (alpha < 0) col[0] = col[1] = col[2] = col[3] = 255;
    int32_t src[4] = {0, 0, (int32_t)g_rd32(img + 0x24), (int32_t)g_rd32(img + 0x28)};
    memcpy(G2H(base), m, 36);
    memcpy(G2H(base + 48), clip, 16);
    memcpy(G2H(base + 64), col, 16);
    memcpy(G2H(base + 80), src, 16);
    c->esp = base;
    uint32_t args[9] = {img, r->x, r->y, base, base + 48, base + 64, r->mode, base + 80, r->blend};
    in_blt = 1;
    call(c, DDIMAGE_BLT_MATRIX, r->dest, 9, args);
    in_blt = 0;
    c->esp = save;
}

/* where a whole picture (w1 x h1, at scale s) goes so that the piece (w0 x h0) is its part at ox, oy:
 * both drawn centred on the transform's origin */
static void fit(float *a, float s, float w1, float h1, float w0, float h0, float ox, float oy)
{
    float t[9] = {s, 0, s * w1 / 2 - ox - w0 / 2, 0, s, s * h1 / 2 - oy - h0 / 2, 0, 0, 1};
    memcpy(a, t, sizeof t);
}

static int bush_piece(CPU *c, uint32_t img, int32_t w, int32_t h);
static void bush_art(CPU *c, int tv);
static int drawing_bush; /* Board::DrawGameObjects is drawing one of our bush reanims */

void game_hook_blt_matrix(CPU *c)
{
    if (!rt_widescreen || in_blt) return;
    uint32_t img = g_rd32(c->esp + 4);
    if (!img) return;
    int32_t w = (int32_t)g_rd32(img + 0x24), h = (int32_t)g_rd32(img + 0x28);
    if (bush_piece(c, img, w, h)) return;
    /* the leaves over the tree's foot (SelectorScreen_Leaves, 240x62): its see-through pixels are white, which
     * the smoothed draw bled into its left edge as a pale line in the grass; the same picture with them
     * coloured as the leaves beside them (wide/menu_leaves.png, tools/make_menu_leaves.py) */
    if (w == 240 && h == 62) {
        uint32_t lv = pic(c, &menu_leaves);
        if (lv && (int32_t)g_rd32(lv + 0x24) == w && (int32_t)g_rd32(lv + 0x28) == h) g_wr32(c->esp + 4, lv);
        return;
    }
    int sky = w == 100 && h == 75, tree = w == 476 && h == 680, hill = w == 730 && h == 560;
    int cloud = (w == 220 && h == 206) || (w == 152 && h == 121) || (w == 239 && h == 91) || (w == 434 && h == 144) ||
                (w == 76 && h == 44) || (w == 211 && h == 73);
    if (cloud && rec_hill.frame + 2 >= frame_no && menu_frame == frame_no) {
        /* the clouds drift in from the right behind the tombstone; beyond it is page 2's own (still) sky,
         * which they would cross. They show left of the tombstone's highest point (piece x 470: its top is
         * up at the sky's), fading out over the 48 px before it, so whatever of a cloud is above the stone
         * there melts in rather than being cut. While the menu rises in (the hill below its resting height)
         * a cloud reaching that far waits unseen, and fades in once the menu has settled. */
        static uint32_t blk;
        static float rest_y = 1e9f, shown;
        if (!blk) blk = rt_heap_alloc(16, 1);
        if (!blk) return;
        static uint32_t vis_frame;
        if (vis_frame != frame_no) {
            vis_frame = frame_no;
            if (rec_hill.m[5] < rest_y) rest_y = rec_hill.m[5];
            int rising = rec_hill.m[5] > rest_y + 0.5f;
            shown = rising ? 0.f : shown + 1.f / 30.f;
            if (shown > 1.f) shown = 1.f;
        }
        Rec r;
        r.frame = frame_no;
        r.img = img;
        r.dest = c->ecx;
        r.x = g_rd32(c->esp + 8), r.y = g_rd32(c->esp + 0xc);
        memcpy(r.m, G2H(g_rd32(c->esp + 0x10)), 36);
        memcpy(r.clip, G2H(g_rd32(c->esp + 0x14)), 16);
        memcpy(r.col, G2H(g_rd32(c->esp + 0x18)), 16);
        r.mode = g_rd32(c->esp + 0x1c), r.blend = g_rd32(c->esp + 0x24) & 0xff;
        const int fade = 48, slices = 8;
        int32_t peak = (int32_t)(rec_hill.m[2] - 365.f) + 470, edge = peak - fade;
        float right = r.m[2] + fabsf(r.m[0]) * (float)w / 2.f;
        int32_t *cl = r.clip;
        if (right <= (float)edge || cl[0] + cl[2] <= edge) return; /* all of it short of the stone */
        int a = (int)(255.f * shown + 0.5f);
        const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (a < 255 && cl[0] < edge && a > 0) { /* (fading in: the whole cloud at that alpha) */
            int32_t part[4] = {cl[0], cl[1], edge - cl[0], cl[3]};
            blt(c, &r, img, ident, part, a);
        }
        for (int i = 0; i < slices && a > 0; i++) {
            int32_t x0 = edge + fade * i / slices, x1 = edge + fade * (i + 1) / slices;
            int32_t l = cl[0] > x0 ? cl[0] : x0, rr = cl[0] + cl[2] < x1 ? cl[0] + cl[2] : x1;
            if (rr <= l) continue;
            int32_t part[4] = {l, cl[1], rr - l, cl[3]};
            blt(c, &r, img, ident, part, a * (slices - i) / (slices + 1));
        }
        int32_t keep[4] = {cl[0], cl[1], a == 255 && edge > cl[0] ? edge - cl[0] : 0, cl[3]};
        memcpy(G2H(blk), keep, 16);
        g_wr32(c->esp + 0x14, blk);
        return;
    }
    if (!sky && !tree && !hill) return;
    if (menu_frame != frame_no) return;
    Rec r;
    r.frame = frame_no;
    r.img = img;
    r.dest = c->ecx;
    r.x = g_rd32(c->esp + 8), r.y = g_rd32(c->esp + 0xc);
    memcpy(r.m, G2H(g_rd32(c->esp + 0x10)), 36);
    memcpy(r.clip, G2H(g_rd32(c->esp + 0x14)), 16);
    memcpy(r.col, G2H(g_rd32(c->esp + 0x18)), 16);
    r.mode = g_rd32(c->esp + 0x1c), r.blend = g_rd32(c->esp + 0x24) & 0xff;
    if (sky) {
        extern int rt_wide_menu_drawn;
        rt_wide_menu_drawn = 1; /* its strips are whole this frame: kept for the window screens */
        rec_sky = r;
        /* the DLC's own sky (a plain gradient) stretched across the 16:9 width: one sky, no seam */
        const float wide[9] = {1066.f / 800.f, 0, 0, 0, 1, 0, 0, 0, 1};
        blt(c, &r, img, wide, r.clip, 255);
        /* and not the game's own 4:3 one over it (its gradient steps at the old edges, through the tree) */
        static uint32_t none;
        if (!none) none = rt_heap_alloc(16, 1);
        if (none) {
            const int32_t z[4] = {0, 0, 0, 0};
            memcpy(G2H(none), z, 16);
            g_wr32(c->esp + 0x14, none);
        }
        return;
    }
    /* the tree and the hill: HenryJk's whole pieces drawn in their place (the DLC's are their middles, pixel
     * for pixel), so nothing joins at the old 4:3 edges. The call's image, matrix and source rect are
     * pointed at HenryJk's, in a block of the game's heap kept for it. */
    if (hill) {
        rec_hill = r; /* the game's own hill; page 2's edge beside it is matched to it (game_hook_page2_bg) */
        /* with its part of the dirt mound in page 2's colour (wide/menu_hill.png, tools/make_page1_mound.py:
         * the same picture otherwise), so the mound is one piece in page 2's look. The piece was drawn just
         * before (directly); this call is a faint pass over it: the new one is drawn whole in its place, and
         * the pass is pointed at it too. */
        uint32_t h2 = pic(c, &menu_hill);
        if (h2 && (int32_t)g_rd32(h2 + 0x24) == w && (int32_t)g_rd32(h2 + 0x28) == h) {
            const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            blt(c, &r, h2, ident, r.clip, -1);
            g_wr32(c->esp + 4, h2);
        }
        return;
    }
    uint32_t hj = pic(c, &menu_tree);
    if (!hj) return;
    float a[9];
    fit(a, 1.f, (float)g_rd32(hj + 0x24), (float)g_rd32(hj + 0x28), (float)w, (float)h, tree ? 133.f : 0.f, 0);
    /* the hill's call here is a faint overlay pass right after the piece's real (direct) draw: HenryJk's
     * hill goes over that, opaque, before the tombstone's buttons */
    blt(c, &r, hj, a, r.clip, 255);
    /* the game's own call (an overlay pass over the piece) then draws HenryJk's tree too, so it stays one
     * picture: its image, matrix and source rect pointed at it, in a block of the game's heap */
    static uint32_t blk;
    if (!blk) blk = rt_heap_alloc(64, 1);
    if (!blk) return;
    float mm[9];
    mat_mul(mm, r.m, a);
    int32_t src[4] = {0, 0, (int32_t)g_rd32(hj + 0x24), (int32_t)g_rd32(hj + 0x28)};
    memcpy(G2H(blk), mm, 36);
    memcpy(G2H(blk + 48), src, 16);
    g_wr32(c->esp + 4, hj);
    g_wr32(c->esp + 0x10, blk);
    g_wr32(c->esp + 0x20, blk + 48);
}

static float g_trans_x(uint32_t g)
{
    float f;
    memcpy(&f, G2H(g + 0x08), 4);
    return f;
}


void game_hook_page2_bg(CPU *c)
{
    /* SideScreen is drawn before its constructor's end has set its flags (+0xb5 hides the jukebox, +0xb6 the
     * Halloween button): they still hold the debug heap's fill (0xcd) then, so the page came in without its
     * jukebox until it was shown once. Unset, they are drawn. */
    if (*(uint8_t *)G2H(c->edi + 0xb5) == 0xcd) *(uint8_t *)G2H(c->edi + 0xb5) = 0;
    if (*(uint8_t *)G2H(c->edi + 0xb6) == 0xcd) *(uint8_t *)G2H(c->edi + 0xb6) = 0;
    if (!rt_widescreen) return;
    uint32_t g = c->esi;
    page2_x = g_trans_x(g);
    page2_frame = frame_no;
    draw_scaled(c, g, pic(c, &page2_right), 800, 0, 1, 1);
    /* page 2's first wall brick, its mortar lines moved to meet page 1's tombstone (tools/make_page2_day.py) */
    draw_alpha(c, g, pic(c, &page2_brick), 0, 0, 1, 255);
    float t = 1.f - page2_x / 800.f; /* how far page 2 has come in */
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (rec_sky.frame != frame_no) return;
    /* page 1's day sky carried over page 2's left edge (page 1's hill covers the rest of it, drawn after this):
     * all of it while page 1 is in view; as page 2 comes in it fades into page 2's night across the edge */
    const float sky[9] = {1066.f / 800.f, 0, 0, 0, 1, 0, 0, 0, 1};
    const int slices = 14, width = 133;
    for (int i = 0; i < slices; i++) {
        int32_t x0 = (int32_t)page2_x + width * i / slices, x1 = (int32_t)page2_x + width * (i + 1) / slices;
        float u = ((float)i + 0.5f) / (float)slices;
        int alpha = (int)(255.f * (1.f - t * u) + 0.5f);
        int32_t clip[4] = {rec_sky.clip[0], rec_sky.clip[1], rec_sky.clip[2], rec_sky.clip[3]};
        int32_t cl = clip[0] > x0 ? clip[0] : x0, cr = clip[0] + clip[2] < x1 ? clip[0] + clip[2] : x1;
        if (alpha <= 0 || cr <= cl) continue;
        clip[0] = cl, clip[2] = cr - cl;
        blt(c, &rec_sky, rec_sky.img, sky, clip, alpha);
    }
    /* page 2's own edge beside page 1's hill, in page 1's day light (wide/menu_page2_day.png,
     * tools/make_page2_day.py): all of it while page 1 is in view, fading into page 2's night across the
     * edge as page 2 comes in, so the mound and wall are one piece in either view */
    uint32_t d = pic(c, &page2_day);
    if (!d) return;
    uint32_t saved[4];
    for (int i = 0; i < 4; i++) saved[i] = g_rd32(g + 0x20 + 4 * i);
    for (int i = 0; i < slices; i++) {
        int32_t x0 = (int32_t)page2_x + 160 * i / slices, x1 = (int32_t)page2_x + 160 * (i + 1) / slices;
        float u = ((float)i + 0.5f) / (float)slices;
        int alpha = (int)(255.f * (1.f - t * u) + 0.5f);
        int32_t cl = (int32_t)saved[0] > x0 ? (int32_t)saved[0] : x0;
        int32_t cr = (int32_t)(saved[0] + saved[2]) < x1 ? (int32_t)(saved[0] + saved[2]) : x1;
        if (alpha <= 0 || cr <= cl) continue;
        g_wr32(g + 0x20, (uint32_t)cl), g_wr32(g + 0x28, (uint32_t)(cr - cl));
        draw_alpha(c, g, d, 0, 0, 1, alpha);
    }
    for (int i = 0; i < 4; i++) g_wr32(g + 0x20 + 4 * i, saved[i]);
    /* page 2's part of the dirt mound in its own colour in either view (the day strip would light it) */
    draw_alpha(c, g, pic(c, &page2_mound), 0, 0, 1, 255);
}

/* ---------------- 4:3 screens ----------------
 * Pictures swapped as they are drawn (Graphics::DrawImage 0xa04f90 / DrawImageF 0xa05c60), drawn dx, dy
 * from where the game puts its own:
 *  - the shop's and the gardens' 800x600 backgrounds: 16:9, 133 px further left (the screen's own picture
 *    in the middle, fused at its edges into the widescreen mod's 1280x720 version of the scene:
 *    tools/make_wide_backgrounds.py);
 *  - Crazy Dave's car, which stopped at the 4:3 screen's edge: whole (tools/make_wide_car.py).
 * The Almanac, the challenge lists and the award screen stay 4:3 windows with the menu behind them. */
typedef struct {
    uint32_t global; /* Image *IMAGE_... */
    int dx, dy;
    Pic pic;
} WideSwap;
static WideSwap swaps[] = {
    /* HenryJk's PvZWidescreen pictures (the same 133 px each side): 1066x600 screens, 1666x600 levels (the
     * lawn where the PC's is; dirt before the street where its bushes stand), the whole car, its roof masks
     * and sod strips */
    {0x1093eb0u, -133, 0, {"hj_Store_Background", 0, 0}}, {0x1093eb4u, -133, 0, {"hj_Store_BackgroundNight", 0, 0}},
    {0x1093ea0u, -133, 0, {"hj_Background_Greenhouse", 0, 0}}, {0x1093ea8u, -133, 0, {"hj_Background_MushroomGarden", 0, 0}},
    {0x1093f3cu, -133, 0, {"hj_aquarium1", 0, 0}},     {0x1093eb8u, 0, 0, {"hj_Store_Car", 0, 0}},
    {0x1093ebcu, 0, 0, {"hj_Store_CarClosed", 0, 0}}, /* (only the closed hatch: the car is drawn under it) */
    {0x1094ab8u, -133, 0, {"credits_wide.jpg", 0, 0}}, /* the DLC credits page (tools/make_wide_credits.py) */
    {0x1093db0u, -133, 0, {"hj_background1", 0, 0}},   {0x1093e30u, -133, 0, {"hj_background1unsodded", 0, 0}},
    {0x1093dbcu, -133, 0, {"hj_background2", 0, 0}},   {0x1093dd4u, -133, 0, {"hj_background3", 0, 0}},
    {0x1093de0u, -133, 0, {"hj_background4", 0, 0}},   {0x1093df4u, -133, 0, {"hj_background5", 0, 0}},
    {0x1093dfcu, -133, 0, {"hj_background6boss", 0, 0}},
    {0x1093df8u, 0, 0, {"hj_background5_gameover_mask", 0, 0}}, {0x1093e00u, 0, 0, {"hj_background6_gameover_mask", 0, 0}},
    {0x1093e34u, 0, 0, {"hj_sod1row", 0, 0}},          {0x1093e38u, 0, 0, {"hj_sod3row", 0, 0}},
    /* the Moon's blood moon (the night roof reddened, faded in over it) to fit HenryJk's night roof: its red
     * moon on HenryJk's moon, the red to both ends (tools/make_wide_bloodmoon.py); the same game-over mask */
    {0x1094a54u, 0, 0, {"jukebox_wide", 0, 0}},       /* the music screen's own wide picture, cut to what shows */
    {0x1094064u, -133, 0, {"hj_bloodmoon", 0, 0}},     {0x1094068u, 0, 0, {"hj_background6_gameover_mask", 0, 0}},
};

/* Multiplayer's P1 / P2 device buttons (IMAGE_PLAYER1/2_KEYBOARD / _MOUSE): the players are controllers, so
 * the game's own buttons with a controller in the device's place (tools/make_pad_buttons.py), faded while that
 * controller isn't connected, and P2's dimmed while the AI plays it (pad_device_state) */
extern int pad_device_state(int player);
static WideSwap dev_swaps[5] = {
    {0, 0, 0, {"player1_pad", 0, 0}}, {0, 0, 0, {"player1_pad_off", 0, 0}},
    {0, 0, 0, {"player2_pad", 0, 0}}, {0, 0, 0, {"player2_pad_off", 0, 0}}, {0, 0, 0, {"player2_cpu", 0, 0}},
};
extern int pad_desktop;
static WideSwap *device_swap(uint32_t img)
{
    if (pad_desktop) return NULL; /* (the PC's own keyboard and mouse pictures) */
    static const uint32_t globals[4] = {0x109493cu, 0x1094934u, 0x1094940u, 0x1094938u}; /* P1 kb, mouse, P2 */
    for (int i = 0; i < 4; i++)
        if (g_rd32(GFN(globals[i])) == img) {
            int p = i / 2, st = pad_device_state(p);
            return &dev_swaps[p ? 2 + (st > 2 ? 2 : st) : (st ? 1 : 0)];
        }
    return NULL;
}

static WideSwap *swap_for(uint32_t img)
{
    if (!img) return NULL;
    WideSwap *dev = device_swap(img);
    if (dev) return dev;
    for (size_t i = 0; i < sizeof swaps / sizeof swaps[0]; i++)
        if (g_rd32(GFN(swaps[i].global)) == img) {
            return &swaps[i];
        }
    return NULL;
}

static Pic hj_car = {"hj_Store_Car", 0, 0};

/* Graphics::DrawImage(Image *, int x, int y) */
/* the blood moon as drawn this frame: its alpha (the Moon's Board::DrawBackdrop fades it in with the Graphics'
 * colour, colourize on: Board +0x5a50 while its flag +0x5820 is set) */
static uint32_t blood_frame = ~0u;
static int blood_alpha;
void game_hook_draw_image(CPU *c)
{
    if (!rt_widescreen) {
        /* 4:3: the credits page's background 224 rows taller too (the port's section: game_hook_credits_port) */
        static Pic credits43 = {"credits_43.jpg", 0, 0};
        uint32_t im = g_rd32(c->esp + 4), bg = g_rd32(GFN(0x1094ab8u)), c43 = im && im == bg ? pic(c, &credits43) : 0;
        if (c43) g_wr32(c->esp + 4, c43);
        return;
    }
    WideSwap *w = swap_for(g_rd32(c->esp + 4));
    uint32_t img = w ? pic(c, &w->pic) : 0;
    if (!img) return;
    if (w->global == 0x1094064u) { /* (drawn twice a frame, a second fade over the first: the strongest counts) */
        int al = *(uint8_t *)G2H(c->ecx + 0x48) ? (int)g_rd32(c->ecx + 0x3c) : 255;
        if (blood_frame != frame_no) blood_frame = frame_no, blood_alpha = 0;
        if (al > blood_alpha) blood_alpha = al;
    }
    /* the music screen's background (its own wide picture): its Draw narrowed the clip to the 4:3 window
     * before drawing it, so it is drawn to the screen's edges (the screen is the music screen's alone) */
    if (w->global == 0x1094a54u && g_rd32(c->ecx + 0x20) == 0 && g_rd32(c->ecx + 0x28) == 800)
        g_wr32(c->ecx + 0x20, (uint32_t)-133), g_wr32(c->ecx + 0x28, 1066);
    /* the shop draws its closed car alone while it turns a page; HenryJk's closed car is only the hatch */
    if (w->global == 0x1093ebcu)
        draw_scaled(c, c->ecx, pic(c, &hj_car), (float)(int32_t)g_rd32(c->esp + 8), (float)(int32_t)g_rd32(c->esp + 12), 1, 1);
    g_wr32(c->esp + 4, img);
    g_wr32(c->esp + 8, g_rd32(c->esp + 8) + (uint32_t)w->dx);
    g_wr32(c->esp + 12, g_rd32(c->esp + 12) + (uint32_t)w->dy);
}

/* Graphics::DrawImageF(Image *, float x, float y) */
void game_hook_draw_image_f(CPU *c)
{
    if (!rt_widescreen) return;
    WideSwap *w = swap_for(g_rd32(c->esp + 4));
    uint32_t img = w ? pic(c, &w->pic) : 0;
    if (!img) return;
    float x, y;
    memcpy(&x, G2H(c->esp + 8), 4);
    memcpy(&y, G2H(c->esp + 12), 4);
    x += (float)w->dx, y += (float)w->dy;
    g_wr32(c->esp + 4, img);
    memcpy(G2H(c->esp + 8), &x, 4);
    memcpy(G2H(c->esp + 12), &y, 4);
}

/* the challenge lists and the award screen replace the menu, and under the Almanac the menu is drawn
 * without its widescreen pieces (the reanim's no-clip path): it stays in view behind them as the strips it
 * last drew (hle_d3d7 clear_sides) */
void game_hook_window_screen(CPU *c)
{
    extern int rt_wide_backdrop;
    static uint32_t last;
    static int from_menu;
    if (!rt_widescreen) return;
    /* only when it came up from the menu, or (the Almanac and the lists) from a board: the plant select's
     * Almanac (the award screen after a level is a picture of its own: black beside it) */
    if (frame_no > last + 2)
        from_menu = menu_frame + 5 >= frame_no ||
                    (board_frame + 5 >= frame_no && !(c->ecx && game_rtti_is(c->ecx, "AwardScreen")));
    last = frame_no;
    if (from_menu) {
        extern void rt_wide_backdrop_now(void);
        rt_wide_backdrop = 1;
        rt_wide_backdrop_now();
    }
    /* an award screen from the menu (the Multiplayer screen's HELP, the mail) is drawn within the 4:3 window
     * (its pictures run past 800 on the right): its Graphics' clip (+0x20 x, +0x28 width) narrowed to 0..800,
     * the menu's strips showing on both sides */
    /* (not the Multiplayer screen's HELP, AwardScreen +0x9c = 4: its table is drawn to the edges, below; nor the
     * menu's Help, type 2: its day lawn is drawn zoomed from -700, past both edges) */
    if (from_menu && c->ecx && game_rtti_is(c->ecx, "AwardScreen") && g_rd32(c->ecx + 0x9c) != 4 &&
        g_rd32(c->ecx + 0x9c) != 2 && g_rd32(c->ecx + 0x9c) != 5) {
        uint32_t g = g_rd32(c->esp + 4);
        if (g) {
            int32_t cx = (int32_t)g_rd32(g + 0x20), cw = (int32_t)g_rd32(g + 0x28);
            int32_t l = cx < 0 ? 0 : cx, r = cx + cw > 800 ? 800 : cx + cw;
            if (r > l) g_wr32(g + 0x20, (uint32_t)l), g_wr32(g + 0x28, (uint32_t)(r - l));
        }
    }
}

/* Graphics::DrawImage(Image *, int x, int y, const Rect &src): a part of a swapped picture (the sod roll
 * on the first levels draws strips of the lawn): the same part of the swapped one, which has its border
 * of -dx, -dy around the original */
void game_hook_draw_image_src(CPU *c)
{
    if (!rt_widescreen) return;
    WideSwap *w = swap_for(g_rd32(c->esp + 4));
    uint32_t img = w ? pic(c, &w->pic) : 0;
    uint32_t r = g_rd32(c->esp + 16);
    if (!img || !r) return;
    g_wr32(c->esp + 4, img);
    g_wr32(r, g_rd32(r) - (uint32_t)w->dx);
    g_wr32(r + 4, g_rd32(r + 4) - (uint32_t)w->dy);
}

/* ---------------- level bushes ----------------
 * As the widescreen mod (and the consoles): a bush at the right edge of each row, so zombies come out from
 * behind something instead of appearing in view. They are the DLC's own bush reanims (types 199-204,
 * bushes3/5/4 and their night versions: the DLC already ships them), made with LawnApp::AddReanimation in
 * the zombie layer of the row below (so a row's bush covers its own zombies), at the mod's places (its
 * cBushPos, less its 240, 60 border). Lawn stages only (the mod's day, night, pool, fog, and the DLC's own
 * lawns), made again whenever the board has none. */
#define APP_ADD_REANIM 0x8d30c0u   /* LawnApp::AddReanimation(x, y, render order, type) -> Reanimation * */
#define APP_REANIM_GET 0x8dcad0u   /* LawnApp::ReanimationTryToGet(id) */
#define APP_REANIM_ID 0x8dcab0u    /* LawnApp::ReanimationGetID(Reanimation *) */
#define REANIM_PLAY 0x9c2220u      /* Reanimation::PlayReanim(track, loop type, blend time, rate) */
#define STR_ANIM_RUSTLE 0xd2da54u  /* "anim_rustle" */
/* PvZ Touch's Board::InitCoverLayer (0x158998 in libGameMain.so): six bushes on every lawn, rows 0..5, in board
 * space, bushes3 / 5 / 4 / 3 / 5 / 4, drawn at 308000 + 10000 row (over everything in their row, under the next) */
/* versus: the bushes this much further right, their left edge where the PC's 4:3 screen ends (x 800), so they cover the
 * target zombies as that edge does: the signs and the fronts of the heads show (the top one's head just peeks out by
 * the brain counter), the bodies are in the leaves (the user's PC comparison; 70 and 90 showed the bodies) */
#define VS_BUSH_DX 55
/* ... and each bush a little further left or right of that, per lawn, so that every row's target shows as much as the
 * others (PvZ Touch's bushes differ in shape and place: on the pool the rows showed from 802 to 848): the leaves start
 * at x ~800 down each row's target zombie, the PC's edge, on 5- and 6-row lawns alike (measured from each bush drawn
 * alone, Mac ctl bushes:MASK; median edge per row 800-805 day, 796-800 pool) */
static const int vs_bush_dx5[6] = {0, -12, -4, -12, -12, 52}, vs_bush_dx6[6] = {-2, -18, -14, -52, -40, -22};
static const int bush_tv[6][2] = {{712, -18}, {724, 110}, {728, 198}, {734, 320}, {725, 401}, {740, 450}};
static const uint32_t bush_tv_day[6] = {199, 201, 200, 199, 201, 200}, bush_tv_night[6] = {202, 204, 203, 202, 204, 203};
static const int bush_pos5[6][2] = {{950, 40}, {962, 168}, {968, 258}, {972, 378}, {964, 459}, {980, 510}};
static const int bush_pos6[6][2] = {{952, 42}, {964, 170}, {968, 258}, {974, 380}, {966, 461}, {979, 509}};
static const uint32_t bush_types[6] = {199, 201, 200, 202, 204, 203}; /* BUSH3, 5, 4, then night */
static uint32_t bush_board, bush_ids[6];

uint32_t wide_bush_id(int row) { return row >= 0 && row < 6 ? bush_ids[row] : 0; }

void wide_bushes(CPU *c, uint32_t board)
{
    uint32_t app = rt_app_object();
    if (!rt_widescreen || !board || !app) return;
    int bg = (int)g_rd32(board + 0x5580); /* Board::mBackground */
    /* the original lawns (with PvZ Touch's leaves, bush_piece); the DLC's Woods (11 day, 15 night) get a trunk in the
     * pole's place instead (pole_upto); the snow map (14) has HenryJk's frosted strips (below) */
    int lawn = bg <= 3;
    int night = bg == 1 || bg == 3 || bg == 15, six = bg == 2 || bg == 3;
    if (!lawn) return;
    if (board == bush_board && call(c, APP_REANIM_GET, app, 1, &bush_ids[0])) return;
    bush_board = board;
    for (int i = 0; i < 6; i++) {
        const int *p = six ? bush_pos6[i] : bush_pos5[i];
        uint32_t args[4] = {f2u((float)(p[0] - 240)), f2u((float)(p[1] - 60)), (uint32_t)(303000 + (i + 1) * 10000),
                            bush_types[i % 3 + (night ? 3 : 0)]};
        if (bg <= 3) { /* the original lawns: exactly as PvZ Touch */
            args[0] = f2u((float)bush_tv[i][0]), args[1] = f2u((float)bush_tv[i][1]);
            args[2] = (uint32_t)(308000 + 10000 * i);
            args[3] = night ? bush_tv_night[i] : bush_tv_day[i];
        }
        /* versus (game modes 144-151): the target zombies stand at the lawn's edge, partly in the hedge as the PC's
         * screen edge cuts them: the bushes further right there (VS_BUSH_DX), drawn over their rows as everywhere */
        uint32_t mode = g_rd32(app + 0x814);
        if (mode >= 144 && mode <= 151) args[0] = f2u((float)(bush_tv[i][0] + VS_BUSH_DX + (six ? vs_bush_dx6 : vs_bush_dx5)[i]));
        uint32_t r = call(c, APP_ADD_REANIM, app, 4, args);
        if (i == 5) bush_art(c, bg <= 3); /* (the definitions are loaded by now) */
        bush_ids[i] = r ? call(c, APP_REANIM_ID, app, 1, &r) : 0;
        if (!r) continue;
        /* still, at the start of the rustle, until a zombie comes */
        uint32_t play[4] = {GFN(STR_ANIM_RUSTLE), 3 /* REANIM_PLAY_ONCE_AND_HOLD */, 0, 0};
        call(c, REANIM_PLAY, r, 4, play);
    }
    rt_log("[wide] bushes for background %d%s%s\n", bg, night ? " (night)" : "", six ? " (6 rows)" : "");
}

/* HenryJk's bushes on the original lawns: a 483x600 picture per row at board x 642, each drawn just after its
 * row (render order 310999 + 10000 i, so it covers that row's zombies and is covered by the next row's),
 * over a dark base behind everything (300001); day / night / pool / fog sets, the night base for fog.
 * Board::DrawGameObjects draws its sorted render items (12 bytes: type, order, object) from [ebp]; the
 * loop's top (0x73a560, eax = the item) draws the pictures whose order comes first, its end (0x73a681) the
 * rest. The Graphics is at [ebp-0x18], the board in edi. */
static Pic hj_bd[5] = {{"hj_BD0", 0, 0}, {"hj_BD1", 0, 0}, {"hj_BD2", 0, 0}, {"hj_BD3", 0, 0}, {"hj_BD4", 0, 0}};
static Pic hj_bn[5] = {{"hj_BN0", 0, 0}, {"hj_BN1", 0, 0}, {"hj_BN2", 0, 0}, {"hj_BN3", 0, 0}, {"hj_BN4", 0, 0}};
static Pic hj_bp[6] = {{"hj_BP0", 0, 0}, {"hj_BP1", 0, 0}, {"hj_BP2", 0, 0}, {"hj_BP3", 0, 0}, {"hj_BP4", 0, 0}, {"hj_BP5", 0, 0}};
static Pic hj_bf[6] = {{"hj_BF0", 0, 0}, {"hj_BF1", 0, 0}, {"hj_BF2", 0, 0}, {"hj_BF3", 0, 0}, {"hj_BF4", 0, 0}, {"hj_BF5", 0, 0}};
static Pic hj_bs[5] = {{"hj_BS0", 0, 0}, {"hj_BS1", 0, 0}, {"hj_BS2", 0, 0}, {"hj_BS3", 0, 0}, {"hj_BS4", 0, 0}};
static Pic hj_base = {"hj_B_base", 0, 0}, hj_base_night = {"hj_BN_base", 0, 0}, hj_base_snow = {"hj_BS_base", 0, 0};
static int strip_next; /* the next picture to draw this pass (0 = the base) */

static int strips(uint32_t board, Pic **set, Pic **base)
{
    switch ((int)g_rd32(board + 0x5580)) {
    case 14: *set = hj_bs, *base = &hj_base_snow; return 5; /* the DLC's snow map: the day set frosted
                                                                  * (tools/make_frost_bushes.py) */
    }
    return 0;
}

/* a bush rustles when a zombie comes into its row (Board::AddZombieInRow while playing): a short sway */
static uint32_t rustle_at[6];
void game_hook_zombie_added(CPU *c)
{
    uint32_t app = rt_app_object(), row = g_rd32(c->esp + 8);
    /* wave zombies only (fromWave >= 0), as PvZ Touch's Board::AddZombieInRow; not graves or bungees; not while
     * the camera pans (the board, ecx, away from its playing place, x +0x34 = 0), nor the zombies the player places
     * (I, Zombie and Zombies vs. Zombies: a zombie seed bank), nor those out of vases: only wave zombies coming from the
     * right, as PvZ Touch (its AddZombieInRow's rustle argument) */
    extern int pad_zombie_bank(void), vase_mode(void);
    if (!rt_widescreen || !app || row >= 6 || g_rd32(app + 0x818) != 3 || (int32_t)g_rd32(c->esp + 12) < 0) return;
    if (!c->ecx || (int32_t)g_rd32(c->ecx + 0x34) != 0 || pad_zombie_bank() || vase_mode()) return; /* (nor vase zombies) */
    rustle_at[row] = frame_no ? frame_no : 1;
    /* the bush reanim's own rustle, as PvZ Touch plays it: once and hold, no blend, rate 10 (42 frames,
     * 4.2 s), restarting for each zombie; no sound */
    uint32_t r = bush_ids[row] ? call(c, APP_REANIM_GET, app, 1, &bush_ids[row]) : 0;
    if (r) {
        uint32_t play[4] = {GFN(STR_ANIM_RUSTLE), 3 /* REANIM_PLAY_ONCE_AND_HOLD */, 0, f2u(10.f)};
        call(c, REANIM_PLAY, r, 4, play);
    }
}

static float rustle_dx(int row)
{
    if (row < 0 || row >= 6 || !rustle_at[row]) return 0;
    float t = (float)(frame_no - rustle_at[row]) / 60.f;
    if (t < 0 || t > 0.8f) return 0;
    return 6.f * sinf(t * 28.f) * (1.f - t / 0.8f);
}

static void strips_upto(CPU *c, uint32_t order)
{
    Pic *set = NULL, *base = NULL;
    int n = rt_widescreen ? strips(c->edi, &set, &base) : 0;
    if (!n) return;
    uint32_t g = g_rd32(c->ebp - 0x18);
    while (strip_next <= n) {
        uint32_t o = strip_next ? 310999u + 10000u * (uint32_t)(strip_next - 1) : 300001u;
        if (o >= order) break;
        float dx = strip_next ? rustle_dx(strip_next - 1) : 0;
        draw_scaled(c, g, pic(c, strip_next ? &set[strip_next - 1] : base), 642 + dx, 0, 1, 1);
        strip_next++;
    }
}

/* the pass's Graphics: something in it narrows the clip to the 800 window part-way (the lawn mowers at the
 * window's left edge came and went with it); each item draws with the screen's whole width */
static void widen_pass_clip(uint32_t g)
{
    if (!rt_widescreen) return;
    if (g_rd32(g + 0x20) == 0 && g_rd32(g + 0x24) == 0 && g_rd32(g + 0x28) == 800 && g_rd32(g + 0x2c) == 600) {
        g_wr32(g + 0x20, (uint32_t)-133);
        g_wr32(g + 0x28, 1066);
    }
}

#define BOARD_DRAW_TOP_RIGHT_UI 0x73cfd0u /* Board::DrawTopRightUI(Graphics *): Menu, fast forward */
static Pic hj_pole = {"hj_pole", 0, 0}, hj_pole_night = {"hj_pole_night", 0, 0}, hj_pole_blood = {"hj_pole_bloodmoon", 0, 0};
static int pole_drawn;
/* HenryJk's obstruction pole on the roofs (4 day, 5 night): at render order 400001, its x following the
 * camera faster than the board (parallax): board-relative (3 bx + 1546) / 2 for the board at window x bx */
static void pole_upto(CPU *c, uint32_t order)
{
    if (pole_drawn || order <= 400001u || !rt_widescreen) return;
    int bg = (int)g_rd32(c->edi + 0x5580);
    /* the Woods (11 day / 15 night): in the pole's place a thick trunk like those along the top of its picture (the
     * Tree of Wisdom's straight middle, recoloured and darkened, an even column the screen's height:
     * tools/make_woods_trunk.py), its left edge at the lawn's right edge at the playing place; moving as the pole */
    int woods = bg == 11 || bg == 15;
    if (bg != 4 && bg != 5 && !woods) return;
    pole_drawn = 1;
    float bx = (float)(int32_t)g_rd32(c->edi + 0x34);
    if (woods) {
        static Pic tr = {"woods_trunk", 0, 0}, tr_n = {"woods_trunk_night", 0, 0};
        uint32_t img = pic(c, bg == 11 ? &tr : &tr_n);
        /* no path may show between the lawn and the trunk (the zombies coming would be seen): the picture's left
         * outline is 24..57 px in, so at its narrowest (57) it is at the lawn's right edge (x ~778), elsewhere a
         * little over it */
        /* versus: the target zombies stand at the lawn's edge, under the trunk: it is further right there, the middle
         * of its outline (44 px in) at x 800 as the bushes elsewhere, so the signs and the fronts of the heads show */
        uint32_t app = rt_app_object(), mode = app ? g_rd32(app + 0x814) : 0;
        float tx = mode >= 144 && mode <= 151 ? 800.f - 44.f : 778.f - 57.f;
        draw_scaled(c, g_rd32(c->ebp - 0x18), img, 1.5f * bx + tx, 0, 1, 1);
        return;
    }
    float px = (3.f * bx + 1546.f) / 2.f;
    static int blood_test = -1; /* (Mac test: RT_BLOOD_TEST=n holds the blood moon at fade n: Board +0x5a50, its
                                 * flag +0x5820 and the countdown it waits for, [Board +0x174] +0x58, under 500) */
    if (blood_test < 0) blood_test = getenv("RT_BLOOD_TEST") ? atoi(getenv("RT_BLOOD_TEST")) : 0;
    if (blood_test && bg == 5 && rt_vm_mapped(g_rd32(c->edi + 0x174))) {
        g_wr32(c->edi + 0x5a50, (uint32_t)blood_test);
        *(uint8_t *)G2H(c->edi + 0x5820) = 1; /* (the blood moon on: else the countdown fades in another picture) */
        g_wr32(g_rd32(c->edi + 0x174) + 0x58, 100);
    }
    draw_scaled(c, g_rd32(c->ebp - 0x18), pic(c, bg == 4 ? &hj_pole : &hj_pole_night), px, 0, 1, 1);
    /* the Moon's blood moon lights the pole red as it does the wall beside it (tools/make_pole_bloodmoon.py),
     * faded in with it */
    if (bg == 5 && blood_frame == frame_no && blood_alpha > 0)
        draw_alpha(c, g_rd32(c->ebp - 0x18), pic(c, &hj_pole_blood), px, 0, 1, blood_alpha > 255 ? 255 : blood_alpha);
}

#define VS_BANK_RIGHT 792 /* the versus seed bank's pane ends here (the brains count; measured) */
#define REANIM_DRAW 0x9c0700u /* Reanimation::Draw(Graphics *) (Board::DrawGameObjects' reanimation items, type 11) */
static uint32_t vs_clip_g, vs_clip[4];

/* the versus seed bank's pane on the screen (window coordinates): its right edge, the brains count, and its bottom edge.
 * The bank is drawn where it is on the screen, not with the lawn: the lawn pans under it at the start of a match while
 * the bank slides down from above (Board +0x148: y +0xc, -87 .. 0; height +0x14) */
int wide_bush_mask = 0x3f; /* (Mac tests, ctl bushes:MASK: the bushes drawn, bit = row) */
static void vs_pane(uint32_t board, int32_t *right, int32_t *bottom)
{
    uint32_t bank = g_rd32(board + 0x148);
    *right = VS_BANK_RIGHT, *bottom = 87;
    if (rt_vm_mapped(bank)) *bottom = (int32_t)g_rd32(bank + 0xc) + (int32_t)g_rd32(bank + 0x14);
}

void game_hook_draw_objects_item(CPU *c)
{
    uint32_t i = c->eax;
    static uint32_t bush_reanims[6];
    if (i == 0) {
        strip_next = 0, pole_drawn = 0;
        /* our bush reanims this frame */
        uint32_t app = rt_app_object();
        for (int k = 0; k < 6; k++)
            bush_reanims[k] = rt_widescreen && app && c->edi == bush_board && bush_ids[k] ? call(c, APP_REANIM_GET, app, 1, &bush_ids[k]) : 0;
    }
    uint32_t g = g_rd32(c->ebp - 0x18);
    if (vs_clip_g) { /* (the top bush's narrowed clip, last item: put back) */
        for (int k = 0; k < 4; k++) g_wr32(vs_clip_g + 0x20 + 4 * (uint32_t)k, vs_clip[k]);
        vs_clip_g = 0;
    }
    int top_bush = 0, hidden = 0;
    {
        uint32_t obj = g_rd32(c->ebp + 12 * i + 8);
        drawing_bush = 0;
        for (int k = 0; k < 6; k++)
            if (obj && bush_reanims[k] == obj) { drawing_bush = 1, top_bush = k == 0, hidden = !(wide_bush_mask >> k & 1); break; }
    }
    pole_upto(c, g_rd32(c->ebp + 12 * i + 4));
    widen_pass_clip(g);
    /* versus: the seed bank is drawn under the lawn's objects, as on PC (the top target zombie's head goes over the
     * brains count), so the top bush, the one that reaches the bank, is cut where the bank's pane is: drawn right of
     * the pane (the game's own draw, clipped) and below it (drawn here first). The pane stays put on the screen while
     * the lawn pans under it (the start of a match), so a cut fixed to the right of the pane alone lost the whole
     * bush while the pan had it left of there */
    uint32_t app = rt_app_object(), mode = app ? g_rd32(app + 0x814) : 0;
    if (hidden) {
        for (int k = 0; k < 4; k++) vs_clip[k] = g_rd32(g + 0x20 + 4 * (uint32_t)k);
        g_wr32(g + 0x28, 0), vs_clip_g = g;
    } else if (top_bush && mode >= 144 && mode <= 151) {
        for (int k = 0; k < 4; k++) vs_clip[k] = g_rd32(g + 0x20 + 4 * (uint32_t)k);
        int32_t x = (int32_t)vs_clip[0], y = (int32_t)vs_clip[1], w = (int32_t)vs_clip[2], h = (int32_t)vs_clip[3];
        int32_t right, bottom;
        vs_pane(c->edi, &right, &bottom);
        int32_t below = bottom > y ? bottom : y, rx = right < x + w ? right : x + w;
        if (rx > x && below < y + h) {
            uint32_t obj = g_rd32(c->ebp + 12 * i + 8);
            g_wr32(g + 0x24, (uint32_t)below), g_wr32(g + 0x28, (uint32_t)(rx - x)), g_wr32(g + 0x2c, (uint32_t)(y + h - below));
            call(c, REANIM_DRAW, obj, 1, &g);
            for (int k = 0; k < 4; k++) g_wr32(g + 0x20 + 4 * (uint32_t)k, vs_clip[k]);
        }
        if (x < right) g_wr32(g + 0x20, (uint32_t)right), g_wr32(g + 0x28, (uint32_t)(x + w > right ? x + w - right : 0));
        vs_clip_g = g;
    }
    strips_upto(c, g_rd32(c->ebp + 12 * i + 4));
}

void game_hook_draw_objects_done(CPU *c)
{
    Pic *set, *base;
    int drew = rt_widescreen && strip_next <= 99 && (strips(c->edi, &set, &base) || c->edi == bush_board);
    if (vs_clip_g) {
        for (int k = 0; k < 4; k++) g_wr32(vs_clip_g + 0x20 + 4 * (uint32_t)k, vs_clip[k]);
        vs_clip_g = 0;
    }
    strips_upto(c, 0xffffffffu);
    pole_upto(c, 0xffffffffu);
    if (pole_drawn && rt_widescreen) drew = 1; /* (the pole / the Woods' trunk cover the top-right buttons too) */
    strip_next = 99;
    {
        /* the Tree of Wisdom has no keyboard cursor (whose draw puts the selector on the Zen Garden's bank):
         * its selector is drawn here, with the board's Graphics */
        uint32_t app = rt_app_object();
        if (app && g_rd32(app + 0x814) == 140) {
            extern void pad_zen_selector(uint32_t g);
            pad_zen_selector(g_rd32(c->ebp - 0x18));
        }
    }
    if (rt_widescreen) {
        extern int rt_wide_menu_drawn;
        rt_wide_menu_drawn = 1; /* the board's strips are whole this frame: kept for a 4:3 screen over it */
        board_frame = frame_no;
    }
    drawing_bush = 0;
    /* the bushes go over the board's own top-right buttons (drawn with its bottom UI): drawn again on top */
    if (drew) {
        uint32_t g = g_rd32(c->ebp - 0x18);
        /* (the versus seed bank isn't drawn again on top: it stays under the lawn's objects, the top bush clipped
         * off it, game_hook_draw_objects_item) */
        call(c, BOARD_DRAW_TOP_RIGHT_UI, c->edi, 1, &g);
    }
}

/* CutScene::PlaceAZombie: the zombies shown on the street before a level stand past the bushes (HenryJk's
 * PvZWidescreen moves them PAD + 30 = 163 the same way) */
void game_hook_street_zombie(CPU *c)
{
    uint32_t board = g_rd32(c->ebx + 4);
    if (!rt_widescreen || !board) return;
    /* the original lawns (backgrounds 0-3): HenryJk's +163 (their street is 133 further); the roofs (4, 5, and
     * the Moon, which is the night roof) none, their zombies stand on the roof as drawn; the DLC's own stages a
     * little further back (+24), so fewer stand inside the bushes */
    float x;
    memcpy(&x, G2H(c->esi + 0x2c), 4);
    uint32_t bg = g_rd32(board + 0x5580);
    x += bg <= 3 ? 163.f : bg <= 5 ? 0.f : 24.f;
    memcpy(G2H(c->esi + 0x2c), &x, 4);
}

/* ---- the bushes' leaves on the original lawns: PvZ Touch's (the console art); the DLC's bush reanims are the
 * same animation over a darker recolour of the same pieces. While Board::DrawGameObjects draws one of our
 * bush reanims (drawing_bush), each piece's draw is pointed at PvZ Touch's picture of it (wide/tvbush/),
 * found by its size; two of PvZ Touch's are shorter (the DLC's copies are padded below), so they are moved
 * up by half the difference to keep their place. */
typedef struct { int16_t w, h; const char *day, *night; Pic pd, pn; } BushPiece;
static BushPiece bush_pieces[] = {
    {267, 170, "tvbush/b1_1", "tvbush/b1_1"}, {313, 187, "tvbush/b1_2", "tvbush/b1_2"}, {307, 235, "tvbush/b1_3", "tvbush/b1_3"},
    {86, 66, "tvbush/b3_1", "tvbush/nb3_1"},  {137, 75, "tvbush/b3_2", "tvbush/nb3_2"},  {110, 85, "tvbush/b3_3", "tvbush/nb3_3"},
    {198, 125, "tvbush/b3_4", "tvbush/nb3_4"}, {74, 78, "tvbush/b3_5", "tvbush/nb3_5"},  {118, 77, "tvbush/b3_6", "tvbush/nb3_6"},
    {84, 75, "tvbush/b3_7", "tvbush/nb3_7"},  {203, 129, "tvbush/b4_1", "tvbush/nb4_1"}, {228, 182, "tvbush/b4_2", "tvbush/nb4_2"},
    {194, 123, "tvbush/b4_3", "tvbush/nb4_3"}, {193, 112, "tvbush/b4_4", "tvbush/nb4_4"},
    {329, 240, "tvbush/bush_base", "tvbush/nbush_base"},
};

static int bush_piece(CPU *c, uint32_t img, int32_t w, int32_t h)
{
    if (!drawing_bush) return 0;
    /* the reanim draws its pieces out of an atlas: the piece is the source rectangle */
    {
        int32_t sr[4];
        memcpy(sr, G2H(g_rd32(c->esp + 0x20)), 16);
        w = sr[2], h = sr[3];
    }
    uint32_t app = rt_app_object(), b = app ? g_rd32(app + 0x780) : 0;
    int bg = b ? (int)g_rd32(b + 0x5580) : -1;
    if (bg < 0 || bg > 3) return 0;
    int night = bg == 1 || bg == 3;
    for (size_t i = 0; i < sizeof bush_pieces / sizeof bush_pieces[0]; i++) {
        BushPiece *p = &bush_pieces[i];
        if (p->w != w || p->h != h) continue;
        Pic *pc = night ? &p->pn : &p->pd;
        if (!pc->name) pc->name = night ? p->night : p->day;
        uint32_t tv = pic(c, pc);
        if (!tv) return 1;
        static uint32_t blk;
        if (!blk) blk = rt_heap_alloc(64, 1);
        if (!blk) return 1;
        int32_t th = (int32_t)g_rd32(tv + 0x28);
        float m[9], mm[9];
        memcpy(m, G2H(g_rd32(c->esp + 0x10)), 36);
        const float a[9] = {1, 0, 0, 0, 1, (float)(th - h) / 2.f, 0, 0, 1};
        mat_mul(mm, m, a);
        int32_t src[4] = {0, 0, (int32_t)g_rd32(tv + 0x24), th};
        memcpy(G2H(blk), mm, 36);
        memcpy(G2H(blk + 48), src, 16);
        g_wr32(c->esp + 4, tv);
        g_wr32(c->esp + 0x10, blk);
        g_wr32(c->esp + 0x20, blk + 48);
        (void)img;
        return 1;
    }
    return 0;
}

/* The bush reanims (types 199-204) keep their pieces in an atlas, so most never reach BltMatrix as pictures of
 * their own: on the original lawns their track data is pointed at PvZ Touch's pieces directly (each
 * transform's image, the atlas's "encoded" entries decoded first, and the atlas dropped so the game draws
 * them one by one). Pieces of a different height (b1_1, the base) stay the game's and are swapped at their
 * draw (bush_piece). The DLC's forest maps get the game's own back. ReanimatorDefinition: 16 bytes in the
 * array at 0x1095f48 (tracks, count, fps, atlas); a track 12 (name, transforms, count); a transform 44 (the
 * image at +32); an atlas entry 20 (x, y, w, h, original image). */
#define REANIM_DEFS 0x1095f48u
static uint32_t *tv_saved[6], tv_saved_n[6], tv_saved_atlas[6];
static int tv_on[6];

static uint32_t tv_for(CPU *c, uint32_t orig, int night)
{
    if (!orig) return 0;
    int32_t w = (int32_t)g_rd32(orig + 0x24), h = (int32_t)g_rd32(orig + 0x28);
    for (size_t i = 0; i < sizeof bush_pieces / sizeof bush_pieces[0]; i++) {
        BushPiece *p = &bush_pieces[i];
        if (p->w != w || p->h != h || (w == 267 && h == 170) || (w == 329 && h == 240)) continue;
        Pic *pc = night ? &p->pn : &p->pd;
        if (!pc->name) pc->name = night ? p->night : p->day;
        uint32_t tv = pic(c, pc);
        return tv ? tv : orig;
    }
    return orig;
}

static void bush_art(CPU *c, int tv)
{
    uint32_t defs = g_rd32(GFN(REANIM_DEFS));
    if (!defs) return;
    for (int k = 0; k < 6; k++) {
        uint32_t def = defs + (199u + (uint32_t)k) * 16u, tracks = g_rd32(def), ntr = g_rd32(def + 4);
        if (!tracks || tv == tv_on[k]) continue;
        if (tv) {
            uint32_t atlas = g_rd32(def + 12), total = 0;
            for (uint32_t t = 0; t < ntr; t++) total += g_rd32(tracks + 12 * t + 8);
            free(tv_saved[k]);
            tv_saved[k] = malloc(total * 4u), tv_saved_n[k] = total, tv_saved_atlas[k] = atlas;
            if (!tv_saved[k]) return;
            uint32_t n = 0;
            for (uint32_t t = 0; t < ntr; t++) {
                uint32_t tf = g_rd32(tracks + 12 * t + 4), cnt = g_rd32(tracks + 12 * t + 8);
                for (uint32_t j = 0; j < cnt; j++, n++) {
                    uint32_t at = tf + 44 * j + 32, img = g_rd32(at), orig = img;
                    tv_saved[k][n] = img;
                    if (atlas && img > 0 && img <= 1000) orig = g_rd32(atlas + 20 * (img - 1) + 16);
                    g_wr32(at, tv_for(c, orig, k >= 3));
                }
            }
            g_wr32(def + 12, 0);
        } else if (tv_saved[k]) {
            uint32_t n = 0;
            for (uint32_t t = 0; t < ntr; t++) {
                uint32_t tf = g_rd32(tracks + 12 * t + 4), cnt = g_rd32(tracks + 12 * t + 8);
                for (uint32_t j = 0; j < cnt && n < tv_saved_n[k]; j++, n++) g_wr32(tf + 44 * j + 32, tv_saved[k][n]);
            }
            g_wr32(def + 12, tv_saved_atlas[k]);
        }
        tv_on[k] = tv;
    }
}

/* AwardScreen::Draw, the Multiplayer HELP pages (award type 4) and the letters (type 5, the table and then its
 * leaves' shadows): it lays the letters' table (800x600) stretched to 1280x720 from 0,0 (the DLC's 1280x720
 * layout). Drawn from -133
 * instead, the same zoom fills the 16:9 screen with the table (the call's x, [esp + 4]) */
void game_hook_help_table(CPU *c)
{
    if (rt_widescreen && (int32_t)g_rd32(c->esp + 4) == 0) g_wr32(c->esp + 4, (uint32_t)-133);
}

/* platform_switch.c: the Switch host for the first hardware runs.
 *
 * Display: the software renderer (rt_gpu stays NULL) into a libnx framebuffer; the game's 1280x720
 * display is copied in (ARGB -> RGBA) with a drawn mouse cursor on top. The deko3d backend replaces
 * this once the CPU side is measured.
 * Input: pad_input.c (shared with the desktop host); the touch screen is the mouse.
 * Audio: audout at 48 kHz from the shared mixer. */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <switch.h>

#include "rt.h"
#include "rt_audio.h"
#include "pad_input.h"

void rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void rt_boot_screen_close(void);
uint32_t rt_screen_bits(void);
extern uint32_t rt_main_hwnd;
extern int32_t rt_mouse_x, rt_mouse_y;

#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_CHAR 0x102
#define WM_CLOSE 0x10

static int inited, fb_ready;
static Framebuffer fb;
static volatile int dirty;
static uint32_t last_bits;
static int last_w, last_h, last_pitch;
static uint64_t last_present_ns, last_vblank_ns;

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* libnx starts pthreads on the process's default core only, so the game's threads and the render
 * workers would all share core 0. Each thread gets the next of the game's three cores as its own and
 * may run on any of them. */
void rt_platform_thread_start(void)
{
    static int next;
    int core = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % 3;
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 0x7);
    if (R_FAILED(rc)) rt_log("[nx] thread core mask: %x\n", (unsigned)rc);
}

/* svcSleepThread wakes on time; nothing like macOS's timer coalescing to work around */
void rt_platform_sleep_us(uint32_t us) { svcSleepThread((int64_t)us * 1000); }

/* ---------------- audio ---------------- */
#define AUDIO_FRAMES 1024 /* 21 ms at 48 kHz */
#define AUDIO_BUFS 5 /* ~107 ms queued: a late wake-up (emulators wake ~13-70 ms late) does not run dry */
static Thread audio_th;

static void audio_main(void *arg)
{
    (void)arg;
    size_t size = (AUDIO_FRAMES * 4 + 0xfff) & ~(size_t)0xfff;
    AudioOutBuffer bufs[AUDIO_BUFS];
    for (int i = 0; i < AUDIO_BUFS; i++) {
        void *data = memalign(0x1000, size);
        memset(data, 0, size);
        bufs[i].next = NULL;
        bufs[i].buffer = data;
        bufs[i].buffer_size = size;
        bufs[i].data_size = AUDIO_FRAMES * 4;
        bufs[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&bufs[i]);
    }
    for (;;) {
        AudioOutBuffer *done = NULL;
        u32 n = 0;
        if (R_FAILED(audoutWaitPlayFinish(&done, &n, UINT64_MAX)) || !done) continue;
        rt_audio_mix(done->buffer, AUDIO_FRAMES);
        audoutAppendAudioOutBuffer(done);
    }
}

static void audio_init(void)
{
    if (R_FAILED(audoutInitialize())) {
        rt_log("[nx] audout unavailable: no sound\n");
        return;
    }
    audoutStartAudioOut();
    if (audoutGetSampleRate() != RT_AUDIO_RATE)
        rt_log("[nx] audout runs at %u Hz, the mixer at %u\n", (unsigned)audoutGetSampleRate(), RT_AUDIO_RATE);
    /* its own core (2), above the game's threads so the mix is never late */
    if (R_SUCCEEDED(threadCreate(&audio_th, audio_main, NULL, NULL, 0x40000, 0x2a, 2) /* the mixer keeps ~40 KiB on the stack */)) threadStart(&audio_th);
}

/* ---------------- input ----------------
 * Controllers go through pad_input.c (versus: keyboard players; otherwise controller 1 is the mouse).
 * The touch screen is the mouse everywhere. */
static PadState pads[2];
static int touching, touch_x, touch_y, touch_x0, touch_y0, touch_held0;
static uint64_t last_input_ns;

/* The screen is 1280x720 (the touch panel's coordinates too); the game's display is last_w x last_h,
 * shown centred at its own aspect ratio (gpu_dk.c). Screen points -> display points. */
void rt_screen_to_display(int *x, int *y)
{
    int w = last_w > 0 ? last_w : 1280, h = last_h > 0 ? last_h : 720;
    if (w == 1280 && h == 720) return;
    float k = 1280.f / (float)w < 720.f / (float)h ? 1280.f / (float)w : 720.f / (float)h;
    float ox = (1280.f - (float)w * k) / 2, oy = (720.f - (float)h * k) / 2;
    int gx = (int)(((float)*x - ox) / k), gy = (int)(((float)*y - oy) / k);
    *x = gx < 0 ? 0 : gx >= w ? w - 1 : gx;
    *y = gy < 0 ? 0 : gy >= h ? h - 1 : gy;
}

void pad_touch_redirect(int *x, int *y);
static void touch_mouse(uint32_t msg, int buttons)
{
    int x = touch_x, y = touch_y;
    rt_screen_to_display(&x, &y);
    pad_touch_redirect(&x, &y);
    if (msg != WM_MOUSEMOVE)
        rt_log("[touch] %s at screen %d,%d -> game %d,%d (display %dx%d, window %08x)\n",
               msg == WM_LBUTTONDOWN ? "down" : "up", touch_x, touch_y, x, y, last_w, last_h, (unsigned)rt_main_hwnd);
    rt_mouse_x = x;
    rt_mouse_y = y;
    rt_post_message(rt_main_hwnd, msg, (uint32_t)buttons, ((uint32_t)y << 16) | ((uint32_t)x & 0xffff));
}

static void input_init(void)
{
    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pads[0]);
    padInitialize(&pads[1], HidNpadIdType_No2);
    hidInitializeTouchScreen();
    last_input_ns = now_ns();
}

static PadIn read_pad(PadState *ps)
{
    static const struct { u64 nx; uint32_t pb; } map[] = {
        {HidNpadButton_A, PB_A}, {HidNpadButton_B, PB_B}, {HidNpadButton_X, PB_X}, {HidNpadButton_Y, PB_Y},
        {HidNpadButton_L, PB_L}, {HidNpadButton_R, PB_R}, {HidNpadButton_ZL, PB_ZL}, {HidNpadButton_ZR, PB_ZR},
        {HidNpadButton_Plus, PB_PLUS}, {HidNpadButton_Minus, PB_MINUS}, {HidNpadButton_Up, PB_UP},
        {HidNpadButton_Down, PB_DOWN}, {HidNpadButton_Left, PB_LEFT}, {HidNpadButton_Right, PB_RIGHT},
    };
    PadIn in = {0};
    padUpdate(ps);
    in.connected = padIsConnected(ps);
    u64 st = padGetStyleSet(ps), b = padGetButtons(ps);
    /* A lone Joy-Con (one each for two players) is held sideways, rail up, as the console's own games
     * take it (android32 rt_pad.c): its face buttons and stick turn with it, SL / SR = L / R, its + or - =
     * +, the stick's click = ZL */
    int single = (st & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
                 !(st & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey));
    if (single) {
        u64 o = b & ~(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_Up |
                      HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_Minus |
                      HidNpadButton_StickL | HidNpadButton_StickR);
        if (st & HidNpadStyleTag_NpadJoyLeft) {
            if (b & HidNpadButton_Down) o |= HidNpadButton_A;
            if (b & HidNpadButton_Left) o |= HidNpadButton_B;
            if (b & HidNpadButton_Up) o |= HidNpadButton_Y;
            if (b & HidNpadButton_Right) o |= HidNpadButton_X;
        } else {
            if (b & HidNpadButton_X) o |= HidNpadButton_A;
            if (b & HidNpadButton_A) o |= HidNpadButton_B;
            if (b & HidNpadButton_B) o |= HidNpadButton_Y;
            if (b & HidNpadButton_Y) o |= HidNpadButton_X;
        }
        if (b & (HidNpadButton_LeftSL | HidNpadButton_RightSL)) o |= HidNpadButton_L;
        if (b & (HidNpadButton_LeftSR | HidNpadButton_RightSR)) o |= HidNpadButton_R;
        if (b & (HidNpadButton_Minus | HidNpadButton_Plus)) o |= HidNpadButton_Plus;
        if (b & (HidNpadButton_StickL | HidNpadButton_StickR)) o |= HidNpadButton_ZL;
        b = o;
    }
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (b & map[i].nx) in.buttons |= map[i].pb;
    /* a lone right Joy-Con's one stick is its "right" one */
    HidAnalogStickState l = padGetStickPos(ps, single && (st & HidNpadStyleTag_NpadJoyRight) ? 1 : 0),
                        r = padGetStickPos(ps, 1);
    in.lx = (float)l.x / 32767.f;
    in.ly = (float)l.y / 32767.f;
    in.rx = single ? 0.f : (float)r.x / 32767.f;
    in.ry = single ? 0.f : (float)r.y / 32767.f;
    if (single) {
        float cx = in.lx, cy = in.ly;
        if (st & HidNpadStyleTag_NpadJoyLeft) in.lx = -cy, in.ly = cx; /* turned anticlockwise */
        else in.lx = cy, in.ly = -cx;                                  /* the right one, clockwise */
    }
    static u64 last_st[2];
    int k = ps == &pads[0] ? 0 : 1;
    if (st != last_st[k]) {
        last_st[k] = st;
        rt_log("[pad] controller %d: style %llx%s\n", k + 1, (unsigned long long)st, single ? " (one Joy-Con, sideways)" : "");
    }
    return in;
}

/* The Switch keyboard for the game's text boxes (the profile name), as in PvZ Touch: A or a tap on a
 * focused text box opens it prefilled; OK replaces the text and presses Enter, Cancel changes nothing */
/* The console's controller applet for two players (the Multiplayer screen's CUSTOMIZE): controllers are
 * connected or given to P1 / P2 there. Either player may stay without one (a computer opponent, or touch). */
/* back from a system applet (the controller screen, the keyboard): its time left out of the game's clocks,
 * and the game's current frame shown on both swapchain images (the screen came back on an older one: a
 * flash of the frame before) */
void rt_time_away(uint64_t ns);
static void gpu_present(void);
static int use_gpu, gpu_pending; /* (tentative: defined with the renderer below) */
static void applet_back(uint64_t t0)
{
    uint64_t t = now_ns();
    rt_time_away(t - t0);
    last_input_ns = t;
    if (use_gpu && inited && !gpu_pending) {
        gpu_present();
        gpu_present();
    }
}

void rt_controller_applet(void)
{
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_min = 0;
    arg.hdr.player_count_max = 2;
    arg.hdr.enable_take_over_connection = 1;
    arg.hdr.enable_left_justify = 1;
    arg.hdr.enable_permit_joy_dual = 1;
    arg.hdr.enable_single_mode = 0;
    arg.hdr.enable_identification_color = 0;
    arg.enable_explain_text = 1;
    hidLaSetExplainText(&arg, "P1", HidNpadIdType_No1);
    hidLaSetExplainText(&arg, "P2", HidNpadIdType_No2);
    HidLaControllerSupportResultInfo info;
    uint64_t t0 = now_ns();
    Result rc = hidLaShowControllerSupportForSystem(&info, &arg, false);
    if (R_FAILED(rc)) rc = hidLaShowControllerSupport(&info, &arg);
    rt_log("[nx] controller applet: %x, %d players\n", (unsigned)rc, R_SUCCEEDED(rc) ? (int)info.player_count : -1);
    applet_back(t0);
}

void rt_text_keyboard(void)
{
    uint32_t edit = game_focused_edit();
    if (!edit) return;
    char cur[64], out[64];
    game_edit_text(edit, cur, sizeof cur);
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        rt_log("[nx] no software keyboard\n");
        return;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_QWERTY);
    swkbdConfigSetHeaderText(&kbd, "Please enter your name:");
    swkbdConfigSetInitialText(&kbd, cur);
    swkbdConfigSetStringLenMax(&kbd, 12); /* the game's own limit for profile names */
    swkbdConfigSetBlurBackground(&kbd, true);
    uint64_t t0 = now_ns();
    Result rc = swkbdShow(&kbd, out, sizeof out);
    swkbdClose(&kbd);
    applet_back(t0);
    if (R_SUCCEEDED(rc) && out[0]) game_edit_replace(edit, out);
    last_input_ns = now_ns(); /* the keyboard paused us; no huge cursor step afterwards */
}

/* the edit widget's rectangle on the display: its mX/mY are relative to its parents, so walk up */
/* a tap (display coordinates) on text box w: its place on the screen is its own plus its parents' (+0x34 / +0x38,
 * the parent at +0x14 up to the widget manager); the display is the game's 800x600 at x1.2 from x 160 */
static int edit_hit(uint32_t w, int x, int y)
{
    int32_t wx = 0, wy = 0;
    for (uint32_t p = w; p && rt_vm_mapped(p); p = g_rd32(p + 0x14)) {
        if (game_rtti_is(p, "WidgetManager@Sexy")) break;
        wx += (int32_t)g_rd32(p + 0x34), wy += (int32_t)g_rd32(p + 0x38);
    }
    float gx = ((float)x - 160.f) / 1.2f, gy = (float)y / 1.2f;
    float ww = (float)(int32_t)g_rd32(w + 0x3c), hh = (float)(int32_t)g_rd32(w + 0x40);
    return gx >= (float)wx - 8 && gx < (float)wx + ww + 8 && gy >= (float)wy - 8 && gy < (float)wy + hh + 8;
}

static void input_poll(void)
{
    uint64_t t = now_ns();
    float dt = (float)(t - last_input_ns) / 1e9f;
    last_input_ns = t;
    PadIn in[2] = {read_pad(&pads[0]), read_pad(&pads[1])};
    pad_input_update(in, dt);

    HidTouchScreenState ts;
    int got = (int)hidGetTouchScreenStates(&ts, 1);
    int touch_now = got && ts.count > 0;
    static int reported;
    if (!reported && got) {
        reported = 1;
        rt_log("[touch] touch screen states available (count %d)\n", (int)ts.count);
    }
    if (touch_now) {
        int x = (int)ts.touches[0].x, y = (int)ts.touches[0].y;
        if (x != touch_x || y != touch_y || !touching) {
            touch_x = x;
            touch_y = y;
            pad_input_pointer(x, y);
            touch_mouse(WM_MOUSEMOVE, touching ? 1 : 0);
        }
        if (!touching) {
            touch_mouse(WM_LBUTTONDOWN, 1);
            touch_x0 = touch_x;
            touch_y0 = touch_y;
            int tx = touch_x, ty = touch_y;
            rt_screen_to_display(&tx, &ty);
            touch_held0 = game_touch_on_shovel(tx, ty); /* the shovel picked up by this touch */
            /* a tap inside a text box opens the keyboard for that box: the focused one, or (the Multiplayer
             * screen's two names) the one the tap has just focused (the press above went to the game first) */
            uint32_t edit = game_focused_edit();
            int x = touch_x, y = touch_y;
            rt_screen_to_display(&x, &y);
            if (edit && !edit_hit(edit, x, y)) edit = 0;
            if (edit) {
                touch_mouse(WM_LBUTTONUP, 0);
                touching = 1;
                rt_text_keyboard();
                return;
            }
        }
    } else if (touching) {
        touch_mouse(WM_LBUTTONUP, 0);
        /* drag and drop: a seed picked up by this touch (or held from before) and let go somewhere else
         * is planted there, as a second click would */
        int dx = touch_x - touch_x0, dy = touch_y - touch_y0;
        /* the same for the shovel: dragged off its button onto a plant digs it on release */
        if ((game_held_packet() >= 0 || touch_held0) && dx * dx + dy * dy > 40 * 40) {
            touch_mouse(WM_LBUTTONDOWN, 1);
            touch_mouse(WM_LBUTTONUP, 0);
        }
    }
    touching = touch_now;
}

/* ---------------- display ---------------- */
/* a plain arrow, outlined, for the stick-driven cursor */
static const char *arrow[] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ", "X.....X    ",
    "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ", "X.X X..X   ", "XX  X..X   ",
    "X    X..X  ", "     X..X  ", "      XX   ",
};

static void open_display(void)
{
    if (fb_ready) return;
    if (R_SUCCEEDED(framebufferCreate(&fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2))) {
        framebufferMakeLinear(&fb);
        fb_ready = 1;
    } else {
        rt_log("[nx] no framebuffer\n");
    }
}

static void draw_frame(void)
{
    if (!fb_ready || !last_bits) return;
    dirty = 0;
    last_present_ns = now_ns();
    u32 stride;
    uint32_t *dst = framebufferBegin(&fb, &stride);
    int w = last_w < 1280 ? last_w : 1280, h = last_h < 720 ? last_h : 720;
    const uint8_t *src = G2H(last_bits);
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * (size_t)last_pitch);
        uint32_t *d = (uint32_t *)((uint8_t *)dst + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            uint32_t p = s[x]; /* ARGB -> RGBA in memory (R in the low byte) */
            d[x] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff);
        }
    }
    int cursor_x, cursor_y, show_cursor = pad_input_cursor(&cursor_x, &cursor_y);
    for (int y = 0; show_cursor && y < (int)(sizeof arrow / sizeof *arrow); y++)
        for (int x = 0; arrow[y][x]; x++) {
            int px = cursor_x + x, py = cursor_y + y;
            if (arrow[y][x] == ' ' || px >= 1280 || py >= 720) continue;
            ((uint32_t *)((uint8_t *)dst + (size_t)py * stride))[px] = arrow[y][x] == 'X' ? 0xff000000u : 0xffffffffu;
        }
    framebufferEnd(&fb); /* waits for a free buffer: vsync */
}

/* deko3d draws unless the folder has a file named "soft" (then the software renderer and a framebuffer) */
static int use_gpu, gpu_pending;
void gpu_dk_init(void);

/* the first frame: close the boot screen and open the real display */
static void open_real_display(void)
{
    if (!gpu_pending) return;
    gpu_pending = 0;
    /* no last boot-screen frame: on hardware, the console's first draw after the game's long silent load
     * (16 s) failed to get a framebuffer and libnx aborted (framebufferBegin, 2026-10-28) */
    rt_boot_screen_close();
    if (use_gpu) gpu_dk_init();
    else open_display();
}
void gpu_dk_init(void);
void gpu_dk_present(int cursor, int cx, int cy);

static int shown_c = -1, shown_x, shown_y;
static void gpu_present(void)
{
    int cx, cy, c = pad_input_cursor(&cx, &cy);
    shown_c = c;
    shown_x = cx;
    shown_y = cy;
    dirty = 0;
    last_present_ns = now_ns();
    gpu_dk_present(c, cx, cy);
}

void rt_gpu_start(void)
{
    if (inited) return;
    inited = 1;
    char path[600];
    snprintf(path, sizeof path, "%s/soft", rt_game_dir);
    FILE *f = fopen(path, "rb");
    use_gpu = !f;
    if (f) fclose(f);
    /* The game opens its display long before its first frame (it loads its resources first): the boot
     * screen stays up until that frame, then the console gives the window to deko3d / the framebuffer.
     * Until then the game's surfaces live in guest memory only (rt_gpu is NULL). */
    rt_boot_progress("Loading the game's resources", 920);
    gpu_pending = 1;
    rt_log("[nx] renderer: %s\n", use_gpu ? "deko3d" : "software");
    audio_init();
    input_init();
}

/* The game's debug messages while it loads: the boot screen follows its resource groups, and closes once
 * the group with the game's own loading screen is in ("LOADED 'LoaderBar'") */
static volatile int loaderbar_ready;
void rt_game_debug_text(const char *s)
{
    if (!gpu_pending) return;
    const char *q = strstr(s, "LOADED");
    if (!q) return;
    char what[64];
    const char *a = strchr(q, '\''), *b = a ? strchr(a + 1, '\'') : NULL;
    if (a && b && b - a < 48) {
        snprintf(what, sizeof what, "Loading %.*s", (int)(b - a - 1), a + 1);
        rt_boot_progress(what, 940);
    }
    if (strstr(q, "LoaderBar")) loaderbar_ready = 1;
}

void rt_present(uint32_t bits, int w, int h, int pitch)
{
    if (gpu_pending && !loaderbar_ready) return; /* still the boot screen */
    open_real_display();
    if (use_gpu) {
        last_w = w;
        last_h = h;
        dirty = 1;
        return;
    }
    if (!bits) bits = rt_screen_bits();
    last_bits = bits;
    last_w = w;
    last_h = h;
    last_pitch = pitch;
    dirty = 1;
}

int rt_wait_vblank(void)
{
    if (gpu_pending) return 0;
    if (use_gpu) {
        /* the swapchain paces: presenting waits for a free image (vsync) */
        if (dirty) gpu_present();
        else svcSleepThread(16000000);
        last_vblank_ns = now_ns();
        return 1;
    }
    if (!fb_ready) return 0;
    if (dirty) draw_frame();
    last_vblank_ns = now_ns();
    return 1;
}

void rt_platform_poll(void)
{
    if (!inited) rt_gpu_start();
    if (!appletMainLoop()) {
        static int closing;
        if (!closing++) rt_post_message(rt_main_hwnd, WM_CLOSE, 0, 0);
        else if (closing > 600) exit(0);
    }
    input_poll();
    uint64_t t = now_ns();
    /* games that don't wait for the vertical blank: show at most 60 frames a second */
    /* the pointer moved while the game drew nothing new (a paused menu redraws only on changes): show the
     * last frame again with it */
    if (use_gpu && !gpu_pending && !dirty && t - last_present_ns >= 16000000ull) {
        int cx, cy, c = pad_input_cursor(&cx, &cy);
        if (c != shown_c || (c && (cx != shown_x || cy != shown_y))) gpu_present();
    }
    if (dirty && t - last_vblank_ns > 100000000ull && t - last_present_ns >= 16000000ull) {
        if (use_gpu) gpu_present();
        else draw_frame();
    }
}

/* ---------------- crashes ---------------- */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

extern char __start__[]; /* the NRO's first byte (libnx's linker script): pc - this = the address in pvzdlc.elf */

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    char what[96];
    snprintf(what, sizeof what, "exception %u (pc %llx = pvzdlc.elf %llx, lr %llx)", (unsigned)ctx->error_desc,
             (unsigned long long)ctx->pc.x, (unsigned long long)(ctx->pc.x - (uintptr_t)__start__),
             (unsigned long long)(ctx->lr.x - (uintptr_t)__start__));
    rt_report_fault(what, (void *)(uintptr_t)ctx->far.x);
    rt_log_flush();
    svcExitProcess();
}

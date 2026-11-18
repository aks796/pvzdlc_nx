/* platform_sdl.c: desktop test host display and input.
 * Presents frames in an SDL2 window and turns mouse/keyboard into window
 * messages. The game writes the primary surface many times a frame (fills,
 * blits, Unlock, Flip) and not always from the window's thread, so rt_present
 * only records the frame; the message pump (rt_platform_poll, the window's own
 * thread, between game frames) draws it. In fullscreen mode the game calls
 * WaitForVerticalBlank once a frame; that is where the window presents, on the
 * display's real vsync. Hidden or minimised windows don't present at all. RT_HEADLESS=1 skips the window; RT_SHOTS=N saves every Nth frame
 * to <game>/shots/frame_NNNNN.png (headless or not). */
#include <SDL.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread/qos.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rt.h"
#include "rt_audio.h"
#include "pad_input.h"

void rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
extern uint32_t rt_main_hwnd;
extern int32_t rt_mouse_x, rt_mouse_y;
extern uint32_t rt_img_delta;

static int inited, headless, shot_every, frames;
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static uint32_t buttons;
static volatile int dirty, have_frame;
static int window_failed;
/* OpenGL window: the GPU backend (gpu_gl.c) draws, the window shows its display texture */
static SDL_GLContext glc, glc_present;
static int use_gl, threaded_present;
void gpu_gl_init(void);
void gpu_gl_present(int win_w, int win_h);
int gpu_gl_snapshot(void **pixels, uint32_t *w, uint32_t *h);
void gpu_gl_publish(void);
int gpu_gl_present_next(int win_w, int win_h, int timeout_ms);
void gpu_gl_swapped(void);
void gpu_gl_wait_swap(int timeout_ms);
static int window_visible(void);
static void *presenter(void *arg);
uint32_t rt_screen_bits(void);
static pthread_t pump_thread;
static int have_pump, presents_since_poll, max_presents_per_poll;
static uint64_t last_present_us, last_vblank_us;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

/* macOS coalesces timers of background-priority threads: the game's Sleep(1) frame pacing then
 * comes back 15-165 ms late, which is a visible hitch. User-interactive threads keep tight timers. */
void rt_platform_thread_start(void)
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    /* App Nap stretches the timers of apps that aren't in front (a hidden or covered window) the same
     * way; a latency-critical activity for the whole run turns that off */
    static int once;
    if (!once++) {
        void *msg = (void *)objc_msgSend; /* always called through a cast to the method's real type */
        id pi = ((id (*)(id, SEL))msg)((id)objc_getClass("NSProcessInfo"), sel_registerName("processInfo"));
        id why = ((id (*)(id, SEL, const char *))msg)((id)objc_getClass("NSString"),
                                                      sel_registerName("stringWithUTF8String:"), "game loop");
        unsigned long long opts = 0x00FFFFFFULL | (1ULL << 20) /* NSActivityUserInitiated */ |
                                  0xFF00000000ULL /* NSActivityLatencyCritical */;
        id act = ((id (*)(id, SEL, unsigned long long, id))msg)(
            pi, sel_registerName("beginActivityWithOptions:reason:"), opts, why);
        ((id (*)(id, SEL))msg)(act, sel_registerName("retain"));
    }
#endif
}

/* The game paces its loop with Sleep(1). Under CPU load a normal thread wakes from that late (8% of
 * wake-ups over 5 ms with all cores busy, up to 40 ms) and the frame hitches. A time-constraint
 * (real-time) thread wakes on time, so a thread is real-time only while it sleeps. */
void rt_platform_sleep_us(uint32_t us)
{
#ifdef __APPLE__
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    double per_ms = 1e6 * (double)tb.denom / (double)tb.numer;
    thread_time_constraint_policy_data_t rt = {(uint32_t)(10 * per_ms), (uint32_t)(2 * per_ms), (uint32_t)(10 * per_ms), 1};
    thread_standard_policy_data_t std = {0};
    mach_port_t self = pthread_mach_thread_np(pthread_self());
    int ok = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&rt,
                               THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
    usleep(us);
    if (ok) thread_policy_set(self, THREAD_STANDARD_POLICY, (thread_policy_t)&std, THREAD_STANDARD_POLICY_COUNT);
#else
    usleep(us);
#endif
}

/* ---- controllers: SDL game controllers, or a virtual one from the control pipe ---- */
static PadIn ctl_pad[2];
static SDL_GameController *gcs[2];

static void pads_poll(float dt)
{
    static int inited_gc;
    if (!inited_gc) {
        inited_gc = 1;
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) SDL_GameControllerEventState(SDL_IGNORE);
    }
    SDL_GameControllerUpdate();
    for (int i = 0, k = 0; i < SDL_NumJoysticks() && k < 2; i++) {
        if (!SDL_IsGameController(i)) continue;
        if (!gcs[k] || !SDL_GameControllerGetAttached(gcs[k])) gcs[k] = SDL_GameControllerOpen(i);
        k++;
    }
    static const struct { SDL_GameControllerButton b; uint32_t pb; } map[] = {
        /* by the letter on the button: the prompts are coloured Xbox letters (an Xbox pad's green A is A); SDL
         * names a Nintendo pad's buttons by their own letters too (SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS) */
        {SDL_CONTROLLER_BUTTON_A, PB_A}, {SDL_CONTROLLER_BUTTON_B, PB_B}, {SDL_CONTROLLER_BUTTON_X, PB_X},
        {SDL_CONTROLLER_BUTTON_Y, PB_Y}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PB_L},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PB_R}, {SDL_CONTROLLER_BUTTON_START, PB_PLUS},
        {SDL_CONTROLLER_BUTTON_BACK, PB_MINUS}, {SDL_CONTROLLER_BUTTON_DPAD_UP, PB_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PB_DOWN}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PB_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PB_RIGHT},
    };
    PadIn in[2];
    for (int k = 0; k < 2; k++) {
        in[k] = ctl_pad[k];
        SDL_GameController *g = gcs[k];
        if (!g || !SDL_GameControllerGetAttached(g)) continue;
        in[k].connected = 1;
        for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
            if (SDL_GameControllerGetButton(g, map[i].b)) in[k].buttons |= map[i].pb;
        if (SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) in[k].buttons |= PB_ZL;
        if (SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) in[k].buttons |= PB_ZR;
        float lx = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
        float ly = -SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f;
        if (lx * lx + ly * ly > in[k].lx * in[k].lx + in[k].ly * in[k].ly) { in[k].lx = lx; in[k].ly = ly; }
    }
    pad_input_update(in, dt);
}

/* The text keyboard (Switch: swkbd). Here: RT_CTL "name:abc" fills the focused text box with abc, the
 * same path the Switch keyboard's OK takes; a pad's A on a text box just logs */
/* (the console's controller applet: nothing to open on the desktop) */
void rt_controller_applet(void) { rt_log("[sdl] controller applet (Switch only)\n"); }

void rt_text_keyboard(void)
{
#ifdef __ANDROID__
    /* the phone's keyboard: its letters come as SDL_TEXTINPUT (WM_CHAR), its Enter accepts the dialog; it is put
     * away again once no text box has the focus (rt_platform_poll) */
    SDL_StartTextInput();
#else
    rt_log("[sdl] text keyboard requested (on the Mac, type into the window or send name:<text>)\n");
#endif
}

static void touch_at(int64_t id, int kind, int x, int y);

/* ---- scripted input ---- */
typedef struct { int frame; char kind[8]; int a, b; char text[64]; } ScriptEv;
static ScriptEv script[128];
static int nscript, script_pos;
static void parse_script(const char *s)
{
    while (s && *s && nscript < 128) {
        ScriptEv *e = &script[nscript];
        char kind[16] = "";
        int n = 0;
        if (sscanf(s, "%d:%15[a-z]%n", &e->frame, kind, &n) < 2) break;
        snprintf(e->kind, sizeof e->kind, "%s", kind);
        const char *p = s + n;
        if (*p == ':') {
            p++;
            if (!strcmp(kind, "text")) {
                size_t k = strcspn(p, ";");
                snprintf(e->text, sizeof e->text, "%.*s", (int)k, p);
            } else {
                sscanf(p, "%d,%d", &e->a, &e->b);
            }
        }
        nscript++;
        s = strchr(s, ';');
        if (s) s++;
    }
}
static void write_png(const char *path, const uint8_t *px, int w, int h, int pitch);
static uint32_t last_bits;
static int last_w, last_h, last_pitch;
static void exec_ev(const ScriptEv *e, const char *shot_name)
{
    uint32_t hw = rt_main_hwnd;
    {
        if (!strcmp(e->kind, "click") || !strcmp(e->kind, "rclick")) {
            int right = e->kind[0] == 'r';
            uint32_t lp = ((uint32_t)e->b << 16) | ((uint32_t)e->a & 0xffff);
            rt_mouse_x = e->a;
            rt_mouse_y = e->b;
            rt_post_message(hw, 0x200, 0, lp);
            rt_post_message(hw, right ? 0x204 : 0x201, right ? 2 : 1, lp);
            rt_post_message(hw, right ? 0x205 : 0x202, 0, lp);
        } else if (!strcmp(e->kind, "move")) {
            rt_mouse_x = e->a;
            rt_mouse_y = e->b;
            pad_input_pointer(e->a, e->b); /* as a touch / the mouse would */
            rt_post_message(hw, 0x200, 0, ((uint32_t)e->b << 16) | ((uint32_t)e->a & 0xffff));
        } else if (!strcmp(e->kind, "down") || !strcmp(e->kind, "up")) { /* left button held / released */
            uint32_t lp = ((uint32_t)e->b << 16) | ((uint32_t)e->a & 0xffff);
            rt_mouse_x = e->a;
            rt_mouse_y = e->b;
            rt_post_message(hw, 0x200, e->kind[0] == 'd' ? 1 : 0, lp);
            rt_post_message(hw, e->kind[0] == 'd' ? 0x201 : 0x202, e->kind[0] == 'd' ? 1 : 0, lp);
        } else if (!strcmp(e->kind, "pad") && e->a >= 0 && e->a < 2) { /* pad:P,MASK (pad_input.h PB_*) */
            ctl_pad[e->a].connected = 1;
            ctl_pad[e->a].buttons = (uint32_t)e->b;
        } else if ((e->kind[0] == 'l' || e->kind[0] == 'r') && (e->kind[1] == 'x' || e->kind[1] == 'y') && !e->kind[2] &&
                   e->a >= 0 && e->a < 2) { /* lx / ly / rx / ry:P,-100..100 */
            ctl_pad[e->a].connected = 1;
            float v = (float)e->b / 100.f;
            if (e->kind[0] == 'l') *(e->kind[1] == 'x' ? &ctl_pad[e->a].lx : &ctl_pad[e->a].ly) = v;
            else *(e->kind[1] == 'x' ? &ctl_pad[e->a].rx : &ctl_pad[e->a].ry) = v;
        } else if (!strcmp(e->kind, "bsnap") || !strcmp(e->kind, "bdiff")) {
            /* the board's memory: bsnap remembers it, bdiff logs the words that changed since */
            static uint32_t snap[0x8000 / 4];
            uint32_t app = rt_app_object(), b = app ? g_rd32(app + 0x780) : 0;
            if (e->a == 1 && app) { /* bsnap:1 / bdiff:1 = the seed chooser instead of the board */
                b = 0;
                for (uint32_t o = 0; o < 0x1000 && !b; o += 4)
                    if (game_rtti_is(g_rd32(app + o), "SeedChooserScreen")) {
                        b = g_rd32(app + o);
                        rt_log("[bdiff] seed chooser at app+%03x\n", o);
                    }
            }
            if (e->a == 2 && b) b = g_rd32(b + 0x178); /* bsnap:2 = P1's keyboard cursor */
            if (e->a == 3 && b) b = g_rd32(b + 0x148); /* bsnap:3 = P1's seed bank */
            if (e->a == 4 && b) b = g_rd32(b + 0x13c); /* bsnap:4 = the mouse's cursor object */
            if (e->a == 5) b = game_top_widget(); /* bsnap:5 = the top screen or dialog */
            if (e->a == 6 && app) b = g_rd32(app + 0x320); /* bsnap:6 = the WidgetManager */
            if (e->a >= 2) rt_log("[bdiff] object %s\n", game_rtti_name(b));
            if (b >= 0x10000u && b < 0xE0000000u) {
                int n = 0;
                for (uint32_t i = 0; i < 0x8000 / 4; i++) {
                    uint32_t v = g_rd32(b + i * 4);
                    if (e->kind[1] == 'd' && v != snap[i] && n++ < 80)
                        rt_log("[bdiff] board+%04x: %08x -> %08x\n", i * 4, (unsigned)snap[i], (unsigned)v);
                    snap[i] = v;
                }
                rt_log("[bdiff] %s board %08x (%d changed)\n", e->kind, (unsigned)b, n);
            }
        } else if (!strcmp(e->kind, "wheel")) { /* wheel:N, N notches (120 each), + = up */
            rt_post_message(rt_main_hwnd, 0x20A, (uint32_t)(e->a * 120) << 16, ((uint32_t)rt_mouse_y << 16) | ((uint32_t)rt_mouse_x & 0xffff));
        } else if (!strcmp(e->kind, "dumpimg")) { /* dumpimg:N = the image at global N (decimal) -> shots/img_N.png */
            uint32_t im = g_rd32((uint32_t)e->a + rt_img_delta);
            if (im && g_rd32(im + 0x38)) {
                char p[600];
                snprintf(p, sizeof p, "%s/shots/img_%x.png", rt_game_dir, (unsigned)e->a);
                int w = (int)g_rd32(im + 0x24), h = (int)g_rd32(im + 0x28);
                write_png(p, G2H(g_rd32(im + 0x38)), w, h, w * 4);
            }
        } else if (!strcmp(e->kind, "scroll")) { /* scroll:N = ChallengeScreen +0x3f8 = N (float) */
            uint32_t w = game_top_widget();
            if (w) {
                float v = (float)e->a;
                memcpy(G2H(w + 0x3f8), &v, 4);
            }
        } else if (!strcmp(e->kind, "fdown") || !strcmp(e->kind, "fmove") || !strcmp(e->kind, "fup")) {
            /* fdown / fmove / fup:ID,X,Y: a finger on the touch screen at display point X, Y (as Android's) */
            int id = 0, x = 0, y = 0;
            if (sscanf(e->text, "%d,%d,%d", &id, &x, &y) == 3) {
                rt_mouse_x = x, rt_mouse_y = y;
                touch_at(id, e->kind[1] == 'd' ? 0 : e->kind[1] == 'm' ? 1 : 2, x, y);
            }
        } else if (!strcmp(e->kind, "csnap") || !strcmp(e->kind, "cdiff")) { /* the seed chooser's first 0x800 bytes */
            static uint8_t snap[0x800];
            uint32_t app = rt_app_object(), sc = app ? g_rd32(app + 0x78c) : 0;
            if (sc && !strcmp(e->kind, "cdiff"))
                for (uint32_t o = 0; o < sizeof snap; o += 4) {
                    uint32_t was, now = g_rd32(sc + o);
                    memcpy(&was, snap + o, 4);
                    if (was != now) rt_log("[cdiff] chooser+%03x: %08x -> %08x\n", (unsigned)o, (unsigned)was, (unsigned)now);
                }
            if (sc) memcpy(snap, G2H(sc), sizeof snap);
        } else if (!strcmp(e->kind, "bpoke")) { /* bpoke:OFF,VALUE = the board's word at +OFF (decimal) */
            extern uint32_t pad_board(void);
            uint32_t b = pad_board();
            if (b) g_wr32(b + (uint32_t)e->a, (uint32_t)e->b), rt_log("[ctl] board+0x%x = %d\n", (unsigned)e->a, e->b);
        } else if (!strcmp(e->kind, "bushes")) { /* bushes:MASK = the widescreen bushes drawn (bit = row) */
            extern int wide_bush_mask;
            wide_bush_mask = e->a;
        } else if (!strcmp(e->kind, "sdlmove") || !strcmp(e->kind, "sdldown") || !strcmp(e->kind, "sdlup")) {
            /* sdlmove / sdldown / sdlup:X,Y = the real mouse there (window point), through SDL's events */
            SDL_Event ev;
            memset(&ev, 0, sizeof ev);
            if (e->kind[3] == 'm') ev.type = SDL_MOUSEMOTION, ev.motion.x = e->a, ev.motion.y = e->b;
            else ev.type = e->kind[3] == 'd' ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP, ev.button.button = SDL_BUTTON_LEFT,
                 ev.button.clicks = 1, ev.button.x = e->a, ev.button.y = e->b;
            SDL_PushEvent(&ev);
        } else if (!strcmp(e->kind, "vsbtn")) {
            extern void pad_versus_buttons_log(void);
            pad_versus_buttons_log();
        } else if (!strcmp(e->kind, "kbcur")) {
            extern void game_kb_cursor_log(void);
            game_kb_cursor_log();
        } else if (!strcmp(e->kind, "cursor")) {
            extern uint32_t rt_game_cursor(void);
            rt_log("[cursor] %08x at %d,%d\n", (unsigned)rt_game_cursor(), (int)rt_mouse_x, (int)rt_mouse_y);
        } else if (!strcmp(e->kind, "fonts")) {
            game_font_probe();
        } else if (!strcmp(e->kind, "images")) {
            game_image_probe();
        } else if (!strcmp(e->kind, "wtree")) {
            game_widget_tree();
        } else if (!strcmp(e->kind, "dlgdump")) { /* the top dialog's first 0x400 bytes */
            extern uint32_t game_top_widget(void);
            uint32_t w = game_top_widget();
            for (uint32_t o = 0; w && o < 0x400; o += 32) {
                char b[160]; int k = 0;
                for (uint32_t i = 0; i < 32; i += 4) k += snprintf(b + k, sizeof b - k, " %08x", g_rd32(w + o + i));
                rt_log("[dlg] %s +%03x%s\n", game_rtti_name(w), (unsigned)o, b);
            }
        } else if (!strcmp(e->kind, "focus")) {
            uint32_t app = rt_app_object(), wm = app ? g_rd32(app + 0x320) : 0;
            if (wm) {
                rt_log("[focus] focus %s, over %s, lastdown %s\n", game_rtti_name(g_rd32(wm + 0x8c)),
                       game_rtti_name(g_rd32(wm + 0x94)), game_rtti_name(g_rd32(wm + 0x90)));
                for (uint32_t o = 0; o < 0x1000; o += 4) {
                    /* MSVC std::list {proxy, head, size}: head node {next, prev, value} */
                    uint32_t head = g_rd32(app + o + 4), n = g_rd32(app + o + 8);
                    if (head < 0x10000 || head >= 0xE0000000u || n == 0 || n > 8) continue;
                    uint32_t node = g_rd32(head);
                    if (node < 0x10000 || node >= 0xE0000000u) continue;
                    const char *nm = game_rtti_name(g_rd32(node + 8));
                    if (*nm) rt_log("[focus] app+%03x list of %u, first %s\n", o, (unsigned)n, nm);
                }
            }
        } else if (!strcmp(e->kind, "ttprobe")) {
            /* tooltips (ToolTipWidget: 3 std::strings of 28 bytes, mX +0x54, mY +0x58, mVisible +0x64) the
             * seed chooser points at */
            uint32_t app = rt_app_object(), sc = app ? g_rd32(app + 0x78c) : 0;
            for (uint32_t o = 0; sc && o < 0x400; o += 4) {
                uint32_t t = g_rd32(sc + o);
                if (t < 0x10000 || t >= 0xE0000000u) continue;
                uint32_t sz = g_rd32(t + 20), cap = g_rd32(t + 24);
                if (sz > 0 && sz < 64 && cap >= 15 && cap < 256) {
                    char txt[80];
                    uint32_t src = cap > 15 ? g_rd32(t + 4) : t + 4;
                    if (src < 0x10000 || src >= 0xE0000000u) continue;
                    rt_a2u(src, txt, sizeof txt);
                    if (strlen(txt) != sz) continue;
                    rt_log("[tt] chooser+%03x -> %08x title \"%s\" x %d y %d visible %u\n", o, (unsigned)t, txt,
                           (int)g_rd32(t + 0x54), (int)g_rd32(t + 0x58), (unsigned)(*(uint8_t *)G2H(t + 0x64)));
                }
            }
        } else if (!strcmp(e->kind, "name")) {
            uint32_t edit = game_focused_edit();
            rt_log("[sdl] name: focused text box %08x\n", (unsigned)edit);
            if (edit) game_edit_replace(edit, e->text);
        } else if (!strcmp(e->kind, "keydown")) {
            rt_post_message(hw, 0x100, (uint32_t)e->a, 1);
            if (e->a == 13 || e->a == 8 || e->a == 27 || e->a == 32) rt_post_message(hw, 0x102, (uint32_t)e->a, 1);
        } else if (!strcmp(e->kind, "keyup")) {
            rt_post_message(hw, 0x101, (uint32_t)e->a, 0xC0000001u);
        } else if (!strcmp(e->kind, "key")) {
            rt_post_message(hw, 0x100, (uint32_t)e->a, 1);
            if (e->a == 13 || e->a == 8 || e->a == 27) rt_post_message(hw, 0x102, (uint32_t)e->a, 1);
            rt_post_message(hw, 0x101, (uint32_t)e->a, 0xC0000001u);
        } else if (!strcmp(e->kind, "text")) {
            for (const char *p = e->text; *p; p++) rt_post_message(hw, 0x102, (uint8_t)*p, 1);
        } else if (!strcmp(e->kind, "shot") && have_frame) {
            uint32_t b = last_bits ? last_bits : rt_screen_bits();
            char p[600];
            if (shot_name) snprintf(p, sizeof p, "%s/shots/%s.png", rt_game_dir, shot_name);
            else snprintf(p, sizeof p, "%s/shots/shot_%05d.png", rt_game_dir, frames);
            void *ov_px;
            uint32_t ow, oh;
            if (use_gl && gpu_gl_snapshot(&ov_px, &ow, &oh)) {
                write_png(p, ov_px, (int)ow, (int)oh, (int)ow * 4);
                free(ov_px);
            } else if (b) write_png(p, G2H(b), last_w, last_h, last_pitch);
        } else if (!strcmp(e->kind, "quit")) {
            rt_log("[script] quit\n");
            exit(0);
        }
    }
}

static void run_script(void)
{
    while (script_pos < nscript && script[script_pos].frame <= frames) {
        ScriptEv *e = &script[script_pos++];
        rt_log("[script] frame %d: %s %d,%d %s\n", frames, e->kind, e->a, e->b, e->text);
        exec_ev(e, NULL);
    }
}

/* RT_CTL=<fifo>: live commands, one per line, run as soon as they arrive: click:x,y rclick:x,y
 * move:x,y down:x,y up:x,y key:VK keydown:VK keyup:VK text:abc shot:NAME quit (display coordinates) */
static int ctl_fd = -1;
static char ctl_buf[4096];
static size_t ctl_len;
static void run_ctl(void)
{
#ifdef _WIN32
    /* (Windows has no FIFOs: RT_CTL is a plain file that grows, read on from where it was last read) */
    static long ctl_off;
    const char *path = getenv("RT_CTL");
    if (!path) return;
    if (ctl_fd < 0) {
        ctl_fd = 0;
        rt_log("[ctl] reading %s\n", path);
    }
    FILE *cf = fopen(path, "rb");
    if (cf) {
        fseek(cf, ctl_off, SEEK_SET);
        size_t n = fread(ctl_buf + ctl_len, 1, sizeof ctl_buf - 1 - ctl_len, cf);
        ctl_off += (long)n;
        ctl_len += n;
        fclose(cf);
    }
#else
    if (ctl_fd < 0) {
        const char *path = getenv("RT_CTL");
        static int tried;
        if (!path || tried++) return;
        mkfifo(path, 0600);
        ctl_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (ctl_fd >= 0) open(path, O_WRONLY | O_NONBLOCK); /* keep a writer so reads never see EOF */
        rt_log("[ctl] listening on %s\n", path);
        return;
    }
    ssize_t n = read(ctl_fd, ctl_buf + ctl_len, sizeof ctl_buf - 1 - ctl_len);
    if (n > 0) ctl_len += (size_t)n;
#endif
    char *nl;
    while ((nl = memchr(ctl_buf, '\n', ctl_len))) {
        *nl = 0;
        ScriptEv e;
        memset(&e, 0, sizeof e);
        char kind[16] = "", arg[64] = "";
        if (sscanf(ctl_buf, "%15[a-z]:%63[^\n]", kind, arg) >= 1) {
            snprintf(e.kind, sizeof e.kind, "%s", kind);
            if (!strcmp(kind, "text") || !strcmp(kind, "name") || kind[0] == 'f') snprintf(e.text, sizeof e.text, "%s", arg);
            else sscanf(arg, "%d,%d", &e.a, &e.b);
            rt_log("[ctl] frame %d: %s\n", frames, ctl_buf);
            exec_ev(&e, !strcmp(kind, "shot") ? arg : NULL);
        }
        size_t used = (size_t)(nl - ctl_buf) + 1;
        memmove(ctl_buf, ctl_buf + used, ctl_len - used);
        ctl_len -= used;
    }
}

/* ---- minimal PNG writer (stored deflate blocks) ---- */
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), data, len);
    be32(h, c);
    fwrite(h, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *px, int w, int h, int pitch)
{
    size_t raw_len = (size_t)h * (size_t)(1 + 3 * w);
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (size_t)(1 + 3 * w);
        const uint32_t *s = (const uint32_t *)(px + (size_t)y * (size_t)pitch);
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            r[1 + 3 * x] = (uint8_t)(s[x] >> 16);
            r[2 + 3 * x] = (uint8_t)(s[x] >> 8);
            r[3 + 3 * x] = (uint8_t)s[x];
        }
    }
    size_t nblk = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + 5 * nblk + 4;
    uint8_t *z = malloc(z_len), *o = z;
    *o++ = 0x78;
    *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_len; off += 65535) {
        uint32_t n = (uint32_t)(raw_len - off < 65535 ? raw_len - off : 65535);
        *o++ = off + n >= raw_len;
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + off, n);
        o += n;
    }
    be32(o, (b << 16) | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        fwrite(sig, 1, 8, f);
        uint8_t ih[13];
        be32(ih, (uint32_t)w);
        be32(ih + 4, (uint32_t)h);
        ih[8] = 8; ih[9] = 2; ih[10] = 0; ih[11] = 0; ih[12] = 0;
        chunk(f, "IHDR", ih, 13);
        chunk(f, "IDAT", z, (uint32_t)(o - z));
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw);
    free(z);
}

/* ---- audio: SDL playback, or RT_WAV=<file> to record the mix in real time ---- */
static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    rt_audio_mix((int16_t *)stream, len / 4);
}
static void *wav_thread(void *arg)
{
    FILE *f = fopen((const char *)arg, "wb");
    if (!f) return NULL;
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, 44, f);
    int16_t buf[441 * 2];
    uint32_t total = 0;
    for (;;) {
        rt_audio_mix(buf, 441); /* 10 ms */
        fwrite(buf, 4, 441, f);
        total += 441 * 4;
        if (total % (44100 * 4) == 0) { /* keep the header valid every second */
            long here = ftell(f);
            uint32_t v;
            fseek(f, 0, SEEK_SET);
            memcpy(hdr, "RIFF", 4); v = 36 + total; memcpy(hdr + 4, &v, 4); memcpy(hdr + 8, "WAVEfmt ", 8);
            v = 16; memcpy(hdr + 16, &v, 4); uint16_t w = 1; memcpy(hdr + 20, &w, 2); w = 2; memcpy(hdr + 22, &w, 2);
            v = 44100; memcpy(hdr + 24, &v, 4); v = 44100 * 4; memcpy(hdr + 28, &v, 4); w = 4; memcpy(hdr + 32, &w, 2);
            w = 16; memcpy(hdr + 34, &w, 2); memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &total, 4);
            fwrite(hdr, 1, 44, f);
            fflush(f);
            fseek(f, here, SEEK_SET);
        }
        usleep(10000);
    }
    return NULL;
}
static void audio_init(void)
{
    const char *wav = getenv("RT_WAV");
    if (wav) {
        pthread_t t;
        pthread_create(&t, NULL, wav_thread, (void *)wav);
        return;
    }
    if (headless) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    else rt_log("[sdl] no audio: %s\n", SDL_GetError());
}

static void init(void)
{
    if (inited) return;
    inited = 1;
    headless = getenv("RT_HEADLESS") && atoi(getenv("RT_HEADLESS"));
    shot_every = getenv("RT_SHOTS") ? atoi(getenv("RT_SHOTS")) : 0;
    parse_script(getenv("RT_INPUT"));
    if (shot_every || nscript) {
        char p[600];
        snprintf(p, sizeof p, "%s/shots", rt_game_dir);
        mkdir(p, 0755);
    }
    audio_init();
}

/* Only ever called from the game's main thread (the message pump), so all SDL video work stays on one
 * thread. The OpenGL window is the default; RT_SOFT=1 keeps the software renderer and an SDL_Renderer
 * window. Headless runs get a hidden OpenGL window so they draw on the GPU too. */
const char *rt_window_title = "PvZ DLC (recompiled)"; /* (the Mac app: the game's name) */
static void open_window(void)
{
    if (win || window_failed) return;
    int soft = getenv("RT_SOFT") && atoi(getenv("RT_SOFT"));
    if (headless && soft) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        rt_log("[sdl] video init failed: %s\n", SDL_GetError());
        window_failed = 1;
        return;
    }
    if (!soft) {
#ifdef __ANDROID__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        win = SDL_CreateWindow(rt_window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
#ifdef __ANDROID__
                                   SDL_WINDOW_FULLSCREEN |
#endif
                                   (headless ? SDL_WINDOW_HIDDEN : 0));
        if (win) glc = SDL_GL_CreateContext(win);
        if (glc) {
            SDL_GL_MakeCurrent(win, glc);
            gpu_gl_init();
            use_gl = 1;
#if defined(__ANDROID__) || defined(_WIN32)
            /* Android and Windows: one context, presenting from the game thread (EGL won't have the window's
             * surface current on two threads at once, as the Mac's presenter thread does; WGL drivers differ on it) */
            SDL_GL_SetSwapInterval(1);
            return;
#endif
            if (!headless) {
                /* the presenter: a second context in the same share group, on its own thread */
                SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
                glc_present = SDL_GL_CreateContext(win);
                SDL_GL_MakeCurrent(win, glc);
                pthread_t t;
                if (glc_present && pthread_create(&t, NULL, presenter, NULL) == 0) threaded_present = 1;
                else rt_log("[sdl] no presenter thread (%s); presenting from the game thread\n", SDL_GetError());
                if (!threaded_present) SDL_GL_SetSwapInterval(1);
                SDL_StartTextInput();
            }
            return;
        }
        rt_log("[sdl] no OpenGL 3 context (%s); software renderer\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        win = NULL;
        if (headless) {
            window_failed = 1;
            return;
        }
    }
    win = SDL_CreateWindow(rt_window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(ren, 1280, 720);
#ifndef __ANDROID__
    SDL_StartTextInput();
#endif
}

static int window_visible(void)
{
    return win && !(SDL_GetWindowFlags(win) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
}

void rt_gpu_start(void)
{
    init();
    open_window();
}

/* Shows the newest published frame on every vsync; the only thread that waits for the display */
static void *presenter(void *arg)
{
    (void)arg;
    rt_platform_thread_start();
    SDL_GL_MakeCurrent(win, glc_present);
    SDL_GL_SetSwapInterval(1);
    for (;;) {
        if (!window_visible()) {
            usleep(16667);
            gpu_gl_swapped();
            continue;
        }
        int dw, dh;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        if (!gpu_gl_present_next(dw, dh, 50)) continue;
        uint64_t t0 = now_us();
        SDL_GL_SwapWindow(win);
        uint64_t dt = now_us() - t0;
        /* a covered window's swap may not wait for anything: don't spin */
        if (dt < 1000) usleep(8333 - (useconds_t)dt);
        gpu_gl_swapped();
        if (rt_verbose) {
            /* new frames shown per second and the longest time between two of them */
            static uint64_t t_last, t_start, worst;
            static int shown_n;
            uint64_t t = now_us();
            if (t_last && t - t_last > worst) worst = t - t_last;
            t_last = t;
            if (!t_start) t_start = t;
            shown_n++;
            if (t - t_start >= 5000000) {
                rt_log("[present] %.1f new frames/s shown, longest gap %.1f ms\n", shown_n * 1e6 / (double)(t - t_start),
                       worst / 1000.0);
                t_start = t;
                shown_n = 0;
                worst = 0;
            }
        }
    }
    return NULL;
}

static void draw_window(int force)
{
    if (use_gl && threaded_present) {
        /* hand the frame to the presenter; never waits */
        if (!have_frame || !(dirty || force)) return;
        dirty = 0;
        last_present_us = now_us();
        gpu_gl_publish();
        return;
    }
    if (use_gl) {
        if (!have_frame || !(dirty || force) || headless || !window_visible()) return;
        dirty = 0;
        last_present_us = now_us();
        int dw, dh;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        gpu_gl_present(dw, dh);
        SDL_GL_SwapWindow(win);
        return;
    }
    if (!ren || !last_bits || !(dirty || force) || !window_visible()) return;
    dirty = 0;
    last_present_us = now_us();
    int w = last_w, h = last_h;
    if (!tex || tex_w != w || tex_h != h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w;
        tex_h = h;
        SDL_RenderSetLogicalSize(ren, w, h);
    }
    SDL_UpdateTexture(tex, NULL, G2H(last_bits), last_pitch);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void rt_present(uint32_t bits, int w, int h, int pitch)
{
    init();
    frames++;
    last_bits = bits; /* 0: a GPU frame (rt_screen_bits() reads it back) */
    last_w = w;
    last_h = h;
    last_pitch = pitch;
    have_frame = 1;
    if (shot_every && frames % shot_every == 0) {
        char p[600];
        snprintf(p, sizeof p, "%s/shots/frame_%05d.png", rt_game_dir, frames);
        uint32_t b = bits ? bits : rt_screen_bits();
        if (b) write_png(p, G2H(b), w, h, pitch);
    }
    dirty = 1;
    presents_since_poll++;
    static int warned;
    if (have_pump && !warned && !pthread_equal(pthread_self(), pump_thread)) {
        warned = 1;
        rt_log("[sdl] a frame was presented from a thread other than the message pump's\n");
    }
}

static uint32_t vk_of(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return (uint32_t)('A' + (k - SDLK_a));
    if (k >= SDLK_0 && k <= SDLK_9) return (uint32_t)k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (uint32_t)(k - SDLK_F1);
    switch (k) {
    case SDLK_RETURN: return 0x0D;
    case SDLK_ESCAPE: case SDLK_AC_BACK: return 0x1B; /* (Android's Back) */
    case SDLK_SPACE: return 0x20;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_DELETE: return 0x2E;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    default: return 0;
    }
}

/* DirectDraw WaitForVerticalBlank: present the current frame on the real vsync. If the present
 * didn't block (window hidden, occluded or no vsync), sleep out the rest of a 60 Hz frame. */
static int vblank_frames;
int rt_wait_vblank(void)
{
    vblank_frames++;
    if (headless || !(ren || use_gl) || !have_pump || !pthread_equal(pthread_self(), pump_thread)) return 0;
    if (threaded_present) {
        draw_window(0);
        gpu_gl_wait_swap(20);
        last_vblank_us = now_us();
        return 1;
    }
    uint64_t t0 = now_us();
    draw_window(1);
    uint64_t t1 = now_us();
    if (t1 - t0 < 2000 && last_vblank_us && t1 - last_vblank_us < 16667) usleep((useconds_t)(16667 - (t1 - last_vblank_us)));
    last_vblank_us = now_us();
    return 1;
}

/* ---------------- the touch screen (Android; RT_CTL fdown/fmove/fup on the test host), as on the Switch ----------------
 * The mouse everywhere (SDL's own touch-to-mouse is off on Android: main_posix.c); a tap on a text box opens the
 * keyboard; a seed picked up (or the shovel's button touched) and let go more than 40 points from where the touch
 * began is planted (dug) there, as a second click would. The mouse is one finger: the first down until it is lifted.
 * In versus on one screen another finger can be player 2's (pad_touch_finger), which the mouse never sees. */
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
static int touching, touch_x, touch_y, touch_x0, touch_y0, touch_held0;
static int64_t touch_finger;
void pad_touch_redirect(int *x, int *y);
int pad_touch_finger(int id, int kind, int x, int y);
static void map_mouse(int x, int y);
static void touch_mouse(uint32_t msg, int buttons)
{
    int x = touch_x, y = touch_y;
    pad_touch_redirect(&x, &y);
    rt_mouse_x = x;
    rt_mouse_y = y;
    rt_post_message(rt_main_hwnd, msg, (uint32_t)buttons, ((uint32_t)y << 16) | ((uint32_t)x & 0xffff));
}
/* a tap (display points) on text box w: its place is its own plus its parents' (+0x34 / +0x38, the parent at
 * +0x14 up to the widget manager); the display is the game's 800x600 at x1.2 from x 160 */
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
/* finger id down (kind 0), moving (1) or up (2) at display point x, y */
static void touch_at(int64_t id, int kind, int x, int y)
{
    if (pad_touch_finger((int)id, kind, x, y)) return;
    if (kind == 0 && touching) return; /* (another finger: the mouse is taken) */
    if (kind != 0 && (!touching || id != touch_finger)) return;
    if (kind == 0) {
        touching = 1, touch_finger = id;
        touch_x = x, touch_y = y, touch_x0 = x, touch_y0 = y;
        pad_input_pointer(x, y);
        touch_mouse(WM_MOUSEMOVE, 0);
        touch_mouse(WM_LBUTTONDOWN, 1);
        touch_held0 = game_touch_on_shovel(x, y);
        uint32_t edit = game_focused_edit();
        if (edit && edit_hit(edit, x, y)) {
            touch_mouse(WM_LBUTTONUP, 0);
            rt_text_keyboard();
        }
    } else if (kind == 1) {
        if (x == touch_x && y == touch_y) return;
        touch_x = x, touch_y = y;
        pad_input_pointer(x, y);
        touch_mouse(WM_MOUSEMOVE, 1);
    } else {
        touch_x = x, touch_y = y;
        touch_mouse(WM_LBUTTONUP, 0);
        int dx = touch_x - touch_x0, dy = touch_y - touch_y0;
        if ((game_held_packet() >= 0 || touch_held0) && dx * dx + dy * dy > 40 * 40) {
            touch_mouse(WM_LBUTTONDOWN, 1);
            touch_mouse(WM_LBUTTONUP, 0);
        }
        touching = 0;
    }
}
#ifdef __ANDROID__
static void touch_event(const SDL_Event *e)
{
    const SDL_TouchFingerEvent *f = &e->tfinger;
    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    map_mouse((int)(f->x * (float)ww), (int)(f->y * (float)wh));
    touch_at((int64_t)f->fingerId, e->type == SDL_FINGERDOWN ? 0 : e->type == SDL_FINGERMOTION ? 1 : 2, rt_mouse_x, rt_mouse_y);
}
#endif

/* Window coordinates -> the game's display (the OpenGL window letterboxes it; SDL_Renderer's logical
 * size already did this for the software window) */
/* the mouse over a button the widescreen draws somewhere else than its place (the versus Menu, in the top-right
 * corner off the brains count) is at that place for the game, as a touch there is (the window's own pointer stays) */
static void mouse_redirect(void)
{
    int x = rt_mouse_x, y = rt_mouse_y;
    pad_touch_redirect(&x, &y);
    rt_mouse_x = x, rt_mouse_y = y;
}
static void map_mouse(int x, int y)
{
    if (use_gl && last_w && last_h) {
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        float k = (float)ww / (float)last_w < (float)wh / (float)last_h ? (float)ww / (float)last_w : (float)wh / (float)last_h;
        float ox = ((float)ww - (float)last_w * k) / 2.f, oy = ((float)wh - (float)last_h * k) / 2.f;
        x = (int)(((float)x - ox) / k);
        y = (int)(((float)y - oy) / k);
    }
    rt_mouse_x = x;
    rt_mouse_y = y;
}

void rt_platform_poll(void)
{
    static int activated;
    init();
    if (!activated && rt_main_hwnd) {
        activated = 1;
        rt_post_message(rt_main_hwnd, 0x1C, 1, 0); /* WM_ACTIVATEAPP */
        rt_post_message(rt_main_hwnd, 0x06, 1, 0); /* WM_ACTIVATE */
        rt_post_message(rt_main_hwnd, 0x07, 0, 0); /* WM_SETFOCUS */
    }
    if (!have_pump) {
        have_pump = 1;
        pump_thread = pthread_self();
    }
    if (presents_since_poll > max_presents_per_poll) {
        max_presents_per_poll = presents_since_poll;
        if (rt_verbose) rt_log("[sdl] up to %d primary writes between message pumps\n", max_presents_per_poll);
    }
    presents_since_poll = 0;
    static int probe = -1;
    if (probe < 0) {
        probe = getenv("RT_PROBE") != NULL;
        rt_log("[probe] %s\n", probe ? "on" : "off");
    }
    if (probe) {
        static int last_frame;
        if (frames - last_frame >= 300) {
            last_frame = frames;
            rt_log("[probe] frame %d\n", frames);
            game_widget_probe();
        }
    }
    if (rt_verbose) {
        /* game frames per second, every 5 s: its once-a-frame vblank waits (fullscreen), else writes to
         * the primary */
        static uint64_t fps_t0;
        static int fps_frames0, fps_vb0;
        uint64_t t = now_us();
        if (!fps_t0) fps_t0 = t, fps_frames0 = frames, fps_vb0 = vblank_frames;
        if (t - fps_t0 >= 5000000) {
            int n = vblank_frames - fps_vb0;
            rt_log("[sdl] %.1f fps\n", (n ? n : frames - fps_frames0) * 1e6 / (double)(t - fps_t0));
            fps_t0 = t;
            fps_frames0 = frames;
            fps_vb0 = vblank_frames;
        }
    }
    if (rt_main_hwnd) {
        run_script();
        run_ctl();
        static uint64_t pad_t;
        uint64_t tnow = now_us();
        pads_poll(pad_t ? (float)(tnow - pad_t) / 1e6f : 0.f);
        pad_t = tnow;
    }
    if (headless) return;
    open_window();
    if (!win) return;
    /* windowed games (no vblank waits): present at most at 60 Hz */
    uint64_t t = now_us();
    if (threaded_present) draw_window(0);
    else if (t - last_vblank_us > 100000 && t - last_present_us >= 16000) draw_window(0);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        uint32_t h = rt_main_hwnd;
        switch (e.type) {
        case SDL_QUIT:
            rt_post_message(h, 0x10, 0, 0); /* WM_CLOSE */
            break;
        case SDL_MOUSEMOTION:
            map_mouse(e.motion.x, e.motion.y);
            pad_input_pointer(rt_mouse_x, rt_mouse_y);
            mouse_redirect();
            rt_post_message(h, 0x200, buttons, ((uint32_t)rt_mouse_y << 16) | ((uint32_t)rt_mouse_x & 0xffff));
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int down = e.type == SDL_MOUSEBUTTONDOWN;
            map_mouse(e.button.x, e.button.y);
            mouse_redirect();
            uint32_t lp = ((uint32_t)rt_mouse_y << 16) | ((uint32_t)rt_mouse_x & 0xffff);
            if (e.button.button == SDL_BUTTON_LEFT) {
                buttons = down ? buttons | 1 : buttons & ~1u;
                rt_post_message(h, down ? (e.button.clicks == 2 ? 0x203 : 0x201) : 0x202, buttons, lp);
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                buttons = down ? buttons | 2 : buttons & ~2u;
                rt_post_message(h, down ? 0x204 : 0x205, buttons, lp);
            }
            break;
        }
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_F11 && !e.key.repeat) {
                /* host fullscreen: fills the screen at its own resolution, no display mode change */
                int fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            /* fall through */
        case SDL_KEYUP: {
            uint32_t vk = vk_of(e.key.keysym.sym);
            if (vk) rt_post_message(h, e.type == SDL_KEYDOWN ? 0x100 : 0x101, vk, e.type == SDL_KEYUP ? 0xC0000001u : 1);
            if (e.type == SDL_KEYDOWN && (vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09))
                rt_post_message(h, 0x102, vk, 1); /* WM_CHAR for control keys */
            break;
        }
        case SDL_TEXTINPUT:
            for (const char *p = e.text.text; *p; p++)
                if ((uint8_t)*p < 0x80) rt_post_message(h, 0x102, (uint8_t)*p, 1);
            break;
#ifdef __ANDROID__
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            touch_event(&e);
            break;
#endif
        default:
            break;
        }
    }
#ifdef __ANDROID__
    if (SDL_IsTextInputActive() && !game_focused_edit()) SDL_StopTextInput(); /* (the name box went) */
#endif
}

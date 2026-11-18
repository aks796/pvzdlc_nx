/* hle_user32.c: windows, classes, one message queue, timers, metrics.
 *
 * There is one screen (RT_SCREEN_W x RT_SCREEN_H) and windows are records
 * whose window procedures are guest code. Input from the platform layer is
 * posted into the queue with rt_post_message. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "rt.h"

#define RT_SCREEN_W 1280
#define RT_SCREEN_H 720
uint32_t rt_tick_ms(void);

/* ---- classes ---- */
typedef struct {
    char name[64];
    uint32_t proc, style, atom;
} WClass;
static WClass classes[32];
static int nclasses;

/* ---- windows ---- */
typedef struct {
    int used;
    char cls[64];
    char title[128];
    uint32_t proc, style, exstyle, parent, userdata, id;
    int32_t x, y, w, h;
    int visible;
} Win;
#define MAX_WIN 64
#define HWND_BASE 0x00010000u
static Win wins[MAX_WIN];
static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t rt_main_hwnd;
int32_t rt_mouse_x, rt_mouse_y;

static Win *win_of(uint32_t h)
{
    if (h < HWND_BASE || (h - HWND_BASE) % 4) return NULL;
    uint32_t i = (h - HWND_BASE) / 4;
    return i < MAX_WIN && wins[i].used ? &wins[i] : NULL;
}

/* The game's app object: SexyAppBase keeps `this` in its window's GWL_USERDATA */
uint32_t rt_app_object(void)
{
    Win *w = win_of(rt_main_hwnd);
    return w ? w->userdata : 0;
}

int rt_window_rect(uint32_t hwnd, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    Win *wn = win_of(hwnd);
    if (!wn) return 0;
    *x = wn->x; *y = wn->y; *w = wn->w; *h = wn->h;
    return 1;
}

static uint32_t call_proc(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Win *w = win_of(hwnd);
    if (!w || !w->proc) return 0;
    uint32_t args[4] = {hwnd, msg, wp, lp};
    return rt_guest_call(c, w->proc, 4, args);
}

/* ---- message queue ---- */
typedef struct {
    uint32_t hwnd, msg, wp, lp, time;
    int32_t x, y;
} Msg;
#define QMAX 1024
static Msg queue[QMAX];
static int qhead, qtail;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;

typedef struct {
    uint32_t hwnd, id, ms, proc, next;
    int used;
} Timer;
static Timer timers[32];

void rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    pthread_mutex_lock(&q_lock);
    int n = (qtail + 1) % QMAX;
    if (n != qhead) {
        queue[qtail] = (Msg){hwnd, msg, wp, lp, rt_tick_ms(), rt_mouse_x, rt_mouse_y};
        qtail = n;
        pthread_cond_broadcast(&q_cv);
    }
    pthread_mutex_unlock(&q_lock);
}

/* Platform input/frame hook (desktop test host or Switch); weak no-op default */
void rt_platform_poll(void) __attribute__((weak));
void rt_platform_poll(void) {}

static void fire_timers(void)
{
    uint32_t now = rt_tick_ms();
    for (int i = 0; i < 32; i++) {
        Timer *t = &timers[i];
        if (t->used && (int32_t)(now - t->next) >= 0) {
            t->next = now + (t->ms ? t->ms : 1);
            /* only one pending WM_TIMER per timer, like Windows */
            int pending = 0;
            for (int k = qhead; k != qtail; k = (k + 1) % QMAX)
                if (queue[k].msg == 0x113 && queue[k].hwnd == t->hwnd && queue[k].wp == t->id) pending = 1;
            if (!pending) {
                int n = (qtail + 1) % QMAX;
                if (n != qhead) {
                    queue[qtail] = (Msg){t->hwnd, 0x113, t->id, t->proc, now, rt_mouse_x, rt_mouse_y};
                    qtail = n;
                }
            }
        }
    }
}

static int match(const Msg *m, uint32_t hwnd, uint32_t lo, uint32_t hi)
{
    if (hwnd && hwnd != 0xffffffffu && m->hwnd != hwnd) return 0;
    if (lo || hi) return m->msg >= lo && m->msg <= hi;
    return 1;
}

static int take(uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    for (int k = qhead; k != qtail; k = (k + 1) % QMAX) {
        if (!match(&queue[k], hwnd, lo, hi)) continue;
        Msg m = queue[k];
        if (remove) {
            for (int j = k; j != qhead; j = (j - 1 + QMAX) % QMAX) queue[j] = queue[(j - 1 + QMAX) % QMAX];
            qhead = (qhead + 1) % QMAX;
        }
        g_wr32(out, m.hwnd);
        g_wr32(out + 4, m.msg);
        g_wr32(out + 8, m.wp);
        g_wr32(out + 12, m.lp);
        g_wr32(out + 16, m.time);
        g_wr32(out + 20, (uint32_t)m.x);
        g_wr32(out + 24, (uint32_t)m.y);
        return 1;
    }
    return 0;
}

HLE(PeekMessageA)
{
    rt_platform_poll();
    pthread_mutex_lock(&q_lock);
    fire_timers();
    int r = take(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4) & 1);
    pthread_mutex_unlock(&q_lock);
    RET(5, r);
}
HLE(GetMessageA)
{
    for (;;) {
        rt_platform_poll();
        pthread_mutex_lock(&q_lock);
        fire_timers();
        if (take(ARG(0), ARG(1), ARG(2), ARG(3), 1)) {
            uint32_t msg = g_rd32(ARG(0) + 4);
            pthread_mutex_unlock(&q_lock);
            RET(4, msg != 0x12); /* WM_QUIT -> 0 */
        }
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t ns = (uint64_t)tv.tv_usec * 1000 + 2000000; /* wake at least every 2 ms for timers/input */
        struct timespec ts = {tv.tv_sec + (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        pthread_cond_timedwait(&q_cv, &q_lock, &ts);
        pthread_mutex_unlock(&q_lock);
    }
}
HLE(PostMessageA)
{
    rt_post_message(ARG(0), ARG(1), ARG(2), ARG(3));
    RET(4, 1);
}
HLE(SendMessageA) { RET(4, call_proc(c, ARG(0), ARG(1), ARG(2), ARG(3))); }
HLE(TranslateMessage) { RET(1, 0); }
HLE(DispatchMessageA)
{
    uint32_t m = ARG(0);
    uint32_t hwnd = g_rd32(m), msg = g_rd32(m + 4), wp = g_rd32(m + 8), lp = g_rd32(m + 12);
    if (msg == 0x113 && lp) { /* WM_TIMER with a TIMERPROC */
        uint32_t args[4] = {hwnd, msg, wp, rt_tick_ms()};
        rt_guest_call(c, lp, 4, args);
        RET(1, 0);
    }
    RET(1, call_proc(c, hwnd, msg, wp, lp));
}
HLE(DefWindowProcA)
{
    uint32_t msg = ARG(1);
    if (msg == 0x81) RET(4, 1);          /* WM_NCCREATE */
    if (msg == 0x10) {                    /* WM_CLOSE */
        call_proc(c, ARG(0), 0x2, 0, 0);  /* WM_DESTROY */
        RET(4, 0);
    }
    RET(4, 0);
}
HLE(DefWindowProcW)
{
    if (ARG(1) == 0x81) RET(4, 1);
    RET(4, 0);
}
HLE(SetTimer)
{
    uint32_t hwnd = ARG(0), id = ARG(1), ms = ARG(2), proc = ARG(3);
    pthread_mutex_lock(&q_lock);
    int slot = -1;
    for (int i = 0; i < 32; i++) {
        if (timers[i].used && timers[i].hwnd == hwnd && timers[i].id == id) { slot = i; break; }
        if (!timers[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) timers[slot] = (Timer){hwnd, id, ms, proc, rt_tick_ms() + ms, 1};
    pthread_mutex_unlock(&q_lock);
    RET(4, slot >= 0 ? (id ? id : (uint32_t)slot + 1) : 0);
}

/* ---- classes & windows ---- */
HLE(RegisterClassA)
{
    uint32_t wc = ARG(0);
    if (nclasses >= 32) RET(1, 0);
    WClass *k = &classes[nclasses];
    rt_a2u(g_rd32(wc + 36), k->name, sizeof k->name);
    k->style = g_rd32(wc);
    k->proc = g_rd32(wc + 4);
    k->atom = 0xC100 + (uint32_t)nclasses++;
    rt_log("[user32] RegisterClass(%s, proc=%08x)\n", k->name, (unsigned)k->proc);
    RET(1, k->atom);
}

static uint32_t create_window(CPU *c, uint32_t exstyle, const char *cls, const char *title, uint32_t style,
                              int32_t x, int32_t y, int32_t w, int32_t h, uint32_t parent, uint32_t param)
{
    WClass *k = NULL;
    for (int i = 0; i < nclasses; i++)
        if (!strcasecmp(classes[i].name, cls)) k = &classes[i];
    if (!k) {
        rt_log("[user32] CreateWindow: unknown class %s\n", cls);
        return 0;
    }
    pthread_mutex_lock(&win_lock);
    uint32_t i;
    for (i = 0; i < MAX_WIN && wins[i].used; i++) {}
    if (i == MAX_WIN) {
        pthread_mutex_unlock(&win_lock);
        return 0;
    }
    Win *wn = &wins[i];
    memset(wn, 0, sizeof *wn);
    wn->used = 1;
    snprintf(wn->cls, sizeof wn->cls, "%s", cls);
    snprintf(wn->title, sizeof wn->title, "%s", title);
    wn->proc = k->proc;
    wn->style = style;
    wn->exstyle = exstyle;
    wn->parent = parent;
    if (x == (int32_t)0x80000000) x = 0;
    if (y == (int32_t)0x80000000) y = 0;
    if (w == (int32_t)0x80000000) w = RT_SCREEN_W;
    if (h == (int32_t)0x80000000) h = RT_SCREEN_H;
    wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    wn->visible = (style & 0x10000000u) != 0;
    pthread_mutex_unlock(&win_lock);
    uint32_t hwnd = HWND_BASE + 4 * i;
    rt_log("[user32] CreateWindow(%s, \"%s\", %dx%d at %d,%d) -> %08x\n", cls, title, w, h, x, y, (unsigned)hwnd);

    /* CREATESTRUCTA for WM_NCCREATE / WM_CREATE */
    uint32_t cs = rt_heap_alloc(48, 1);
    g_wr32(cs, param);
    g_wr32(cs + 4, rt_image.base);
    g_wr32(cs + 12, parent);
    g_wr32(cs + 16, (uint32_t)h);
    g_wr32(cs + 20, (uint32_t)w);
    g_wr32(cs + 24, (uint32_t)y);
    g_wr32(cs + 28, (uint32_t)x);
    g_wr32(cs + 32, style);
    g_wr32(cs + 44, exstyle);
    if (!call_proc(c, hwnd, 0x81, 0, cs)) {
        rt_log("[user32] WM_NCCREATE refused\n");
    }
    if ((int32_t)call_proc(c, hwnd, 0x1, 0, cs) == -1) {
        rt_heap_free(cs);
        wins[i].used = 0;
        return 0;
    }
    rt_heap_free(cs);
    if (!parent && !rt_main_hwnd && strcasecmp(cls, "InvisWindow")) rt_main_hwnd = hwnd;
    return hwnd;
}
HLE(CreateWindowExA)
{
    char cls[64] = "", title[128] = "";
    if (ARG(1) >= 0x10000) rt_a2u(ARG(1), cls, sizeof cls);
    else
        for (int i = 0; i < nclasses; i++)
            if (classes[i].atom == ARG(1)) snprintf(cls, sizeof cls, "%s", classes[i].name);
    if (ARG(2)) rt_a2u(ARG(2), title, sizeof title);
    RET(12, create_window(c, ARG(0), cls, title, ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6),
                          (int32_t)ARG(7), ARG(8), ARG(11)));
}
HLE(CreateWindowExW)
{
    char cls[64] = "", title[128] = "";
    if (ARG(1) >= 0x10000) rt_w2u(ARG(1), cls, sizeof cls);
    if (ARG(2)) rt_w2u(ARG(2), title, sizeof title);
    RET(12, create_window(c, ARG(0), cls, title, ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6),
                          (int32_t)ARG(7), ARG(8), ARG(11)));
}
HLE(DestroyWindow)
{
    Win *w = win_of(ARG(0));
    if (!w) RET(1, 0);
    call_proc(c, ARG(0), 0x2, 0, 0);
    w->used = 0;
    if (rt_main_hwnd == ARG(0)) rt_main_hwnd = 0;
    RET(1, 1);
}
HLE(ShowWindow)
{
    Win *w = win_of(ARG(0));
    int was = w ? w->visible : 0;
    if (w) w->visible = ARG(1) != 0;
    RET(2, was);
}
HLE(MoveWindow)
{
    Win *w = win_of(ARG(0));
    if (w) {
        w->x = (int32_t)ARG(1); w->y = (int32_t)ARG(2); w->w = (int32_t)ARG(3); w->h = (int32_t)ARG(4);
    }
    RET(6, w != NULL);
}
static void put_rect(uint32_t p, int32_t l, int32_t t, int32_t r, int32_t b)
{
    g_wr32(p, (uint32_t)l); g_wr32(p + 4, (uint32_t)t); g_wr32(p + 8, (uint32_t)r); g_wr32(p + 12, (uint32_t)b);
}
HLE(GetClientRect)
{
    Win *w = win_of(ARG(0));
    put_rect(ARG(1), 0, 0, w ? w->w : RT_SCREEN_W, w ? w->h : RT_SCREEN_H);
    RET(2, 1);
}
HLE(GetWindowRect)
{
    Win *w = win_of(ARG(0));
    if (w) put_rect(ARG(1), w->x, w->y, w->x + w->w, w->y + w->h);
    else put_rect(ARG(1), 0, 0, RT_SCREEN_W, RT_SCREEN_H);
    RET(2, 1);
}
HLE(AdjustWindowRect) { RET(3, 1); } /* borderless: client == window */
HLE(AdjustWindowRectEx) { RET(4, 1); }
HLE(ClientToScreen)
{
    Win *w = win_of(ARG(0));
    if (w) {
        g_wr32(ARG(1), g_rd32(ARG(1)) + (uint32_t)w->x);
        g_wr32(ARG(1) + 4, g_rd32(ARG(1) + 4) + (uint32_t)w->y);
    }
    RET(2, 1);
}
HLE(ScreenToClient)
{
    Win *w = win_of(ARG(0));
    if (w) {
        g_wr32(ARG(1), g_rd32(ARG(1)) - (uint32_t)w->x);
        g_wr32(ARG(1) + 4, g_rd32(ARG(1) + 4) - (uint32_t)w->y);
    }
    RET(2, 1);
}
HLE(GetCursorPos)
{
    Win *w = win_of(rt_main_hwnd);
    g_wr32(ARG(0), (uint32_t)(rt_mouse_x + (w ? w->x : 0)));
    g_wr32(ARG(0) + 4, (uint32_t)(rt_mouse_y + (w ? w->y : 0)));
    RET(1, 1);
}
HLE(WindowFromPoint) { RET(2, rt_main_hwnd); }
HLE(GetActiveWindow) { RET(0, rt_main_hwnd); }
HLE(SetFocus) { RET(1, rt_main_hwnd); }
HLE(SetForegroundWindow) { RET(1, 1); }
HLE(SetCapture) { RET(1, 0); }
HLE(ReleaseCapture) { RET(0, 1); }
HLE(IsIconic) { RET(1, 0); }
HLE(IsWindowVisible)
{
    Win *w = win_of(ARG(0));
    RET(1, w && w->visible);
}
HLE(GetWindowLongA)
{
    Win *w = win_of(ARG(0));
    int32_t i = (int32_t)ARG(1);
    if (!w) RET(2, 0);
    RET(2, i == -21 ? w->userdata : i == -4 ? w->proc : i == -16 ? w->style : i == -20 ? w->exstyle : i == -12 ? w->id : 0);
}
HLE(SetWindowLongA)
{
    Win *w = win_of(ARG(0));
    int32_t i = (int32_t)ARG(1);
    uint32_t v = ARG(2), old = 0;
    if (!w) RET(3, 0);
    if (i == -21) { old = w->userdata; w->userdata = v; }
    else if (i == -4) { old = w->proc; w->proc = v; }
    else if (i == -16) { old = w->style; w->style = v; }
    else if (i == -20) { old = w->exstyle; w->exstyle = v; }
    RET(3, old);
}
HLE(GetWindowInfo)
{
    Win *w = win_of(ARG(0));
    uint32_t p = ARG(1);
    memset(G2H(p + 4), 0, 56);
    if (w) {
        put_rect(p + 4, w->x, w->y, w->x + w->w, w->y + w->h);
        put_rect(p + 20, w->x, w->y, w->x + w->w, w->y + w->h);
        g_wr32(p + 36, w->style);
        g_wr32(p + 40, w->exstyle);
    }
    RET(2, w != NULL);
}
HLE(GetWindowPlacement)
{
    Win *w = win_of(ARG(0));
    uint32_t p = ARG(1);
    memset(G2H(p + 4), 0, 40);
    g_wr32(p + 8, 1); /* SW_SHOWNORMAL */
    if (w) put_rect(p + 28, w->x, w->y, w->x + w->w, w->y + w->h);
    RET(2, 1);
}
HLE(SetWindowTextA)
{
    Win *w = win_of(ARG(0));
    if (w) rt_a2u(ARG(1), w->title, sizeof w->title);
    RET(2, 1);
}
HLE(GetWindowTextA)
{
    Win *w = win_of(ARG(0));
    RET(3, w ? (uint32_t)rt_u2a(w->title, ARG(1), ARG(2)) : 0);
}
HLE(GetClassNameA)
{
    Win *w = win_of(ARG(0));
    RET(3, w ? (uint32_t)rt_u2a(w->cls, ARG(1), ARG(2)) : 0);
}
HLE(EnumWindows) { RET(2, 1); }
HLE(EnumChildWindows) { RET(3, 1); }

/* ---- metrics & desktop ---- */
HLE(GetSystemMetrics)
{
    switch (ARG(0)) {
    case 0: case 16: case 78: RET(1, RT_SCREEN_W);  /* CXSCREEN, CXFULLSCREEN, CXVIRTUALSCREEN */
    case 1: case 17: case 79: RET(1, RT_SCREEN_H);
    case 4: RET(1, 0);                              /* CYCAPTION: borderless */
    case 5: case 6: case 7: case 8: case 32: case 33: RET(1, 0);
    case 11: case 12: RET(1, 32);                   /* CXICON */
    case 13: case 14: RET(1, 32);                   /* CXCURSOR */
    case 19: RET(1, 1);                             /* MOUSEPRESENT */
    case 36: case 37: RET(1, 4);                    /* double-click area */
    case 43: RET(1, 3);                             /* mouse buttons */
    case 80: RET(1, 1);                             /* CMONITORS */
    default: RET(1, 0);                             /* incl. 86 SM_TABLETPC */
    }
}
HLE(SystemParametersInfoA)
{
    uint32_t action = ARG(0), pv = ARG(2);
    if (action == 0x30 && pv) put_rect(pv, 0, 0, RT_SCREEN_W, RT_SCREEN_H); /* SPI_GETWORKAREA */
    else if (pv && (action == 14 || action == 16 || action == 0x72 || action == 97)) g_wr32(pv, 0);
    RET(4, 1);
}
static void put_mode(uint32_t dm, uint32_t w, uint32_t h)
{
    memset(G2H(dm + 40), 0, 124 - 40);
    g_wr16(dm + 36, 124);
    g_wr32(dm + 40, 0x5c0000); /* BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY */
    g_wr32(dm + 104, 32);
    g_wr32(dm + 108, w);
    g_wr32(dm + 112, h);
    g_wr32(dm + 120, 60);
}
HLE(EnumDisplaySettingsA)
{
    int32_t mode = (int32_t)ARG(1);
    if (mode == -1 || mode == -2) { put_mode(ARG(2), RT_SCREEN_W, RT_SCREEN_H); RET(3, 1); }
    static const uint32_t modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {RT_SCREEN_W, RT_SCREEN_H}};
    if (mode >= 0 && mode < 4) { put_mode(ARG(2), modes[mode][0], modes[mode][1]); RET(3, 1); }
    RET(3, 0);
}
HLE(ChangeDisplaySettingsA) { RET(2, 0); }
HLE(RegisterWindowMessageA)
{
    char s[128];
    rt_a2u(ARG(0), s, sizeof s);
    uint32_t h = 0;
    for (const char *p = s; *p; p++) h = h * 31 + (uint8_t)*p;
    RET(1, 0xC000 + h % 0x3000);
}

/* ---- cursors, icons, caret, painting ---- */
HLE(LoadCursorA) { RET(2, 0x00020000u + (ARG(1) & 0xffff)); }
HLE(LoadIconA) { RET(2, 0x00030001u); }
static uint32_t next_cursor = 0x00020100u;
HLE(CreateCursor) { RET(7, next_cursor++); } /* each its own handle: the host tells them apart */
HLE(DestroyCursor) { RET(1, 1); }
static uint32_t cur_cursor;
HLE(SetCursor)
{
    uint32_t old = cur_cursor;
    cur_cursor = ARG(0);
    RET(1, old);
}
HLE(GetCursor) { RET(0, cur_cursor); }
/* the cursor the game last set (LoadCursor's handles are 0x20000 + the IDC_ id): the host can tell what
 * the pointer is over (a hand over something to click) */
uint32_t rt_game_cursor(void) { return cur_cursor; }
HLE(CreateCaret) { RET(4, 1); }
HLE(DestroyCaret) { RET(0, 1); }
HLE(ShowCaret) { RET(1, 1); }
HLE(HideCaret) { RET(1, 1); }
HLE(SetCaretPos) { RET(2, 1); }
#define FAKE_DC 0x00040001u
HLE(GetDC) { RET(1, FAKE_DC); }
HLE(ReleaseDC) { RET(2, 1); }
HLE(BeginPaint)
{
    memset(G2H(ARG(1)), 0, 64);
    g_wr32(ARG(1), FAKE_DC);
    RET(2, FAKE_DC);
}
HLE(EndPaint) { RET(2, 1); }
HLE(FillRect) { RET(3, 1); }
HLE(DrawTextExA) { RET(6, 16); }
HLE(GetSysColorBrush) { RET(1, 0x00050001u); }
HLE(OffsetRect)
{
    uint32_t r = ARG(0), dx = ARG(1), dy = ARG(2);
    g_wr32(r, g_rd32(r) + dx); g_wr32(r + 4, g_rd32(r + 4) + dy);
    g_wr32(r + 8, g_rd32(r + 8) + dx); g_wr32(r + 12, g_rd32(r + 12) + dy);
    RET(3, 1);
}

/* ---- dialogs & clipboard ---- */
HLE(MessageBoxA)
{
    char text[1024] = "", cap[256] = "";
    if (ARG(1)) rt_a2u(ARG(1), text, sizeof text);
    if (ARG(2)) rt_a2u(ARG(2), cap, sizeof cap);
    rt_log("[user32] MessageBox \"%s\": %s\n", cap, text);
    RET(4, (ARG(3) & 0xf) == 4 ? 6 : 1); /* MB_YESNO -> IDYES, else IDOK */
}
HLE(MessageBoxW)
{
    char text[1024] = "", cap[256] = "";
    if (ARG(1)) rt_w2u(ARG(1), text, sizeof text);
    if (ARG(2)) rt_w2u(ARG(2), cap, sizeof cap);
    rt_log("[user32] MessageBox \"%s\": %s\n", cap, text);
    RET(4, (ARG(3) & 0xf) == 4 ? 6 : 1);
}
HLE(DialogBoxIndirectParamA) { RET(5, 0xffffffffu); }
HLE(EndDialog) { RET(2, 1); }
HLE(GetDlgItem) { RET(2, 0); }
HLE(OpenClipboard) { RET(1, 0); }
HLE(CloseClipboard) { RET(0, 1); }
HLE(EmptyClipboard) { RET(0, 0); }
HLE(GetClipboardData) { RET(1, 0); }
HLE(SetClipboardData) { RET(2, 0); }

const HleEntry hle_user32[] = {
#define U(n) {"user32.dll", #n, n}
    U(PeekMessageA), U(GetMessageA), U(PostMessageA), U(SendMessageA), U(TranslateMessage), U(DispatchMessageA),
    U(DefWindowProcA), U(DefWindowProcW), U(SetTimer), U(RegisterClassA), U(CreateWindowExA), U(CreateWindowExW),
    U(DestroyWindow), U(ShowWindow), U(MoveWindow), U(GetClientRect), U(GetWindowRect), U(AdjustWindowRect),
    U(AdjustWindowRectEx), U(ClientToScreen), U(ScreenToClient), U(GetCursorPos), U(WindowFromPoint),
    U(GetActiveWindow), U(SetFocus), U(SetForegroundWindow), U(SetCapture), U(ReleaseCapture), U(IsIconic),
    U(IsWindowVisible), U(GetWindowLongA), U(SetWindowLongA), U(GetWindowInfo), U(GetWindowPlacement),
    U(SetWindowTextA), U(GetWindowTextA), U(GetClassNameA), U(EnumWindows), U(EnumChildWindows),
    U(GetSystemMetrics), U(SystemParametersInfoA), U(EnumDisplaySettingsA), U(ChangeDisplaySettingsA),
    U(RegisterWindowMessageA), U(LoadCursorA), U(LoadIconA), U(CreateCursor), U(DestroyCursor), U(SetCursor),
    U(GetCursor), U(CreateCaret), U(DestroyCaret), U(ShowCaret), U(HideCaret), U(SetCaretPos), U(GetDC),
    U(ReleaseDC), U(BeginPaint), U(EndPaint), U(FillRect), U(DrawTextExA), U(GetSysColorBrush), U(OffsetRect),
    U(MessageBoxA), U(MessageBoxW), U(DialogBoxIndirectParamA), U(EndDialog), U(GetDlgItem), U(OpenClipboard),
    U(CloseClipboard), U(EmptyClipboard), U(GetClipboardData), U(SetClipboardData),
#undef U
    {NULL, NULL, NULL}};

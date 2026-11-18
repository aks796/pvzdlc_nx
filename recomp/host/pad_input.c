/* pad_input.c: how a controller plays the DLC, as on the Xbox 360 version and PvZ Touch.
 *
 * In a level (versus or not), each controller is one of the DLC's keyboard players: the game's own grid
 * cursors on the seed bank and the lawn, which also collect sun and coins by moving over them. Keys from
 * the save (P1KE / P2KE: up down left right, then five action keys):
 *   D-pad / left stick = move, L / R = the 5th / 6th keys, A = 7th, B = 8th, X = 9th, + = menu.
 * Outside versus the DLC only runs its keyboard players in its co-op mode, so in a level the port turns
 * that on in PlayerInfo (NCOP +0x1164 = 0, P1KB +0x5cd4 = 1, TASO +0x5df4 = 1) with no player 2
 * (P2KB +0x5cd5 = 0): the campaign is single player.
 * The campaign's seed chooser has no keyboard player (its KeyDown, 0x90bfe0, is versus-only), so the port
 * drives it: the DLC's bracket index (SeedChooserScreen +0x10c, 8 cards a row) moves with the D-pad /
 * stick, A clicks that card, B takes back the last pick (+0xe0 = cards in the bank), X = Let's Rock,
 * ZL / ZR = the page arrows, Y = Almanac, - = Shop, + = the Menu button.
 *
 * Menus: controller 1's left stick moves the mouse, A clicks, B right clicks (menu navigation by
 * controller comes later). The touch screen is the mouse everywhere (host side).
 *
 * The DLC's LawnApp (found by diffing the live object, NOTES.md): game mode +0x814 (versus 144-151),
 * scene +0x818 (3 = playing), board +0x780, PlayerInfo +0x848. Board: cursor object +0x13c (its +0x24 =
 * the held packet, -1 none). */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "rt.h"
#include "pad_input.h"
#include "overlay.h"

void rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
extern uint32_t rt_main_hwnd;
extern int32_t rt_mouse_x, rt_mouse_y;

#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_CHAR 0x102
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205

#define DISPLAY_W 1280
#define DISPLAY_H 720

/* ---------------- the save's key bindings ---------------- */
static uint32_t pkeys[2][9] = {
    {'W', 'S', 'A', 'D', 'Q', 'E', 'J', 'K', 0x20},
    {'T', 'G', 'F', 'H', 'R', 'Y', 'O', 'P', 0x0D},
};
/* KBLO: seed slots 1-10, shovel, menu, pause, speed up */
static uint32_t hotkeys[14] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'X', 0x1B, 0x20, 'C'};
enum { HK_SHOVEL = 10, HK_MENU, HK_PAUSE, HK_SPEED };

static uint32_t vk_of_name(const char *n)
{
    static const struct { const char *name; uint32_t vk; } named[] = {
        {"SPACE", 0x20}, {"RETURN", 0x0D}, {"ENTER", 0x0D}, {"ESCAPE", 0x1B}, {"TAB", 0x09}, {"BACKSPACE", 0x08},
        {"SHIFT", 0x10}, {"CONTROL", 0x11}, {"CTRL", 0x11}, {"ALT", 0x12}, {"LEFT", 0x25}, {"UP", 0x26},
        {"RIGHT", 0x27}, {"DOWN", 0x28},
    };
    if (n[0] && !n[1]) return (uint32_t)(n[0] >= 'a' && n[0] <= 'z' ? n[0] - 32 : n[0]);
    for (unsigned i = 0; i < sizeof named / sizeof *named; i++)
        if (!strcmp(n, named[i].name)) return named[i].vk;
    return 0;
}

/* "TAG":["A","B",...] in the decoded save into keys[0..n); 1 if all n were read */
static int read_keys(const char *text, const char *tag, uint32_t *keys, int n)
{
    const char *q = strstr(text, tag);
    if (!q || !(q = strchr(q, '['))) return 0;
    const char *end = strchr(q, ']');
    if (!end) return 0;
    uint32_t got[16];
    int k = 0;
    while (k < n && (q = strchr(q, '"')) && q < end) {
        const char *e = strchr(q + 1, '"');
        if (!e) break;
        char name[16];
        snprintf(name, sizeof name, "%.*s", (int)(e - q - 1), q + 1);
        got[k++] = vk_of_name(name);
        q = e + 1;
    }
    if (k != n) return 0;
    memcpy(keys, got, sizeof(uint32_t) * (size_t)n);
    return 1;
}

/* The save is JSON with every byte +1; the newest userN.dlc is the player's. Read again when it changes. */
static void load_bindings(void)
{
    static time_t loaded_t;
    char path[600], best[600] = "";
    time_t best_t = 0;
    struct stat st;
    for (int u = 1; u <= 8; u++) {
        snprintf(path, sizeof path, "%s/userdata/user%d.dlc", rt_game_dir, u);
        if (stat(path, &st) == 0 && st.st_mtime >= best_t) {
            best_t = st.st_mtime;
            snprintf(best, sizeof best, "%s", path);
        }
    }
    if (!best[0] || best_t == loaded_t) return;
    loaded_t = best_t;
    FILE *f = fopen(best, "rb");
    if (!f) return;
    static char text[64 << 10];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) text[i] = (char)(text[i] - 1);
    text[n] = 0;
    read_keys(text, "\"P1KE\"", pkeys[0], 9);
    read_keys(text, "\"P2KE\"", pkeys[1], 9);
    read_keys(text, "\"KBLO\"", hotkeys, 14);
    rt_log("[pad] bindings from %s\n", best);
}

/* ---------------- the game's state ---------------- */
#define RD8_G(a) (*(volatile uint8_t *)G2H(a))
#define WR8_G(a, v) (*(volatile uint8_t *)G2H(a) = (uint8_t)(v))
/* a pointer worth following: aligned, in the guest's range and mapped (on the Switch an unmapped guest
 * address faults, and the game's uninitialised fields hold 0xcdcdcdcd-style junk: the versus list crashed) */
static int valid(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u && !(p & 3) && rt_vm_mapped(p) && rt_vm_mapped(p + 0xfff); }

static int game_mode(void)
{
    uint32_t app = rt_app_object();
    return valid(app) ? (int)g_rd32(app + 0x814) : -1;
}
static int is_versus(void)
{
    int m = game_mode();
    return m >= 144 && m <= 151; /* the DLC's eight versus maps */
}
static int seed_packets(int *held);
static uint32_t player_info(void)
{
    uint32_t app = rt_app_object();
    uint32_t p = valid(app) ? g_rd32(app + 0x848) : 0;
    return valid(p) ? p : 0;
}
/* a touch at display point x, y started on the shovel's button (touch drag and drop) */
static int zen_tools;
int game_touch_on_shovel(int x, int y)
{
    float gx = ((float)x - 160.f) / 1.2f, gy = (float)y / 1.2f;
    /* the Zen Garden's tools work the same way: dragged from their slot onto a plant, used there */
    if (game_mode() == 50) return zen_tools > 0 && gx >= 30 && gx < 30 + 70.f * (float)zen_tools && gy >= 0 && gy < 72;
    int held;
    int n = seed_packets(&held);
    uint32_t app = rt_app_object(), b = valid(app) ? g_rd32(app + 0x780) : 0;
    if (!n || !valid(b) || !RD8_G(b + 0x564d)) return 0; /* (no shovel yet: Board +0x564d, shovel_shown) */
    static const int extra[11] = {0, 0, 0, 0, 0, 0, 0, 60, 76, 112, 153};
    float sx = 456.f + (float)extra[n];
    return gx >= sx && gx < sx + 70 && gy >= 0 && gy < 87;
}

/* the seed packet the mouse cursor holds, -1 none (touch drag and drop) */
int game_held_packet(void)
{
    int held;
    seed_packets(&held);
    return held;
}
static uint32_t board(void)
{
    uint32_t app = rt_app_object();
    uint32_t b = valid(app) ? g_rd32(app + 0x780) : 0;
    return valid(b) ? b : 0;
}
/* the number of seed packets, and which one the cursor holds (-1 none); 0 packets without a board */
static int seed_packets(int *held)
{
    uint32_t b = board();
    *held = -1;
    if (!b) return 0;
    uint32_t bank = g_rd32(b + 0x148), cursor = g_rd32(b + 0x13c);
    int n = valid(bank) ? (int)g_rd32(bank + 0x24) : 0;
    if (valid(cursor)) *held = (int)g_rd32(cursor + 0x24);
    return n > 0 && n <= 10 ? n : 0;
}

/* ---------------- the game's widgets (MSVC RTTI) ----------------
 * An object's vtable[-1] is its CompleteObjectLocator: +12 type descriptor (its name at +8, e.g.
 * ".?AVEditWidget@Sexy@@"), +16 class hierarchy (+8 base count, +12 base array of descriptors whose +0
 * is each base's type descriptor). */
static int rtti_name_ok(uint32_t td, const char *want)
{
    if (!valid(td)) return 0;
    char name[64];
    rt_a2u(td + 8, name, sizeof name);
    return strstr(name, want) != NULL;
}
/* obj's class name (the RTTI type descriptor's), or "" */
const char *game_rtti_name(uint32_t obj)
{
    static char name[64];
    name[0] = 0;
    if (!valid(obj)) return name;
    uint32_t vt = g_rd32(obj);
    if (!valid(vt) || vt < 0x00400000u || vt > 0x02000000u) return name;
    uint32_t col = g_rd32(vt - 4);
    if (!valid(col) || col < 0x00400000u || col > 0x02000000u) return name;
    uint32_t td = g_rd32(col + 12);
    if (valid(td)) rt_a2u(td + 8, name, sizeof name);
    return name;
}

/* obj's class is `want` or derives from it */
int game_rtti_is(uint32_t obj, const char *want)
{
    if (!valid(obj)) return 0;
    uint32_t vt = g_rd32(obj);
    if (!valid(vt) || vt < 0x00400000u || vt > 0x02000000u) return 0;
    uint32_t col = g_rd32(vt - 4);
    if (!valid(col) || col < 0x00400000u || col > 0x02000000u) return 0;
    if (rtti_name_ok(g_rd32(col + 12), want)) return 1;
    uint32_t chd = g_rd32(col + 16);
    if (!valid(chd)) return 0;
    uint32_t n = g_rd32(chd + 8), bca = g_rd32(chd + 12);
    if (n > 32 || !valid(bca)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bcd = g_rd32(bca + 4 * i);
        if (valid(bcd) && rtti_name_ok(g_rd32(bcd), want)) return 1;
    }
    return 0;
}

/* The focused text box (the DLC: LawnApp+0x320 WidgetManager, its +0x8c mFocusWidget), or 0 */
uint32_t game_focused_edit(void)
{
    uint32_t app = rt_app_object();
    if (!valid(app)) return 0;
    uint32_t wm = g_rd32(app + 0x320);
    if (!valid(wm)) return 0;
    uint32_t fw = g_rd32(wm + 0x8c);
    return game_rtti_is(fw, "EditWidget@Sexy") ? fw : 0;
}

/* The text box's text: an MSVC std::string at EditWidget+0x94 (16-byte buffer or pointer, size, capacity) */
void game_edit_text(uint32_t edit, char *out, size_t n)
{
    out[0] = 0;
    uint32_t size = g_rd32(edit + 0x94 + 16), cap = g_rd32(edit + 0x94 + 20);
    if (size > 256 || cap < 15) return;
    uint32_t src = cap > 15 ? g_rd32(edit + 0x94) : edit + 0x94;
    if (valid(src)) rt_a2u(src, out, n);
}

/* Replace the text box's text with `text`, as typed: select all (Ctrl+A is not universal in SexyApp,
 * so Backspace enough times), then the characters, then Enter to accept the dialog */
static void key(uint32_t vk, int down);
void game_edit_replace(uint32_t edit, const char *text)
{
    char cur[260];
    game_edit_text(edit, cur, sizeof cur);
    size_t n = strlen(cur);
    rt_post_message(rt_main_hwnd, WM_KEYDOWN, 0x23, 1); /* End */
    rt_post_message(rt_main_hwnd, WM_KEYUP, 0x23, 0xC0000001u);
    for (size_t i = 0; i < n; i++) {
        rt_post_message(rt_main_hwnd, WM_KEYDOWN, 0x08, 1);
        rt_post_message(rt_main_hwnd, WM_CHAR, 0x08, 1);
        rt_post_message(rt_main_hwnd, WM_KEYUP, 0x08, 0xC0000001u);
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p >= 0x20 && *p < 0x7f) rt_post_message(rt_main_hwnd, WM_CHAR, *p, 1);
    key(0x0D, 1);
    key(0x0D, 0);
}

/* -v probe: which app and widget-manager fields point at what (to find the DLC's offsets) */
void game_widget_probe(void)
{
    uint32_t app = rt_app_object();
    if (!valid(app)) return;
    for (uint32_t o = 0; o < 0x1000; o += 4) {
        uint32_t wm = g_rd32(app + o);
        if (!game_rtti_is(wm, "WidgetManager@Sexy")) continue;
        rt_log("[probe] app+%03x = WidgetManager %08x\n", o, (unsigned)wm);
        uint32_t fw = g_rd32(wm + 0x8c);
        if (game_rtti_is(fw, "EditWidget@Sexy")) {
            /* MSVC std::string: 16-byte buffer (or pointer when capacity > 15), size, capacity */
            for (uint32_t k = 0; k < 0x200; k += 4) {
                uint32_t size = g_rd32(fw + k + 16), cap = g_rd32(fw + k + 20);
                if (size < 64 && cap >= 15 && cap < 4096 && size <= cap) {
                    char txt[80];
                    uint32_t src = cap > 15 ? g_rd32(fw + k) : fw + k;
                    if (!valid(src)) continue;
                    rt_a2u(src, txt, sizeof txt);
                    if (strlen(txt) == size && size) rt_log("[probe]   edit+%03x string \"%s\" (size %u cap %u)\n", k, txt, (unsigned)size, (unsigned)cap);
                }
            }
        }
        for (uint32_t k = 0; k < 0x200; k += 4) {
            uint32_t w = g_rd32(wm + k);
            if (game_rtti_is(w, "EditWidget@Sexy")) rt_log("[probe]   wm+%03x -> EditWidget %08x\n", k, (unsigned)w);
            else if (game_rtti_is(w, "Widget@Sexy")) rt_log("[probe]   wm+%03x -> Widget %08x\n", k, (unsigned)w);
        }
    }
}

/* An MSVC std::string of this build (28 bytes: debug proxy, 16-byte buffer or pointer, size, capacity) */
static void game_string(uint32_t str, char *out, size_t n)
{
    out[0] = 0;
    uint32_t size = g_rd32(str + 20), cap = g_rd32(str + 24);
    if (size > 512 || cap < 15 || size > cap) return;
    uint32_t src = cap > 15 ? g_rd32(str + 4) : str + 4;
    if (valid(src) || cap == 15) rt_a2u(src, out, n);
}

/* -v probe: the widget tree under the WidgetManager (LawnApp+0x320), with class, absolute rect and the
 * flag bytes after mZOrder (WidgetContainer: list head +8 / size +0xc, x y w h +0x34..+0x40) */
static void tree_dump(uint32_t wc, int ox, int oy, int depth)
{
    uint32_t head = g_rd32(wc + 8), n = g_rd32(wc + 0xc);
    if (!valid(head) || n > 200) return;
    uint32_t node = g_rd32(head);
    for (uint32_t i = 0; i < n && valid(node) && node != head; i++, node = g_rd32(node)) {
        uint32_t w = g_rd32(node + 8);
        if (!valid(w)) continue;
        int x = ox + (int32_t)g_rd32(w + 0x34), y = oy + (int32_t)g_rd32(w + 0x38);
        uint8_t f[8];
        for (int k = 0; k < 8; k++) f[k] = RD8_G(w + 0x58 + k);
        char label[64] = "";
        if (game_rtti_is(w, "ButtonWidget@Sexy")) game_string(w + 0x90, label, sizeof label);
        rt_log("[wtree] %*s%s %08x at %d,%d %dx%d flags %d%d%d%d%d%d%d%d id %d \"%s\"\n", depth * 2, "",
               game_rtti_name(w), (unsigned)w, x, y, (int)g_rd32(w + 0x3c), (int)g_rd32(w + 0x40),
               f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], (int)g_rd32(w + 0x8c), label);
        if (getenv("RT_WFIELDS") && (game_rtti_is(w, "Checkbox@Sexy") || game_rtti_is(w, "Slider@Sexy") ||
                                     game_rtti_is(w, "ButtonWidget@Sexy"))) {
            for (uint32_t o = 0x80; o < 0x170; o += 4) {
                uint32_t v = g_rd32(w + o);
                if (v) rt_log("[wtree] %*s  +%03x = %08x %s %s\n", depth * 2, "", (unsigned)o, (unsigned)v,
                              valid(v) ? game_rtti_name(v) : "",
                              valid(v) && game_rtti_is(v, "Image@Sexy") ? "(image)" : "");
            }
        }
        if (depth < 6) tree_dump(w, x, y, depth + 1);
    }
}
/* -v probe: ButtonWidget pointers held inside each top-level screen (buttons a screen draws itself) */
static void member_scan(uint32_t obj)
{
    for (uint32_t o = 0; o < 0x3000; o += 4) {
        uint32_t w = g_rd32(obj + o);
        if (!valid(w) || w == obj) continue;
        const char *nm = game_rtti_name(w);
        if (!nm || !(strstr(nm, "Widget") || strstr(nm, "Checkbox") || strstr(nm, "Edit") || strstr(nm, "Button"))) continue;
        char label[64];
        game_string(w + 0x90, label, sizeof label);
        rt_log("[wscan]   +%04x %s %08x at %d,%d %dx%d vis %d dis %d id %d \"%s\"\n", o, game_rtti_name(w), (unsigned)w,
               (int)g_rd32(w + 0x34), (int)g_rd32(w + 0x38), (int)g_rd32(w + 0x3c), (int)g_rd32(w + 0x40),
               RD8_G(w + 0x58), RD8_G(w + 0x5a), (int)g_rd32(w + 0x8c), label);
        if (strstr(nm, "NewLawnButton") && g_rd32(w + 0x8c) == 5) {
            uint32_t im = g_rd32(w + 0xb4);
            if (valid(im)) rt_log("[wscan]     b4 img %dx%d\n", (int)g_rd32(im + 0x24), (int)g_rd32(im + 0x28));
            im = g_rd32(w + 0xb8);
            if (valid(im)) rt_log("[wscan]     b8 img %dx%d\n", (int)g_rd32(im + 0x24), (int)g_rd32(im + 0x28));
        }
        if (strstr(nm, "NewLawnButton"))
            rt_log("[wscan]     img b4 %x b8 %x bc %x c0 %x  off 14c %d 150 %d  nodraw %d %d\n", g_rd32(w + 0xb4), g_rd32(w + 0xb8), g_rd32(w + 0xbc), g_rd32(w + 0xc0),
                   (int)g_rd32(w + 0x14c), (int)g_rd32(w + 0x150), RD8_G(w + 0x104), RD8_G(w + 0x105));
    }
}

/* -v probe: the game's image globals (Sexy::IMAGE_*) and their sizes (Image +0x24 w, +0x28 h, found by
 * the conveyor belt's 0x1094050) */
static int font_ascent(uint32_t font);
static int font_width(uint32_t font, const char *text);
/* -v probe: the game's font globals (Sexy::FONT_*), their ascent and a sample width */
void game_font_probe(void)
{
    /* the title link's font: its vtable and members (pointers with their class names) */
    uint32_t f = g_rd32(0x1093f5cu);
    if (valid(f)) {
        uint32_t vt = g_rd32(f);
        char row[400];
        int n = snprintf(row, sizeof row, "[fontp] vtable");
        for (int i = 0; i < 14; i++) n += snprintf(row + n, sizeof row - n, " %d:%06x", i, (unsigned)g_rd32(vt + 4 * i));
        rt_log("%s\n", row);
        for (uint32_t o = 4; o < 0x60; o += 4) {
            uint32_t v = g_rd32(f + o);
            rt_log("[fontp] font+%02x = %08x %s\n", (unsigned)o, (unsigned)v, valid(v) ? game_rtti_name(v) : "");
        }
        /* the width table: in a std::list node of FontData (a FontLayer), find the stride and offset where
         * the words for ' ', 'A', 'B', 'a', 'z' are those characters' widths */
        int wsp = font_width(f, " "), wA = font_width(f, "A"), wB = font_width(f, "B"), wa = font_width(f, "a"), wz = font_width(f, "z");
        rt_log("[fontp] widths sp %d A %d B %d a %d z %d\n", wsp, wA, wB, wa, wz);
        uint32_t fd = g_rd32(f + 0x14);
        {
            uint32_t head = g_rd32(fd + 0x1b4), node = valid(head) ? g_rd32(head) : 0;
            int shown = 0;
            for (uint32_t o = 0x140; o < 0x4000 && valid(node) && shown < 40; o += 16) {
                if (!g_rd32(node + 8 + o) && !g_rd32(node + 12 + o) && !g_rd32(node + 16 + o) && !g_rd32(node + 20 + o)) continue;
                shown++;
                rt_log("[fontp] layer+%03x: %08x %08x %08x %08x\n", (unsigned)o, (unsigned)g_rd32(node + 8 + o),
                       (unsigned)g_rd32(node + 12 + o), (unsigned)g_rd32(node + 16 + o), (unsigned)g_rd32(node + 20 + o));
            }
            for (uint32_t o = 0; o < 0x10 && valid(node); o += 16)
                rt_log("[fontp] layer+%03x: %08x %08x %08x %08x\n", (unsigned)o, (unsigned)g_rd32(node + 8 + o),
                       (unsigned)g_rd32(node + 12 + o), (unsigned)g_rd32(node + 16 + o), (unsigned)g_rd32(node + 20 + o));
        }
        for (uint32_t o = 0; o < 0xa0; o += 4) {
            uint32_t v = g_rd32(fd + o);
            rt_log("[fontp] fd+%02x = %08x %s\n", (unsigned)o, (unsigned)v, valid(v) ? game_rtti_name(v) : "");
        }
        for (uint32_t o = 0x180; o < 0x260; o += 4) {
            uint32_t head = g_rd32(fd + o + 4), cnt = g_rd32(fd + o + 8);
            if (o < 0x1c0) rt_log("[fontp] fd+%03x = %08x\n", (unsigned)o, (unsigned)g_rd32(fd + o));
            if (!valid(head) || cnt == 0 || cnt > 8) continue;
            uint32_t node = g_rd32(head);
            for (uint32_t k = 0; k < cnt && valid(node) && node != head; k++, node = g_rd32(node)) {
                uint32_t val = node + 8;
                for (uint32_t st = 16; st <= 128; st += 4)
                    for (uint32_t off = 0; off < 0x200; off += 4) {
                        uint32_t b = val + off;
                        int d = wA - (int)g_rd32(b + 'A' * st);
                        if (d >= -6 && d <= 6 && (int)g_rd32(b + ' ' * st) + d == wsp && (int)g_rd32(b + 'B' * st) + d == wB &&
                            (int)g_rd32(b + 'a' * st) + d == wa && (int)g_rd32(b + 'z' * st) + d == wz)
                            rt_log("[fontp] FontData+%03x list, node %u: CharData stride %u, width at +%u, spacing %d (layer %08x)\n",
                                   (unsigned)o, (unsigned)k, (unsigned)st, (unsigned)off, d, (unsigned)val);
                    }
            }
        }
    }
    (void)0;

    for (uint32_t a = 0x1093000u; a < 0x1095800u; a += 4) {
        uint32_t f = g_rd32(a);
        if (!valid(f) || !game_rtti_is(f, "Font@Sexy")) continue;
        rt_log("[font] %07x %s ascent %d, \"press to continue\" %d px\n", (unsigned)a, game_rtti_name(f), font_ascent(f),
               font_width(f, "press to continue"));
    }
}

void game_image_probe(void)
{
    /* the focused button's pictures: ButtonWidget fields with their classes, then the first image's members */
    uint32_t app = rt_app_object(), wm = valid(app) ? g_rd32(app + 0x320) : 0;
    uint32_t over = valid(wm) ? g_rd32(wm + 0x94) : 0;
    if (valid(over)) {
        rt_log("[imgp] over widget %s %08x\n", game_rtti_name(over), (unsigned)over);
        for (uint32_t o = 0xa0; o < 0x130; o += 4) {
            uint32_t v = g_rd32(over + o);
            rt_log("[imgp]   +%03x = %08x %s\n", (unsigned)o, (unsigned)v, valid(v) ? game_rtti_name(v) : "");
        }
        for (uint32_t o = 0xa0; o < 0x130; o += 4) {
            uint32_t im = g_rd32(over + o);
            if (!valid(im) || !game_rtti_is(im, "Image@Sexy")) continue;
            rt_log("[imgp] image +%03x %s w %d h %d\n", (unsigned)o, game_rtti_name(im), (int)g_rd32(im + 0x24), (int)g_rd32(im + 0x28));
            for (uint32_t k = 0; k < 0x100; k += 4) {
                uint32_t v = g_rd32(im + k);
                if (valid(v)) rt_log("[imgp]     im+%03x = %08x %s first words %08x %08x\n", (unsigned)k, (unsigned)v,
                                     game_rtti_name(v), (unsigned)g_rd32(v), (unsigned)g_rd32(v + 4));
            }
            break;
        }
    }

    for (uint32_t a = 0x1093000u; a < 0x1095800u; a += 4) {
        uint32_t im = g_rd32(a);
        if (!valid(im) || !game_rtti_is(im, "Image@Sexy")) continue;
        char row[200];
        int n = snprintf(row, sizeof row, "[img] %07x %s", (unsigned)a, game_rtti_name(im));
        for (uint32_t o = 0x18; o <= 0x30 && n < (int)sizeof row - 16; o += 4) n += snprintf(row + n, sizeof row - n, " %d", (int)g_rd32(im + o));
        rt_log("%s\n", row);
    }
}

void game_widget_tree(void)
{
    uint32_t app = rt_app_object();
    uint32_t wm = valid(app) ? g_rd32(app + 0x320) : 0;
    if (valid(wm)) tree_dump(wm, 0, 0, 0);
    uint32_t head = valid(wm) ? g_rd32(wm + 8) : 0, n = valid(wm) ? g_rd32(wm + 0xc) : 0;
    uint32_t node = valid(head) ? g_rd32(head) : 0;
    for (uint32_t i = 0; i < n && i < 64 && valid(node) && node != head; i++, node = g_rd32(node)) {
        uint32_t w = g_rd32(node + 8);
        if (!valid(w) || game_rtti_is(w, "ButtonWidget@Sexy")) continue;
        rt_log("[wscan] %s %08x\n", game_rtti_name(w), (unsigned)w);
        member_scan(w);
    }
}

/* ---------------- output ---------------- */
static float cur_x = DISPLAY_W / 2, cur_y = DISPLAY_H / 2;
static int cursor_shown, mouse_buttons;
enum { SCR_OTHER, SCR_MENU, SCR_SIDE, SCR_ALMANAC, SCR_STORE, SCR_CHALLENGE, SCR_OPTIONS, SCR_MAIL, SCR_AWARD };
static int screen_kind;
static uint32_t screen_obj;
int pad_title_held; /* A held on the title screen (its start prompt drawn pressed) */
static int touch_mode; /* the touch screen was used last: no highlight until a controller press */
/* The Mac and Windows apps (main_posix.c; RT_DESKTOP=1 on the test host): the PC game as it is, mouse and keyboard
 * first (touch_mode from the start: the mouse is the touch screen there), with the PC's own wording, versus set up by
 * the game's own Multiplayer screen (mouse vs keyboard) and its own device pictures; a controller press switches to
 * the controller's play (as on the Switch) and the mouse switches back, the game's settings as they were. */
int pad_desktop;
/* The Android app (main_posix.c; RT_TOUCH_FIRST=1 on the test host): the touch screen plays first (touch_mode from the
 * start) and the game's own wording says tap where it said click (touch_text); a controller press switches to the
 * controller's play, as on the Switch (versus too). */
int pad_touch_first;
static int scheme_switched; /* the controls just changed hands (level <-> menu): held buttons are not presses */
static int list_scrolls; /* the screen is a list that scrolls with the mouse wheel */
static int list_more;    /* ... and it has rows out of view (else it doesn't scroll at all) */
static int list_cards;   /* ... its cards, in view or not (none: the DLC's credits page) */
static float nav_freeze; /* the main menu's pages sliding: no highlight, no moves until it settles */

/* PvZ Touch's button badges: the small Xbox 360 cel of the pad button that works a thing, at the thing's
 * bottom right (x + w - size + 2, y + h - size + 4), only while the controller is in use */
/* Things the game draws over the buttons a badge sits on (its tooltips, opaque boxes): the badge leaves
 * their boxes out, so a tooltip covers a badge as it covers the button (badges are drawn after the game's
 * frame). Game coordinates; set each poll. */
static struct { float x, y, w, h; } occl[4];
static int n_occl;
static void occlude(float x, float y, float w, float h)
{
    if (n_occl < 4 && w > 0 && h > 0) occl[n_occl].x = x, occl[n_occl].y = y, occl[n_occl].w = w, occl[n_occl].h = h, n_occl++;
}
/* the button picture (cel) in x,y,w,h, less the occluders from k on */
static void badge_part(int cel, float sx, float sy, float sw, float sh, float x, float y, float w, float h, int k)
{
    if (w <= 0.5f || h <= 0.5f) return;
    for (; k < n_occl; k++) {
        float ox0 = occl[k].x, oy0 = occl[k].y, ox1 = ox0 + occl[k].w, oy1 = oy0 + occl[k].h;
        if (ox1 <= x || oy1 <= y || ox0 >= x + w || oy0 >= y + h) continue;
        /* the parts above, below, left and right of the box (each with its share of the picture) */
        float kx = sw / w, ky = sh / h;
        float top = oy0 - y, bot = y + h - oy1, lft = ox0 - x, rgt = x + w - ox1;
        float my0 = top > 0 ? oy0 : y, my1 = bot > 0 ? oy1 : y + h;
        if (top > 0) badge_part(cel, sx, sy, sw, top * ky, x, y, w, top, k + 1);
        if (bot > 0) badge_part(cel, sx, sy + (oy1 - y) * ky, sw, bot * ky, x, oy1, w, bot, k + 1);
        if (lft > 0) badge_part(cel, sx, sy + (my0 - y) * ky, lft * kx, (my1 - my0) * ky, x, my0, lft, my1 - my0, k + 1);
        if (rgt > 0) badge_part(cel, sx + (ox1 - x) * kx, sy + (my0 - y) * ky, rgt * kx, (my1 - my0) * ky, ox1, my0, rgt, my1 - my0, k + 1);
        return;
    }
    ov_image(OV_HELP, (int)(42 * cel + sx), (int)sy, (int)(sw + 0.5f), (int)(sh + 0.5f), x, y, w, h, 0xffffffffu);
}
static void badge_img(int cel, float x, float y, float w, float h) { badge_part(cel, 0, 0, 42, 42, x, y, w, h, 0); }

/* every ToolTipWidget an object keeps (its pointers found once per class, its first 0x6000 bytes), shown
 * ones as occluders: the Board's (the shovel, "visit other garden") */
static void occlude_tooltips_of(uint32_t obj)
{
    static struct { uint32_t vt; uint16_t off[4]; int n; } known[8];
    static int n_known;
    if (!valid(obj)) return;
    uint32_t vt = g_rd32(obj);
    int k = 0;
    while (k < n_known && known[k].vt != vt) k++;
    if (k == n_known) {
        if (n_known == 8) return;
        known[k].vt = vt, known[k].n = 0, n_known++;
        for (uint32_t o = 4; o < 0x6000 && known[k].n < 4; o += 4) {
            uint32_t q = g_rd32(obj + o);
            if (valid(q) && valid(g_rd32(q)) && game_rtti_is(q, "ToolTipWidget")) known[k].off[known[k].n++] = (uint16_t)o;
        }
        rt_log("[pad] %s keeps %d tooltips\n", game_rtti_name(obj), known[k].n);
    }
    for (int i = 0; i < known[k].n; i++) {
        uint32_t tt = g_rd32(obj + known[k].off[i]);
        if (valid(tt) && RD8_G(tt + 0x64))
            occlude((float)(int32_t)g_rd32(tt + 0x54), (float)(int32_t)g_rd32(tt + 0x58), (float)(int32_t)g_rd32(tt + 0x5c),
                    (float)(int32_t)g_rd32(tt + 0x60));
    }
}

static void badge(float x, float y, float w, float h, int cel)
{
    if (touch_mode) return;
    /* the 42 px sheet at 26: the small sheet's shoulder buttons were too faint to read */
    badge_img(cel, x + w - 26 * 0.45f, y + h - 26 + 6, 26, 26); /* half past the right edge: the label stays clear */
}

/* a page turn's shoulder button, on that side of the thing (ZL out to its left, ZR out to its right),
 * middle height, as a console game shows them; the trigger pictures are narrow, so drawn larger */
/* a page turn's button next to the label it turns, on its inner side, clear of the text: ZL just right of
 * a left-hand "previous", ZR just left of a right-hand "next" (dir +1 = after the rect, -1 = before) */
static void badge_inner(float x, float y, float w, float h, int cel, int dir)
{
    if (touch_mode) return;
    float sz = 30, by = y + (h - sz) / 2, bx = dir > 0 ? x + w + 3 : x - sz - 3;
    badge_img(cel, bx, by, sz, sz);
}

static void badge_side(float x, float y, float w, float h, int cel, int side)
{
    if (touch_mode) return;
    /* +-1: half outside the edge; +-2: just inside it (buttons in a row, where outside is a neighbour) */
    float sz = side == -2 || side == 2 ? 30 : 34, by = y + (h - sz) / 2;
    float bx = side == -1 ? x - sz * 0.55f : side == 1 ? x + w - sz * 0.45f : side == -2 ? x - 4 : x + w - sz + 4;
    badge_img(cel, bx, by, sz, sz);
}

/* Screen (1280x720) -> the game's display, when the host shows that letterboxed; identity by default */
__attribute__((weak)) void rt_screen_to_display(int *x, int *y) { (void)x; (void)y; }

static void mouse(uint32_t msg)
{
    int x = (int)cur_x, y = (int)cur_y;
    rt_screen_to_display(&x, &y);
    rt_mouse_x = x;
    rt_mouse_y = y;
    rt_post_message(rt_main_hwnd, msg, (uint32_t)mouse_buttons, ((uint32_t)y << 16) | ((uint32_t)x & 0xffff));
}
static void key(uint32_t vk, int down)
{
    if (!vk) return;
    rt_post_message(rt_main_hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, down ? 1u : 0xC0000001u);
    if (down && (vk == 0x0D || vk == 0x1B || vk == 0x08)) rt_post_message(rt_main_hwnd, WM_CHAR, vk, 1);
}
static void tap(uint32_t vk)
{
    key(vk, 1);
    key(vk, 0);
}

/* The host's text keyboard (Switch: swkbd), if it has one */
void rt_text_keyboard(void) __attribute__((weak));

void pad_input_pointer(int x, int y)
{
    cur_x = (float)x;
    cur_y = (float)y;
    cursor_shown = 0;
    touch_mode = 1;
}

int pad_input_cursor(int *x, int *y)
{
    *x = (int)cur_x;
    *y = (int)cur_y;
    return cursor_shown;
}

/* The seed chooser (LawnApp +0x78c) while it is up (scene != 3, playing), else 0 */
static uint32_t seed_chooser(void)
{
    uint32_t app = rt_app_object();
    if (!valid(app) || g_rd32(app + 0x818) == 3) return 0;
    uint32_t sc = g_rd32(app + 0x78c);
    if (!game_rtti_is(sc, "SeedChooserScreen")) return 0;
    /* it exists off screen during Crazy Dave's talk: up = shown (+0x59) and slid in (mY +0x38 near 0) */
    int32_t y = (int32_t)g_rd32(sc + 0x38);
    return *(volatile uint8_t *)G2H(sc + 0x59) && y > -60 && y < 60 ? sc : 0;
}

/* ---------------- versus on one touch screen (the Android app) ----------------
 * Two players, a finger each: player 1 is the mouse (platform_sdl.c's touch), player 2 the DLC's keyboard player
 * (P2KB; the settings while touch plays versus), driven here by the finger that lands on its side. In the draft, a
 * tap on a zombie card moves player 2's bracket there (SeedChooserScreen +0x108, its card's seed type +0xf4 = 0x58 +
 * the index; the cards 53 x 70 from 483,123, 5 a row, measured) and presses its pick key on release. In the match a
 * finger that lands on player 2's seed bank (Board +0x14c) picks that packet (its cursor, Board +0x17c: packet +0x48),
 * the cursor (+0x24 / +0x28) follows it, and letting go on the lawn presses its plant key: drag and drop. A finger
 * that lands right of the red line (player 2's side) only moves the cursor (collecting brains). The DLC only counts a key held
 * a little over 0.1 s, so a press is held 0.15 s (p2_hold, pad_input_update). */
extern uint32_t rt_img_delta;
#define GAME_FN(va) ((va) + rt_img_delta)
static uint32_t guest_this(uint32_t fn, uint32_t self, int nargs, const uint32_t *args);
#define P2_CARDS_X 483.f
#define P2_CARDS_Y 123.f
#define P2_LAWN_X 520 /* the versus lawn's red line (column 6's left edge: zombies go right of it), board points */
static struct { int on, id, mode, card; } p2t;
static float p2_hold;
static uint32_t p2_vk;
static uint32_t player_info(void);
static uint32_t board(void);
static int is_versus(void);
static void p2_key(uint32_t vk)
{
    uint32_t b = board(), cur = b ? g_rd32(b + 0x17c) : 0;
    float cx = 0, cy = 0;
    if (valid(cur)) memcpy(&cx, G2H(cur + 0x24), 4), memcpy(&cy, G2H(cur + 0x28), 4);
    rt_log("[touch] player 2's key %02x, its cursor at %.0f,%.0f on packet %d\n", (unsigned)vk, cx, cy, valid(cur) ? (int)g_rd32(cur + 0x48) : -1);
    if (p2_hold > 0) key(p2_vk, 0);
    key(vk, 1);
    p2_vk = vk, p2_hold = 0.15f;
}
/* the screen is shared: player 1 on the mouse, player 2 the keyboard player, nobody on the AI */
static int p2_touch_on(void)
{
    uint32_t pi = player_info();
    return pad_touch_first && is_versus() && pi && !RD8_G(pi + 0x5cd4) && RD8_G(pi + 0x5cd5) && RD8_G(pi + 0x5df4);
}
static int in_rect(float x, float y, float rx, float ry, float rw, float rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
static void p2_cursor_to(uint32_t b, float gx, float gy)
{
    uint32_t cur = g_rd32(b + 0x17c);
    if (!valid(cur)) return;
    float x = gx - (float)(int32_t)g_rd32(b + 0x34), y = gy - (float)(int32_t)g_rd32(b + 0x38);
    memcpy(G2H(cur + 0x24), &x, 4);
    memcpy(G2H(cur + 0x28), &y, 4);
    g_wr32(cur + 0x148, 100); /* (as the DLC's own move sets it) */
}
/* Player 2's cursor (its bracket and label on the lawn, its bracket in the seed bank) isn't drawn while fingers play
 * (game_patches.c: Board::Draw 0x73def9, SeedBank::Draw 0x9134c0), as the mouse player has none: only while its
 * finger drags a zombie (its drop place), or with a second controller */
static int pads_connected[2];
int pad_p2_cursor_hidden(void)
{
    return p2_touch_on() && !pads_connected[1] && !(p2t.on && p2t.mode == 2);
}
/* finger id going down (kind 0), moving (1) or up (2) at display point x, y; 1 if it is player 2's (the mouse
 * doesn't see it) */
int pad_touch_finger(int id, int kind, int x, int y)
{
    if (kind != 0 && (!p2t.on || id != p2t.id)) return 0;
    if (kind == 0 && (p2t.on || !p2_touch_on())) return 0;
    float gx = ((float)x - 160.f) / 1.2f, gy = (float)y / 1.2f;
    /* versus keeps both sides' packets in one bank (Board +0x148: the plants, then the zombies, each at its own
     * place); player 2's cursor counts in it too */
    uint32_t sc = seed_chooser(), b = board(), bank = b ? g_rd32(b + 0x148) : 0;
    int on_bank = 0, packet = -1;
    if (!sc && valid(bank)) {
        float bx = (float)(int32_t)g_rd32(bank + 8), by = (float)(int32_t)g_rd32(bank + 0xc);
        on_bank = gy < by + (float)(int32_t)g_rd32(bank + 0x14);
        int n = (int)g_rd32(bank + 0x24);
        for (int i = 0; on_bank && i < n && i < 16; i++) {
            uint32_t pk = bank + 0x54 * (uint32_t)i, t = g_rd32(pk + 0x5c);
            if (t != 0x59 && (t >= 0x200 || !(guest_this(GAME_FN(0x915df0u), 0, 1, &t) & 0xff))) continue; /* (a plant) */
            if (in_rect(gx, gy, bx + (float)(int32_t)g_rd32(pk + 0x30), by + (float)(int32_t)g_rd32(pk + 0x34), 50, 70)) packet = i;
        }
    }
    if (kind == 0) {
        int mode = 0, card = -1;
        if (sc && in_rect(gx, gy, P2_CARDS_X, P2_CARDS_Y, 5 * 53, 6 * 70)) {
            mode = 1;
            card = (int)((gy - P2_CARDS_Y) / 70) * 5 + (int)((gx - P2_CARDS_X) / 53);
        } else if (!sc && b && packet >= 0) {
            mode = 2;
        } else if (!sc && b && !on_bank && gx >= (float)(P2_LAWN_X + (int32_t)g_rd32(b + 0x34))) {
            mode = 3; /* (player 2's side of the lawn: right of the red line) */
        }
        if (!mode) return 0;
        p2t.on = 1, p2t.id = id, p2t.mode = mode, p2t.card = card;
        if (mode == 1) {
            g_wr32(sc + 0x108, (uint32_t)card);
            g_wr32(sc + 0xf4, 0x58u + (uint32_t)card);
        }
        if (mode == 2) {
            uint32_t cur = g_rd32(b + 0x17c);
            if (valid(cur)) g_wr32(cur + 0x48, (uint32_t)packet);
        }
        if (mode >= 2) p2_cursor_to(b, gx, gy);
        rt_log("[touch] player 2's finger: %s at %.0f,%.0f%s\n", mode == 1 ? "card" : mode == 2 ? "seed bank" : "lawn", gx, gy,
               packet >= 0 ? " (a packet)" : "");
        return 1;
    }
    if (p2t.mode >= 2 && b) p2_cursor_to(b, gx, gy);
    if (kind == 2) {
        if (p2t.mode == 1 && sc && in_rect(gx, gy, P2_CARDS_X, P2_CARDS_Y, 5 * 53, 6 * 70)) p2_key(pkeys[1][6]); /* pick */
        if (p2t.mode == 2 && b && !on_bank) p2_key(pkeys[1][6]); /* dropped on the lawn: plant */
        p2t.on = 0, p2t.id = -1;
    }
    return 1;
}

/* a click at game coordinates (800x600), through the display's 1280x720; the button goes up a few
 * polls later (SexyApp buttons fire on a release that comes after a frame with them pressed) */
static int pending_up;
static void click_game(float gx, float gy)
{
    cur_x = 160.f + gx * 1.2f;
    cur_y = gy * 1.2f;
    mouse(WM_MOUSEMOVE);
    mouse_buttons |= 1;
    mouse(WM_LBUTTONDOWN);
    pending_up = 4;
}
static float ret_x = -1, ret_y;
static int park_after; /* after this click the hidden mouse leaves the screen (a page slides away) */
/* a pad button holding the (hidden) mouse button down on a target: let go only when it is (every press
 * completes on release, as a mouse click does, and the target shows pressed meanwhile) */
static uint32_t hold_pb;
static int hold_delay; /* polls until the mouse button goes down: the pointer rests on the target first
                        * (the Shop's Prev only answers a press it was already hovered for) */
static void press_hold(uint32_t pb, float gx, float gy)
{
    if (hold_pb || pending_up) return;
    cur_x = 160.f + gx * 1.2f;
    cur_y = gy * 1.2f;
    mouse(WM_MOUSEMOVE);
    hold_pb = pb;
    hold_delay = 5;
}
static void hold_release(uint32_t buttons)
{
    if (!hold_pb) return;
    if (hold_delay && --hold_delay == 0) {
        mouse_buttons |= 1;
        mouse(WM_LBUTTONDOWN);
    }
    if (buttons & hold_pb) return;
    if (hold_delay) { /* let go before the press went down: a whole click */
        hold_delay = 0;
        hold_pb = 0;
        mouse_buttons |= 1;
        mouse(WM_LBUTTONDOWN);
        pending_up = 4;
        return;
    }
    hold_pb = 0;
    mouse_buttons &= ~1;
    mouse(WM_LBUTTONUP);
    if (park_after) {
        park_after = 0;
        rt_mouse_x = rt_mouse_y = -400;
        rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
    }
    if (ret_x >= 0) {
        cur_x = ret_x;
        cur_y = ret_y;
        mouse(WM_MOUSEMOVE);
        ret_x = -1;
    }
}
static void click_release(void)
{
    if (pending_up && --pending_up == 0) {
        mouse_buttons &= ~1;
        mouse(WM_LBUTTONUP);
        if (park_after) {
            park_after = 0;
            cur_x = -400, cur_y = -400;
            rt_mouse_x = rt_mouse_y = -400;
            rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
        }
        if (ret_x >= 0) { /* back to where the highlight is (a page arrow's hover look would stay) */
            cur_x = ret_x;
            cur_y = ret_y;
            mouse(WM_MOUSEMOVE);
            ret_x = -1;
        }
    }
}

static uint32_t stick_dirs(const PadIn *p, uint32_t prev);

/* a card in the chooser's cell row, col (8 x 6) on page `page`: the DLC's first page is full; its second has
 * two full rows and the bottom row's cells 0, 1, 5, 6, 7; its third one row and the bottom row's first two */
static int chooser_cell(int page, int row, int col)
{
    if (page == 1) return row <= 1 || row == 5; /* (its bottom row is full once the DLC's cards are unlocked) */
    if (page == 2) return row == 0 || (row == 5 && col <= 1);
    return 1;
}

/* Controller 1 on the campaign's seed chooser; 1 while the chooser is up */
static void warm_glow(float x, float y, float w, float h, float k);
static uint32_t pad_poll_no; /* pad_input_update's count */
static int on_imit; /* the selector right of the chooser's grid: 1 the Imitater, 2 the clear arrow (pad_input_update
                     * turns the game's bracket off meanwhile) */
static int imit_picker_was_open; /* (its picker was up: imitater_update) */
static int chooser_update(const PadIn *p, float dt)
{
    static uint32_t prev, prev_dirs, last_poll;
    static int a_bank, a_log;
    static float repeat;
    uint32_t sc = seed_chooser();
    /* not run while something was over the chooser (the Imitater's picker, a warning, the menu): what is held
     * then (its B, its A) is not a press here */
    if (last_poll + 1 != pad_poll_no) prev = p->connected ? p->buttons : 0, prev_dirs = prev & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT);
    last_poll = pad_poll_no;
    if (!sc) {
        prev = p->connected ? p->buttons : 0; /* held on the way in = not a press */
        prev_dirs = 0;
        on_imit = 0;
        return 0;
    }
    /* The chooser keeps three tooltips: the mouse's (+0xe8), player 1's bracket (+0xec) and player 2's
     * (+0xf0, ToolTipWidget mVisible +0x64), which the co-op mode shows for P2's (unused) bracket at the
     * bottom left. It is only shown again when P2's bracket moves (+0xf4 = the card it showed), so hiding it
     * here keeps it hidden. */
    /* its visible tooltips (ToolTipWidget +0x54 x, +0x58 y, +0x5c w, +0x60 h, +0x64 shown) cover the badges */
    for (uint32_t o = 0xe8; o <= 0xf0; o += 4) {
        uint32_t tt = g_rd32(sc + o);
        if (valid(tt) && RD8_G(tt + 0x64))
            occlude((float)(int32_t)g_rd32(tt + 0x54), (float)(int32_t)g_rd32(tt + 0x58), (float)(int32_t)g_rd32(tt + 0x5c),
                    (float)(int32_t)g_rd32(tt + 0x60));
    }
    uint32_t tt2 = g_rd32(sc + 0xf0);
    if (valid(tt2)) {
        /* it is placed at P2's (zombie) bracket, off the bottom of the screen in the campaign, and the
         * tooltip's clamps (mMinLeft +0x68, mMaxBottom +0x6c) pulled it back into view: let it stay off screen */
        g_wr32(tt2 + 0x68, (uint32_t)-100000);
        g_wr32(tt2 + 0x6c, 100000);
    }
    uint32_t dirs = p->connected ? (stick_dirs(p, prev_dirs) | (p->buttons & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT))) : 0;
    uint32_t b = p->connected ? p->buttons : 0, pressed = b & ~prev;
    prev = b;
    /* after a touch (the bracket goes with the keyboard player) the controller's next press brings it back */
    if (pressed || (dirs & ~prev_dirs)) touch_mode = 0;
    /* a held direction repeats: first after 0.35 s, then every 0.12 s */
    uint32_t step = dirs & ~prev_dirs;
    if (dirs && dirs == prev_dirs) {
        repeat -= dt;
        if (repeat <= 0) { step = dirs; repeat = 0.12f; }
    } else {
        repeat = 0.35f;
    }
    prev_dirs = dirs;
    int sel = (int)g_rd32(sc + 0x10c), page = (int)g_rd32(sc + 0x1bc);
    if (sel < 0 || sel > 47) sel = 0;
    int col = sel % 8, row = sel / 8;
    /* the bracket steps over the empty cells of the DLC's later pages (+0x1bc the page) */
    if (!chooser_cell(page, row, col)) { /* (a page turned under it: onto the first card) */
        row = 0, col = 0;
        step |= PB_LEFT << 16; /* (moved) */
    }
    /* Right of the grid, two stops of their own (on_imit: 1 the Imitater, 2 the clear arrow; meanwhile the
     * game's bracket is off: the keyboard player off, as in touch play):
     *  - the DLC's yellow arrow (465,456 43x46), which puts every chosen card back: Right from row 4's last card
     *    (or the bottom row's without an Imitater), the warm light on it; A or L press it (the game's button acts
     *    as it is let go, pressed in while held);
     *  - the Imitater (bought: purchases +0xfd0, item 8; 467,517 45x66), Right from the bottom row's last card or
     *    Down from the arrow, the chooser's own bracket on it (pad_chooser_drawn); A or R open its picker as they
     *    are let go (the game opens it as the mouse goes down: the pointer rests on it while held, then a click).
     * Left goes back to the row's last card, Up / Down between the two. */
    uint32_t pi = player_info();
    int imit = pi && (int32_t)g_rd32(pi + 0xfd0 + 4 * 8) > 0;
    static uint32_t imit_rel; /* the button that opens the Imitater's picker when let go */
    if (!imit && on_imit == 1) on_imit = 0;
    int last5 = 7, last4 = 7;
    while (last5 > 0 && !chooser_cell(page, 5, last5)) last5--;
    while (last4 > 0 && !chooser_cell(page, 4, last4)) last4--;
    int had = on_imit;
    if (on_imit) {
        if (step & PB_LEFT) {
            row = on_imit == 2 && chooser_cell(page, 4, last4) ? 4 : 5;
            col = row == 4 ? last4 : last5;
            on_imit = 0;
            step = PB_LEFT << 16; /* (moved: below) */
        } else if ((step & PB_UP) && on_imit == 1) {
            on_imit = 2, step = 0;
        } else if ((step & PB_DOWN) && on_imit == 2 && imit) {
            on_imit = 1, step = 0;
        } else {
            step = 0;
        }
    } else if (step & PB_RIGHT) {
        if (row == 5 && col == last5) on_imit = imit ? 1 : 2, step = 0;
        else if (row == 4 && col == last4 && chooser_cell(page, 4, last4)) on_imit = 2, step = 0;
    }
    if (on_imit && on_imit != had) {
        /* the pointer rests on it: its own hover look and tooltip (the bracket's tooltip hidden) */
        cur_x = 160.f + (on_imit == 1 ? 490.f : 486.f) * 1.2f, cur_y = (on_imit == 1 ? 550.f : 479.f) * 1.2f;
        mouse(WM_MOUSEMOVE);
        uint32_t tt = g_rd32(sc + 0xec);
        if (valid(tt)) WR8_G(tt + 0x64, 0);
    }
    if (imit_picker_was_open && !hold_pb && !pending_up) { /* its picker closed: the pointer back on the Imitater
                                                             * (not on the card that was under the picked one) */
        imit_picker_was_open = 0;
        if (on_imit) {
            cur_x = 160.f + 490.f * 1.2f, cur_y = 550.f * 1.2f;
            mouse(WM_MOUSEMOVE);
        }
    }
    if (on_imit == 2) warm_glow(465, 456, 43, 46, 2.6f);
    if (((pressed & PB_A) && on_imit == 2) || (pressed & PB_L)) press_hold(pressed & (PB_A | PB_L), 486.f, 479.f);
    if (imit && !imit_rel && !hold_pb && (((pressed & PB_A) && on_imit == 1) || (pressed & PB_R))) {
        imit_rel = pressed & (PB_A | PB_R);
        cur_x = 160.f + 490.f * 1.2f, cur_y = 550.f * 1.2f;
        mouse(WM_MOUSEMOVE);
    }
    if (imit_rel && !(b & imit_rel)) {
        imit_rel = 0;
        click_game(490.f, 550.f);
    }
    for (int t = 0; t < 8 && (step & (PB_LEFT | PB_RIGHT)); t++) {
        if (step & PB_LEFT) col = col > 0 ? col - 1 : 7;
        else col = col < 7 ? col + 1 : 0;
        if (chooser_cell(page, row, col)) break;
    }
    for (int t = 0; t < 6 && (step & (PB_UP | PB_DOWN)); t++) {
        if (step & PB_UP) row = row > 0 ? row - 1 : 5;
        else row = row < 5 ? row + 1 : 0;
        if (chooser_cell(page, row, col)) break;
        /* (no card in this column there: the nearest in the row) */
        int found = -1;
        for (int d = 1; d < 8 && found < 0; d++) {
            if (col - d >= 0 && chooser_cell(page, row, col - d)) found = col - d;
            else if (col + d < 8 && chooser_cell(page, row, col + d)) found = col + d;
        }
        if (found >= 0) { col = found; break; }
    }
    /* the card's centre (SeedChooserScreen::GetSeedPositionInChooser: 53 x 70 cells from 22,123) */
    float cx = 22.f + 53.f * (float)col + 25.f, cy = 123.f + 70.f * (float)row + 35.f;
    if (step) {
        g_wr32(sc + 0x10c, (uint32_t)(row * 8 + col));
        cur_x = 160.f + cx * 1.2f;
        cur_y = cy * 1.2f;
        mouse(WM_MOUSEMOVE); /* the card's tooltip */
        cursor_shown = 0;
    }
    if ((pressed & PB_A) && !on_imit) {
        press_hold(PB_A, cx, cy);
        a_bank = (int)g_rd32(sc + 0xe0), a_log = 40;
        rt_log("[chooser] A on page %d row %d col %d (%d in the bank)\n", page, row, col, a_bank);
    }
    if (a_log && --a_log == 0) /* (what that A did, for a hardware report) */
        rt_log("[chooser] %d in the bank after\n", (int)g_rd32(sc + 0xe0));
    if (pressed & PB_B) {
        /* the last card in the seed bank: its slot's place (the board's bank, Board +0x148: its x +8, y +0xc,
         * the slot's packet +0x30 x / +0x34 y at 0x54 apart; 7-10 slots sit closer than 60 apart) */
        int n = (int)g_rd32(sc + 0xe0);
        uint32_t bd = board(), bank = bd ? g_rd32(bd + 0x148) : 0;
        if (n > 0 && n <= 10) {
            float bx = 111.f + 60.f * (float)(n - 1), by = 42.f;
            if (valid(bank) && (int)g_rd32(bank + 0x24) >= n) {
                bx = (float)(int32_t)(g_rd32(bank + 8) + g_rd32(bank + 0x54 * (uint32_t)(n - 1) + 0x30)) + 25.f;
                by = (float)(int32_t)(g_rd32(bank + 0xc) + g_rd32(bank + 0x54 * (uint32_t)(n - 1) + 0x34)) + 35.f;
            }
            rt_log("[chooser] B: card %d of the bank at %.0f,%.0f\n", n, bx, by);
            press_hold(PB_B, bx, by);
            ret_x = 160.f + cx * 1.2f;
            ret_y = cy * 1.2f;
        }
    }
    if (pressed & PB_X) press_hold(PB_X, 230.f, 564.f); /* Let's Rock */
    if (pressed & (PB_ZL | PB_ZR)) { /* the page arrows (on release, as a click); the pointer goes back to the card */
        press_hold(pressed & (PB_ZL | PB_ZR), (pressed & PB_ZL) ? 56.f : 409.f, 567.f);
        ret_x = 160.f + cx * 1.2f;
        ret_y = cy * 1.2f;
    }
    if (pressed & PB_Y) press_hold(PB_Y, 615.f, 583.f);  /* Almanac, as on the TV edition */
    if (pressed & PB_MINUS) press_hold(PB_MINUS, 735.f, 583.f); /* Shop */
    if (pressed & PB_PLUS) press_hold(PB_PLUS, 737.f, 14.f); /* the Menu button (Esc does nothing here) */
    /* the chooser's own buttons (drawn by it, measured on screen) */
    badge(465, 456, 43, 46, HELP_L);     /* back to an empty bank */
    if (imit) badge(467, 517, 45, 66, HELP_R); /* the Imitater */
    badge(682, 0, 111, 29, HELP_PLUS);   /* Menu */
    badge(157, 546, 150, 42, HELP_X);    /* Let's Rock! */
    badge_inner(23, 546, 67, 42, HELP_ZL, 1); /* page arrows */
    badge_inner(375, 546, 68, 42, HELP_ZR, -1);
    badge(563, 575, 105, 21, HELP_Y);    /* Almanac */
    badge(682, 575, 105, 21, HELP_MINUS); /* Shop */
    return 1;
}

/* the buttons a stick direction counts as, with a little hysteresis */
static uint32_t stick_dirs(const PadIn *p, uint32_t prev)
{
    uint32_t d = 0;
    float on = 0.55f, off = 0.4f;
    if (p->ly > ((prev & PB_UP) ? off : on)) d |= PB_UP;
    if (p->ly < -((prev & PB_DOWN) ? off : on)) d |= PB_DOWN;
    if (p->lx < -((prev & PB_LEFT) ? off : on)) d |= PB_LEFT;
    if (p->lx > ((prev & PB_RIGHT) ? off : on)) d |= PB_RIGHT;
    return d;
}

/* In a level the stick steers the keyboard cursor at any angle (game_patches.c replaces the DLC's
 * 8-way direction): a direction key is held while it is tilted past the dead zone, so the DLC moves. */
#define LEVEL_DEAD 0.2f
static float lvl_x[2], lvl_y[2];
static int lvl_on[2];

int pad_level_stick(int p, float *x, float *y)
{
    if (p < 0 || p > 1 || !lvl_on[p]) return 0;
    *x = lvl_x[p];
    *y = lvl_y[p];
    return 1;
}

/* the stick's direction keys for a keyboard player (its octant), and its vector for pad_level_stick */
static uint32_t level_stick(int pi, const PadIn *p)
{
    lvl_on[pi] = 0;
    if (!p->connected) return 0;
    float sx = p->lx, sy = -p->ly, m = sqrtf(sx * sx + sy * sy);
    if (m < LEVEL_DEAD) return 0;
    /* speed grows with the tilt: a quarter of the DLC's at the dead zone, all of it at full tilt */
    float t = (m - LEVEL_DEAD) / (1.f - LEVEL_DEAD);
    t = t > 1.f ? 1.f : t;
    float k = (0.25f + 0.75f * t * sqrtf(t)) / m;
    lvl_x[pi] = sx * k;
    lvl_y[pi] = sy * k;
    lvl_on[pi] = 1;
    uint32_t d = 0;
    if (sx > 0.38f * m) d |= PB_RIGHT;
    if (sx < -0.38f * m) d |= PB_LEFT;
    if (sy > 0.38f * m) d |= PB_DOWN;
    if (sy < -0.38f * m) d |= PB_UP;
    return d;
}

/* ---------------- calling the game ----------------
 * From the game's own thread (pad_input_update runs in its message pump), with a scratch area below
 * its stack: the game's std::string functions and its fonts' virtual methods. */
#define STR_CTOR 0x72a830u   /* std::string(const char *) */
#define STR_ASSIGN 0x72d2a0u /* std::string::operator=(const std::string &) */
#define STR_DTOR 0x72cd50u   /* ~std::string() */

static uint32_t guest_this(uint32_t fn, uint32_t self, int nargs, const uint32_t *args)
{
    RtThread *t = rt_thread_self();
    if (!t) return 0;
    CPU *c = &t->cpu;
    uint32_t ecx = c->ecx, edx = c->edx, eax = c->eax;
    c->ecx = self;
    uint32_t r = rt_guest_call(c, fn, nargs, args);
    c->ecx = ecx, c->edx = edx, c->eax = eax;
    return r;
}

/* a scratch block under the guest stack: [0..28) a std::string, [32..) its text */
static uint32_t scratch_string(const char *text)
{
    RtThread *t = rt_thread_self();
    if (!t) return 0;
    uint32_t base = (t->cpu.esp - 0x400) & ~15u;
    size_t n = strlen(text);
    if (n > 200) n = 200;
    memcpy(G2H(base + 32), text, n);
    *(char *)G2H(base + 32 + n) = 0;
    t->cpu.esp = base; /* the game's calls use the stack below it */
    uint32_t arg = base + 32;
    guest_this(GAME_FN(STR_CTOR), base, 1, &arg);
    return base;
}

/* Font (Sexy::Font virtuals: 1 GetAscent, 7 StringWidth(const std::string &)) */
static int font_ascent(uint32_t font)
{
    if (!valid(font)) return 0;
    return (int)guest_this(g_rd32(g_rd32(font) + 4), font, 0, NULL);
}

static int font_width(uint32_t font, const char *text)
{
    if (!valid(font)) return 0;
    RtThread *t = rt_thread_self();
    if (!t) return 0;
    uint32_t esp = t->cpu.esp;
    uint32_t str = scratch_string(text);
    int w = (int)guest_this(g_rd32(g_rd32(font) + 28), font, 1, &str);
    guest_this(GAME_FN(STR_DTOR), str, 0, NULL);
    t->cpu.esp = esp;
    return w;
}

/* The port in the DLC's credits roll (ChallengeScreen page 11), after the DLC team (user: the mod creator, the mod
 * team, then the port): a section as the DLC's own are made ("Mod Creator" over "Samen"), 113 px under the
 * playtesters' last row (5713) as their sections are spaced, its title in their titles' font (0x1093fb8) and
 * white, the name 56 px under it in their names' font (0x1093fb4) and colour (the Color at 0x10d619c), both
 * centred on the page (TodDrawString, 0x9c97f0, cdecl: Graphics *, const std::string &, x, y, Font *, const
 * Color &, justification 2). On the screen y = page y - scroll; the rest of the page moves down to make room
 * (game_hook_credits_port). */
#define TOD_DRAW_STRING 0x9c97f0u
void pad_credits_port(uint32_t g, int32_t scroll)
{
    static const struct { const char *text; uint32_t font; int32_t y; int title; } line[2] = {
        {"Switch, Mac, Windows and Android Port", 0x1093fb8u, 5826, 1},
        {"aks796", 0x1093fb4u, 5882, 0},
    };
    RtThread *t = rt_thread_self();
    if (!t || !valid(g)) return;
    for (int i = 0; i < 2; i++) {
        int32_t y = line[i].y - scroll;
        uint32_t font = g_rd32(line[i].font + rt_img_delta);
        if (y < -100 || y > 700 || !valid(font)) continue;
        uint32_t esp = t->cpu.esp;
        uint32_t str = scratch_string(line[i].text);
        uint32_t color = 0x10d619cu + rt_img_delta;
        if (line[i].title) {
            static const int32_t white[4] = {255, 255, 255, 255};
            color = str - 16;
            memcpy(G2H(color), white, 16);
            t->cpu.esp = color;
        }
        uint32_t args[7] = {g, str, 400, (uint32_t)y, font, color, 2};
        guest_this(GAME_FN(TOD_DRAW_STRING), 0, 7, args);
        guest_this(GAME_FN(STR_DTOR), str, 0, NULL);
        t->cpu.esp = esp;
    }
}

/* set a game std::string (e.g. a button's label) to text */
static void game_string_set(uint32_t dst, const char *text)
{
    RtThread *t = rt_thread_self();
    if (!t) return;
    uint32_t esp = t->cpu.esp;
    uint32_t str = scratch_string(text);
    guest_this(GAME_FN(STR_ASSIGN), dst, 1, &str);
    guest_this(GAME_FN(STR_DTOR), str, 0, NULL);
    t->cpu.esp = esp;
}

/* ---------------- button pictures in the game's text ----------------
 * As PvZ Touch's text: the console wording for the game's strings (PvZ Touch's console_strings.txt, the
 * ones that match this port's controls), with the buttons as pictures. In a string a button is a marker
 * byte, 1 + its cel in the Xbox 360 sheet (no glyph in the game's fonts, so no width), followed by four
 * spaces that make the gap; Font::DrawString (hooked at its start, game_patches.c) finds the markers and
 * puts the picture in each gap, as tall as 0.9 of the font's ascent (PvZ Touch's size) when it fits. A string
 * can ask for another size (badge, of the ascent): the Slot Machine's prompts sit in the seed bank's narrow strip
 * in a font whose ascent (24) is far taller than its capitals (13.5), so their X is 0.68 of it, 1.2x the capitals,
 * inside the strip. */
static const struct { const char *key, *text; float badge; } console_text[] = {
    {"CLICK_TO_START", "PRESS <A> TO START!"},
    {"CLICK_TO_CONTINUE", "Press <A> to continue"},
    {"ADVICE_CLICK_SEED_PACKET", "Use <L> and <R> to pick a seed packet!"},
    {"ADVICE_CLICK_ON_GRASS", "Press <A> on the grass to plant your seed!"},
    {"ADVICE_CLICK_SHOVEL", "Hold <B> over a plant to dig it up!"},
    {"ADVICE_CLICK_ON_SUN", "Move over the sun to collect it!"},
    /* the mini-games' own buttons (PvZ Touch's console_strings.txt) */
    {"ADVICE_SLOT_MACHINE_PULL", "Press <X> to pull handle!", 0.68f},
    {"ADVICE_SLOT_MACHINE_SPIN_AGAIN", "<X> To Spin Again!", 0.68f},
    {"ADVICE_BEGHOULED_DRAG_TO_MATCH_3", "Hold <X> and move plants to make matches of 3"},
    {"ADVICE_BEGHOULED_TWIST_TO_MATCH_3", "Press <X> to twist plants to make matches of 3"},
    {"ADVICE_ZOMBIQUARIUM_CLICK_TO_FEED", "Press <X> on tank to feed zombies"},
    {"ADVICE_ZOMBIQUARIUM_BUY_SNORKEL", "Press <A> to buy more snorkel zombies!"},
    {"ADVICE_USE_SHOVEL_ON_POTS", "Press <X> on a vase to see what's inside!"},
};
/* the game's own strings, with tap for click (the Android app: pad_touch_first) */
static const struct { const char *key, *text; } touch_text[] = {
    {"CLICK_TO_START", "TAP TO START!"},
    {"CLICK_TO_CONTINUE", "Tap to continue"},
    {"CLICK_TO_VIEW", "tap to view"},
    {"CREDITS_PAUSE_BODY", "Tap to resume the credits. "},
    {"UPSELL_PAUSE_BODY", "Tap to resume to continue."},
    {"ADVICE_CLICK_ON_SUN", "Tap the falling sun to collect it!"},
    {"ADVICE_CLICK_PEASHOOTER", "Tap the peashooter to plant another one!"},
    {"ADVICE_CLICK_SEED_PACKET", "Tap a seed packet to pick it up!"},
    {"ADVICE_CLICK_ON_GRASS", "Tap the grass to plant your seed!"},
    {"ADVICE_CLICK_SHOVEL", "Tap the shovel to pick it up!"},
    {"ADVICE_CLICK_PLANT", "Tap a plant to remove it!"},
    {"ADVICE_BEGHOULED_DRAG_TO_MATCH_3", "Drag plants to make matches of 3"},
    {"ADVICE_BEGHOULED_TWIST_TO_MATCH_3", "Tap to twist plants to make matches of 3"},
    {"ADVICE_ZOMBIQUARIUM_CLICK_TO_FEED", "Tap the tank to feed the zombies"},
    {"ADVICE_ZOMBIQUARIUM_CLICK_TROPHY", "Tap the trophy to complete the level!"},
    {"ADVICE_ZOMBIQUARIUM_BUY_SNORKEL", "Tap to buy more snorkel zombies!"},
    {"ADVICE_ZEN_GARDEN_PICK_UP_WATER", "Tap the watering can to pick it up!"},
    {"ADVICE_ZEN_GARDEN_WATER_PLANT", "Tap a sprout to water it!"},
    {"ADVICE_USE_SHOVEL_ON_POTS", "Tap a vase to see what's inside!"},
    {"ADVICE_STINKY_SLEEPING", "Stinky the Snail fell asleep! Tap him to wake him up!"},
    {"ADVICE_BUTTER", "Butter the zombies by tapping them!"},
    {"ADVICE_JUKEBOX_CLICK_ALBUM", "Tap the album to insert the disc!"},
    {"ADVICE_JUKEBOX_CLICK_PAUSE", "Tap the pause button to pause the track!"},
    {"ADVICE_JUKEBOX_CLICK_RESUME", "Tap it again to resume the track!"},
    {"ADVICE_JUKEBOX_CLICK_ARROWS", "Tap the arrow buttons to switch tracks in the album!"},
    {"ADVICE_JUKEBOX_USE_MUSIC", "Tapping the \"Use Music\" button will replace level music with the track you are currently on."},
    {"ADVICE_JUKEBOX_ENABLE_HORDE", "For tracks with horde themes, tapping the \"Enable Horde\" button previews the horde version."},
    {"COB_CANNON_TOOLTIP", "Tap to launch deadly cobs of corn"},
    {"COB_CANNON_DESCRIPTION", "Tap the Cob Cannon to launch deadly cobs of corn."},
    {"CRAZY_DAVE_2007", "{NO_SOUND}{NO_CLICK}Plant these on your kernel-pults to turn them into cob cannons! Tap a cob cannon "
                        "to launch a deadly attack!"},
    {"TREE_OF_WISDOM_9", "Have you tried tapping the flowers on the main menu? Give it a shot! I'll wait here."},
    {"TREE_OF_WISDOM_31", "Once you buy the imitater, try tapping the little drawing in the upper left corner of your Almanac "
                          "to access the entry on that sucker."},
};
static const struct { const char *tag; int cel; } button_tags[] = {
    {"<A>", HELP_A}, {"<B>", HELP_B}, {"<X>", HELP_X}, {"<Y>", HELP_Y}, {"<S>", HELP_PLUS}, {"<b>", HELP_MINUS},
    {"<L>", HELP_L}, {"<R>", HELP_R}, {"<LT>", HELP_ZL}, {"<RT>", HELP_ZR}, {"<D>", HELP_DPAD},
};
#define ICON_GAP "    "

/* tags -> marker byte + gap */
static void console_markup(const char *in, char *out, size_t n)
{
    size_t o = 0;
    while (*in && o + 1 < n) {
        int done = 0;
        for (unsigned k = 0; k < sizeof button_tags / sizeof *button_tags && !done; k++) {
            size_t tl = strlen(button_tags[k].tag);
            if (!strncmp(in, button_tags[k].tag, tl) && o + sizeof ICON_GAP < n) {
                out[o++] = (char)(1 + button_tags[k].cel);
                memcpy(out + o, ICON_GAP, sizeof ICON_GAP - 1);
                o += sizeof ICON_GAP - 1;
                in += tl;
                done = 1;
            }
        }
        if (!done) out[o++] = *in++;
    }
    out[o] = 0;
}

/* TodStringListFind returned `result` (a std::string) for the key in `name` (game_patches.c hook) */
void pad_string_found(uint32_t result, uint32_t name)
{
    if (pad_desktop) return; /* (the PC's own wording) */
    char key[64];
    game_string(name, key, sizeof key);
    if (pad_touch_first) {
        for (unsigned i = 0; i < sizeof touch_text / sizeof *touch_text; i++)
            if (!strcmp(key, touch_text[i].key)) game_string_set(result, touch_text[i].text);
        return;
    }
    for (unsigned i = 0; i < sizeof console_text / sizeof *console_text; i++)
        if (!strcmp(key, console_text[i].key)) {
            char text[160];
            console_markup(console_text[i].text, text, sizeof text);
            game_string_set(result, text);
            return;
        }
}

/* Font::DrawString(g, x, baseline y, text, ...) is about to draw: the pictures for its markers */
/* the title's start prompt while A is held (pad_title_held): drawn 2 down and right and a little darker, as a
 * button pushed in (game_hook_font_draw moves the call's x, y and colour) */
int pad_font_pushed(uint32_t str, uint32_t *col)
{
    if (!pad_title_held) return 0;
    char text[64];
    game_string(str, text, sizeof text);
    if (!strstr(text, "TO START")) return 0;
    static uint32_t dark;
    if (valid(*col) && (dark || (dark = rt_heap_alloc(16, 1)))) {
        for (int i = 0; i < 3; i++) g_wr32(dark + 4 * (uint32_t)i, g_rd32(*col + 4 * (uint32_t)i) * 3 / 4);
        g_wr32(dark + 12, g_rd32(*col + 12));
        *col = dark;
    }
    return 2;
}
static int vs_ai_box; /* Multiplayer's AI Opponent box ticked (while the screen is up: versus_screen) */
void pad_font_draw(uint32_t font, uint32_t g, int x, int y, uint32_t str, uint32_t *grey_col)
{
    char text[200];
    game_string(str, text, sizeof text);
    /* Multiplayer's "AI Difficulty:" greyed with its pepper while the AI is off: drawn with a grey Color of its
     * own (r g b a ints, in a block of the game's heap: the call's colour argument is pointed at it; the red
     * it had is a shared one, so it isn't changed) */
    if (screen_kind == SCR_CHALLENGE && grey_col && !vs_ai_box && !strcmp(text, "AI Difficulty:")) {
        static uint32_t grey;
        if (!grey && (grey = rt_heap_alloc(16, 1))) {
            const uint32_t c[4] = {130, 110, 100, 170};
            memcpy(G2H(grey), c, 16);
        }
        if (grey) *grey_col = grey;
    }
    const char *m = text;
    while (*m && !(*m >= 1 && *m <= 13)) m++;
    if (!*m || !valid(font) || !valid(g)) return;
    float tx, ty;
    memcpy(&tx, G2H(g + 8), 4); /* Graphics mTransX / mTransY */
    memcpy(&ty, G2H(g + 12), 4);
    int asc = font_ascent(font), gap = font_width(font, ICON_GAP);
    /* PvZ Touch's size and place: as tall as 0.9 of the ascent, centred 0.67 of the ascent down from the
     * top of the line (the baseline less the ascent) */
    float sz = (float)lroundf((float)asc * 0.9f);
    for (unsigned i = 0; i < sizeof console_text / sizeof *console_text; i++) /* (a string with its own size) */
        if (console_text[i].badge > 0) {
            char marked[200];
            console_markup(console_text[i].text, marked, sizeof marked);
            if (!strcmp(marked, text)) sz = (float)lroundf((float)asc * console_text[i].badge);
        }
    /* (fading with the text: the alpha of its colour, Graphics +0x30 mColor r, g, b, a) */
    uint32_t alpha = g_rd32(g + 0x3c);
    if (alpha > 255) alpha = 255;
    ov_use_frame_list(1);
    for (const char *q = text; *q; q++) {
        if (!(*q >= 1 && *q <= 13)) continue;
        char prefix[200];
        snprintf(prefix, sizeof prefix, "%.*s", (int)(q - text), text);
        float px = tx + (float)x + (float)font_width(font, prefix) + ((float)gap - sz) / 2;
        float py = ty + (float)y - (float)asc + (float)lroundf((float)asc * 0.67f - sz / 2);
        ov_image(OV_HELP, 42 * (*q - 1), 0, 42, 42, px, py, sz, sz, alpha << 24 | 0xffffffu);
    }
    ov_use_frame_list(0);
}

/* Crazy Dave's bubble: LawnApp::DrawCrazyDave draws the literal "click to continue" (0xd343dc, room
 * for 23 characters) centred at (bubble x + 139, baseline bubble y + 140) in FONT_PICO129 (drawn in a
 * bigger font instead, game_patches.c). The literal becomes "press <A> to continue" in the marked-up form
 * above, so its picture comes with the text. */

void pad_dave_prompt(int x, int y, uint32_t font)
{
    (void)x, (void)y, (void)font;
    /* the desktop apps keep the PC's own words while the mouse plays, the Android app says tap while touch plays;
     * a controller's play (and the Switch) says press A. Written again whenever that changes. */
    const char *want = "press <A> to continue";
    if (pad_desktop && touch_mode) want = "click to continue";
    else if (pad_touch_first && touch_mode) want = "tap to continue";
    static const char *done;
    if (done == want) return;
    done = want;
    char text[24];
    console_markup(want, text, sizeof text); /* 23 bytes at most */
    memcpy(G2H(GAME_FN(0xd343dcu)), text, strlen(text) + 1);
}

/* ---------------- menus by controller ----------------
 * As in PvZ Touch: the D-pad / stick moves a highlight between the screen's buttons, A presses the
 * highlighted one (it stays pressed in until A is let go, as a held mouse button does), B goes back.
 * The highlight is the game's own hover look: the pointer (hidden) rests on the chosen button.
 * Buttons come from the game's widget tree (any ButtonWidget, Checkbox or Slider that is shown, enabled
 * and on screen; only the top dialog's while one is open), plus the buttons some screens keep and draw
 * themselves (the mini-games list). */
typedef struct { float x, y, w, h; uint32_t obj; int kind, in_list; char label[32]; } Spot;
enum { SPOT_BUTTON, SPOT_CHECK, SPOT_SLIDER, SPOT_AREA, SPOT_EDIT }; /* AREA: a place on a screen that draws its own buttons */
#define MAX_SPOTS 160
static Spot spots[MAX_SPOTS];
static int n_spots;
static uint32_t focus_obj;

static int spot_index(uint32_t obj)
{
    for (int i = 0; i < n_spots; i++)
        if (spots[i].obj == obj) return i;
    return -1;
}

static void add_spot(uint32_t w, float x, float y, int kind)
{
    if (n_spots == MAX_SPOTS) return;
    float ww = (float)(int32_t)g_rd32(w + 0x3c), hh = (float)(int32_t)g_rd32(w + 0x40);
    if (ww < 4 || hh < 4 || ww > 800 || hh > 600) return;
    float cx = x + ww / 2, cy = y + hh / 2;
    if (cx < 0 || cx >= 800 || cy < 0 || cy >= 600) return; /* off screen (slid away, scrolled out) */
    Spot *sp = &spots[n_spots++];
    sp->x = x, sp->y = y, sp->w = ww, sp->h = hh, sp->obj = w, sp->kind = kind, sp->in_list = 0;
    sp->label[0] = 0;
    if (kind == SPOT_BUTTON) game_string(w + 0x90, sp->label, sizeof sp->label);
}

/* a widget shown, reacting to the mouse and enabled (Widget +0x58 mVisible, +0x59 mMouseVisible,
 * +0x5a mDisabled) */
static int widget_live(uint32_t w)
{
    return RD8_G(w + 0x58) && RD8_G(w + 0x59) && !RD8_G(w + 0x5a);
}

static void collect_members(uint32_t w, float x, float y);
static void collect_in(uint32_t wc, float ox, float oy, int depth)
{
    uint32_t head = g_rd32(wc + 8), n = g_rd32(wc + 0xc);
    if (!valid(head) || n > 200) return;
    uint32_t node = g_rd32(head);
    for (uint32_t i = 0; i < n && valid(node) && node != head; i++, node = g_rd32(node)) {
        uint32_t w = g_rd32(node + 8);
        if (!valid(w) || !RD8_G(w + 0x58)) continue;
        float x = ox + (float)(int32_t)g_rd32(w + 0x34), y = oy + (float)(int32_t)g_rd32(w + 0x38);
        /* (the jukebox's album now playing is a disabled button, ids 30004..30007: the selector still stops on it,
         * as the album it is on; A does nothing there, as a click wouldn't) */
        uint32_t wid = g_rd32(w + 0x8c);
        if (!widget_live(w) && wid >= 30004 && wid <= 30007 && RD8_G(w + 0x59) && game_rtti_is(wc, "MusicScreen"))
            add_spot(w, x, y, SPOT_BUTTON);
        else if (widget_live(w)) {
            if (game_rtti_is(w, "ButtonWidget@Sexy")) add_spot(w, x, y, SPOT_BUTTON);
            else if (game_rtti_is(w, "Checkbox@Sexy")) add_spot(w, x, y, SPOT_CHECK);
            else if (game_rtti_is(w, "Slider@Sexy")) add_spot(w, x, y, SPOT_SLIDER);
            else if (game_rtti_is(w, "EditWidget@Sexy")) add_spot(w, x, y, SPOT_EDIT);
        }
        if (depth < 6) collect_in(w, x, y, depth + 1);
        /* the mini-games / puzzle / survival list keeps its buttons as members and draws them itself */
        if (depth == 0 && game_rtti_is(w, "ChallengeScreen")) collect_members(w, x, y);
    }
}

/* the mini-games / puzzle / survival list keeps its buttons as members and draws them itself; it
 * scrolls: rows out of its view report made-up places, so only whole rows count and stepping past the
 * last one scrolls (menu_update) */
static void collect_members(uint32_t w, float x, float y)
{
    list_scrolls = 1;
    list_more = 0;
    list_cards = 0;
    /* (the Multiplayer screen keeps its two name boxes and its two checkboxes as members too, after them) */
    for (uint32_t o = 0x400; o < 0x440; o += 4) {
        uint32_t b = g_rd32(w + o);
        if (!valid(b) || !widget_live(b) || spot_index(b) >= 0) continue;
        float bx = x + (float)(int32_t)g_rd32(b + 0x34), by = y + (float)(int32_t)g_rd32(b + 0x38);
        if (game_rtti_is(b, "Checkbox@Sexy")) add_spot(b, bx, by, SPOT_CHECK);
        else if (game_rtti_is(b, "EditWidget@Sexy")) add_spot(b, bx, by, SPOT_EDIT);
    }
    for (uint32_t o = 0x98; o < 0x400; o += 4) {
        uint32_t b = g_rd32(w + o);
        if (!valid(b) || !game_rtti_is(b, "ButtonWidget@Sexy") || !widget_live(b)) continue;
        if (spot_index(b) >= 0) continue;
        float bx = x + (float)(int32_t)g_rd32(b + 0x34), by = y + (float)(int32_t)g_rd32(b + 0x38);
        float bh = (float)(int32_t)g_rd32(b + 0x40);
        if (bh >= 60) list_cards++;
        if (bh >= 60 && (by < 60 || by + bh > 590)) list_more = 1;
        if (bh < 60) add_spot(b, bx, by, SPOT_BUTTON);
        else if (by >= 60 && by + bh <= 590) {
            add_spot(b, bx, by, SPOT_BUTTON);
            if (n_spots) spots[n_spots - 1].in_list = 1;
        }
    }
}

/* the open dialog on top (LawnApp+0x330: SexyApp's dialog list, std::list {proxy, head, size}), or 0 */
static uint32_t top_dialog(void)
{
    uint32_t app = rt_app_object();
    if (!valid(app)) return 0;
    uint32_t head = g_rd32(app + 0x334), n = g_rd32(app + 0x338);
    if (!valid(head) || n == 0 || n > 16) return 0;
    uint32_t last = g_rd32(head + 4); /* the sentinel's prev */
    uint32_t d = valid(last) ? g_rd32(last + 8) : 0;
    return valid(d) ? d : 0;
}

/* the open dialog, else the topmost screen (the WidgetManager's last child that isn't an overlay) */
uint32_t game_top_widget(void)
{
    uint32_t d = top_dialog();
    if (d) return d;
    uint32_t app = rt_app_object(), wm = valid(app) ? g_rd32(app + 0x320) : 0;
    if (!valid(wm)) return 0;
    uint32_t head = g_rd32(wm + 8), n = g_rd32(wm + 0xc), node = valid(head) ? g_rd32(head) : 0, top = 0;
    for (uint32_t i = 0; i < n && i < 64 && valid(node) && node != head; i++, node = g_rd32(node)) {
        uint32_t w = g_rd32(node + 8);
        int32_t x = (int32_t)g_rd32(w + 0x34);
        /* on screen (the main menu's second page waits at x 800), full size, not an overlay */
        if (valid(w) && RD8_G(w + 0x58) && g_rd32(w + 0x3c) >= 700 && x > -100 && x < 100 &&
            !strstr(game_rtti_name(w), "Overlay"))
            top = w;
    }
    return top;
}

static void add_screen_areas(void);
static void collect_spots(void)
{
    n_spots = 0;
    list_scrolls = 0;
    list_more = 0;
    add_screen_areas();
    uint32_t app = rt_app_object(), wm = valid(app) ? g_rd32(app + 0x320) : 0;
    if (!valid(wm)) return;
    uint32_t dlg = top_dialog();
    if (dlg) {
        /* the dialog's own position, then its children */
        float x = (float)(int32_t)g_rd32(dlg + 0x34), y = (float)(int32_t)g_rd32(dlg + 0x38);
        collect_in(dlg, x, y, 1);
        if (n_spots) return;
    }
    /* a full screen over another (the jukebox over the second page): only its own buttons */
    uint32_t top = screen_obj;
    if (top && screen_kind != SCR_MENU && screen_kind != SCR_SIDE) {
        float x = (float)(int32_t)g_rd32(top + 0x34), y = (float)(int32_t)g_rd32(top + 0x38);
        collect_in(top, x, y, 1);
        if (screen_kind == SCR_CHALLENGE) collect_members(top, x, y);
        return;
    }
    collect_in(wm, 0, 0, 0);
}

/* a place the screen hit-tests itself (no widget): `id` makes it a stable focus target */
static void add_area(uint32_t id, float x, float y, float w, float h, int in_list)
{
    if (n_spots == MAX_SPOTS) return;
    Spot *sp = &spots[n_spots++];
    sp->x = x, sp->y = y, sp->w = w, sp->h = h, sp->obj = 0xFE000000u | id, sp->kind = SPOT_AREA, sp->in_list = in_list;
    sp->label[0] = 0;
}

/* a member ButtonWidget of screen w with this id, or 0 */
static uint32_t button_with_id(uint32_t w, int id)
{
    for (uint32_t o = 0x90; o < 0x400; o += 4) {
        uint32_t b = g_rd32(w + o);
        if (valid(b) && game_rtti_is(b, "ButtonWidget@Sexy") && (int)g_rd32(b + 0x8c) == id) return b;
    }
    return 0;
}

static int widget_live(uint32_t w);
/* a screen with a button of its own showing (a widget child) */
static int has_live_button(uint32_t wc)
{
    uint32_t head = g_rd32(wc + 8), n = g_rd32(wc + 0xc), node = valid(head) ? g_rd32(head) : 0;
    for (uint32_t i = 0; i < n && i < 64 && valid(node) && node != head; i++, node = g_rd32(node)) {
        uint32_t c = g_rd32(node + 8);
        if (valid(c) && game_rtti_is(c, "ButtonWidget@Sexy") && widget_live(c)) return 1;
    }
    return 0;
}

/* screens that draw (some of) their buttons themselves: their places, in game points (from the
 * game's layout: the DLC's Almanac and Shop, measured on screen) */

static void add_screen_areas(void)
{
    uint32_t w = game_top_widget();
    screen_obj = w;
    const char *n = game_rtti_name(w);
    screen_kind = strstr(n, "GameSelector") ? SCR_MENU : strstr(n, "SideScreen") ? SCR_SIDE
                : strstr(n, "AlmanacDialog") ? SCR_ALMANAC : strstr(n, "StoreScreen") ? SCR_STORE
                : strstr(n, "ChallengeScreen") ? SCR_CHALLENGE : strstr(n, "OptionsDialog") ? SCR_OPTIONS
                : strstr(n, "AwardScreen") && !button_with_id(w, 1000) && !has_live_button(w) ? SCR_AWARD : SCR_OTHER;
    /* the mailbox is an award screen opened from the second page; the others (the zombies' note before
     * the music video...) are reached by winning */
    static uint32_t mail_obj;
    static int kind_before;
    if (screen_kind == SCR_AWARD) {
        if (w != mail_obj && kind_before == SCR_SIDE) mail_obj = w;
        if (w == mail_obj) screen_kind = SCR_MAIL;
    }
    if (screen_kind != SCR_AWARD && screen_kind != SCR_MAIL) kind_before = screen_kind;
    if (screen_kind == SCR_MENU) {
        add_area(0x100, 29, 325, 117, 208, 0); /* the trophy: its tooltip shows on hover */
    } else if (screen_kind == SCR_ALMANAC) {
        int page = (int)g_rd32(w + 0x238); /* 0 index, 1 plants, 2 zombies */
        if (page == 0) {
            add_area(0x200, 132, 346, 153, 39, 0); /* View Plants */
            add_area(0x201, 489, 346, 201, 39, 0); /* View Zombies */
        } else {
            int cols = page == 1 ? 8 : 5, rows = 6;
            float x0 = page == 1 ? 26.7f : 25.f, y0 = page == 1 ? 92.5f : 88.3f;
            float px = page == 1 ? 51.7f : 85.f, py = page == 1 ? 78.3f : 80.f;
            float cw = page == 1 ? 48.3f : 70.8f, ch = page == 1 ? 68.3f : 70.f;
            /* only the cells with a card to pick (the Almanac's own hit tests, at each cell's middle:
             * SeedHitTest 0x7daa70 / ZombieHitTest 0x7db970 (x, y) -> type or -1, and for a zombie its
             * 0x7dba80 (met: else a silhouette that a click leaves be)); kept per page / sub-page (+0x8a0) */
            static uint64_t have_key = ~0ull, have;
            uint64_t key = (uint64_t)w << 8 | (uint64_t)(page << 4) | (g_rd32(w + 0x8a0) & 15);
            if (key != have_key) {
                have_key = key, have = 0;
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++) {
                        uint32_t xy[2] = {(uint32_t)(int)(x0 + px * (float)c + cw / 2), (uint32_t)(int)(y0 + py * (float)r + ch / 2)};
                        int32_t t = (int32_t)guest_this(GAME_FN(page == 1 ? 0x7daa70u : 0x7db970u), w, 2, xy);
                        if (t != -1 && page == 2) {
                            uint32_t z = (uint32_t)t;
                            if (!(guest_this(GAME_FN(0x7dba80u), w, 1, &z) & 0xff)) t = -1;
                        }
                        if (t != -1) have |= 1ull << (r * cols + c);
                    }
            }
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++)
                    if (have >> (r * cols + c) & 1)
                        add_area(0x300 + (uint32_t)(page * 0x40 + r * cols + c), x0 + px * (float)c, y0 + py * (float)r, cw, ch, 0);
            add_area(0x210, 35, 569, 158, 21, 0);  /* Almanac Index */
            add_area(0x211, 348, 569, 159, 21, 0); /* Last Page */
            add_area(0x212, 513, 569, 160, 21, 0); /* Next Page */
        }
        add_area(0x213, 679, 569, 83, 21, 0); /* Close */
    } else if (screen_kind == SCR_STORE) {
        /* the page's items, as the shop's own UpdateMouse finds them: StoreScreen::GetStoreItemType
         * (0x91a050) per spot, skipping empty and unavailable ones (IsItemUnavailable, 0x91a430); each
         * item's place is GetStorePosition's 50 x 87 (the card and its price) */
        for (int i = 0; i < 8; i++) {
            uint32_t spot = (uint32_t)i;
            int32_t type = (int32_t)guest_this(GAME_FN(0x91a050u), w, 1, &spot);
            if (type < 0) continue;
            uint32_t ty = (uint32_t)type;
            if (guest_this(GAME_FN(0x91a430u), w, 1, &ty) & 0xff) continue;
            float x = i < 4 ? 422.f + 74.f * (float)i : 372.f + 74.f * (float)(i - 4), y = i < 4 ? 206.f : 310.f;
            add_area(0x400 + (uint32_t)i, x, y, 50, 87, 0);
        }
    } else if (screen_kind == SCR_AWARD) {
        add_area(0x510, 300, 520, 200, 70, 0); /* Roll Credits! (the award's own button, drawn by it) */
        add_area(0x502, 679, 18, 105, 22, 0);  /* MAIN MENU */
    } else if (screen_kind == SCR_MAIL) {
        /* the mailbox draws its own words to click */
        add_area(0x500, 21, 558, 100, 26, 0);  /* Last Page! */
        add_area(0x501, 667, 558, 112, 26, 0); /* Next Page! */
        add_area(0x502, 679, 18, 105, 22, 0);  /* MAIN MENU */
    }
}

static Spot *spot_of(uint32_t obj)
{
    for (int i = 0; i < n_spots; i++)
        if (spots[i].obj == obj) return &spots[i];
    return NULL;
}

/* where the highlight starts on a new screen: the biggest button (the main menu's Adventure sign, a
 * dialog's OK / Back To Game) */
static Spot *default_spot(void)
{
    Spot *best = NULL;
    for (int i = 0; i < n_spots; i++)
        if (spots[i].kind == SPOT_BUTTON && (!best || spots[i].w * spots[i].h > best->w * best->h)) best = &spots[i];
    return best ? best : n_spots ? &spots[0] : NULL;
}

/* PvZ Touch's StepSpot: the nearest spot that way, cost = distance ahead + 3x sideways, anything 8 px or
 * less ahead (or more than twice as far sideways) skipped; sideways first tries the spots sharing the row */
static Spot *step_spot_in(Spot *from, int dxs, int dys, int list_only);
static Spot *step_spot(Spot *from, int dxs, int dys)
{
    return step_spot_in(from, dxs, dys, 0);
}

static Spot *step_spot_in(Spot *from, int dxs, int dys, int list_only)
{
    float fx = from->x + from->w / 2, fy = from->y + from->h / 2;
    Spot *best = NULL;
    float best_cost = 1e9f;
    /* left / right: first what has its middle within this one's height (More Settings: a checkbox reaching 10 px
     * into Back's row, nearer, came before Next beside it), then what overlaps the row at all, then anything */
    for (int pass = dxs ? -1 : 1; pass < 2 && !best; pass++)
        for (int i = 0; i < n_spots; i++) {
            Spot *sp = &spots[i];
            if (sp == from || (list_only && !sp->in_list)) continue;
            if (pass == -1 && (sp->y + sp->h / 2 < from->y || sp->y + sp->h / 2 > from->y + from->h)) continue;
            if (pass == 0 && (sp->y >= from->y + from->h || sp->y + sp->h <= from->y)) continue; /* not in the row */
            float vx = sp->x + sp->w / 2 - fx, vy = sp->y + sp->h / 2 - fy;
            float ahead = vx * (float)dxs + vy * (float)dys;
            /* sideways: the gap between the two along the other axis (0 when they overlap: a wide Cancel under
             * two buttons is "down" from both of them) */
            float aside = dxs ? fmaxf(0.f, fmaxf(sp->y - (from->y + from->h), from->y - (sp->y + sp->h)))
                              : fmaxf(0.f, fmaxf(sp->x - (from->x + from->w), from->x - (sp->x + sp->w)));
            /* and it must lie past the middle of this one's far half (the main menu's signs are stacked a few
             * pixels apart sideways: "left" from Mini-Games found Puzzle) */
            float half = dxs ? from->w / 2 : from->h / 2;
            if (ahead < half * 0.5f) continue;
            /* that way at all: ahead, and within a cone (the main menu's signs are stacked a few pixels
             * apart sideways, so "left" from Mini-Games found Puzzle, 9 px left and below it) */
            if (ahead <= 8 || aside > 2 * ahead + 20) continue;
            float cost = ahead + 3 * aside;
            if (cost < best_cost) best_cost = cost, best = sp;
        }
    return best;
}

/* B: the screen's way back (a dialog's Cancel / No / Back / OK, the mini-games list's Back to Menu) */
static Spot *back_spot(void)
{
    static const char *words[] = {"cancel", "no", "back", "close", "main menu", "ok", "done", "leave"};
    for (unsigned k = 0; k < sizeof words / sizeof *words; k++)
        for (int i = 0; i < n_spots; i++) {
            char l[32];
            snprintf(l, sizeof l, "%s", spots[i].label);
            for (char *q = l; *q; q++) *q = (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
            size_t n = strlen(words[k]);
            if (!strncmp(l, words[k], n) && (l[n] == 0 || l[n] == ' ' || l[n] == '!')) return &spots[i];
        }
    return NULL;
}

static void log_focus(const char *why, const Spot *f)
{
    if (rt_verbose && f)
        rt_log("[pad] focus (%s): %s id %d \"%s\" at %.0f,%.0f %.0fx%.0f of %d\n", why,
               f->kind == SPOT_AREA ? "area" : game_rtti_name(f->obj),
               f->kind == SPOT_AREA ? (int)(f->obj & 0xffff) : (int)g_rd32(f->obj + 0x8c), f->label, f->x, f->y, f->w, f->h,
               n_spots);
}

static void point_at(float gx, float gy)
{
    cur_x = 160.f + gx * 1.2f;
    cur_y = gy * 1.2f;
    mouse(WM_MOUSEMOVE);
}

/* a slider's value (Slider +0x90 mVal, a double 0..1) */
static double slider_val(uint32_t w)
{
    double v;
    memcpy(&v, G2H(w + 0x90), 8);
    return v >= 0 && v <= 1 ? v : 0.5;
}

/* click a member button of the top screen by id (a page arrow, Back), the hidden pointer coming back to
 * the highlight afterwards */
/* pb: the pad button that pressed it (held until it is let go), 0 for a click of its own (a flick) */
/* The main menu's page buttons (MORE, page 2's Back) used as shortcuts: the click goes in whole when the
 * button is let go, the pointer then off the screen, so the sign's hover look never shows (it lit up while
 * the shortcut was held, and as the pages began to slide) */
static uint32_t page_pb;
static float page_x, page_y;
/* while the shortcut is held, the page button shows pressed: its down picture (+0xbc) as its own (+0xb4), put
 * back as it goes in */
static uint32_t page_btn, page_btn_img;
static void page_pressed(uint32_t b)
{
    if (page_btn && page_btn != b) {
        if (valid(page_btn) && page_btn_img) g_wr32(page_btn + 0xb4, page_btn_img);
        page_btn = 0, page_btn_img = 0;
    }
    if (!b || page_btn) return;
    uint32_t dn = g_rd32(b + 0xbc), up = g_rd32(b + 0xb4);
    if (!valid(dn) || !valid(up) || dn == up) return;
    page_btn = b, page_btn_img = up;
    g_wr32(b + 0xb4, dn);
}
static void page_click_update(uint32_t buttons)
{
    if (!page_pb || (buttons & page_pb)) return;
    page_pb = 0;
    page_pressed(0);
    cur_x = 160.f + page_x * 1.2f;
    cur_y = page_y * 1.2f;
    mouse(WM_MOUSEMOVE);
    mouse(WM_LBUTTONDOWN);
    mouse(WM_LBUTTONUP);
    cur_x = cur_y = -400;
    rt_mouse_x = rt_mouse_y = -400;
    rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
}
static int page_click(int id, uint32_t pb)
{
    uint32_t b = screen_obj ? button_with_id(screen_obj, id) : 0;
    if (!b || !widget_live(b) || page_pb) return 0;
    Spot *sp = spot_of(b);
    if (!sp) return 0;
    page_x = sp->x + sp->w / 2, page_y = sp->y + sp->h / 2;
    page_pb = pb ? pb : ~0u;
    if (pb) page_pressed(b);
    if (!pb) page_click_update(0);
    return 1;
}

static int click_button_id(int id, uint32_t pb)
{
    uint32_t b = screen_obj ? button_with_id(screen_obj, id) : 0;
    if (!b || !widget_live(b)) return 0;
    for (int i = 0; i < n_spots; i++)
        if (spots[i].obj == b) {
            float x = spots[i].x + spots[i].w / 2, y = spots[i].y + spots[i].h / 2;
            if (pb) press_hold(pb, x, y);
            else click_game(x, y);
            return 1;
        }
    return 0;
}

static void click_area(uint32_t id, uint32_t pb)
{
    for (int i = 0; i < n_spots; i++)
        if (spots[i].obj == (0xFE000000u | id)) {
            float x = spots[i].x + spots[i].w / 2, y = spots[i].y + spots[i].h / 2;
            if (pb) press_hold(pb, x, y);
            else click_game(x, y);
            return;
        }
}

/* the mini-games / puzzle / survival list: its scroll position (ChallengeScreen +0x3f8, a float in tens of
 * pixels; the game clamps it to the list) glides one row per step in 0.1 s, as on the TV
 * edition; a held direction steps again on the usual repeat, and the list stops on a whole row */
static float scroll_target, prev_scroll;
static int scroll_gliding;

static float list_scroll(void)
{
    float v = 0;
    if (screen_kind == SCR_CHALLENGE && screen_obj) memcpy(&v, G2H(screen_obj + 0x3f8), 4);
    return v;
}

static void list_scroll_set(float v)
{
    if (screen_kind == SCR_CHALLENGE && screen_obj) memcpy(G2H(screen_obj + 0x3f8), &v, 4);
}

static void list_glide(float dt)
{
    static float set_to;
    static int was_gliding;
    float v = list_scroll();
    /* the game clamps the position to its list on its next update: a glide that got nowhere since the last
     * frame has reached the list's end (the target, a whole row on, can lie past it) */
    if (scroll_gliding && was_gliding && fabsf(v - set_to) > 0.001f && fabsf(v - prev_scroll) < 0.001f) {
        scroll_gliding = 0;
        list_scroll_set(v); /* (left past the end, the list reports none of its cards) */
    }
    prev_scroll = v;
    was_gliding = scroll_gliding;
    if (!scroll_gliding) return;
    float d = scroll_target - v, stepv = 120.f * dt; /* 1200 px/s */
    if (fabsf(d) <= stepv) {
        list_scroll_set(scroll_target);
        scroll_gliding = 0;
    } else {
        list_scroll_set(v + (d > 0 ? stepv : -stepv));
    }
    set_to = list_scroll();
}

/* ---- the focus glow ----
 * A soft green halo hugging the focused thing's outline, outside it, pulsing gently (as PvZ Touch's
 * focus glows): for a button with a picture, traced from that picture's own pixels (DDImage +0x38, w x h
 * ARGB at +0x24 / +0x28); for everything else a rounded rectangle of the place's size. Built once per
 * shape on the CPU, kept as an overlay picture. */
#define GLOW_R 9

/* the halo around `mask` (sw x sh, 1 = inside) into out ((sw+2R) x (sh+2R)) */
static void glow_from_mask(const uint8_t *mask, int sw, int sh, uint32_t *out)
{
    int ow = sw + 2 * GLOW_R, oh = sh + 2 * GLOW_R;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int mx = x - GLOW_R, my = y - GLOW_R;
            if (mx >= 0 && mx < sw && my >= 0 && my < sh && mask[my * sw + mx]) {
                out[y * ow + x] = 0;
                continue;
            }
            int best = GLOW_R * GLOW_R + 1;
            for (int dy = -GLOW_R; dy <= GLOW_R && best > 1; dy++) {
                int yy = my + dy;
                if (yy < 0 || yy >= sh) continue;
                for (int dx = -GLOW_R; dx <= GLOW_R; dx++) {
                    int xx = mx + dx, d2 = dx * dx + dy * dy;
                    if (xx < 0 || xx >= sw || d2 >= best) continue;
                    if (mask[yy * sw + xx]) best = d2;
                }
            }
            float d = sqrtf((float)best), a = d > GLOW_R ? 0 : 1.f - (d - 1.f) / (float)GLOW_R;
            if (a > 1) a = 1;
            a = a * a * (3 - 2 * a); /* smooth fall-off */
            out[y * ow + x] = (uint32_t)(a * 255.f) << 24 | 0xffffffu;
        }
}

/* the glow picture for a widget's image (key = the image) or for a w x h rounded rectangle */
static int glow_image(uint32_t img, int w, int h, int *gw, int *gh)
{
    uint32_t key = img ? img : (0x80000000u | (uint32_t)w << 12 | (uint32_t)h);
    int i = ov_dynamic_find(key, gw, gh);
    if (i >= 0) return i;
    int sw = w, sh = h;
    const uint32_t *bits = NULL;
    if (img) {
        sw = (int)g_rd32(img + 0x24);
        sh = (int)g_rd32(img + 0x28);
        uint32_t b = g_rd32(img + 0x38);
        if (sw <= 0 || sh <= 0 || sw > 600 || sh > 600 || !valid(b)) return -1;
        bits = (const uint32_t *)G2H(b);
    }
    if (sw <= 0 || sh <= 0 || sw > 800 || sh > 600) return -1;
    uint8_t *mask = malloc((size_t)sw * (size_t)sh);
    uint32_t *out = malloc(sizeof(uint32_t) * (size_t)(sw + 2 * GLOW_R) * (size_t)(sh + 2 * GLOW_R));
    if (!mask || !out) { free(mask); free(out); return -1; }
    float rad = sw < 60 || sh < 60 ? 6.f : 10.f;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            if (bits) {
                mask[y * sw + x] = (bits[y * sw + x] >> 24) >= 96;
            } else { /* rounded corners */
                float cx = x < rad ? rad - (float)x : x >= sw - rad ? (float)x - ((float)sw - rad - 1) : 0;
                float cy = y < rad ? rad - (float)y : y >= sh - rad ? (float)y - ((float)sh - rad - 1) : 0;
                mask[y * sw + x] = cx * cx + cy * cy <= rad * rad;
            }
        }
    glow_from_mask(mask, sw, sh, out);
    i = ov_dynamic(key, sw + 2 * GLOW_R, sh + 2 * GLOW_R, out);
    free(mask);
    free(out);
    *gw = sw + 2 * GLOW_R;
    *gh = sh + 2 * GLOW_R;
    return i;
}

/* the soft round light Labyrinth 2 and PvZ Touch's Zombatar screen use: a white disc fading linearly
 * to nothing at its rim, tinted and added (64 x 64, made once) */
static int radial_image(void)
{
    int w, h, i = ov_dynamic_find(0x7fff0001u, &w, &h);
    if (i >= 0) return i;
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = ((float)x + .5f - 32.f) / 32.f, dy = ((float)y + .5f - 32.f) / 32.f, r = sqrtf(dx * dx + dy * dy);
            float a = r >= 1 ? 0 : 1 - r;
            px[y * 64 + x] = (uint32_t)(a * 255.f) << 24 | 0xffffffu;
        }
    return ov_dynamic(0x7fff0001u, 64, 64, px);
}

/* Labyrinth 2's / PvZ Touch's warm focus light (lab_draw_glow_at): a warm halo out past the thing and
 * a paler light over it, added, steady: the thing looks lit up, no outline */
static void warm_glow(float x, float y, float w, float h, float k)
{
    int ri = radial_image();
    if (ri < 0) return;
    float cx = x + w / 2, cy = y + h / 2, rx = w / 2 + 12, ry = h / 2 + 12;
    ov_additive(1);
    /* a button with a text label (wide: twice as wide as tall or more) half as lit, its paler inner light (the
     * one over the letters) a quarter: lit up, its label still read through the light */
    int text = w >= 2.f * h;
    uint32_t a1 = (uint32_t)fminf(255.f, 44.f * k * (text ? 0.55f : 1.f)), a2 = (uint32_t)fminf(255.f, 20.f * k * (text ? 0.25f : 1.f));
    ov_image(ri, 0, 0, 64, 64, cx - rx, cy - ry, 2 * rx, 2 * ry, a1 << 24 | 0xffe278u);
    rx = w * 0.5f * 0.85f, ry = h * 0.5f * 0.85f;
    ov_image(ri, 0, 0, 64, 64, cx - rx, cy - ry, 2 * rx, 2 * ry, a2 << 24 | 0xfff0beu);
    ov_additive(0);
}

/* the traced halo around `img` (drawn with its top left at ix, iy), or around a w x h rounded box */
static void traced_glow(uint32_t img, float ix, float iy, float w, float h, uint32_t rgb)
{
    static float t;
    t += 0.016f;
    float breath = 0.5f - 0.5f * cosf(6.2831853f * fmodf(t, 1.5f) / 1.5f); /* PvZ Touch's 1.5 s breath */
    uint32_t a = (uint32_t)(255.f * (0.7f + 0.3f * breath));
    int gw, gh, gi = glow_image(img, img ? 0 : (int)w, img ? 0 : (int)h, &gw, &gh);
    if (gi < 0 && img) gi = glow_image(0, (int)w, (int)h, &gw, &gh);
    if (gi < 0) return;
    ov_additive(1);
    ov_image(gi, 0, 0, gw, gh, ix - GLOW_R, iy - GLOW_R, (float)gw, (float)gh, a << 24 | rgb);
    ov_additive(0);
}

static int image_ok(uint32_t im);
static double slider_val(uint32_t w);
/* a slider: its bar (the track picture's height across the slider) and its knob where it sits, as one
 * shape; made again when the knob moves (one picture per 1/20 step) */
static void slider_glow(const Spot *f, uint32_t rgb)
{
    uint32_t tr = g_rd32(f->obj + 0x9c), th = g_rd32(f->obj + 0xa0);
    if (!image_ok(th)) return;
    int sw = (int)f->w, sh = (int)f->h, kw = (int)g_rd32(th + 0x24), kh = (int)g_rd32(th + 0x28);
    int bh = image_ok(tr) ? (int)g_rd32(tr + 0x28) : 8;
    if (bh > sh) bh = sh;
    int step = (int)(slider_val(f->obj) * 20.0 + 0.5);
    int kx = (int)((float)step / 20.f * (float)(sw - kw)), ky = (sh - kh) / 2;
    uint32_t key = 0x40000000u | (f->obj & 0xffffff) << 5 | (uint32_t)step;
    int gw, gh, gi = ov_dynamic_find(key, &gw, &gh);
    if (gi < 0) {
        uint8_t *mask = calloc((size_t)sw * (size_t)sh, 1);
        uint32_t *out = malloc(sizeof(uint32_t) * (size_t)(sw + 2 * GLOW_R) * (size_t)(sh + 2 * GLOW_R));
        if (!mask || !out) { free(mask); free(out); return; }
        int by = (sh - bh) / 2;
        for (int y = by; y < by + bh; y++)
            for (int x = 2; x < sw - 2; x++) mask[y * sw + x] = 1;
        const uint32_t *kb = (const uint32_t *)G2H(g_rd32(th + 0x38));
        for (int y = 0; y < kh; y++)
            for (int x = 0; x < kw; x++) {
                int mx = kx + x, my = ky + y;
                if (mx >= 0 && mx < sw && my >= 0 && my < sh && (kb[y * kw + x] >> 24) >= 96) mask[my * sw + mx] = 1;
            }
        glow_from_mask(mask, sw, sh, out);
        gi = ov_dynamic(key, sw + 2 * GLOW_R, sh + 2 * GLOW_R, out);
        free(mask);
        free(out);
        gw = sw + 2 * GLOW_R, gh = sh + 2 * GLOW_R;
        if (gi < 0) return;
    }
    static float t;
    t += 0.016f;
    float breath = 0.5f - 0.5f * cosf(6.2831853f * fmodf(t, 1.5f) / 1.5f);
    uint32_t a = (uint32_t)(255.f * (0.7f + 0.3f * breath));
    ov_additive(1);
    ov_image(gi, 0, 0, gw, gh, f->x - GLOW_R, f->y - GLOW_R, (float)gw, (float)gh, a << 24 | rgb);
    ov_additive(0);
}

static int image_ok(uint32_t im)
{
    return valid(im) && game_rtti_is(im, "Image@Sexy") && valid(g_rd32(im + 0x38)) && (int)g_rd32(im + 0x24) > 0 &&
           (int)g_rd32(im + 0x28) > 0;
}

/* The focus look, per place (after PvZ Touch and the console editions):
 *  - the tombstone's signs and the second page's Challenges stone: the console's purple, traced
 *  - the stone dialogs (options, pause, quit, name...): PvZ Touch's green, traced around the stone
 *    button, the picture button, the checkbox's box or the slider's knob
 *  - lists, the Shop, the Almanac, the mail, the jukebox: Labyrinth 2's warm light
 *  - the title's link and the main menu's other things: none (the game's own hover look) */
/* The main menu's stone signs light their lettering when focused, as on the consoles (the Xbox 360 / PS3
 * menu's white ADVENTURE): from the sign's own picture (DDImage +0x38, w x h ARGB at +0x24 / +0x28), its dark
 * letters (dark, solid, and more than a few pixels inside the stone, so not its outline) as a white picture of
 * their own, drawn over the sign. Built once per sign. */
/* chamfer distance (1 / 1.414) to the nearest seed pixel (seed[k] != 0); outside the picture counts as a seed
 * when edge_seed */
static void chamfer(const uint8_t *seed, int w, int h, int edge_seed, float *d)
{
    const float INF = 1e9f;
    for (int k = 0; k < w * h; k++) d[k] = seed[k] ? 0 : INF;
    for (int pass = 0; pass < 2; pass++)
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
                int y = pass ? h - 1 - yy : yy, x = pass ? w - 1 - xx : xx, k = y * w + x, s = pass ? 1 : -1;
                const int nb[4][2] = {{s, 0}, {0, s}, {s, s}, {-s, s}};
                const float cost[4] = {1, 1, 1.414f, 1.414f};
                for (int j = 0; j < 4; j++) {
                    int nx = x + nb[j][0], ny = y + nb[j][1];
                    float v = (nx >= 0 && nx < w && ny >= 0 && ny < h) ? d[ny * w + nx] + cost[j] : (edge_seed ? cost[j] : INF);
                    if (v < d[k]) d[k] = v;
                }
            }
}

/* A main-menu sign made into its "selected" look, as PvZ Touch's console pictures are (measured from them by its
 * port's session): the letters' dark ink replaced by the stone under it, then a cool lilac-white fill on the
 * ink's core (0.40-0.48 of the ink's darkness, 0.55-0.63 on the small signs); a pink-lilac rim inside the stone's outer edge (its
 * narrow cracks closed first): the big signs (rimk 1) (255,140,248) 0.82 e^-0.27d to 8 px, the small ones (230,125,255)
 * 0.32 e^-0.55d to 6 px plus a faint lilac wash; a 1 px soft
 * fringe outside; the stone otherwise unchanged. Only the letters are lit (each sign's, picked one by one into
 * port_art/sign_WxH.png); the LEVEL plaque is left as drawn (the game draws the level over it). The picture is
 * 1 px larger each side. */
static struct { uint32_t key; int w, h; uint32_t *px; } lit_cache[8];
static const uint32_t *lettering_image(uint32_t img, float rimk, int *gw, int *gh)
{
    uint32_t key = img + (rimk < 1 ? 2 : 0);
    for (int k = 0; k < 8; k++)
        if (lit_cache[k].px && lit_cache[k].key == key) {
            *gw = lit_cache[k].w, *gh = lit_cache[k].h;
            return lit_cache[k].px;
        }
    int sw = (int)g_rd32(img + 0x24), sh = (int)g_rd32(img + 0x28);
    uint32_t bits = g_rd32(img + 0x38);
    if (sw <= 0 || sh <= 0 || sw > 600 || sh > 600 || !valid(bits)) return NULL;
    int w = sw + 2, h = sh + 2, n = w * h;
    /* made ahead (making one took about a second on the Switch, a stutter at the sign's first focus or the page
     * turn): port_art/signlit_WxH_K.png, K 1 the big signs, 0 the small (written by the Mac host with
     * RT_SIGN_BAKE=1, tools/bake_sign_lit.py) */
    if (!getenv("RT_SIGN_BAKE")) {
        char file[40];
        snprintf(file, sizeof file, "signlit_%dx%d_%d.png", sw, sh, rimk >= 1);
        int bw = 0, bh = 0;
        uint8_t *px = ov_art_rgba(file, &bw, &bh);
        if (px && bw == w && bh == h) {
            uint32_t *argb = malloc(sizeof(uint32_t) * (size_t)n);
            if (argb) {
                for (int k = 0; k < n; k++)
                    argb[k] = (uint32_t)px[4 * k + 3] << 24 | (uint32_t)px[4 * k] << 16 | (uint32_t)px[4 * k + 1] << 8 | px[4 * k + 2];
                static int next_b;
                int k = next_b++ % 8;
                free(lit_cache[k].px);
                lit_cache[k].key = key, lit_cache[k].w = w, lit_cache[k].h = h, lit_cache[k].px = argb;
                free(px);
                *gw = w, *gh = h;
                return argb;
            }
        }
        free(px);
    }
    const uint32_t *src = (const uint32_t *)G2H(bits);
    if (getenv("RT_SIGN_DUMP")) { /* (the picture, for tools/make_sign_letters.py) */
        char p[600];
        snprintf(p, sizeof p, "%s/shots/sign_%dx%d.argb", rt_game_dir, sw, sh);
        FILE *df = fopen(p, "wb");
        if (df) fwrite(src, 4, (size_t)sw * sh, df), fclose(df);
    }
    uint32_t *N = calloc((size_t)n, 4), *out = calloc((size_t)n, 4);
    float *L = calloc((size_t)n, sizeof(float)), *din = malloc(sizeof(float) * (size_t)n), *dout = malloc(sizeof(float) * (size_t)n),
          *cov = calloc((size_t)n, sizeof(float)), *raw = calloc((size_t)n, sizeof(float)), *f = calloc((size_t)n, sizeof(float));
    uint8_t *solid = calloc((size_t)n, 1), *notsolid = calloc((size_t)n, 1), *seen = calloc((size_t)n, 1), *clear = calloc((size_t)n, 1);
    if (!N || !out || !L || !din || !dout || !cov || !raw || !f || !solid || !notsolid || !seen || !clear) goto fail;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) N[(y + 1) * w + x + 1] = src[y * sw + x];
    for (int k = 0; k < n; k++) {
        uint32_t c = N[k];
        L[k] = (float)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10.f;
        solid[k] = (c >> 24) >= 128;
        notsolid[k] = !solid[k];
    }
    /* the depth into the stone: for the ink, from its real outline (cracks included); for the rim, from its outline
     * with narrow cracks closed (grown 4 px and shrunk back), so the rim follows only the outside */
    float *drim = malloc(sizeof(float) * (size_t)n);
    if (!drim) goto fail;
    chamfer(notsolid, w, h, 1, din);
    chamfer(solid, w, h, 0, dout);
    for (int k = 0; k < n; k++) seen[k] = dout[k] > 4.f; /* (outside the grown stone) */
    chamfer(seen, w, h, 1, drim);
    for (int k = 0; k < n; k++) seen[k] = drim[k] <= 4.f; /* (outside the closed stone) */
    chamfer(seen, w, h, 1, drim);
    for (int k = 0; k < n; k++) seen[k] = 0;

    /* the stone's brightness (median of the inside's lighter part) and the ink's (its darkest) */
    {
        static float vals[360000];
        int cnt = 0;
        float ink = 255;
        for (int k = 0; k < n; k++)
            if (solid[k] && din[k] > 5) {
                if (L[k] > 70 && cnt < 360000) vals[cnt++] = L[k];
                if (L[k] < ink) ink = L[k];
            }
        float stone = 110;
        if (cnt) { /* (a median by counting: brightness is 0..255) */
            int hist[256] = {0}, half = cnt / 2, acc = 0;
            for (int j = 0; j < cnt; j++) hist[(int)vals[j]]++;
            for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc > half) { stone = (float)v; break; } }
        }
        for (int k = 0; k < n; k++)
            if (solid[k] && din[k] > 5) {
                float v = (stone - 12.f - L[k]) / (stone - 12.f - ink);
                cov[k] = v <= 0 ? 0 : fminf(1.f, v * 1.6f);
                raw[k] = fmaxf(0.f, fminf(1.f, v));
            }
    }
    /* only the letters, picked one by one for each sign (tools/make_sign_letters.py, port_art/sign_WxH.png: red
     * = a letter's pixels, green = the LEVEL plaque, left see-through so the game's own shows) */
    {
        char file[32];
        snprintf(file, sizeof file, "sign_%dx%d.png", sw, sh);
        int mw = 0, mh = 0;
        uint8_t *m = ov_art_rgba(file, &mw, &mh);
        if (m && (mw != sw || mh != sh)) free(m), m = NULL;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                int k = (y + 1) * w + x + 1;
                const uint8_t *mp = m ? m + 4 * (y * sw + x) : NULL;
                if (!mp || mp[0] < 128) cov[k] = 0;
                if (mp && mp[1] >= 128) clear[k] = 1, cov[k] = 0;
            }
        free(m);
    }
    /* the lit fill: the ink's core, f = clamp((v - t) / 0.08), t 0.40 on the big signs and 0.55 on the small ones
     * (PvZ Touch's weight: lit / ink about 0.95), crisp (the 1.2x display softens it a little) */
    const float t0 = rimk >= 1 ? 0.40f : 0.55f;
    for (int k = 0; k < n; k++) f[k] = cov[k] == 0 ? 0 : fmaxf(0.f, fminf(1.f, (raw[k] - t0) / 0.08f));
    /* the plain stone around each pixel (mean of the stone that's neither ink nor near a letter, 15 x 15), from
     * summed tables */
    double *sum = calloc((size_t)(w + 1) * (h + 1) * 4, sizeof(double));
    if (!sum) goto fail;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int k = y * w + x;
            double v[4] = {0, 0, 0, 0};
            if (solid[k] && din[k] > 3 && cov[k] == 0) {
                int near = 0;
                for (int d = 1; d <= 4 && !near; d++)
                    near = (y >= d && cov[k - d * w] > 0) || (x + d < w && cov[k + d] > 0) || (y + d < h && cov[k + d * w] > 0) ||
                           (x >= d && cov[k - d] > 0);
                if (!near) {
                    uint32_t c = N[k];
                    v[0] = (c >> 16) & 255, v[1] = (c >> 8) & 255, v[2] = c & 255, v[3] = 1;
                }
            }
            for (int j = 0; j < 4; j++) {
                size_t o = ((size_t)(y + 1) * (w + 1) + x + 1) * 4 + j;
                sum[o] = v[j] + sum[o - 4] + sum[o - (size_t)(w + 1) * 4] - sum[o - (size_t)(w + 1) * 4 - 4];
            }
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int k = y * w + x;
            uint32_t c = N[k];
            float r = (float)((c >> 16) & 255), g = (float)((c >> 8) & 255), bl = (float)(c & 255);
            uint32_t a = c >> 24;
            if (cov[k] > 0) {
                /* the ink becomes the stone around it (the mean of non-ink stone within 7 px), then the fill over it */
                float ur = 0, ug = 0, ub = 0;
                int cnt = 0;
                for (int yy = y - 9; yy <= y + 9; yy++)
                    for (int xx = x - 9; xx <= x + 9; xx++) {
                        if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
                        int j = yy * w + xx;
                        if (solid[j] && din[j] > 3 && cov[j] == 0) {
                            uint32_t cc = N[j];
                            ur += (float)((cc >> 16) & 255), ug += (float)((cc >> 8) & 255), ub += (float)(cc & 255), cnt++;
                        }
                    }
                if (cnt) ur /= (float)cnt, ug /= (float)cnt, ub /= (float)cnt;
                else ur = ug = ub = 110;
                r += (ur - r) * cov[k], g += (ug - g) * cov[k], bl += (ub - bl) * cov[k];
                /* sunk into the stone: the cut's shadow inside the letter along its top and left edges (stone 1, 2, 3 px
                 * above or to the left: x0.62, 0.78, 0.90), its lit bottom and right edges left bright */
                float fill = 1.f;
                if (f[k] >= 0.5f) {
                    static const float shade[4] = {1.f, 0.62f, 0.78f, 0.90f};
                    for (int d = 1; d <= 3; d++)
                        if ((y >= d && f[k - d * w] < 0.5f) || (x >= d && f[k - d] < 0.5f)) {
                            fill = shade[d];
                            break;
                        }
                }
                r += (222.f * fill - r) * f[k], g += (214.f * fill - g) * f[k], bl += (228.f * fill - bl) * f[k];
            }
            /* the stone round a letter: the sign's own carving shading below and left of it (a bright band below, a
             * shadow at the left) goes back to plain stone, so nothing outside reads as a drop shadow (d = how far
             * the letter is, 1-4 px) */
            if (solid[k] && f[k] < 0.5f) {
#define FV(xx, yy) (((xx) >= 0 && (xx) < w && (yy) >= 0 && (yy) < h) ? f[(yy) * w + (xx)] : 0.f)
                int du = 0, dd = 0, dl = 0, dr = 0; /* the letter's distance up, down, left, right (0: none) */
                for (int d = 4; d >= 1; d--) {
                    if (FV(x, y - d) >= 0.5f) du = d;
                    if (FV(x, y + d) >= 0.5f) dd = d;
                    if (FV(x - d, y) >= 0.5f) dl = d;
                    if (FV(x + d, y) >= 0.5f) dr = d;
                }
#undef FV
                if (du || (dr && dr <= 3)) {
                    int x0 = x - 7 < 0 ? 0 : x - 7, y0 = y - 7 < 0 ? 0 : y - 7, x1 = x + 8 > w ? w : x + 8, y1 = y + 8 > h ? h : y + 8;
                    double s4[4];
                    for (int j = 0; j < 4; j++)
                        s4[j] = sum[((size_t)y1 * (w + 1) + x1) * 4 + j] - sum[((size_t)y0 * (w + 1) + x1) * 4 + j] -
                                sum[((size_t)y1 * (w + 1) + x0) * 4 + j] + sum[((size_t)y0 * (w + 1) + x0) * 4 + j];
                    /* (only pixels far from the stone, more than 15 brightness: the band below lighter, the shadow
                     * at the left darker; their difference is cut to 6 on the small signs, so the stone's grain and
                     * cracks stay, and to 0 on the big ones, whose letters have no light below and a light band at
                     * the left) */
                    if (s4[3] > 0) {
                        float mr = (float)(s4[0] / s4[3]), mg = (float)(s4[1] / s4[3]), mb = (float)(s4[2] / s4[3]);
                        float dlum = (r * 3 + g * 6 + bl - (mr * 3 + mg * 6 + mb)) / 10.f;
                        if ((du && dlum > 15.f) || (dr && dr <= 3 && dlum < -15.f)) {
                            float keep = rimk >= 1 ? 0.f : 6.f / fabsf(dlum); /* (big signs: none kept) */
                            r = mr + (r - mr) * keep, g = mg + (g - mg) * keep, bl = mb + (bl - mb) * keep;
                        }
                    }
                }
                /* the stone round the cut: a thin light edge where the cut's top and left walls meet the surface (the
                 * light catches them), nothing below or right (a shadow there reads as a raised letter) */
                float m = 1.f + (dd == 1 ? 0.10f : 0.f) + (dr == 1 ? 0.06f : 0.f);
                r = fminf(255.f, r * m), g = fminf(255.f, g * m), bl = fminf(255.f, bl * m);
            }
            /* the small signs' lilac wash: the rim's long tail, (20,8,20) to 9 px in, down to (5,2,5) by 15 */
            if (solid[k] && rimk < 1) {
                float ww = drim[k] <= 9.f ? 1.f : drim[k] >= 15.f ? 0.25f : 1.f - 0.75f * (drim[k] - 9.f) / 6.f;
                r = fminf(255.f, r + 20 * ww), g = fminf(255.f, g + 8 * ww), bl = fminf(255.f, bl + 20 * ww);
            }
            if (solid[k] && rimk >= 1 && drim[k] <= 8.f) { /* the big sign: (255,140,248) 0.82 e^-0.27d, out by 6-8 */
                float kk = 0.82f * expf(-0.27f * drim[k]);
                if (drim[k] > 6.f) kk *= (8.f - drim[k]) / 2.f;
                r = fminf(255.f, r + 255.f * kk), g = fminf(255.f, g + 140.f * kk), bl = fminf(255.f, bl + 248.f * kk);
            }
            if (solid[k] && rimk < 1 && drim[k] <= 6) { /* the small ones: (230,125,255) 0.32 e^-0.55d, to 6 px */
                float kk = 0.32f * expf(-0.55f * drim[k]);
                r = fminf(255.f, r + 230.f * kk), g = fminf(255.f, g + 125.f * kk), bl = fminf(255.f, bl + 255.f * kk);
            }
            if (!solid[k] && dout[k] <= 1.5f) {
                r = 110, g = 95, bl = 150;
                uint32_t fa = dout[k] <= 1 ? 64 : 24;
                if (fa > a) a = fa;
            }
            out[k] = clear[k] ? N[k] : a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl; /* (the plaque: as drawn) */
        }
    {
        static int next;
        int k = next++ % 8;
        free(lit_cache[k].px);
        lit_cache[k].key = key, lit_cache[k].w = w, lit_cache[k].h = h, lit_cache[k].px = out;
    }
    if (getenv("RT_SIGN_BAKE")) {
        char p[600];
        snprintf(p, sizeof p, "%s/shots/signlit_%dx%d_%d.argb", rt_game_dir, sw, sh, rimk >= 1);
        FILE *df = fopen(p, "wb");
        if (df) fwrite(out, 4, (size_t)n, df), fclose(df);
    }
    *gw = w, *gh = h;
    free(drim), free(sum);
    free(N), free(L), free(din), free(dout), free(cov), free(raw), free(f), free(solid), free(notsolid), free(seen),
        free(clear);
    return out;
fail:
    free(N), free(out), free(L), free(din), free(dout), free(cov), free(raw), free(f), free(solid), free(notsolid), free(seen),
        free(clear);
    return NULL;
}

/* PvZ Touch's focused stone (GameButton::DrawButtonImage 0x1a2f10 in libGameMain.so): the picture drawn again,
 * additive, white, at A = min(0.25, 0.2 (1 + 0.5 sin(0.04 t) + bounce)), t in 1/100 s from 0.15 s after the
 * focus came (the phase starts there), bounce = 1 - |1 - 2 age / 15| over the first 15 (age in 1/100 s); the
 * sign is drawn 1 + 0.04 bounce times its size about its centre (a pop, once per focus).
 * PvZ Touch's own "selected" pictures (lighter stone, off-white letters inside a thin dark outline) have no match
 * among the DLC's signs: the DLC's sign made into one (lettering_image).
 * It's drawn in the sign's own place, by its button: each sign is a button whose picture (+0xb4) the game draws
 * among the menu's widgets, so the vases of Options / Help / Quit (buttons added later) stay in front of it. The
 * button's hover picture (+0xb8, the same size, shown on the PC when the mouse is over it) gets the lit sign with
 * the pulse in its pixels (the additive pass is c (1 + A) on the picture's own colours), +1,+1 while A holds it
 * down, each frame (MemoryImage +0x38 bits, +0x3c mBitsChangedCount: the game uploads it again), and the button
 * shows it as its picture while focused. The pop is the button's own draw scaled (DialogButton::Draw 0x886240,
 * hooked at its start and its one exit 0x8865e0: Graphics +0x10 / +0x14 mScaleX / Y, +0x18 / +0x1c the scale's
 * origin), as PvZ Touch's GameButton::Draw does: in the sign's own layer, the vases still in front. */
static uint32_t lit_obj, lit_btn, lit_orig, lit_over;
static float lit_scale = 1.f;
static uint64_t lit_since, lit_last;
static int lit_frame;
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
/* w is still in the game's widget tree (from the WidgetManager down) */
static int widget_in_tree_from(uint32_t wc, uint32_t target, int depth)
{
    uint32_t head = g_rd32(wc + 8), n = g_rd32(wc + 0xc);
    if (!valid(head) || n > 200) return 0;
    uint32_t node = g_rd32(head);
    for (uint32_t i = 0; i < n && valid(node) && node != head; i++, node = g_rd32(node)) {
        uint32_t w = g_rd32(node + 8);
        if (!valid(w)) continue;
        if (w == target || (depth < 6 && widget_in_tree_from(w, target, depth + 1))) return 1;
    }
    return 0;
}
static int widget_in_tree(uint32_t target)
{
    uint32_t app = rt_app_object(), wm = valid(app) ? g_rd32(app + 0x320) : 0;
    return valid(wm) && widget_in_tree_from(wm, target, 0);
}
/* the sign that was lit gets its own picture back (if its button is still there) */
static void sign_unlit(void)
{
    if (!lit_btn) return;
    if (widget_in_tree(lit_btn) && g_rd32(lit_btn + 0xb4) == lit_over) g_wr32(lit_btn + 0xb4, lit_orig);
    lit_btn = 0;
    lit_scale = 1.f;
}
/* DialogButton::Draw starts (begin) or ends for button btn, drawing with Graphics g: the lit sign's pop */
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float lit_origin[2]; /* the lit sign's pop centre (screen coordinates), from its last draw */
/* GameSelector's overlay draws Adventure's level digits on the sign's "LEVEL -" plate (IMAGE_SELECTORSCREEN_
 * LEVELNUMBERS, three TodDrawImageCelF calls, cdecl: g, image, x, y, cel col, row) at the selector reanim's track,
 * out of reach of the sign's pop. While the sign is lit they are drawn here as the sign is, so they pop and light
 * with "LEVEL -": Graphics::DrawImage(image, x, y, cel rect) with the Graphics' scale set about the sign's centre
 * (the sign's own scale, pad_button_draw; this draw honours it, TodDrawImageCelF doesn't; TodDrawImageCelScaledF's
 * matrix blit did scale on the Mac but not on the Switch), then again additively at the sign's light (c + light c
 * = sign_lit's c (1 + light)). 1: drawn here (the call skipped) */
#define GRAPHICS_DRAW_IMAGE_SRC 0xa050f0u /* Graphics::DrawImage(Image *, int x, int y, const Rect &src) */
static float lit_light; /* the lit sign's light this frame (sign_lit: its picture's colours times 1 + this) */
int pad_level_digit_draw(uint32_t args)
{
    uint32_t g = g_rd32(args), img = g_rd32(args + 4);
    int lit = lit_btn && g_rd32(lit_btn + 0x8c) == 100 && now_ms() - lit_last < 120;
    RtThread *t = rt_thread_self();
    if (!valid(g) || !valid(img) || !lit || !t) return 0; /* (no image_ok: the digits' picture keeps no pixels in memory) */
    int cols = (int)g_rd32(img + 0x30), rows = (int)g_rd32(img + 0x2c), col = (int)g_rd32(args + 16), row = (int)g_rd32(args + 20);
    if (cols <= 0 || rows <= 0) return 0;
    int cw = (int)g_rd32(img + 0x24) / cols, ch = (int)g_rd32(img + 0x28) / rows;
    float x, y, k = lit_scale;
    memcpy(&x, G2H(args + 8), 4);
    memcpy(&y, G2H(args + 12), 4);
    if (k != 1.f && lit_origin[0] == 0.f) return 0; /* (the sign not drawn popping yet: no centre) */
    /* the rect under the stack (the calls use the stack below it) */
    uint32_t esp = t->cpu.esp, r = (esp - 0x600) & ~15u, rect[4] = {(uint32_t)(col * cw), (uint32_t)(row * ch), (uint32_t)cw, (uint32_t)ch};
    memcpy(G2H(r), rect, sizeof rect);
    uint32_t saved[4], col4[4], mode = g_rd32(g + 0x44);
    uint8_t cz = RD8_G(g + 0x48);
    for (int i = 0; i < 4; i++) saved[i] = g_rd32(g + 0x10 + 4 * (uint32_t)i), col4[i] = g_rd32(g + 0x30 + 4 * (uint32_t)i);
    float v[4] = {k, k, lit_origin[0], lit_origin[1]};
    memcpy(G2H(g + 0x10), v, sizeof v);
    uint32_t a[4] = {img, (uint32_t)(int)lroundf(x), (uint32_t)(int)lroundf(y), r};
    t->cpu.esp = r;
    guest_this(GAME_FN(GRAPHICS_DRAW_IMAGE_SRC), g, 4, a);
    if (lit_light > 0.f) { /* Graphics +0x44 draw mode (1 additive), +0x48 colourize, +0x30 colour (r, g, b, a) */
        g_wr32(g + 0x44, 1);
        WR8_G(g + 0x48, 1);
        g_wr32(g + 0x30, 255), g_wr32(g + 0x34, 255), g_wr32(g + 0x38, 255);
        g_wr32(g + 0x3c, (uint32_t)(int)(lit_light * 255.f + 0.5f));
        guest_this(GAME_FN(GRAPHICS_DRAW_IMAGE_SRC), g, 4, a);
    }
    t->cpu.esp = esp;
    g_wr32(g + 0x44, mode);
    WR8_G(g + 0x48, cz);
    for (int i = 0; i < 4; i++) g_wr32(g + 0x10 + 4 * (uint32_t)i, saved[i]), g_wr32(g + 0x30 + 4 * (uint32_t)i, col4[i]);
    static int told;
    if (!told++) rt_log("[pad] Adventure's level digits drawn lit (x%.3f about %.0f,%.0f, digit at %.0f,%.0f, cel %dx%d, light %.2f)\n", k,
                        lit_origin[0], lit_origin[1], x, y, cw, ch, lit_light);
    return 1;
}
void pad_button_draw(uint32_t btn, uint32_t g, int begin)
{
    static uint32_t saved[4], on;
    if (begin) {
        if (on || !btn || btn != lit_btn || lit_scale == 1.f || !valid(g)) return;
        uint32_t im = g_rd32(btn + 0xb4);
        if (!image_ok(im)) return;
        float tx, ty;
        memcpy(&tx, G2H(g + 8), 4); /* Graphics mTransX / mTransY */
        memcpy(&ty, G2H(g + 12), 4);
        float v[4] = {lit_scale, lit_scale, tx + (float)g_rd32(im + 0x24) / 2.f, ty + (float)g_rd32(im + 0x28) / 2.f};
        lit_origin[0] = v[2], lit_origin[1] = v[3];
        for (int i = 0; i < 4; i++) saved[i] = g_rd32(g + 0x10 + 4 * (uint32_t)i);
        memcpy(G2H(g + 0x10), v, sizeof v);
        on = btn;
    } else if (on && on == btn) {
        for (int i = 0; i < 4; i++) g_wr32(g + 0x10 + 4 * (uint32_t)i, saved[i]);
        on = 0;
    }
}
static void sign_lit(uint32_t obj, uint32_t img, float rimk)
{
    uint64_t now = now_ms();
    if (obj != lit_obj || now - lit_last > 250) lit_obj = obj, lit_since = now; /* (focus came, or came back) */
    lit_last = now;
    lit_frame = 1;
    static float slow = -1; /* (Mac test: RT_POP_SLOW=n plays the pop n times slower) */
    if (slow < 0) slow = getenv("RT_POP_SLOW") ? (float)atof(getenv("RT_POP_SLOW")) : 1.f;
    float age = (float)(now - lit_since) / 10.f / slow, t = age > 15 ? age - 15 : 0;
    float bounce = age <= 15 ? 1.f - fabsf(1.f - 2.f * age / 15.f) : 0;
    float a = 0.2f * (1.f + 0.5f * sinf(0.04f * t) + bounce);
    if (a > 0.25f) a = 0.25f;
    if (obj != lit_btn) {
        sign_unlit();
        uint32_t over = g_rd32(obj + 0xb8), cur = g_rd32(obj + 0xb4);
        if (!image_ok(over) || over == cur || g_rd32(over + 0x24) != g_rd32(img + 0x24) || g_rd32(over + 0x28) != g_rd32(img + 0x28) ||
            !valid(g_rd32(over + 0x38))) {
            static uint32_t told;
            if (told != obj) told = obj, rt_log("[pad] sign %08x: no hover picture to light (%08x, %08x)\n", obj, over, cur);
            return;
        }
        lit_btn = obj, lit_orig = cur, lit_over = over;
    }
    if (img == lit_over) img = lit_orig; /* (Challenges' stone is read from its button) */
    int lw, lh;
    const uint32_t *lit = lettering_image(img, rimk, &lw, &lh);
    if (!lit) return;
    int sw = lw - 2, sh = lh - 2, dx = 1, dy = 1; /* (the lit picture is 1 px larger each side) */
    if (mouse_buttons & 1) dx = 0, dy = 0;        /* (pressed: +1,+1) */
    uint32_t *dst = (uint32_t *)G2H(g_rd32(lit_over + 0x38));
    int mul = (int)((1.f + a) * 256.f);
    lit_light = obj == lit_btn ? a : 0.f;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            uint32_t c = lit[(y + dy) * lw + x + dx];
            uint32_t r = ((c >> 16) & 255) * (uint32_t)mul >> 8, g = ((c >> 8) & 255) * (uint32_t)mul >> 8, b = (c & 255) * (uint32_t)mul >> 8;
            dst[y * sw + x] = (c & 0xff000000u) | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    g_wr32(lit_over + 0x3c, g_rd32(lit_over + 0x3c) + 1);
    g_wr32(obj + 0xb4, lit_over);
    static int pop_test = -1; /* (Mac test: RT_POP_TEST holds the pop at its fullest) */
    if (pop_test < 0) pop_test = getenv("RT_POP_TEST") != NULL;
    lit_scale = 1.f + 0.04f * (pop_test ? 1.f : bounce);
}

/* The first page's wood signs (Multiplayer, id 111, and "If this is not you, click here.", id 106): the game
 * shows their hover pictures (IMAGE_REANIM_SELECTORSCREEN_WOODSIGN3_PRESS 0x10942d0, WOODSIGN2_PRESS 0x10942c8)
 * while focused, and PvZ Touch's pulse is added over them (the picture again, additive, at the signs' A). The
 * pictures sit at the button less its TrackButton offset (woodsign2: 24, 10). */
static void wood_lit(uint32_t obj, uint32_t img, float x, float y)
{
    uint64_t now = now_ms();
    if (obj != lit_obj || now - lit_last > 250) lit_obj = obj, lit_since = now;
    lit_last = now;
    static float slow = -1; /* (Mac test: RT_POP_SLOW=n plays the pop n times slower) */
    if (slow < 0) slow = getenv("RT_POP_SLOW") ? (float)atof(getenv("RT_POP_SLOW")) : 1.f;
    float age = (float)(now - lit_since) / 10.f / slow, t = age > 15 ? age - 15 : 0;
    float bounce = age <= 15 ? 1.f - fabsf(1.f - 2.f * age / 15.f) : 0;
    float a = 0.2f * (1.f + 0.5f * sinf(0.04f * t) + bounce);
    if (a > 0.25f) a = 0.25f;
    int w = (int)g_rd32(img + 0x24), h = (int)g_rd32(img + 0x28);
    int i = ov_dynamic_find(img + 9, &w, &h);
    uint32_t bits = g_rd32(img + 0x38);
    if (i < 0 && w > 0 && h > 0 && w <= 600 && h <= 600 && valid(bits)) i = ov_dynamic(img + 9, w, h, (const uint32_t *)G2H(bits));
    if (i < 0) return;
    ov_additive(1);
    ov_image(i, 0, 0, w, h, x, y, (float)w, (float)h, (uint32_t)(a * 255.f) << 24 | 0xffffffu);
    ov_additive(0);
}

/* the Almanac's plant / zombie cards (areas 0x340.. page 1, 0x380.. page 2: add_spots) */
static int almanac_card(const Spot *f)
{
    if (screen_kind != SCR_ALMANAC || !f || f->kind != SPOT_AREA) return 0;
    uint32_t id = f->obj & 0xffffu;
    return id >= 0x340 && id < 0x3c0;
}
static Spot almanac_focus; /* the card the bracket goes on (w 0: none) */
static uint32_t p2_orig[3]; /* IMAGE_P2_1..3 as the game loaded them (pad_input_update points them at P1's
                             * outside versus) */
/* AlmanacDialog::Draw, before its Close button (0x7d7db4, g = its Graphics): the chooser's bracket (IMAGE_P1_1,
 * as SeedChooserScreen::Draw puts it on a 50 x 70 card: centred, 8 / 15) fitted to the card: the plants' a
 * little smaller; the zombies' square and blue (IMAGE_P2_1, the user's idea) */
void pad_almanac_drawn(uint32_t g)
{
    const Spot *f = &almanac_focus;
    /* the zombies' page: player 2's blue bracket (board_selector_BLUE), the plants' P1's yellow */
    int zombies = f->w > 0 && (f->obj & 0xffffu) >= 0x380;
    uint32_t img = zombies && valid(p2_orig[0]) ? p2_orig[0] : g_rd32(0x1094180u);
    if (screen_kind != SCR_ALMANAC || touch_mode || almanac_focus.w <= 0 || !valid(g) || !valid(img)) return;
    float iw = (float)g_rd32(img + 0x24), ih = (float)g_rd32(img + 0x28);
    float sx = 8.f / 15.f * f->w / 50.f, sy = 8.f / 15.f * f->h / 70.f;
    uint32_t args[6] = {g, img, f2u(f->x + f->w / 2 - iw * sx / 2), f2u(f->y + f->h / 2 - ih * sy / 2), f2u(sx), f2u(sy)};
    guest_this(GAME_FN(0x9c9640u), 0, 6, args); /* TodDrawImageScaledF */
}
static void draw_focus(const Spot *f)
{
    almanac_focus.w = 0;
    if (f && almanac_card(f) && !touch_mode) {
        almanac_focus = *f;
        return;
    }
    if (!f || touch_mode || !rt_gpu) return;
    int id = f->kind == SPOT_AREA ? -1 : (int)g_rd32(f->obj + 0x8c);
    if (f->kind != SPOT_AREA && game_rtti_is(f->obj, "HyperlinkWidget@Sexy")) return;
    /* the jukebox's album now playing (disabled: the game shows it no hover look): the soft light round it */
    if (f->kind == SPOT_BUTTON && id >= 30004 && id <= 30007 && !widget_live(f->obj)) {
        warm_glow(f->x, f->y, f->w, f->h, 4.f); /* (stronger: on the panel's dark blue the usual light hardly shows) */
        return;
    }
    /* (the Shop and the Almanac are dialogs too, but not stone ones) */
    if (top_dialog() && screen_kind != SCR_STORE && screen_kind != SCR_ALMANAC && screen_kind != SCR_MAIL) {
        const uint32_t green = 0x64ff64u;
        if (f->kind == SPOT_SLIDER) {
            slider_glow(f, green);
            return;
        }
        if (f->kind == SPOT_CHECK) {
            uint32_t im = g_rd32(f->obj + 0x98);
            if (image_ok(im)) traced_glow(im, f->x, f->y, 0, 0, green);
            return;
        }
        if (f->kind == SPOT_BUTTON && game_rtti_is(f->obj, "LawnStoneButton")) {
            /* the stone is 6 px short of the button, and only as wide as DrawStoneButton draws it: its left
             * and right ends (IMAGE_BUTTON_LEFT / RIGHT) with as many whole middles (IMAGE_BUTTON_MIDDLE)
             * as fit between, so it stops short of the button's right edge */
            float w = f->w;
            uint32_t l = g_rd32(GAME_FN(0x1094030u)), m = g_rd32(GAME_FN(0x1094034u)), r = g_rd32(GAME_FN(0x1094038u));
            if (image_ok(l) && image_ok(m) && image_ok(r)) {
                int lw = (int)g_rd32(l + 0x24), mw = (int)g_rd32(m + 0x24), rw = (int)g_rd32(r + 0x24);
                int n = mw > 0 ? ((int)f->w - lw - rw) / mw : 0;
                /* (the right end's picture ends in clear and faint shadow columns: up to its outline) */
                int rs = rw, rh = (int)g_rd32(r + 0x28);
                uint32_t rb = g_rd32(r + 0x38);
                if (valid(rb) && rw > 0 && rw < 200 && rh > 0 && rh < 200) {
                    const uint32_t *px = (const uint32_t *)G2H(rb);
                    for (rs = rw; rs > 1; rs--) {
                        int solid = 0;
                        for (int y = 0; y < rh && !solid; y++) solid = (px[y * rw + rs - 1] >> 24) >= 128;
                        if (solid) break;
                    }
                }
                if (n >= 0) w = (float)(lw + n * mw + rs);
            }
            traced_glow(0, f->x, f->y, w, f->h - 6, green);
            return;
        }
        if (f->kind == SPOT_BUTTON && game_rtti_is(f->obj, "ButtonWidget@Sexy")) {
            uint32_t im = g_rd32(f->obj + 0xb4);
            int nlb = game_rtti_is(f->obj, "NewLawnButton");
            float ox = nlb ? (float)(int32_t)g_rd32(f->obj + 0x14c) : 0, oy = nlb ? (float)(int32_t)g_rd32(f->obj + 0x150) : 0;
            if (image_ok(im) && (int)g_rd32(im + 0x24) <= (int)f->w + 8 && (int)g_rd32(im + 0x28) <= (int)f->h + 8 &&
                (int)g_rd32(f->obj + 0xcc) == 0 /* the whole picture, not a cel of a sheet */)
                traced_glow(im, f->x + ox, f->y + oy, 0, 0, green);
            else
                traced_glow(0, f->x, f->y, f->w, f->h, green);
            return;
        }
        if (f->kind == SPOT_EDIT) traced_glow(0, f->x, f->y, f->w, f->h, green);
        return;
    }
    if (screen_kind == SCR_MENU) {
        /* the signs are drawn by the menu's animation over invisible buttons, each picture with its top
         * left on the button's (measured against the screen) */
        static const struct { int id; uint32_t g[2]; } signs[] = {
            {100, {0x1094294u, 0x10942b0u}}, {101, {0x10942b8u, 0}}, {102, {0x10942a4u, 0}}, {110, {0x10942c0u, 0}},
        };
        for (unsigned k = 0; k < sizeof signs / sizeof *signs; k++)
            if (signs[k].id == id)
                for (int j = 0; j < 2 && signs[k].g[j]; j++) {
                    uint32_t im = g_rd32(GAME_FN(signs[k].g[j]));
                    if (image_ok(im)) {
                        sign_lit(f->obj, im, k == 0 ? 1.f : 0.4f); /* (Adventure the big one) */
                        return;
                    }
                }
        if (id == 111 || id == 106) {
            uint32_t im = g_rd32(GAME_FN(id == 111 ? 0x10942d0u : 0x10942c8u));
            if (image_ok(im)) wood_lit(f->obj, im, f->x - (id == 106 ? 24.f : 0.f), f->y - (id == 106 ? 10.f : 0.f));
        }
        return;
    }
    if (screen_kind == SCR_SIDE) {
        if (id == 1) {
            uint32_t im = g_rd32(f->obj + 0xb4);
            if (image_ok(im)) sign_lit(f->obj, im, 1.f);
            else traced_glow(0, f->x, f->y, f->w, f->h, 0xd66effu);
        }
        return;
    }
    /* a list's cards get a slight light (they have the game's own hover look too); the rest more */
    warm_glow(f->x, f->y, f->w, f->h, screen_kind == SCR_CHALLENGE ? 1.3f : 2.6f);
}

/* the pad's shortcuts on screen: B on the way back, ZL / ZR on page turns */
static void badge_spot(const Spot *sp, int cel)
{
    if (sp) badge(sp->x, sp->y, sp->w, sp->h, cel);
}
static int id_of(const Spot *f) { return f->kind == SPOT_AREA ? -1 : (int)g_rd32(f->obj + 0x8c); }
static Spot *spot_area(uint32_t id) { return spot_of(0xFE000000u | id); }
static Spot *spot_button(int id)
{
    uint32_t b = screen_obj ? button_with_id(screen_obj, id) : 0;
    return b ? spot_of(b) : NULL;
}

/* The Multiplayer screen (a ChallengeScreen with the CUSTOMIZE button, id 700). Its P1 / P2 buttons (ids 800
 * and 900, 69x41 at 205,502 and 205,543) choose each player's device, keyboard or mouse; on the Switch both
 * players are keyboard players (a controller each), so the buttons keep that (the PlayerInfo bytes P1KB +0x5cd4
 * and P2KB +0x5cd5 are set by pad_input_update) and only show whether that player's controller is connected
 * (their pictures: pad_device_state). CUSTOMIZE opens the console's controller applet for two
 * players (rt_controller_applet: the host's), where controllers are connected and P1 / P2 given; it is drawn
 * with its label. */
static int pads_connected[2];
static uint8_t ai_off = 1; /* the AI Opponent box unticked (pad_input_update) */
static int ai_match;       /* versus against the AI: controller 1 alone */
void rt_controller_applet(void) __attribute__((weak));
static int is_versus_screen(void)
{
    return screen_kind == SCR_CHALLENGE && screen_obj && button_with_id(screen_obj, 700) && widget_live(button_with_id(screen_obj, 700));
}
static void versus_screen(void)
{
    if (!is_versus_screen() || pad_desktop) return; /* (the PC's own Multiplayer screen) */
    /* the AI Opponent box: the player's choice put back once as the screen opens (it was made from the
     * profile byte the port keeps set), then read */
    static uint32_t synced;
    uint32_t box = g_rd32(screen_obj + 0x424);
    int ai = 0;
    if (valid(box) && game_rtti_is(box, "Checkbox@Sexy")) {
        if (synced != screen_obj) synced = screen_obj, WR8_G(box + 0x94, !ai_off);
        ai = RD8_G(box + 0x94) != 0;
        ai_off = !ai;
    }
    /* CUSTOMIZE opens the console's controller screen here: named for it */
    uint32_t cz = button_with_id(screen_obj, 700);
    if (cz) {
        char lb[24];
        game_string(cz + 0x90, lb, sizeof lb);
        if (strcmp(lb, "CONTROLLERS")) game_string_set(cz + 0x90, "CONTROLLERS");
    }
    /* the AI Difficulty pepper (button 2000) only with the AI: off (the selector passes it) and drawn grey
     * (pad_button_draw) while the box is unticked */
    uint32_t dif = button_with_id(screen_obj, 2000);
    if (dif && RD8_G(dif + 0x5a) != !ai) WR8_G(dif + 0x5a, !ai);
    /* its picture's colour: the button's colour table (a vector of 16-byte r g b a at +0x60, begin +0x64),
     * entry 5 (the image colour DialogButton::Draw sets): grey and half seen, white again with the AI */
    uint32_t cb = dif ? g_rd32(dif + 0x64) : 0, ce = dif ? g_rd32(dif + 0x68) : 0;
    if (valid(cb) && ce >= cb + 6 * 16) {
        const uint32_t grey[4] = {120, 120, 120, 150}, white[4] = {255, 255, 255, 255};
        for (int i = 0; i < 4; i++) g_wr32(cb + 5 * 16 + 4 * (uint32_t)i, ai ? white[i] : grey[i]);
    }
    for (int p = 0; p < 2; p++) {
        uint32_t b = button_with_id(screen_obj, p ? 900 : 800);
        if (!b) continue;
        /* against the AI there is no player 2: its button off (drawn dimmed: wide_art.c device_swap), and
         * its name box dimmed over its frame (measured: 166x33 from 173 left of the button, 3 down) */
        if (p && RD8_G(b + 0x5a) != ai) WR8_G(b + 0x5a, ai);
        float x = (float)(int32_t)g_rd32(b + 0x34), y = (float)(int32_t)g_rd32(b + 0x38);
        if (p && ai) ov_rect(x - 173, y + 3, 166, 33, 0x80000000u);
    }
    vs_ai_box = ai;
}
/* the device buttons' pictures (wide_art.c): 0 the controller connected, 1 not, 2 the AI plays (player 2) */
int pad_device_state(int p)
{
    if (p == 1 && vs_ai_box) return 2;
    return pads_connected[p & 1] ? 0 : 1;
}
/* the jukebox (MusicScreen): ZL / ZR are its track arrows (buttons 30003 back, 30001 forward) */
static int is_music_screen(void) { return screen_kind == SCR_OTHER && screen_obj && game_rtti_is(screen_obj, "MusicScreen"); }
static uint32_t prev_on_release; /* a button that acts as the mouse goes down: pointed at while held (its hover
                                   * look), clicked whole when let go, at release_x / release_y */
static float release_x = 300.f, release_y = 440.f;
/* a B over a close button's red X: pressed (the left button down with the pointer on the button), the button's
 * picture is drawn a pixel down and right, and the B goes with it (part of the X showed beside it) */
static void badge_on_x(const Spot *cl, float x, float y)
{
    float px = (cur_x - 160.f) / 1.2f, py = cur_y / 1.2f;
    int down = (mouse_buttons & 1) && cl && px >= cl->x && px < cl->x + cl->w && py >= cl->y && py < cl->y + cl->h;
    badge_img(HELP_B, x + (down ? 1.f : 0.f), y + (down ? 1.f : 0.f), 24, 24);
}
static void menu_badges(void)
{
    if (is_music_screen() && !top_dialog()) {
        Spot *l = spot_button(30003), *r = spot_button(30001), *cl = back_spot();
        /* on the arrows' outer ends; held (the arrow pressed in: DialogButton draws a pressed button 1 down and
         * right), the badge goes with it */
        float dl = prev_on_release & (PB_ZL | PB_L) ? 1.f : 0, dr = prev_on_release & (PB_ZR | PB_R) ? 1.f : 0;
        if (l) badge_side(l->x + dl, l->y + dl, l->w, l->h, HELP_ZL, -1);
        if (r) badge_side(r->x + dr, r->y + dr, r->w, r->h, HELP_ZR, 1);
        /* its Shop tag (30010): Y, as in the gardens (plant select's Y is its Almanac, which isn't here) */
        Spot *sh = spot_button(30010);
        if (sh) badge(sh->x, sh->y, sh->w, sh->h - 18, HELP_Y); /* (the tag's picture ends 18 above its button's bottom) */
        /* Close ends in a red X (its own picture): the B goes over it, in its place */
        if (cl && !touch_mode) {
            static int logged;
            if (!logged++) rt_log("[pad] music Close at %.0f,%.0f %.0fx%.0f\n", cl->x, cl->y, cl->w, cl->h);
            badge_on_x(cl, cl->x + 58, cl->y + cl->h / 2 - 12); /* (the picture is 87 of its 111 wide) */
        }
        return;
    }
    if (screen_kind == SCR_STORE) {
        /* on Main Menu even while the trunk turns a page (the button is off then, but drawn) */
        uint32_t mm = screen_obj ? button_with_id(screen_obj, 100) : 0;
        if (mm) badge((float)(int32_t)g_rd32(mm + 0x34), (float)(int32_t)g_rd32(mm + 0x38), (float)(int32_t)g_rd32(mm + 0x3c),
                      (float)(int32_t)g_rd32(mm + 0x40), HELP_B);
        /* only while the shop shows its page arrows (they go while the trunk turns a page) */
        uint32_t pv = screen_obj ? button_with_id(screen_obj, 101) : 0, nb = screen_obj ? button_with_id(screen_obj, 102) : 0;
        (void)pv;
        Spot *nx = nb && widget_live(nb) ? spot_of(nb) : NULL; /* (Prev is drawn with it, wrapping round) */
        (void)nx;
        if (!touch_mode) {
            /* on the car's body beside each tail light, leaning as its edge does */
            ov_image_rot(OV_HELP, 42 * HELP_ZL, 0, 42, 42, 350, 438, 22, 22, -12.f, 0xffffffffu);
            ov_image_rot(OV_HELP, 42 * HELP_ZR, 0, 42, 42, 598, 438, 22, 22, 16.f, 0xffffffffu);
        }
        return;
    }
    if (screen_kind == SCR_ALMANAC) {
        int page = (int)g_rd32(screen_obj + 0x238);
        if (page) {
            badge_spot(spot_area(0x210), HELP_B);
        } else {
            /* Close ends in a red X (part of the Almanac's own picture): the B goes over it, in its place */
            Spot *cl = spot_area(0x213);
            if (cl && !touch_mode) badge_on_x(cl, cl->x + cl->w - 14 - 12, cl->y + cl->h / 2 - 11);
        }
        if (page && !touch_mode) {
            /* Last Page / Next Page end in their arrows (drawn by the Almanac): ZL / ZR on the arrow's inner side,
             * pushed in with the button (the Almanac draws a pressed one a pixel down and right) */
            Spot *l = spot_area(0x211), *r = spot_area(0x212);
            float px = (cur_x - 160.f) / 1.2f, py = cur_y / 1.2f, sz = 26;
            for (int k = 0; k < 2; k++) {
                Spot *sp = k ? r : l;
                if (!sp) continue;
                int down = (mouse_buttons & 1) && px >= sp->x && px < sp->x + sp->w && py >= sp->y && py < sp->y + sp->h;
                float bx = k ? sp->x + sp->w - 20 - sz : sp->x + 20, by = sp->y + (sp->h - sz) / 2;
                badge_img(k ? HELP_ZR : HELP_ZL, bx + (float)down, by + (float)down, sz, sz);
            }
        }
        return;
    }
    if (screen_kind == SCR_AWARD) {
        /* the Multiplayer HELP pages (award type +0x9c = 4): Close ends in a red X, the B goes over it */
        Spot *cl = spot_area(0x502);
        if (g_rd32(screen_obj + 0x9c) == 2) { /* the menu's Help: its Main Menu at the bottom (325,523 155x34) */
            badge(325, 523, 155, 34, HELP_B);
        } else if (cl && g_rd32(screen_obj + 0x9c) == 4) {
            if (!touch_mode) badge_on_x(cl, 748, cl->y + cl->h / 2 - 12);
        } else badge_spot(cl, HELP_B);
        return;
    }
    if (screen_kind == SCR_MAIL) {
        badge_spot(spot_area(0x502), HELP_B);
        Spot *l = spot_area(0x500), *r = spot_area(0x501);
        /* (held: the page turns on release; its badge pushed in meanwhile) */
        float dl = prev_on_release & (PB_ZL | PB_L) ? 1.5f : 0, dr = prev_on_release & (PB_ZR | PB_R) ? 1.5f : 0;
        if (l) badge_inner(l->x + dl, l->y + dl, l->w, l->h, HELP_ZL, 1);
        if (r) badge_inner(r->x + dr, r->y + dr, r->w, r->h, HELP_ZR, -1);
        return;
    }
    if (screen_kind == SCR_CHALLENGE) { badge_spot(spot_button(100), HELP_B); versus_screen(); return; }
    if (screen_kind == SCR_SIDE) return; /* (side_badge) */
    /* popups get none: their No / Cancel is one step away */
    if (!top_dialog() && screen_kind == SCR_OTHER) badge_spot(back_spot(), HELP_B);
}

/* the focused row's button before the list scrolled: its middle (x, and y in the list, unscrolled) */
static float list_cell_x, list_cell_y;
/* the list's button in that column on the next row that way (dir 1 down, -1 up) */
static Spot *list_cell(int dir)
{
    float at = list_scroll() * 10.f;
    Spot *best = NULL;
    float best_d = 1e9f;
    for (int i = 0; i < n_spots; i++) {
        Spot *sp = &spots[i];
        if (!sp->in_list || fabsf(sp->x + sp->w / 2 - list_cell_x) > sp->w / 2) continue;
        float d = (sp->y + sp->h / 2 + at - list_cell_y) * (float)dir;
        if (d > 20 && d < best_d) best = sp, best_d = d;
    }
    return best;
}
static int dirs_held_over;
static int page_focus_id[2] = {-1, -1}; /* the main menu's pages' last focus, by button id */
static int page2_return;                /* the second page came back after a screen opened from it */

/* A screen opened from the main menu's second page (Challenges, the credits, the mailbox...) goes back to
 * the second page when it closes, not the first. The menu that comes back rises in on its first page: its
 * opening (the selector reanim, LawnApp::ReanimationTryToGet 0x8dcad0 of GameSelector +0xe0) is taken to
 * its last frame and the menu is turned straight to its second page (GameSelector::ButtonDepress 0x820770
 * for MORE, id 112, on its ButtonListener at +0x8c, with the slide counter +0x140 at its last tick); its
 * state +0x12c is put back to the opening (MORE sets it past), so GameSelector::Update still finishes the
 * opening (buttons shown, the signs dropping in) behind the second page */
/* A screen opened from the first page comes back to a new menu, which plays its whole opening again (the
 * stones and signs dropping in from the top). After the 4:3 screens that keep the menu in view beside them
 * (the Almanac, the lists, the award screens drawn in the window) that makes no sense: the menu was there all
 * along. There the opening (state +0x12c 0) and then the signs' drop (anim_sign, which the opening's end
 * starts) are taken to their last frame. Full screens (Help, the Shop, a level) keep the opening. */
/* the award screen on top: its type (+0x9c), else -1 */
int pad_award_type(void)
{
    return screen_obj && (screen_kind == SCR_AWARD || screen_kind == SCR_MAIL || game_rtti_is(screen_obj, "AwardScreen"))
               ? (int)g_rd32(screen_obj + 0x9c) : -1;
}
static int quick_armed; /* a 4:3 popup is (or was last) up: the next new menu skips its opening */
/* GameSelector::Draw starts (game_hook_menu_draw): a new menu while armed is drawn from its first frame with its
 * opening, then its signs' drop, at their last frames */
/* The menu's six clouds (reanims, ids at GameSelector +0xe4.., their counters at +0xfc..; a reanim's time +4,
 * rate +8) start at random places in each new menu: they jumped as the menu came back. Each frame's places are
 * kept, and a new menu (after the first) is given them, so a cloud is where the screen beside a popup showed it */
static struct { uint32_t t, rate, counter; } clouds[6];
static int clouds_kept;
static void menu_clouds(uint32_t sel, int restore)
{
    uint32_t app = rt_app_object();
    if (!valid(app)) return;
    for (int i = 0; i < 6; i++) {
        uint32_t id = g_rd32(sel + 0xe4 + 4 * (uint32_t)i);
        uint32_t r = guest_this(GAME_FN(0x8dcad0u), app, 1, &id);
        if (!valid(r)) continue;
        if (restore) {
            g_wr32(r + 4, clouds[i].t), g_wr32(r + 8, clouds[i].rate);
            g_wr32(sel + 0xfc + 4 * (uint32_t)i, clouds[i].counter);
        } else {
            clouds[i].t = g_rd32(r + 4), clouds[i].rate = g_rd32(r + 8);
            clouds[i].counter = g_rd32(sel + 0xfc + 4 * (uint32_t)i);
        }
    }
    if (!restore) clouds_kept = 1;
}
void pad_menu_drawn(uint32_t sel)
{
    static uint32_t seen, seen_id;
    static int quick;
    if (!valid(sel)) return;
    /* (a new menu is often made where the last one was: told apart by its selector reanim's id) */
    if (sel != seen || g_rd32(sel + 0xe0) != seen_id) {
        seen = sel, seen_id = g_rd32(sel + 0xe0);
        quick = quick_armed && g_rd32(sel + 0x12c) == 0;
        if (quick) quick_armed = 0;
        if (clouds_kept) menu_clouds(sel, 1);
    }
    menu_clouds(sel, 0);
    if (!quick) return;
    uint32_t app = rt_app_object(), id = g_rd32(sel + 0xe0);
    uint32_t r = valid(app) ? guest_this(GAME_FN(0x8dcad0u), app, 1, &id) : 0;
    if (!valid(r)) return;
    float end = 0.999f;
    memcpy(G2H(r + 4), &end, 4);
    if (g_rd32(sel + 0x12c) != 0) { /* (the opening done: this was the signs' drop) */
        quick = 0;
        rt_log("[pad] back to the menu without its opening\n");
    }
}

/* Leaving a versus match (its menu's Main Menu, or the match's end) goes back to Multiplayer, where the next
 * match is chosen, not the main menu: the menu that comes back (without its opening, quick_menu) presses its
 * Multiplayer sign (id 111) through GameSelector::ButtonDepress once its opening is done */
static int from_versus;
static uint32_t to_multiplayer;
static void versus_return_step(void)
{
    if (board()) from_versus = is_versus();
    if (from_versus) quick_armed = 1;
    if (to_multiplayer && (screen_kind != SCR_MENU || screen_obj != to_multiplayer)) to_multiplayer = 0;
    if (!to_multiplayer || g_rd32(to_multiplayer + 0x12c) == 0) return;
    uint32_t mp = 111;
    guest_this(GAME_FN(0x820770u), to_multiplayer + 0x8c, 1, &mp);
    rt_log("[pad] back to Multiplayer after a versus match\n");
    to_multiplayer = 0;
}

static void back_to_page2(void)
{
    static int kind_was = -1, from_page2, award_was = -1;
    static uint32_t pending;
    versus_return_step();
    if (pending && (screen_kind != SCR_MENU || screen_obj != pending)) pending = 0;
    if (pending) {
        uint32_t app = rt_app_object(), id = g_rd32(pending + 0xe0);
        uint32_t r = valid(app) ? guest_this(GAME_FN(0x8dcad0u), app, 1, &id) : 0;
        float t = 0.999f;
        if (valid(r) && g_rd32(pending + 0x12c) == 0) memcpy(G2H(r + 4), &t, 4); /* the opening, nearly done */
        /* (only if the opening hasn't finished yet: once it has, its end has run — the trophy's sparkle added — and
         * running it again asserts, GameSelector.cpp(528)) */
        int opening = g_rd32(pending + 0x12c) == 0;
        uint32_t more = 112;
        guest_this(GAME_FN(0x820770u), pending + 0x8c, 1, &more);
        g_wr32(pending + 0x140, 1);
        if (opening) g_wr32(pending + 0x12c, 0); /* (MORE skips the opening's end: it still runs, off screen) */
        rt_log("[pad] back to the menu's second page\n");
        page2_return = 1;
        pending = 0;
    }
    if (screen_kind == kind_was || !screen_obj) return; /* (nothing on top for a moment as the pages slide) */
    if (rt_verbose) rt_log("[pad] screen kind %d -> %d (%s)\n", kind_was, screen_kind, game_rtti_name(screen_obj));
    if (kind_was == SCR_SIDE && screen_kind != SCR_MENU) from_page2 = 1;
    /* back on the second page itself (the jukebox closes onto it): nothing to restore; a menu after it (its
     * Back) is not a return from a screen (restoring it re-ran the menu's opening: the trophy sparkle's assert) */
    if (screen_kind == SCR_SIDE) from_page2 = 0;
    if (screen_kind == SCR_AWARD && rt_verbose) rt_log("[pad] award screen type %d\n", (int)g_rd32(screen_obj + 0x9c));
    if (screen_kind == SCR_AWARD || screen_kind == SCR_MAIL) award_was = (int)g_rd32(screen_obj + 0x9c);
    /* the 4:3 popups the menu shows beside (the Almanac, the lists, award screens in the window: not the menu's
     * Help, 2, nor the Multiplayer HELP, 4, nor the letters, 5, now drawn wide) arm the menu's quick return;
     * any other screen (a level, the Shop, Help) disarms it */
    if (from_page2) quick_armed = 0; /* (a screen opened from page 2 goes back there: back_to_page2's own way) */
    else if (screen_kind == SCR_ALMANAC || screen_kind == SCR_CHALLENGE ||
        ((screen_kind == SCR_AWARD || screen_kind == SCR_MAIL) && award_was != 2 && award_was != 4 && award_was != 5))
        quick_armed = 1;
    else if (screen_kind != SCR_MENU && screen_kind != SCR_SIDE && screen_kind != SCR_OPTIONS)
        quick_armed = 0;
    if (screen_kind == SCR_MENU) {
        if (from_page2 && button_with_id(screen_obj, 112)) pending = screen_obj, quick_armed = 0;
        /* (back from versus: straight on to Multiplayer, a 4:3 screen too) */
        if (from_versus && button_with_id(screen_obj, 111)) to_multiplayer = screen_obj;
        from_versus = 0;
        from_page2 = 0;
    }
    kind_was = screen_kind;
}

/* the second page's Back (SideScreen, LawnApp +0x79c, its button id 0) gets its B wherever the page is,
 * so it slides in and out with it rather than appearing once the page has settled */
static void side_badge(void)
{
    if (screen_kind != SCR_MENU && screen_kind != SCR_SIDE) return;
    uint32_t app = rt_app_object(), ss = valid(app) ? g_rd32(app + 0x79c) : 0;
    if (!valid(ss) || !game_rtti_is(ss, "SideScreen")) return;
    uint32_t bk = button_with_id(ss, 0);
    if (!bk) return;
    float x = (float)(int32_t)(g_rd32(ss + 0x34) + g_rd32(bk + 0x34)), y = (float)(int32_t)(g_rd32(ss + 0x38) + g_rd32(bk + 0x38));
    float w = (float)(int32_t)g_rd32(bk + 0x3c), h = (float)(int32_t)g_rd32(bk + 0x40);
    if (x + w < -140 || x > 940) return;
    badge(x, y, w, h, HELP_B);
}

/* The Imitater's picker (ImitaterDialog, over the seed chooser): its cards in pages of 8 x 5 (+0x228 the page;
 * cells 51 x 71 apart from 43,91 in the dialog, cards 50 x 70), its own arrows (buttons 1 back, 0 next) turn the
 * page. The D-pad / stick walks the cards there are (the dialog's own hit test, 0x6fcc6e(x, y) -> the seed or -1,
 * asked of each cell's middle: the pages hold what the save has), ZL / ZR turn the page, A picks the card (a
 * click: its MouseDown), B closes it unused (its Escape). The chooser's bracket marks the card
 * (pad_imitater_drawn); the pointer rests on it: its tooltip. */
#define IMITATER_HIT_TEST 0x6fcc6eu
static int imit_card(uint32_t dlg, int row, int col)
{
    uint32_t xy[2] = {(uint32_t)(43 + 51 * col + 25), (uint32_t)(91 + 71 * row + 35)};
    return (int32_t)guest_this(GAME_FN(IMITATER_HIT_TEST), dlg, 2, xy) != -1;
}
/* SeedChooserScreen::Draw's end (0x90a67a, g = its Graphics): with the selector on the Imitater, the chooser's
 * own bracket (IMAGE_P1_1) on its packet (467,517 45x66), as the chooser draws it on a card (centred, 8 / 15 of
 * the picture for a 50 x 70 card: here fitted to the Imitater's) */
void pad_chooser_drawn(uint32_t g)
{
    uint32_t img = g_rd32(0x1094180u);
    if (on_imit != 1 || touch_mode || !seed_chooser() || top_dialog() || !valid(g) || !valid(img)) return;
    float iw = (float)g_rd32(img + 0x24), ih = (float)g_rd32(img + 0x28);
    float sx = 8.f / 15.f * 45.f / 50.f, sy = 8.f / 15.f * 66.f / 70.f;
    uint32_t args[6] = {g, img, f2u(467.f + 22.5f - iw * sx / 2), f2u(517.f + 33.f - ih * sy / 2), f2u(sx), f2u(sy)};
    guest_this(GAME_FN(0x9c9640u), 0, 6, args); /* TodDrawImageScaledF */
}
static int imit_drawn_row = -1, imit_drawn_col; /* the card the bracket goes on (-1 none) */
/* ImitaterDialog::Draw (0x824f10) has drawn its cards (0x8250a7, before its tooltip; g = the dialog's Graphics):
 * the chooser's own bracket (IMAGE_P1_1, board_selector_YELLOW, 104 x 144) on the chosen card, as the
 * chooser draws it (centred on the card, scaled 8 / 15) */
#define TOD_DRAW_SCALED_F 0x9c9640u /* TodDrawImageScaledF(Graphics *, Image *, x, y, sx, sy), cdecl */
void pad_imitater_drawn(uint32_t g)
{
    uint32_t img = g_rd32(0x1094180u);
    if (imit_drawn_row < 0 || touch_mode || !valid(g) || !valid(img)) return;
    float k = 8.f / 15.f, w = (float)g_rd32(img + 0x24) * k, h = (float)g_rd32(img + 0x28) * k;
    float cx = 43.f + 51.f * (float)imit_drawn_col + 25.f, cy = 91.f + 71.f * (float)imit_drawn_row + 35.f;
    uint32_t args[6] = {g, img, f2u(cx - w / 2), f2u(cy - h / 2), f2u(k), f2u(k)};
    guest_this(GAME_FN(TOD_DRAW_SCALED_F), 0, 6, args);
}
static int imitater_update(const PadIn *p, float dt)
{
    static uint32_t prev, prev_dirs, open_dlg;
    static int row, col, page = -1;
    static float repeat;
    uint32_t dlg = top_dialog();
    if (!dlg || !seed_chooser() || !game_rtti_is(dlg, "ImitaterDialog")) {
        open_dlg = 0, imit_drawn_row = -1;
        return 0;
    }
    uint32_t b = p->connected ? p->buttons : 0;
    imit_picker_was_open = 1;
    if (dlg != open_dlg) { /* opened: on its first card; what is held on the way in is not a press */
        open_dlg = dlg, page = -1, prev = b, prev_dirs = b & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT);
    }
    uint32_t dirs = p->connected ? (stick_dirs(p, prev_dirs) | (b & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT))) : 0;
    uint32_t pressed = b & ~prev;
    prev = b;
    if (pressed || (dirs & ~prev_dirs)) touch_mode = 0;
    uint32_t step = dirs & ~prev_dirs;
    if (dirs && dirs == prev_dirs) {
        repeat -= dt;
        if (repeat <= 0) { step = dirs; repeat = 0.11f; }
    } else {
        repeat = 0.38f;
    }
    prev_dirs = dirs;
    float dx = (float)(int32_t)g_rd32(dlg + 0x34), dy = (float)(int32_t)g_rd32(dlg + 0x38);
    int pg = (int)g_rd32(dlg + 0x228), moved = 0;
    if (pg != page) { /* a new page: its first card */
        page = pg, row = col = 0, moved = 1;
        for (int i = 0; i < 40 && !imit_card(dlg, row, col); i++) col = (i + 1) % 8, row = (i + 1) / 8;
    }
    for (int t = 0; t < 8 && (step & (PB_LEFT | PB_RIGHT)); t++) {
        if (step & PB_LEFT) col = col > 0 ? col - 1 : 7;
        else col = col < 7 ? col + 1 : 0;
        if (imit_card(dlg, row, col)) { moved = 1; break; }
    }
    for (int t = 0; t < 5 && (step & (PB_UP | PB_DOWN)); t++) {
        int r = (step & PB_UP) ? (row > 0 ? row - 1 : 4) : (row < 4 ? row + 1 : 0), found = -1;
        for (int d = 0; d < 8 && found < 0; d++) { /* (no card in this column there: the nearest in the row) */
            if (col - d >= 0 && imit_card(dlg, r, col - d)) found = col - d;
            else if (col + d < 8 && imit_card(dlg, r, col + d)) found = col + d;
        }
        row = r;
        if (found >= 0) { col = found, moved = 1; break; }
    }
    float cx = dx + 43.f + 51.f * (float)col, cy = dy + 91.f + 71.f * (float)row;
    int here = imit_card(dlg, row, col);
    if (moved && here && !hold_pb) {
        cur_x = 160.f + (cx + 25.f) * 1.2f, cur_y = (cy + 35.f) * 1.2f;
        mouse(WM_MOUSEMOVE); /* (its tooltip) */
    }
    imit_drawn_row = here ? row : -1, imit_drawn_col = col;
    if ((pressed & PB_A) && here) press_hold(PB_A, cx + 25.f, cy + 35.f);
    for (int k = 0; k < 2; k++) { /* the arrows (their place within the dialog: +0x34 x, +0x38 y, +0x3c w, +0x40 h) */
        uint32_t ab = button_with_id(dlg, k ? 0 : 1), pb = k ? (PB_ZR | PB_R) : (PB_ZL | PB_L);
        if (!ab) continue;
        float ax = dx + (float)(int32_t)g_rd32(ab + 0x34), ay = dy + (float)(int32_t)g_rd32(ab + 0x38);
        float aw = (float)(int32_t)g_rd32(ab + 0x3c), ah = (float)(int32_t)g_rd32(ab + 0x40);
        if (pressed & pb) press_hold(pressed & pb, ax + aw / 2, ay + ah / 2);
        badge_inner(ax, ay, aw, ah, k ? HELP_ZR : HELP_ZL, k ? -1 : 1); /* (on the inner side, clear of the arrow) */
    }
    if (pressed & PB_B) tap(0x1B); /* closed unused */
    return 1;
}

/* The Almanac's description (plant / zombie page) scrolls when it is longer than its box (AlmanacDialog +0x8cc
 * above 0: the game's own scrollbar; +0x8c8 the offset in pixels, 0..+0x8cc; its mouse wheel, only with the mouse
 * over the box, +0x8c0, moves it by +0x8c4 a notch): the right stick scrolls it, smoothly, faster the further it is pushed (up = towards the top) */
static void almanac_scroll(const PadIn *p, float dt)
{
    static float vel;
    uint32_t w = screen_obj;
    float max = 0;
    if (screen_kind == SCR_ALMANAC && w && g_rd32(w + 0x238)) memcpy(&max, G2H(w + 0x8cc), 4);
    if (!(max > 0.f) || RD8_G(w + 0x8fc) || !p->connected) { /* (+0x8fc: its scrollbar being dragged) */
        vel = 0;
        return;
    }
    float ry = p->ry, a = fabsf(ry), want = 0;
    if (a > 0.2f) want = (ry > 0 ? -1.f : 1.f) * powf((a - 0.2f) / 0.8f, 1.5f) * 420.f; /* px / s */
    float k = dt * 12.f;
    vel += (want - vel) * (k > 1.f ? 1.f : k);
    if (fabsf(vel) < 1.f && want == 0) { vel = 0; return; }
    float off;
    memcpy(&off, G2H(w + 0x8c8), 4);
    off += vel * dt;
    if (off < 0) off = 0, vel = 0;
    if (off > max) off = max, vel = 0;
    memcpy(G2H(w + 0x8c8), &off, 4);
}
static void menu_update(const PadIn *p, float dt)
{
    /* (test hook, Mac host: RT_START_MODE=n starts game mode n from the main menu, as the menu does) */
    static int start_done;
    if (!start_done && screen_kind == SCR_MENU && getenv("RT_START_MODE")) {
        static int wait = 200;
        if (--wait == 0) {
            start_done = 1;
            uint32_t app = rt_app_object(), args[2] = {(uint32_t)atoi(getenv("RT_START_MODE")), 0};
            guest_this(GAME_FN(0x8daf40u), app, 0, NULL); /* LawnApp::KillGameSelector */
            guest_this(GAME_FN(0x8dc710u), app, 2, args);  /* LawnApp::PreNewGame(mode, false) */
        }
    }
    /* (... and RT_START_PAGE=n opens the challenge screen's page n: 12 = the DLC's credits) */
    if (!start_done && screen_kind == SCR_MENU && getenv("RT_START_PAGE")) {
        static int wait = 200;
        if (--wait == 0) {
            start_done = 1;
            uint32_t app = rt_app_object(), page = (uint32_t)atoi(getenv("RT_START_PAGE"));
            guest_this(GAME_FN(0x8daf40u), app, 0, NULL); /* LawnApp::KillGameSelector */
            guest_this(GAME_FN(0x8dcf10u), app, 1, &page); /* LawnApp::ShowChallengeScreen(page) */
        }
    }
    static uint32_t prev, prev_dirs;
    static float repeat;
    static int scroll_dir;
    uint32_t b = p->connected ? p->buttons : 0;
    almanac_scroll(p, dt);
    static uint32_t last_poll;
    /* not run last poll (a level, a garden, the chooser): what is still held from there is not a press here */
    if (last_poll + 1 != pad_poll_no) prev = b, prev_dirs = b & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT);
    last_poll = pad_poll_no;
    if (scheme_switched) { /* the + that opened the pause menu is still down: not a press here */
        scheme_switched = 0;
        prev = b;
        prev_dirs = b & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT);
    }
    uint32_t dirs = p->connected ? (stick_dirs(p, prev_dirs) | (b & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT))) : 0;
    uint32_t down = b & ~prev, up = prev & ~b;
    prev = b;
    uint32_t step = dirs & ~prev_dirs;
    /* a held direction repeats after 0.38 s, then every 0.11 s (PvZ Touch's D-pad) */
    if (dirs && dirs == prev_dirs) {
        repeat -= dt;
        if (repeat <= 0) { step = dirs; repeat = 0.11f; }
    } else {
        repeat = 0.38f;
    }
    prev_dirs = dirs;
    page_click_update(b);
    if (prev_on_release && !(b & prev_on_release)) {
        prev_on_release = 0;
        click_game(release_x, release_y);
    }
    cursor_shown = 0;
    /* a press let go while the screen changes (navigation paused below) still lets go */
    if ((up & PB_A) && (mouse_buttons & 1) && (nav_freeze > 0 || !spot_of(focus_obj))) {
        mouse_buttons &= ~1;
        mouse(WM_LBUTTONUP);
        up &= ~PB_A;
    }
    collect_spots();
    list_glide(dt);
    back_to_page2();
    side_badge();
    {
        static int kind_was = -1;
        if ((screen_kind == SCR_MENU && kind_was == SCR_SIDE) || (screen_kind == SCR_SIDE && kind_was == SCR_MENU))
            nav_freeze = nav_freeze > 0.45f ? nav_freeze : 0.45f;
        kind_was = screen_kind;
    }
    if (nav_freeze > 0) {
        nav_freeze -= dt;
        focus_obj = 0; /* chosen afresh for the page it lands on */
        dirs_held_over = 1;
        return;
    }
    /* a direction still held from before the pages slid (the push that turned the page) doesn't go on
     * stepping on the new page: it has to be let go first */
    if (dirs_held_over) {
        if (dirs) step = 0;
        else dirs_held_over = 0;
    }
    /* the main menu coming in (from the title, or dropping in after a level): nothing until its buttons
     * are there and it has stopped moving (its trophy is there before them) */
    if (screen_kind == SCR_MENU || screen_kind == SCR_SIDE) {
        int buttons = 0;
        for (int i = 0; i < n_spots; i++) buttons |= spots[i].kind == SPOT_BUTTON;
        static int32_t last_y = 1 << 30;
        int32_t y = screen_obj ? (int32_t)g_rd32(screen_obj + 0x38) : 0;
        int moving = y != last_y || y != 0;
        last_y = y;
        if (!buttons || moving) {
            focus_obj = 0;
            return;
        }
    }
    /* the main menu's pages sliding over: no highlight until they settle (the old page's buttons and
     * then the new one's arrive mid-way) */
    {
        uint32_t app = rt_app_object(), wm = valid(app) ? g_rd32(app + 0x320) : 0;
        uint32_t head = valid(wm) ? g_rd32(wm + 8) : 0, n = valid(wm) ? g_rd32(wm + 0xc) : 0;
        uint32_t node = valid(head) ? g_rd32(head) : 0;
        for (uint32_t i = 0; i < n && i < 64 && valid(node) && node != head; i++, node = g_rd32(node)) {
            uint32_t w = g_rd32(node + 8);
            if (!valid(w) || !RD8_G(w + 0x58) || g_rd32(w + 0x3c) < 700) continue;
            int32_t x = (int32_t)g_rd32(w + 0x34);
            const char *nm = game_rtti_name(w);
            if ((strstr(nm, "GameSelector") || strstr(nm, "SideScreen")) && x != 0 && x != 800 && x != -800) return;
        }
    }
    if ((down | step) && touch_mode) {
        /* the first press after the touch screen only brings the highlight back, where it was */
        touch_mode = 0;
        down &= ~(PB_A | PB_B);
        step = 0;
        Spot *f = spot_of(focus_obj);
        if (f) point_at(f->x + f->w / 2, f->y + f->h / 2);
    }
    if (touch_mode) return;
    Spot *f = spot_of(focus_obj);
    /* in a scrolling list the focus is a place in the list (its buttons are reused for other rows as it
     * scrolls): the button now at that place */
    static int lf_on;
    static float lf_x, lf_cy;
    if (lf_on && list_scrolls) {
        float at = list_scroll() * 10.f;
        for (int i = 0; i < n_spots; i++) {
            Spot *sp = &spots[i];
            if (sp->in_list && fabsf(sp->x + sp->w / 2 - lf_x) < 6 && fabsf(sp->y + sp->h / 2 + at - lf_cy) < 10) {
                if (sp != f) f = sp, focus_obj = sp->obj;
                break;
            }
        }
    }
    /* a screen's buttons can come a few frames after its own places (the main menu's trophy): a start
     * made before them is made again */
    static int defaulted_to_area;
    if (f && defaulted_to_area && f->kind == SPOT_AREA && screen_kind != SCR_STORE && screen_kind != SCR_ALMANAC)
        for (int i = 0; i < n_spots; i++)
            if (spots[i].kind == SPOT_BUTTON) { f = NULL; defaulted_to_area = 0; focus_obj = 0; break; }
    /* the main menu's two pages remember their focus; arriving on the second page starts at its left,
     * coming back starts at MORE */
    static uint32_t page_focus[2];
    static int last_kind = -1;
    int came_from = last_kind;
    if (screen_kind != last_kind && screen_obj) last_kind = screen_kind; /* (not the moment with nothing on top) */
    if (!f && n_spots && (screen_kind == SCR_MENU || screen_kind == SCR_SIDE)) {
        Spot *m = spot_of(page_focus[screen_kind == SCR_SIDE]);
        if (m && m->kind == SPOT_AREA && screen_kind == SCR_MENU) m = NULL; /* (the trophy was only a stand-in) */
        /* the first page made anew (back from a screen opened from it): its button with the same id (the old
         * pointer may now be another button's: Puzzle came back on the Shop) */
        if (screen_kind == SCR_MENU && page_focus_id[0] >= 0) {
            Spot *b = spot_button(page_focus_id[0]);
            if (b) m = b;
        }
        if (screen_kind == SCR_SIDE && came_from == SCR_MENU) m = NULL;
        /* back from a screen opened here (back_to_page2): on the button that opened it (the page is made anew,
         * so it is found by its id) */
        if (screen_kind == SCR_SIDE && page2_return && page_focus_id[1] >= 0) {
            Spot *b = spot_button(page_focus_id[1]);
            if (b) m = b;
        }
        page2_return = 0;
        if (screen_kind == SCR_MENU && came_from == SCR_SIDE) {
            uint32_t more = button_with_id(screen_obj, 112);
            if (more) m = spot_of(more);
        }
        if (!m && screen_kind == SCR_SIDE)
            for (int i = 0; i < n_spots; i++)
                if (spots[i].kind == SPOT_BUTTON && (!m || spots[i].x < m->x)) m = &spots[i];
        if (m) {
            f = m;
            focus_obj = m->obj;
            log_focus("page", m);
            point_at(m->x + m->w / 2, m->y + m->h / 2);
        }
    }
    int music_wait = 0;
    if (!f && n_spots && screen_obj && game_rtti_is(screen_obj, "MusicScreen")) {
        Spot *pp = spot_button(30002); /* (the pause button, not the album, which may be all there is at first) */
        if (pp) { f = pp; focus_obj = pp->obj; log_focus("new screen", pp); point_at(pp->x + pp->w / 2, pp->y + pp->h / 2); }
        else music_wait = 1;
    }
    if (!f && n_spots && !music_wait) {
        f = default_spot();
        defaulted_to_area = f->kind == SPOT_AREA;
        if (screen_kind == SCR_STORE || screen_kind == SCR_ALMANAC || screen_kind == SCR_MAIL || screen_kind == SCR_AWARD)
            for (int i = 0; i < n_spots; i++)
                if (spots[i].kind == SPOT_AREA) { f = &spots[i]; if (screen_kind != SCR_MAIL || spots[i].obj == 0xFE000501u) break; }
        if (screen_kind == SCR_AWARD) f = spot_area(0x510) ? spot_area(0x510) : f;
        focus_obj = f->obj;
        log_focus("new screen", f);
        point_at(f->x + f->w / 2, f->y + f->h / 2);
    }
    /* the DLC's credits page (a ChallengeScreen with no list): the stick / D-pad scroll it smoothly,
     * faster with the tilt; its scroll position is the list's (+0x3f8, 10 px units, clamped by the game) */
    if (screen_kind == SCR_CHALLENGE && list_scrolls) {
        /* (only a page with no cards at all: a card list whose cards are all out of view for a moment,
         * as the event's are while it opens, is not the credits) */
        if (!list_cards) {
            /* a speed eased towards the stick's (or the D-pad's), rising the longer it is held (up to 3x
             * after a second and a half), and easing out when let go */
            static float vel, held;
            float v = p->connected ? p->ly : 0;
            if (b & PB_UP) v = 0.6f;
            if (b & PB_DOWN) v = -0.6f;
            if (fabsf(v) < 0.15f) v = 0;
            held = v != 0 ? held + dt : 0;
            float boost = 1.f + 2.f * fminf(held / 1.5f, 1.f);
            float want = -v * fabsf(v) * 90.f * boost;
            vel += (want - vel) * fminf(1.f, dt * (v != 0 ? 8.f : 6.f));
            if (fabsf(vel) < 0.2f && v == 0) vel = 0;
            if (vel != 0) list_scroll_set(list_scroll() + vel * dt);
            step &= ~(PB_UP | PB_DOWN);
        }
    }
    /* the mailbox: left / right turn the letter's pages (its own Last Page! / Next Page!) */
    if (screen_kind == SCR_MAIL && (step & (PB_LEFT | PB_RIGHT)) && !(mouse_buttons & 1)) {
        click_area((step & PB_LEFT) ? 0x500 : 0x501, 0);
        step &= ~(PB_LEFT | PB_RIGHT);
    }
    /* the list hides all its cards (at y 600) while the pointer is outside it (on Back to Menu): a step into
     * it first rests the hidden pointer between two cards, and is taken once the list has laid them out */
    static int probe_wait;
    static uint32_t probe_step;
    if (probe_wait && --probe_wait == 0) step = probe_step;
    else if (probe_wait) step = 0;
    if (f && step && !f->in_list && list_scrolls && list_cards && (step & (PB_UP | PB_DOWN)) && !scroll_gliding) {
        int shown = 0;
        for (int i = 0; i < n_spots; i++) shown |= spots[i].in_list;
        if (!shown) {
            point_at(167.f, 300.f);
            probe_wait = 4;
            probe_step = step;
            step = 0;
        }
    }
    /* a pressed button stays where it is until A is let go */
    if (f && !(mouse_buttons & 1) && step && !scroll_gliding) {
        if (f->kind == SPOT_SLIDER && (step & (PB_LEFT | PB_RIGHT))) {
            /* the slider's bar: a click there sets the value (SexyApp's Slider::MouseDown); 0.05 a step */
            double v = slider_val(f->obj) + ((step & PB_LEFT) ? -0.05 : 0.05);
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            float x = f->x + (float)v * f->w;
            x = x < f->x + 1 ? f->x + 1 : x > f->x + f->w - 1 ? f->x + f->w - 1 : x;
            click_game(x, f->y + f->h / 2);
        } else {
            int dxs = (step & PB_RIGHT) ? 1 : (step & PB_LEFT) ? -1 : 0, dys = (step & PB_DOWN) ? 1 : (step & PB_UP) ? -1 : 0;
            /* in a scrolling list, a step from one of its buttons stays in the list (scrolling it) until
             * its end, and only then leaves it (for Back to Menu) */
            Spot *t = list_scrolls && list_more && f->in_list && dys ? step_spot_in(f, dxs, dys, 1) : step_spot(f, dxs, dys);
            /* the second page's layout: right from Challenges is the mailbox, not the credits sign above it */
            if (screen_kind == SCR_SIDE && id_of(f) == 1 && dxs > 0) {
                Spot *mb = spot_button(2);
                if (mb) t = mb;
            }
            /* up from the Zen Garden is the Multiplayer sign (id 111), not the trophy beside it */
            if (screen_kind == SCR_MENU && id_of(f) == 109 && dys < 0 && !dxs) {
                Spot *mp = spot_button(111);
                if (mp) t = mp;
            }
            /* ... and up from Back is Challenges (the stone above it), not the Event pumpkin beside it */
            if (screen_kind == SCR_SIDE && id_of(f) == 0 && dys < 0 && !dxs) {
                Spot *ch = spot_button(1);
                if (ch) t = ch;
            }
            if (t) {
                f = t;
                focus_obj = t->obj;
                log_focus("moved", t);
                point_at(t->x + t->w / 2, t->y + t->h / 2);
            } else if (list_scrolls && list_more && dys && f->in_list) {
                /* nothing further that way on screen: glide the list a row (the rows' own spacing) and take
                 * that row's button in the same column. The list reuses its buttons for the rows that come
                 * into view, so the focus is found again by its place in the list, not by its button. */
                float before = list_scroll(), pitch = 0;
                for (int i = 0; i < n_spots; i++) {
                    float d = fabsf(spots[i].y - f->y);
                    if (spots[i].in_list && fabsf(spots[i].x - f->x) < 4 && d > 20 && (pitch == 0 || d < pitch)) pitch = d;
                }
                if (pitch == 0) pitch = 120;
                list_cell_x = f->x + f->w / 2;
                list_cell_y = f->y + f->h / 2 + before * 10.f;
                scroll_target = before + pitch / 10.f * (float)dys;
                if (scroll_target < 0) scroll_target = 0;
                scroll_gliding = scroll_target != before;
                scroll_dir = dys;
                if (!scroll_gliding) { /* the end of the list: on to what is beyond it (Back to Menu) */
                    Spot *t2 = step_spot(f, 0, dys);
                    if (t2) { f = t2; focus_obj = t2->obj; log_focus("past the list", t2); }
                    scroll_dir = 0;
                }
            } else if (rt_verbose && !(list_scrolls && dys)) {
                for (int i = 0; i < n_spots; i++)
                    rt_log("[pad]   no step: spot %d kind %d at %.0f,%.0f %.0fx%.0f\n", i, spots[i].kind, spots[i].x, spots[i].y,
                           spots[i].w, spots[i].h);
            }
            if (!t && dxs > 0 && screen_kind == SCR_MENU) {
                if (page_click(112, 0)) nav_freeze = 1.0f; /* the right edge of the main menu: MORE, the second page */
            } else if (!t && dxs < 0 && screen_kind == SCR_SIDE) {
                if (page_click(0, 0)) nav_freeze = 1.0f;   /* the left edge of the second page: back to the first */
            }
        }
    }
    static int glide_more;
    if (scroll_dir && !scroll_gliding && f && !(mouse_buttons & 1)) {
        Spot *t = list_cell(scroll_dir);
        float at = list_scroll();
        if (!t && glide_more < 2 && fabsf(at - scroll_target) < 0.01f) {
            /* the row came in not quite whole: a little further */
            glide_more++;
            scroll_target = at + 4.f * (float)scroll_dir;
            scroll_gliding = 1;
        } else {
            if (!t) t = step_spot(f, 0, scroll_dir); /* the list was at its end */
            if (t) {
                f = t;
                focus_obj = t->obj;
                log_focus("scrolled to", t);
            }
            scroll_dir = 0;
            glide_more = 0;
        }
    }
    /* (not once a page turn has started: the pointer stays parked off the screen, or the second page's
     * Challenges stone, sliding in under MORE's place, lit up as it passed) */
    if (f && almanac_card(f)) {
        /* an Almanac card: the chooser's bracket marks it (pad_almanac_drawn), the pointer rests off the screen
         * (the game's hover look, a whitened card, came on top of it); A's press goes to the card */
        if (!pending_up && !hold_pb && !(mouse_buttons & 1) && rt_mouse_x != -400) {
            rt_mouse_x = rt_mouse_y = -400;
            rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
        }
    } else if (f && nav_freeze <= 0 && !page_pb && !pending_up && !hold_pb && !prev_on_release && !probe_wait && ((int)(160.f + (f->x + f->w / 2) * 1.2f) != (int)cur_x || (int)((f->y + f->h / 2) * 1.2f) != (int)cur_y))
        point_at(f->x + f->w / 2, f->y + f->h / 2);
    if (f && !defaulted_to_area && (screen_kind == SCR_MENU || screen_kind == SCR_SIDE)) {
        page_focus[screen_kind == SCR_SIDE] = f->obj;
        page_focus_id[screen_kind == SCR_SIDE] = id_of(f);
    }
    lf_on = f && f->in_list && list_scrolls;
    if (lf_on && !scroll_gliding) lf_x = f->x + f->w / 2, lf_cy = f->y + f->h / 2 + list_scroll() * 10.f;
    if (!scroll_gliding) draw_focus(f); /* (mid-glide the list's buttons are changing rows) */
    if (!lit_frame) sign_unlit(); /* (no sign focused this frame: the lit one gets its own picture back) */
    lit_frame = 0;
    menu_badges();
    /* A on the text box (only when the highlight is on it): the Switch keyboard */
    static int kbd_wait;
    if (kbd_wait && --kbd_wait == 0 && rt_text_keyboard && game_focused_edit()) rt_text_keyboard();
    if ((down & PB_A) && f && f->kind == SPOT_EDIT) {
        /* the box focused first (a click on it: there are two on the Multiplayer screen), then its keyboard */
        if (game_focused_edit() != f->obj) {
            point_at(f->x + f->w / 2, f->y + f->h / 2);
            mouse(WM_LBUTTONDOWN);
            mouse(WM_LBUTTONUP);
            kbd_wait = 3;
        } else if (rt_text_keyboard) {
            rt_text_keyboard();
        }
        prev |= PB_A;
        return;
    }
    /* the title screen starts the game on the mouse going down: the press goes in whole on A's release, as
     * PvZ Touch's start button (pad_title_held shows it pressed meanwhile) */
    if (screen_obj && game_rtti_is(screen_obj, "TitleScreen")) {
        if (down & PB_A) pad_title_held = 1;
        if ((up & PB_A) && pad_title_held) {
            pad_title_held = 0;
            if (f) point_at(f->x + f->w / 2, f->y + f->h / 2);
            else point_at(400, 300);
            mouse(WM_LBUTTONDOWN);
            mouse(WM_LBUTTONUP);
        }
        down &= ~PB_A, up &= ~PB_A;
    } else {
        pad_title_held = 0;
    }
    /* A on the jukebox's album already playing (a disabled button, kept as a place to stand): nothing */
    if ((down & PB_A) && f && f->kind == SPOT_BUTTON && !widget_live(f->obj)) { down &= ~PB_A; prev |= PB_A; }
    if ((down & PB_A) && f && is_versus_screen() && f->kind == SPOT_BUTTON) {
        int id = id_of(f);
        if (id == 800 || id == 900) { down &= ~PB_A; prev |= PB_A; } /* (the devices stay keyboard) */
        if (id == 700) {
            down &= ~PB_A;
            prev |= PB_A;
            if (rt_controller_applet) rt_controller_applet();
        }
    }
    if (down & PB_A) {
        if (f) point_at(f->x + f->w / 2, f->y + f->h / 2);
        else point_at(400, 300); /* no buttons (Crazy Dave's talk): a click anywhere */
        mouse_buttons |= 1;
        mouse(WM_LBUTTONDOWN);
    }
    if ((up & PB_A) && (mouse_buttons & 1)) {
        mouse_buttons &= ~1;
        mouse(WM_LBUTTONUP);
        /* MORE / the second page's Back: the pages slide, no highlight until they settle */
        if (f && f->kind == SPOT_BUTTON && ((screen_kind == SCR_MENU && id_of(f) == 112) || (screen_kind == SCR_SIDE && id_of(f) == 0))) {
            nav_freeze = 1.0f;
            rt_mouse_x = rt_mouse_y = -400; /* (its hover look would stay lit while the page slides) */
            rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
        }
    }
    /* the pages of a screen: ZL / ZR (and L / R) */
    uint32_t prevp = down & (PB_ZL | PB_L), nextp = down & (PB_ZR | PB_R), pg = prevp | nextp;
    if (pg && !(mouse_buttons & 1)) {
        if (screen_kind == SCR_STORE) {
            /* Next is a button; Prev is drawn by the shop itself over its 0x0 widget */
            if (prevp) {
                /* the shop turns back on the mouse going down: the pointer rests on Prev while ZL is held and
                 * the click goes on its release, as everything else works */
                prev_on_release = pg;
                release_x = 300.f, release_y = 440.f;
                point_at(300.f, 440.f);
            } else {
                click_button_id(102, pg);
            }
        } else if (is_music_screen()) {
            /* the jukebox's track arrows (back 30003 / forward 30001) skip as the mouse goes down, and again while
             * it is held: pointed at while ZL / ZR is held (their lit look), clicked once on release */
            uint32_t ab = screen_obj ? button_with_id(screen_obj, prevp ? 30003 : 30001) : 0;
            Spot *as = ab ? spot_of(ab) : NULL;
            if (as) {
                prev_on_release = pg;
                release_x = as->x + as->w / 2, release_y = as->y + as->h / 2;
                point_at(release_x, release_y);
            }
        } else if (screen_kind == SCR_ALMANAC && (int)g_rd32(screen_obj + 0x238) != 0) {
            click_area(prevp ? 0x211 : 0x212, pg);
        } else if (screen_kind == SCR_MENU && nextp) {
            if (page_click(112, pg)) nav_freeze = 1.0f;
        } else if (screen_kind == SCR_SIDE && prevp) {
            if (page_click(0, pg)) nav_freeze = 1.0f;
        } else if (screen_kind == SCR_AWARD) {
        add_area(0x510, 300, 520, 200, 70, 0); /* Roll Credits! (the award's own button, drawn by it) */
        add_area(0x502, 679, 18, 105, 22, 0);  /* MAIN MENU */
    } else if (screen_kind == SCR_MAIL) {
            /* the letters' Last / Next Page turn as the mouse goes down: lit while held, turned on release */
            for (int i = 0; i < n_spots; i++)
                if (spots[i].obj == (0xFE000000u | (prevp ? 0x500u : 0x501u))) {
                    prev_on_release = pg;
                    release_x = spots[i].x + spots[i].w / 2, release_y = spots[i].y + spots[i].h / 2;
                    point_at(release_x, release_y);
                    break;
                }
        }
    }
    Spot *shop_tag;
    if ((down & PB_Y) && is_music_screen() && !top_dialog() && !(mouse_buttons & 1) && (shop_tag = spot_button(30010))) {
        focus_obj = shop_tag->obj; /* the jukebox's Shop tag */
        press_hold(PB_Y, shop_tag->x + shop_tag->w / 2, shop_tag->y + shop_tag->h / 2);
    }
    uint32_t backp = down & (PB_B | PB_MINUS);
    if (backp && !(mouse_buttons & 1)) {
        Spot *bk = NULL;
        if (screen_kind == SCR_SIDE) { if (page_click(0, backp)) nav_freeze = 1.0f; }
        else if (screen_kind == SCR_STORE) click_button_id(100, backp);
        else if (screen_kind == SCR_ALMANAC) click_area((int)g_rd32(screen_obj + 0x238) != 0 ? 0x210 : 0x213, backp);
        /* (the menu's Help, award type 2: its Main Menu is the bottom button) */
        else if (screen_kind == SCR_AWARD && g_rd32(screen_obj + 0x9c) == 2) click_area(0x510, backp);
        else if (screen_kind == SCR_MAIL || screen_kind == SCR_AWARD) click_area(0x502, backp);
        else if (screen_kind == SCR_CHALLENGE) click_button_id(100, backp);
        else if ((down & PB_B) && (bk = back_spot())) {
            focus_obj = bk->obj;
            press_hold(PB_B, bk->x + bk->w / 2, bk->y + bk->h / 2);
        } else if ((down & PB_B) && !board()) {
            tap(0x1B); /* Escape: the game's own back where it has one (the help screen) */
        }
    }
    if (down & PB_PLUS) {
        /* the Menu button wherever a level shows one (plant select, versus); Escape elsewhere */
        if (board() && !top_dialog()) press_hold(PB_PLUS, 737.f, 14.f);
        else tap(hotkeys[HK_MENU]);
    }
    if (rt_verbose && (down & (PB_A | PB_B)))
        rt_log("[pad] menu %s: %d spots, focus %s \"%s\"\n", (down & PB_A) ? "A" : "B", n_spots,
               f ? game_rtti_name(f->obj) : "-", f ? f->label : "");
}

/* ---------------- in a level: hold B to dig, the B badge ----------------
 * As on the Xbox 360 edition (and PvZ Touch): B held over a plant fills the dig meter (the shovel button
 * with a blue ring, from 20 frames on), and the plant comes up when it is full, 1.25 s, B still held; an
 * early release digs nothing. The digging is the PC shovel's, one plant at a time (shovel_dig_p). */
/* the Xbox 360 / TV timing (PvZ Touch): the meter appears after 20 updates (0.2 s) of B; it fills a little quicker
 * than theirs (1.25 s in all, not 1.5: a tester's request) */
#define DIG_SHOW 0.2f
#define DIG_FULL 1.25f
static float dig_ts[2];
static int dig_dones[2];
#define dig_t dig_ts[0]
#define dig_done dig_dones[0]

/* The dig itself is the PC shovel's: Board::MouseDownWithTool (0x74c9f0, thunk 0x706e53: x, y, click count, cursor
 * type 6 the shovel) at the plant's middle, as a shovel click there. It digs one plant, the top one first (the
 * plant, then a pumpkin round it, then the pot or lily pad under it: ToolHitTest's order), counts it and moves
 * Crazy Dave's shovel lesson on. The DLC's keyboard dig key took up every plant in the cell at once (a plant
 * and its flower pot: the tester's report). */
#define BOARD_MOUSE_DOWN_WITH_TOOL 0x706e53u
static uint32_t plant_under_cursor_p(int p);
static int shovel_dig_p(int p)
{
    uint32_t b = board(), pl = plant_under_cursor_p(p);
    if (!b || !valid(pl)) return 0;
    /* (GameObject +8 / +0xc: its place on the board; its cell is 80 x 100, the middle 40, 40 in) */
    uint32_t args[4] = {g_rd32(pl + 8) + 40u, g_rd32(pl + 0xc) + 40u, 1, 6};
    guest_this(GAME_FN(BOARD_MOUSE_DOWN_WITH_TOOL), b, 4, args);
    return 1;
}

static int kb_cursor(float *x, float *y);
static int kb_cursor_p(int p, float *x, float *y);
int vase_mode(void);
static int cursor_cell_mid(float *x, float *y);
/* a plant in the keyboard cursor's cell (Board::PixelToGridX/Y 0x7523d0 / 0x7524f0, then the board's plant
 * at that column and row, 0x741ff0) */
static int plant_under_cursor(void) { return plant_under_cursor_p(0) != 0; }
/* (the plant, 0 none) */
static uint32_t plant_under_cursor_p(int p)
{
    uint32_t b = board();
    float x, y;
    if (!b || !kb_cursor_p(p, &x, &y)) return 0;
    uint32_t xy[2] = {(uint32_t)(int32_t)x, (uint32_t)(int32_t)y};
    uint32_t col = guest_this(GAME_FN(0x7523d0u), b, 2, xy), row = guest_this(GAME_FN(0x7524f0u), b, 2, xy);
    if ((int32_t)col < 0 || (int32_t)row < 0 || col > 12 || row > 6) return 0;
    uint32_t cr[2] = {col, row};
    return guest_this(GAME_FN(0x741ff0u), b, 2, cr);
}

static int zombie_bank(void);
/* the board shows its shovel (Board +0x564d, mShowShovel: Board::DrawShovel draws the shovel's slot only then; off
 * in the first Adventure levels until Crazy Dave hands it over). The DLC's keyboard dig key doesn't look at it. */
static int shovel_shown(void)
{
    uint32_t b = board();
    static int test = -1; /* (Mac test: RT_NO_SHOVEL clears the flag, as before the shovel is unlocked) */
    if (test < 0) test = getenv("RT_NO_SHOVEL") != NULL;
    if (test && b) WR8_G(b + 0x564d, 0);
    static int sun = -1; /* (Mac test: RT_SUN keeps both players' sun at 9000: Board +0x5594 / +0x5598) */
    if (sun < 0) sun = getenv("RT_SUN") != NULL;
    if (sun && b) g_wr32(b + 0x5594, 9000), g_wr32(b + 0x5598, 9000);
    return b && RD8_G(b + 0x564d);
}
/* B held over a plant: keyboard player p's dig key once the meter fills (none for a zombie bank, nor before the
 * shovel is unlocked: digging then broke the Adventure's order) */
static void dig_update_p(int p, int held, float dt)
{
    float *t = &dig_ts[p];
    int *done = &dig_dones[p];
    /* nothing to dig: no shovel sound, no meter (it starts when the cursor reaches a plant, B still held) */
    if (held && !*done && (!plant_under_cursor_p(p) || zombie_bank() || !shovel_shown())) held = 0;
    if (!held) {
        *t = 0;
        *done = 0;
        return;
    }
    if (*t == 0) {
        /* the shovel's clink as it is taken up (the DLC's Board::PickUpTool shovel case, 0x7508e0: the
         * wooden shovel's sound when 0x8da980 says so, else FOLEY_SHOVEL) */
        uint32_t app = rt_app_object();
        if (valid(app)) {
            uint32_t foley = (guest_this(GAME_FN(0x8da980u), app, 0, NULL) & 0xff) ? 173 : 76;
            guest_this(GAME_FN(0x8dc5b0u), app, 1, &foley);
        }
    }
    *t += dt;
    if (*t >= DIG_FULL && !*done) {
        *done = 1;
        rt_log("[pad] dig: player %d B held %.2f s, digging %s\n", p + 1, *t, shovel_dig_p(p) ? "one plant" : "nothing");
    }
}
static void dig_update(int held, float dt) { dig_update_p(0, held, dt); }

/* the keyboard player's cursor (Board +0x178, its position +0x24 / +0x28 in board points), 0 if none */
static int kb_cursor_p(int p, float *x, float *y);
static int kb_cursor(float *x, float *y) { return kb_cursor_p(0, x, y); }
/* keyboard player p's cursor (Board +0x178 + 4 p): its place (+0x24 / +0x28) */
static int kb_cursor_p(int p, float *x, float *y)
{
    uint32_t b = board(), cur = b ? g_rd32(b + 0x178 + 4 * (uint32_t)p) : 0;
    if (!valid(cur)) return 0;
    memcpy(x, G2H(cur + 0x24), 4);
    memcpy(y, G2H(cur + 0x28), 4);
    return *x > -100 && *x < 900 && *y > -100 && *y < 700;
}

/* The flashlight (Adventure 7-5 and game mode 108: Board's render item 15, 0x737030) is centred on the mouse
 * (WidgetManager +0xe4 / +0xe8), which the port parks off the screen while the controller plays: then it is the
 * keyboard cursor's place, the middle of its cell when it rests (the DLC rests it 33, 45 into an 80 x 100 cell),
 * moved by the right stick's free look (flash_look_update) */
static float look_x, look_y;
int pad_light_point(int *x, int *y)
{
    float kx, ky;
    if (touch_mode || is_versus() || !kb_cursor(&kx, &ky)) return 0;
    *x = (int)(kx + 7.f + look_x);
    *y = (int)(ky + 5.f + look_y);
    return 1;
}
/* Free look in the flashlight levels (Board's test, 0x756180): the right stick moves the light away from the cursor,
 * faster than the cursor goes, to look over the lawn. It stays where it is left; moving the cursor (left stick or
 * D-pad, `moving`) brings it back at once. It stays over the widescreen picture (-111 .. 911 board points). */
static void flash_look_update(const PadIn *p, int moving, float dt)
{
    uint32_t b = board();
    float kx, ky;
    if (!b || touch_mode || !p->connected || !kb_cursor(&kx, &ky) || !(guest_this(GAME_FN(0x756180u), b, 0, NULL) & 0xff)) {
        look_x = look_y = 0;
        return;
    }
    if (moving) {
        float k = expf(-dt * 18.f);
        look_x *= k, look_y *= k;
        if (fabsf(look_x) < 0.5f && fabsf(look_y) < 0.5f) look_x = look_y = 0;
        return;
    }
    float sx = p->rx, sy = -p->ry, m = sqrtf(sx * sx + sy * sy);
    if (m < 0.2f) return;
    float t = (m - 0.2f) / 0.8f;
    t = t > 1.f ? 1.f : t;
    float v = (150.f + 1250.f * t * sqrtf(t)) / m * dt; /* up to 1400 points a second */
    look_x += sx * v, look_y += sy * v;
    float lx = kx + 7.f + look_x, ly = ky + 5.f + look_y;
    if (lx < -111.f) look_x += -111.f - lx;
    if (lx > 911.f) look_x -= lx - 911.f;
    if (ly < 0.f) look_y -= ly;
    if (ly > 600.f) look_y -= ly - 600.f;
}

extern int game_belt_level;
void game_kb_cursor_log(void)
{
    float x, y;
    if (!kb_cursor(&x, &y)) return;
    /* (and its packet index, the bank's packets' seed types, the board's +0x5824: BOMB All Together's count) */
    uint32_t b = board(), cur = g_rd32(b + 0x178), bank = g_rd32(b + 0x148);
    char row[160];
    int n = 0, cnt = valid(bank) ? (int)g_rd32(bank + 0x24) : 0;
    for (int i = 0; i < cnt && i < 10; i++) n += snprintf(row + n, sizeof row - n, " %x", (unsigned)g_rd32(bank + 0x5c + 0x54 * (uint32_t)i));
    rt_log("[kbcur] %.1f,%.1f packet %d of%s (bombs %d)\n", x, y, (int)g_rd32(cur + 0x48), row, (int)g_rd32(b + 0x5824));
}

/* X, as PvZ Touch: the mini-games' own action. The Slot Machine's handle (Challenge::SlotMachineGetHandleRect
 * 473,0 55x80) anywhere on the lawn; elsewhere the mouse button at the cursor, held as long as X is and following
 * it: the mallet (Whack a Zombie), a vase (Vasebreaker), feeding (Zombiquarium), a twist (Beghouled Twist), a
 * plant dragged to its neighbour (Beghouled). The pointer is parked again after. */
static void raw_mouse(float gx, float gy, uint32_t msg, int buttons);
/* Vasebreaker: the seed bank is kept above the screen (Board +0x148: y +0xc, height +0x14; -87, 87) while playing;
 * not the gardens */
int vase_mode(void)
{
    uint32_t bd = board(), bank = bd ? g_rd32(bd + 0x148) : 0, app = rt_app_object();
    int m = game_mode();
    return valid(bank) && (int32_t)(g_rd32(bank + 0xc) + g_rd32(bank + 0x14)) <= 0 && m != 50 && m != 140 && !is_versus() && valid(app) && g_rd32(app + 0x818) == 3;
}
/* the middle of the keyboard cursor's cell (the lawn's cells, 80 x 100 from 40, 80): a click there hits the vase or
 * packet in it, wherever in the cell the free cursor sits */
static int cursor_cell_mid(float *x, float *y)
{
    uint32_t b = board();
    float kx, ky;
    if (!b || !kb_cursor(&kx, &ky)) return 0;
    int col = (int)floorf((kx - 40.f) / 80.f), row = (int)floorf((ky - 80.f) / 100.f);
    if (col < 0 || row < 0 || col > 8 || row > 5) return 0;
    *x = 40.f + 80.f * (float)col + 40.f;
    *y = 80.f + 100.f * (float)row + 50.f;
    return 1;
}

/* The D-pad on the lawn: keyboard player p's cursor jumps to the next cell (dx, dy) at once, as the menus' highlight
 * steps (the stick still steers it at any angle). It lands where the DLC rests it in a cell once it stops moving
 * (Board::Update eases it there over its +0x148 count, from 100 as it moves): Board::GridToPixelX / Y (0x742f20 /
 * 0x742f70) + 33, + 45 (+ 60 on the roof, StageHasRoof 0x756390), so it doesn't drift after. Its cell now is the
 * DLC's own, PixelToGridX / YKeepOnBoard (0x7523d0 / 0x7524f0); a row is there if PixelToGridY gives it back (it
 * clamps to the lawn's last: 5 rows, 6 by the pool). */
static int lawn_cell(uint32_t b, float x, float y, int *col, int *row)
{
    uint32_t xy[2] = {(uint32_t)(int32_t)x, (uint32_t)(int32_t)y};
    *col = (int32_t)guest_this(GAME_FN(0x7523d0u), b, 2, xy);
    *row = (int32_t)guest_this(GAME_FN(0x7524f0u), b, 2, xy);
    return *col >= 0 && *col <= 8 && *row >= 0 && *row <= 5;
}
static void lawn_step(int p, int dx, int dy)
{
    uint32_t b = board(), cur = b ? g_rd32(b + 0x178 + 4 * (uint32_t)p) : 0;
    float x, y;
    int col, row;
    if (!valid(cur) || !kb_cursor_p(p, &x, &y) || !lawn_cell(b, x, y, &col, &row)) return;
    int tc = col + dx, tr = row + dy;
    if (tc < 0 || tc > 8 || tr < 0 || tr > 5) return;
    uint32_t cr[2] = {(uint32_t)tc, (uint32_t)tr};
    int roof = guest_this(GAME_FN(0x756390u), b, 0, NULL) & 0xff;
    float nx = (float)((int32_t)guest_this(GAME_FN(0x742f20u), b, 2, cr) + 33);
    float ny = (float)((int32_t)guest_this(GAME_FN(0x742f70u), b, 2, cr) + (roof ? 60 : 45));
    int c2, r2;
    if (!lawn_cell(b, nx, ny, &c2, &r2) || c2 != tc || r2 != tr) return; /* (no such row) */
    memcpy(G2H(cur + 0x24), &nx, 4);
    memcpy(G2H(cur + 0x28), &ny, 4);
    g_wr32(cur + 0x148, 100); /* (as the DLC's own move sets it) */
    if (rt_verbose) rt_log("[pad] lawn step %d,%d: cell %d,%d -> %d,%d at %.0f,%.0f\n", dx, dy, col, row, tc, tr, nx, ny);
}
/* the D-pad's steps for player p on the lawn: a press, then a held one again after 0.38 s and every 0.11 s
 * (the menus' D-pad) */
static void lawn_dpad(int p, uint32_t dp, float dt)
{
    static uint32_t prev_dp[2], last_poll[2];
    static float repeat[2];
    /* not run last poll (a menu, the chooser): what is still held from there is not a press here */
    if (last_poll[p] + 1 != pad_poll_no) prev_dp[p] = dp;
    last_poll[p] = pad_poll_no;
    uint32_t step = dp & ~prev_dp[p];
    if (dp && dp == prev_dp[p]) {
        repeat[p] -= dt;
        if (repeat[p] <= 0) { step = dp; repeat[p] = 0.11f; }
    } else {
        repeat[p] = 0.38f;
    }
    prev_dp[p] = dp;
    if (step) lawn_step(p, ((step & PB_RIGHT) != 0) - ((step & PB_LEFT) != 0), ((step & PB_DOWN) != 0) - ((step & PB_UP) != 0));
}
/* I, Zombie (LawnApp::IsIZombieLevel, 0x6fce62) */
uint32_t pad_board(void);
int pad_izombie(void)
{
    uint32_t app = rt_app_object();
    return valid(app) && (guest_this(GAME_FN(0x6fce62u), app, 0, NULL) & 0xff);
}
uint32_t pad_board(void) { return board(); }
static void x_action(uint32_t held, uint32_t pressed, int mode)
{
    static int down;
    static float lx, ly;
    float kx, ky;
    int have = vase_mode() ? cursor_cell_mid(&kx, &ky) : kb_cursor(&kx, &ky);
    if (pressed && mode == 25) {
        raw_mouse(500, 40, WM_MOUSEMOVE, 0);
        raw_mouse(500, 40, WM_LBUTTONDOWN, 1);
        raw_mouse(500, 40, WM_LBUTTONUP, 0);
        rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
        return;
    }
    if (mode == 25) return;
    if (pressed && have && !down) {
        down = 1;
        lx = kx, ly = ky;
        raw_mouse(kx, ky, WM_MOUSEMOVE, 0);
        raw_mouse(kx, ky, WM_LBUTTONDOWN, 1);
    } else if (down && held && have && ((int)kx != (int)lx || (int)ky != (int)ly)) {
        lx = kx, ly = ky;
        raw_mouse(kx, ky, WM_MOUSEMOVE, 1);
    } else if (down && !held) {
        down = 0;
        raw_mouse(lx, ly, WM_LBUTTONUP, 0);
        rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
    }
}

/* the seed bank holds zombies (Zombies vs. Zombies, I, Zombie): no shovel. Its first packet (bank +0x5c) is a
 * zombie: the DLC's own IsZombieSeedType (0x915df0), or the gravestone, or Zombies vs. Zombies' newer zombies */
/* keyboard player p's bank (Board +0x148 / +0x14c) holds zombies (versus' zombie side) */
static int bank_is_zombie(int p)
{
    uint32_t bd = board(), bank = bd ? g_rd32(bd + 0x148 + 4 * (uint32_t)p) : 0;
    if (!valid(bank)) return 0;
    uint32_t t0 = g_rd32(bank + 0x5c);
    return t0 == 0x59 || (t0 < 0x200 && (guest_this(GAME_FN(0x915df0u), 0, 1, &t0) & 0xff));
}
static int zombie_bank(void);
int pad_izombie(void);
int pad_zombie_bank(void) { return zombie_bank(); }
static int zombie_bank(void)
{
    uint32_t bd = board(), bank = bd ? g_rd32(bd + 0x148) : 0;
    if (!valid(bank) || is_versus()) return 0;
    uint32_t t0 = g_rd32(bank + 0x5c);
    return t0 == 0x59 || (game_mode() == 102 && t0 < 0x200) || pad_izombie() || (guest_this(GAME_FN(0x915df0u), 0, 1, &t0) & 0xff);
}

/* The sun magnet (ZR held), as PvZ Touch's vacuum (its mod's switchhud::VacuumUpdate on the engine's console
 * motion "from near cursor", Coin::UpdateFall 0x199100 in libGameMain.so): while held (and 0.15 s after), every
 * sun and coin on the lawn flies to the player's cursor, from rest, 0.1024 px faster each 1/100 s up to 9.6 px
 * (960 px/s), homing on the live cursor; within 35 px it is collected (Coin::Collect, 0x783480: it then flies to
 * its bank as the game's own). The zombie modes' brains are the same coins. The DLC (a PC game) has no such
 * motion, so the coins are moved here: the board's coins DataArray (+0xe8 block, +0xec used, 0xd8 bytes each),
 * Coin +0x24 / +0x28 place, +0x2c / +0x30 velocity, +0x10 / +0x14 size, +0x38 dead, +0x48 ground y, +0x50 being
 * collected, +0x58 type (1-6 money and sun; the zombie modes' brains are 4). The coin's own fall is stopped while
 * it is pulled (no velocity, its ground where it is). */
static struct { uint32_t coin; float speed; } vacs[2][64];
static int vac_owner(uint32_t co, int p)
{
    for (int k = 0; k < 64; k++)
        if (vacs[!p][k].coin == co) return 1;
    return 0;
}
static int vacuum_takes(int p, uint32_t type)
{
    if (type < 1 || type > 6) return 0;
    if (!is_versus()) return 1;
    /* versus: each side its own (the plant side's sun, the zombie side's brains): sun 4-6 vs the side */
    return 1;
}
static void vacuum_update(int p, int held, float dt)
{
    __typeof__(vacs[0][0]) *vac = vacs[p]; /* (each player's own pull) */
    static float linger[2];
    if (held) linger[p] = 0.15f;
    else if (linger[p] > 0) linger[p] -= dt;
    uint32_t bd = board();
    float cx, cy;
    if (!bd || !kb_cursor_p(p, &cx, &cy)) return;
    uint32_t blk = g_rd32(bd + 0xe8), used = g_rd32(bd + 0xec);
    if (!valid(blk) || used > 1024) return;
    for (uint32_t i = 0; i < used; i++) {
        uint32_t co = blk + 0xd8 * i;
        if (!g_rd32(co) || RD8_G(co + 0x38) || RD8_G(co + 0x50)) continue;
        if (!vacuum_takes(p, g_rd32(co + 0x58))) continue;
        if (vac_owner(co, p)) continue; /* (the other player is pulling it) */
        int k, freek = -1;
        for (k = 0; k < 64 && vac[k].coin != co; k++)
            if (!vac[k].coin && freek < 0) freek = k;
        if (k == 64) { /* not being pulled yet: only while the trigger is (or just was) held */
            if (linger[p] <= 0 || freek < 0) continue;
            k = freek;
            vac[k].coin = co, vac[k].speed = 0;
        }
        float x, y, w = (float)(int32_t)g_rd32(co + 0x10), h = (float)(int32_t)g_rd32(co + 0x14);
        memcpy(&x, G2H(co + 0x24), 4);
        memcpy(&y, G2H(co + 0x28), 4);
        float dx = cx - w / 2 - x, dy = cy - h / 2 - y, d2 = dx * dx + dy * dy;
        if (d2 < 1225.f) {
            vac[k].coin = 0;
            guest_this(GAME_FN(0x783480u), co, 0, NULL); /* Coin::Collect */
            continue;
        }
        vac[k].speed += 640.f * dt;
        if (vac[k].speed > 600.f) vac[k].speed = 600.f;
        float d = sqrtf(d2), step = vac[k].speed * 1.6f * dt;
        if (step > d) step = d;
        x += dx / d * step, y += dy / d * step;
        const float zero = 0;
        memcpy(G2H(co + 0x24), &x, 4);
        memcpy(G2H(co + 0x28), &y, 4);
        memcpy(G2H(co + 0x2c), &zero, 4);
        memcpy(G2H(co + 0x30), &zero, 4);
        memcpy(G2H(co + 0x48), &y, 4); /* (its ground where it is: it stops falling) */
    }
    for (int k = 0; k < 64; k++) /* coins that went (collected, or the level changed) */
        if (vac[k].coin && (vac[k].coin < blk || vac[k].coin >= blk + 0xd8 * used || RD8_G(vac[k].coin + 0x38) || RD8_G(vac[k].coin + 0x50)))
            vac[k].coin = 0;
}

static void dig_meter(int p)
{
    float cx, cy;
    if (dig_ts[p] >= DIG_SHOW && !dig_dones[p] && kb_cursor_p(p, &cx, &cy)) {
        /* the meter's top-left at the cursor, as the TV edition draws it */
        float frac = (dig_ts[p] - DIG_SHOW) / (DIG_FULL - DIG_SHOW);
        ov_image(OV_METER_EMPTY, 0, 0, 46, 46, cx, cy, 46, 46, 0xffffffffu);
        ov_pie(OV_METER_FULL, cx + 23, cy + 23, 22, frac > 1 ? 1 : frac, 0xffffffffu);
    }
}

/* versus: the board's own buttons at the top right (its child widgets: x > 560, y < 120), top first. The
 * first is Menu (+), the next the DLC's pause / flag buttons (- and then X... as there are) */
static int versus_buttons(uint32_t *out, int max)
{
    uint32_t bd = board();
    if (!bd) return 0;
    uint32_t head = g_rd32(bd + 8), n = g_rd32(bd + 0xc);
    if (!valid(head) || n > 200) return 0;
    int got = 0;
    uint32_t node = g_rd32(head);
    for (uint32_t i = 0; i < n && valid(node) && node != head && got < max; i++, node = g_rd32(node)) {
        uint32_t w = g_rd32(node + 8);
        if (!valid(w) || !RD8_G(w + 0x58) || !game_rtti_is(w, "ButtonWidget@Sexy")) continue;
        int32_t x = (int32_t)g_rd32(w + 0x34), y = (int32_t)g_rd32(w + 0x38);
        if (x < 560 || y > 120) continue;
        int k = got++;
        while (k > 0 && (int32_t)g_rd32(out[k - 1] + 0x38) > y) { out[k] = out[k - 1]; k--; }
        out[k] = w;
    }
    return got;
}
/* versus plant select: its Menu button (a GameButton, SeedChooserScreen +0xac: x +8, y +0xc, width +0x10,
 * height +0x14; 681,-10 117x46) sits at the top right over the zombie side's brains count; widescreen shows 133
 * more on the right, so it is drawn in that top-right corner, with its + badge (pad_chooser_menu_draw). Its
 * place stays the game's own: a press (+, or a tap where it is drawn: pad_touch_redirect) goes there. */
static uint32_t versus_chooser_menu(void)
{
    uint32_t sc = is_versus() ? seed_chooser() : 0, mb = sc ? g_rd32(sc + 0xac) : 0;
    return valid(mb) ? mb : 0;
}
/* ... also while the plant select slides in and while it slides away (the lawn panning back after Let's Rock): its
 * Menu is drawn then too, and at its own place it was over the brains count (the user's pan screenshot) */
static uint32_t versus_chooser_menu_shown(void)
{
    uint32_t app = rt_app_object();
    if (!is_versus() || !valid(app) || g_rd32(app + 0x818) == 3) return 0;
    uint32_t sc = g_rd32(app + 0x78c), mb = game_rtti_is(sc, "SeedChooserScreen") ? g_rd32(sc + 0xac) : 0;
    return valid(mb) ? mb : 0;
}
/* the corner's x for a button w wide (room for the badge on its edge) */
static int32_t menu_corner_x(int32_t w) { return 933 - 14 - w; }
/* SeedChooserScreen::Draw draws its Menu (GameButton::Draw at 0x909c18, ecx = the button): drawn in the corner,
 * its place (+8) left as the game's own so its clicks still hit it (the game takes clicks only in its 4:3 area;
 * moving it for a press showed it jumping back) */
static uint32_t menu_drawn;
static int32_t menu_drawn_x;
void pad_chooser_menu_draw(uint32_t btn, int begin)
{
    if (begin) {
        menu_drawn = 0;
        if (!rt_widescreen || !btn || btn != versus_chooser_menu_shown()) return;
        int32_t w = (int32_t)g_rd32(btn + 0x10), y = (int32_t)g_rd32(btn + 0xc);
        if (y >= 40 || w <= 0 || w >= 200) return;
        menu_drawn = btn, menu_drawn_x = (int32_t)g_rd32(btn + 8);
        g_wr32(btn + 8, (uint32_t)menu_corner_x(w));
    } else if (menu_drawn) {
        g_wr32(menu_drawn + 8, (uint32_t)menu_drawn_x);
        menu_drawn = 0;
    }
}
/* (Mac tests, ctl vsbtn: the versus top-right buttons) */
void pad_versus_buttons_log(void)
{
    uint32_t bt[4], bd = board();
    int n = versus_buttons(bt, 4);
    if (bd) rt_log("[vsbtn] board %08x: %d buttons (the board's own Menu, +0x150, is the DLC's bottom pause in versus)\n", (unsigned)bd, n);
    for (int i = 0; i < n; i++) {
        uint32_t w = bt[i], vt = g_rd32(w);
        rt_log("[vsbtn] %d %s %08x at %d,%d %dx%d vis %d id %d vtable %08x:", i, game_rtti_name(w), (unsigned)w, (int)g_rd32(w + 0x34),
               (int)g_rd32(w + 0x38), (int)g_rd32(w + 0x3c), (int)g_rd32(w + 0x40), RD8_G(w + 0x58), (int)g_rd32(w + 0x9c), (unsigned)vt);
        rt_log("\n");
    }
}
static void versus_menu_corner(void)
{
    uint32_t mb = versus_chooser_menu();
    if (!rt_widescreen || !mb || touch_mode) return;
    int32_t y = (int32_t)g_rd32(mb + 0xc), w = (int32_t)g_rd32(mb + 0x10);
    if (y >= 40 || w <= 0 || w >= 200) return;
    badge((float)menu_corner_x(w), 0, (float)w, (float)((int32_t)g_rd32(mb + 0x14) + y), HELP_PLUS);
}
static void versus_menu_press(void)
{
    uint32_t mb = versus_chooser_menu();
    if (!mb) return;
    press_hold(PB_PLUS, (float)(int32_t)g_rd32(mb + 8) + (float)(int32_t)g_rd32(mb + 0x10) / 2, (float)(int32_t)(g_rd32(mb + 0xc) + g_rd32(mb + 0x14)) / 2);
}
/* Versus: the shovel's slot is at the 4:3 screen's bottom-left corner (Board::GetShovelButtonRect 0x7426b0: x 0,
 * y 557), over the bottom row's lawn mower in widescreen (user); it is drawn in the wide screen's corner instead,
 * SHOVEL_DX further left (pad_shovel_rect, from Board::DrawShovel's rect, [ebp+0x34] after its call at 0x73c8e4).
 * Its place stays the game's own (the game takes clicks only inside 4:3): a touch or click where it is drawn goes
 * there, and one on its place itself (the lawn's corner cell, the mower) goes just above it, so nothing unseen is
 * picked up there. Only while the board has just drawn it (not in the draft, the pan, or another mode). */
#define SHOVEL_DX 133
static int32_t shovel_rect[4];
static uint32_t shovel_poll = ~0u;
void pad_shovel_rect(uint32_t rect)
{
    if (!rt_widescreen || !is_versus()) return;
    for (int k = 0; k < 4; k++) shovel_rect[k] = (int32_t)g_rd32(rect + 4 * (uint32_t)k);
    g_wr32(rect, (uint32_t)(shovel_rect[0] - SHOVEL_DX));
    shovel_poll = pad_poll_no;
}
static void shovel_redirect(int *x, int *y)
{
    if (!rt_widescreen || !is_versus() || shovel_poll == ~0u || pad_poll_no - shovel_poll > 10) return;
    float gx = ((float)*x - 160.f) / 1.2f, gy = (float)*y / 1.2f;
    int32_t rx = shovel_rect[0], ry = shovel_rect[1], rw = shovel_rect[2], rh = shovel_rect[3];
    if (rw <= 0 || rh <= 0 || gy < (float)ry || gy >= (float)(ry + rh)) return;
    if (gx >= (float)(rx - SHOVEL_DX) && gx < (float)(rx - SHOVEL_DX + rw))
        *x = (int)(160.f + (gx + SHOVEL_DX) * 1.2f + 0.5f); /* (where it is drawn: its own place) */
    else if (gx >= (float)rx && gx < (float)(rx + rw))
        *y = (int)((float)(ry - 1) * 1.2f); /* (its own place: the lawn just above it) */
}

/* a touch (display point, 1280x720) on the Menu or the versus shovel where it is drawn goes to its own place */
void pad_touch_redirect(int *x, int *y)
{
    shovel_redirect(x, y);
    uint32_t mb = versus_chooser_menu_shown();
    if (!mb || !rt_widescreen) return;
    int32_t w = (int32_t)g_rd32(mb + 0x10), top = (int32_t)g_rd32(mb + 0xc), h = (int32_t)g_rd32(mb + 0x14);
    float gx = ((float)*x - 160.f) / 1.2f, gy = (float)*y / 1.2f, l = (float)menu_corner_x(w);
    if (gx < l || gx >= l + (float)w || gy >= (float)(top + h)) return;
    *x = (int)(160.f + ((float)(int32_t)g_rd32(mb + 8) + (float)w / 2) * 1.2f);
    *y = (int)((float)(top + h) / 2 * 1.2f);
}
static void versus_button_press(int which, uint32_t pb)
{
    uint32_t bt[4];
    int n = versus_buttons(bt, 4);
    if (which >= n) return;
    float x = (float)(int32_t)g_rd32(bt[which] + 0x34) + (float)(int32_t)g_rd32(bt[which] + 0x3c) / 2;
    float y = (float)(int32_t)g_rd32(bt[which] + 0x38) + (float)(int32_t)g_rd32(bt[which] + 0x40) / 2;
    press_hold(pb, x, y);
}

static void level_overlay(int solo)
{
    if (!solo) {
        dig_meter(0);
        dig_meter(1);
        if (!touch_mode) {
            uint32_t bt[4];
            int n = versus_buttons(bt, 4);
            static const int cel[3] = {HELP_PLUS, HELP_MINUS, HELP_X};
            for (int i = 0; i < n && i < 2; i++)
                badge((float)(int32_t)g_rd32(bt[i] + 0x34), (float)(int32_t)g_rd32(bt[i] + 0x38), (float)(int32_t)g_rd32(bt[i] + 0x3c),
                      (float)(int32_t)g_rd32(bt[i] + 0x40), cel[i]);
        }
    }
    if (!solo || touch_mode) return;
    float cx, cy;
    if (dig_t >= DIG_SHOW && !dig_done && kb_cursor(&cx, &cy)) {
        /* the meter's top-left at the cursor, as the TV edition draws it */
        float frac = (dig_t - DIG_SHOW) / (DIG_FULL - DIG_SHOW);
        ov_image(OV_METER_EMPTY, 0, 0, 46, 46, cx, cy, 46, 46, 0xffffffffu);
        ov_pie(OV_METER_FULL, cx + 23, cy + 23, 22, frac > 1 ? 1 : frac, 0xffffffffu);
    }
    /* the board's own buttons: Menu (+) and fast forward (Y) */
    badge(682, 0, 111, 29, HELP_PLUS);
    badge(740, 33, 53, 40, HELP_Y);
    /* B by the shovel (Board::GetShovelButtonRect: x = 456 + the bank's extra width) */
    int held, n = seed_packets(&held);
    if (game_mode() == 25) /* the Slot Machine: X on its handle (Challenge::SlotMachineGetHandleRect: 473,0 55x80) */
        badge_img(HELP_X, 473 + 55 - 20, 52, 30, 30);
    /* no shovel for a zombie bank (Zombies vs. Zombies, I, Zombie): the bank's first packet is a zombie
     * (the DLC's own IsZombieSeedType, 0x915df0, or the gravestone, or Zombies vs. Zombies' newer zombies) */
    if (n && (zombie_bank() || !shovel_shown())) n = 0;
    if (n && !game_belt_level) {
        static const int extra[11] = {0, 0, 0, 0, 0, 0, 0, 60, 76, 112, 153};
        float sx = 456.f + (float)extra[n];
        if (game_mode() == 25) sx = 600; /* (Board::GetShovelButtonRect: the Slot Machine's shovel is at 600) */
        badge_img(HELP_B, sx + 50, 40, 42, 42);
    }
}

/* ---------------- the Zen Garden: a pointer ----------------
 * The garden is all pointing (pots, tools, the Shop): the stick moves a pointer, A clicks (held as long as
 * A is), B lets go of the tool in hand (a right click), ZL / ZR pick up the previous / next tool from the
 * bank at the top left, + is Main Menu, Y the Shop. The bank shows only the tools the player has, side by
 * side 70 apart from x 30 (zen_find_tools). */
static int zen_tool = -1;
static int zen_active; /* in the garden (0 = arriving next time) */

static void raw_mouse(float gx, float gy, uint32_t msg, int buttons)
{
    int x = (int)(160.f + gx * 1.2f), y = (int)(gy * 1.2f);
    rt_screen_to_display(&x, &y);
    rt_post_message(rt_main_hwnd, msg, (uint32_t)buttons, ((uint32_t)y << 16) | ((uint32_t)x & 0xffff));
}
static float zen_slot_x(int i) { return 64.f + 70.f * (float)i; }

/* The Zen Garden, as on the Xbox 360 edition: the keyboard player's grid cursor walks the pots (the DLC's
 * own, moved by the stick / D-pad as in a level). ZL / ZR (or L / R) choose the previous / next tool in the
 * bank, which stays in the bank (the selector marks it); A on a plant uses it there: the tool is taken up
 * (a click on its slot), used on the plant (a click on the cursor's cell) and, once the game has done with
 * it, put back if it is still in hand (a right click). A where there is no plant is a plain click there
 * (coins), and so is A while a plant is carried (the glove's or the wheelbarrow's: it is set down there); the
 * wheelbarrow also works on an empty pot. B puts back whatever is in hand and clears the choice, + is Main
 * Menu, Y the Shop. The cursor object's type (Board +0x13c, +0x30): 0 nothing, 3 / 5 a plant from the glove /
 * wheelbarrow, 9..17 a tool (14 the glove, 16 the wheelbarrow). The hidden mouse rests on the cursor's cell (its position is the
 * cell's middle). */
static int zen_type[12], zen_use, zen_back;

/* The bank's tools as DrawZenButtons (0x73ead0) lays them out: each GameObjectType from the watering can (6)
 * to the wheelbarrow (13) that Board::CanUseGameObject (0x731d50) allows, one slot each, in that order; the
 * cursor type of tool t is 9 + (t - 6). Asked of the game itself on the way in: the DLC keeps its tools'
 * purchases at its own indices (fertilizer +0x1050, chocolate +0x1080, ...) and the money sign needs the
 * adventure finished. (Resting the hidden mouse on each slot to count them lit each tool in turn.) */
#define BOARD_CAN_USE_OBJECT 0x731d50u
static void zen_find_tools(void)
{
    uint32_t bd = board();
    zen_tools = 0;
    for (uint32_t t = 6; t <= 13 && bd; t++)
        if (guest_this(GAME_FN(BOARD_CAN_USE_OBJECT), bd, 1, &t) & 0xff) zen_type[zen_tools++] = 9 + (int)(t - 6);
    zen_tool = zen_tools ? 0 : -1; /* the watering can, chosen from the start */
    char line[160];
    int len = snprintf(line, sizeof line, "[pad] zen garden: %d tools:", zen_tools);
    for (int i = 0; i < zen_tools; i++) len += snprintf(line + len, sizeof line - len, " %d", zen_type[i]);
    rt_log("%s\n", line);
}

/* The gardens' yellow arrow (Board's NEXT_GARDEN button, x 564; ZenGarden::GotoNextGarden: the next owned
 * garden, round the Zen Garden, the Mushroom Garden, the Aquarium and the Tree of Wisdom) is ZR, pressed on
 * release as every button is. ZL does nothing (going back a garden, by pressing the arrow round, didn't work
 * well). */
static void zen_click(float x, float y);
/* ZR; called every poll in a garden */
/* ZR: the yellow arrow to the next garden (Board's NEXT_GARDEN button, 564,0 ~64x50), pressed when ZR is let go
 * (as every other button); while held, the arrow is drawn pressed (darkened, a pixel down and right: its own
 * picture, IMAGE_ZEN_NEXTGARDEN, at DrawZenButtons' draw of it) and its badge with it */
int pad_next_garden_held; /* ZR held on the arrow: Board::DrawZenButtons draws it pressed (game_patches.c) */
static void garden_keys(uint32_t b, uint32_t pressed)
{
    static int held, stale;
    static uint32_t last_poll;
    (void)pressed;
    /* not run last poll (a garden coming in, a dialog): a ZR still down from before is not a new press; it
     * counts again once let go (held over the change, it went on to the next garden as well) */
    if (last_poll + 1 != pad_poll_no) held = 0, stale = (b & PB_ZR) != 0;
    last_poll = pad_poll_no;
    if (stale && !(b & PB_ZR)) stale = 0;
    if ((b & PB_ZR) && !stale) held = 1;
    else if (held && !(b & PB_ZR)) {
        held = 0;
        zen_click(596.f, 25.f);
    }
    pad_next_garden_held = held;
    badge(561 + held, held, 70, 50, HELP_ZR);
}
static int zen_cursor_type(void)
{
    uint32_t bd = board(), co = bd ? g_rd32(bd + 0x13c) : 0;
    return valid(co) ? (int)g_rd32(co + 0x30) : 0;
}
static void zen_click(float x, float y)
{
    raw_mouse(x, y, WM_MOUSEMOVE, 0);
    raw_mouse(x, y, WM_LBUTTONDOWN, 1);
    raw_mouse(x, y, WM_LBUTTONUP, 0);
}
static void zen_put_back(float x, float y)
{
    raw_mouse(x, y, WM_RBUTTONDOWN, 2);
    raw_mouse(x, y, WM_RBUTTONUP, 0);
}
static void zen_keys(uint32_t b, uint32_t pressed, int unused)
{
    (void)unused;
    if (getenv("RT_ZEN_TEST")) { /* (Mac test: the mushroom garden owned, for the next-garden arrow) */
        uint32_t pi = player_info();
        if (pi) g_wr32(pi + 0x1060, 1);
    }
    if (zen_active != 2) { /* arrived: the bank's tools, from the game's own Board::CanUseGameObject */
        zen_active = 2;
        zen_tool = -1;
        zen_use = zen_back = 0;
        pressed = 0;
        zen_find_tools();
    }
    if (getenv("ZENLOG")) {
        static int lastbg = -2;
        uint32_t bd = board();
        int bg = bd ? (int)g_rd32(bd + 0x5580) : -1;
        if (bg != lastbg) { lastbg = bg; rt_log("[zenlog] bg %d mode %d\n", bg, game_mode()); }
    }
    if (hold_pb) return; /* (Main Menu or the Shop being pressed: pad_input_update lets go) */
    float kx, ky;
    int have = kb_cursor(&kx, &ky);
    static int lx = -1, ly = -1;
    if (have && ((int)kx != lx || (int)ky != ly)) {
        lx = (int)kx, ly = (int)ky;
        raw_mouse(kx, ky, WM_MOUSEMOVE, 0);
    }
    /* a tool taken up a moment ago (the game moves it to the pointer on its next update): used on the cell */
    if (zen_use && --zen_use == 0 && have) {
        zen_click(kx, ky);
        zen_back = 6;
    }
    /* a tool used a moment ago: back to the bank if it is still in hand */
    if (zen_back && --zen_back == 0) {
        int ct = zen_cursor_type();
        if (ct >= 9 && ct <= 17 && have) zen_put_back(kx, ky);
        if (have) raw_mouse(kx, ky, WM_MOUSEMOVE, 0);
    }
    if ((pressed & PB_A) && have && !zen_back && !zen_use) {
        int ct = zen_cursor_type(), t = zen_tool >= 0 ? zen_type[zen_tool] : 0;
        if (ct == 3 || ct == 5 || zen_tool < 0 || (t != 16 && !plant_under_cursor())) {
            zen_click(kx, ky);
        } else {
            if (ct) zen_put_back(kx, ky);
            zen_click(zen_slot_x(zen_tool), 36);
            raw_mouse(kx, ky, WM_MOUSEMOVE, 0);
            zen_use = 3;
        }
    }
    if ((pressed & PB_B) && have) {
        if (zen_cursor_type()) zen_put_back(kx, ky);
        zen_tool = -1;
    }
    garden_keys(b, pressed);
    if ((pressed & (PB_L | PB_R)) && zen_tools > 0) {
        int dir = (pressed & PB_R) ? 1 : -1;
        zen_tool = zen_tool < 0 ? (dir > 0 ? 0 : zen_tools - 1) : (zen_tool + dir + zen_tools) % zen_tools;
    }
    if (pressed & PB_PLUS) press_hold(PB_PLUS, 737.f, 14.f); /* Main Menu */
    if (pressed & PB_Y) press_hold(PB_Y, 733.f, 54.f);       /* the Shop */
    /* the console's marks: + on Main Menu, Y on the Shop */
    badge(682, 0, 111, 29, HELP_PLUS);
    badge(705, 38, 80, 34, HELP_Y);
}

/* The Tree of Wisdom (game mode 140), as on the PS3, with the Zen Garden's bank: no cursor. Its two things
 * (the tree food, the bank's first slot at x 30 as Board::GetZenButtonRect puts it, and the yellow arrow to
 * the next garden, Board's NEXT_GARDEN button at x 564) carry the seed bank's selector; L / R move it, A
 * uses it (ZR: the next garden, garden_keys): the food is taken up and given to the tree (a click inside
 * Challenge::TreeOfWisdomHitTest's rect at every size, 392,300), the arrow is pressed. Y the Shop, + or B
 * Main Menu. */
static int tree_sel = -1;
static void tree_keys(uint32_t b, uint32_t pressed)
{
    static int feed;
    if (tree_sel < 0) tree_sel = 0;
    if (hold_pb) return;
    if (feed && --feed == 0) zen_click(392, 300);
    garden_keys(b, pressed);
    if (pressed & (PB_L | PB_R)) tree_sel ^= 1;
    if ((pressed & PB_A) && !feed) {
        if (tree_sel == 0) {
            zen_click(65, 36);
            raw_mouse(392, 300, WM_MOUSEMOVE, 0);
            feed = 3;
        } else {
            press_hold(PB_A, 596.f, 25.f); /* the next garden */
        }
    }
    if (pressed & PB_Y) press_hold(PB_Y, 733.f, 54.f);                          /* the Shop */
    else if (pressed & (PB_PLUS | PB_B)) press_hold(pressed & (PB_PLUS | PB_B), 737.f, 14.f); /* Main Menu */
    badge(682, 0, 111, 29, HELP_PLUS);
    badge(705, 38, 80, 34, HELP_Y);
}

/* The tool in hand gets the seed bank's own selector: the game's board_selector_YELLOW (P1's packet
 * brackets), drawn at half size through DrawImageMatrix as the game draws it on a packet. The tool box
 * is square, so each corner is drawn on its own (clipped to that corner) and set on the box's corner.
 * Called from the keyboard cursor's draw with the board's Graphics. */
#define IMG_P1_SELECTOR 0x1094180u   /* Image *IMAGE_P1_1 */
#define DRAW_IMAGE_MATRIX 0xa05e90u  /* Graphics::DrawImageMatrix(Image *, const SexyMatrix3 &, float x, float y) */
void pad_zen_selector(uint32_t g)
{
    RtThread *t = rt_thread_self();
    uint32_t img = g_rd32(IMG_P1_SELECTOR + rt_img_delta);
    int tree = game_mode() == 140;
    if (tree ? tree_sel < 0 : (!zen_active || zen_tool < 0)) return;
    if (touch_mode || !t || !valid(img) || !valid(g)) return;
    uint32_t esp = t->cpu.esp, m = (esp - 0x400) & ~15u;
    const float mat[9] = {.5f, 0, 0, 0, .5f, 0, 0, 0, 1};
    memcpy(G2H(m), mat, sizeof mat);
    t->cpu.esp = m;
    int32_t clip[4];
    for (int i = 0; i < 4; i++) clip[i] = (int32_t)g_rd32(g + 0x20 + 4 * i);
    float tx, ty;
    memcpy(&tx, G2H(g + 0x08), 4);
    memcpy(&ty, G2H(g + 0x0c), 4);
    /* the box: one pixel past the tool's 68 x 68 slot, as the selector sits on a packet; the image is
     * 52 x 72 at half size and each corner bracket fits in its quarter */
    float x0 = 30.f + 70.f * (float)(tree ? 0 : zen_tool), y0 = 1, x1 = x0 + 70, y1 = y0 + 70;
    if (tree && tree_sel == 1) x0 = 561, y0 = 0, x1 = 631, y1 = 50; /* (the arrow) */
    for (int q = 0; q < 4; q++) {
        int right = q & 1, bottom = q >> 1;
        float cx = right ? x1 - 26 : x0 + 26, cy = bottom ? y1 - 36 : y0 + 36;
        int32_t qx = (int32_t)(right ? x1 - 26 : x0) + (int32_t)tx, qy = (int32_t)(bottom ? y1 - 35 : y0) + (int32_t)ty;
        int32_t ax = qx > clip[0] ? qx : clip[0], ay = qy > clip[1] ? qy : clip[1];
        int32_t bx = qx + 26 < clip[0] + clip[2] ? qx + 26 : clip[0] + clip[2];
        int32_t by = qy + 35 < clip[1] + clip[3] ? qy + 35 : clip[1] + clip[3];
        if (bx <= ax || by <= ay) continue;
        g_wr32(g + 0x20, (uint32_t)ax), g_wr32(g + 0x24, (uint32_t)ay);
        g_wr32(g + 0x28, (uint32_t)(bx - ax)), g_wr32(g + 0x2c, (uint32_t)(by - ay));
        uint32_t args[4] = {img, m, 0, 0};
        memcpy(&args[2], &cx, 4);
        memcpy(&args[3], &cy, 4);
        guest_this(GAME_FN(DRAW_IMAGE_MATRIX), g, 4, args);
    }
    for (int i = 0; i < 4; i++) g_wr32(g + 0x20 + 4 * i, (uint32_t)clip[i]);
    t->cpu.esp = esp;
}

/* Versus against the AI, plant select: the AI drafts its zombie only after the mouse picks a card (in the
 * chooser's MouseDown, 0x90d8b0; the keyboard player's pick, in its KeyDown, has no AI turn after it). So A
 * there clicks the card under player 1's bracket: its card (bracket +0x10c, plus 40 on the second page and 73
 * on the third, as the KeyDown counts) from the chooser's std::map<int, ChosenSeed> at +0xc0 (operator[],
 * 0x6f8fa6): ChosenSeed x +0, y +4 in the chooser, +0x24 state (3 = in the chooser) */
static int ai_chooser_pick(void)
{
    uint32_t sc = seed_chooser();
    RtThread *t = rt_thread_self();
    if (!sc || !t) return 0;
    int page = (int)g_rd32(sc + 0x1bc);
    uint32_t esp = t->cpu.esp, base = (esp - 0x400) & ~15u;
    g_wr32(base, g_rd32(sc + 0x10c) + (page == 1 ? 40u : page == 2 ? 73u : 0u));
    t->cpu.esp = base;
    uint32_t arg = base;
    uint32_t cs = guest_this(GAME_FN(0x6f8fa6u), sc + 0xc0, 1, &arg);
    t->cpu.esp = esp;
    if (!valid(cs) || g_rd32(cs + 0x24) != 3) return 0;
    float x = (float)(int32_t)(g_rd32(sc + 0x34) + g_rd32(cs)) + 25.f;
    float y = (float)(int32_t)(g_rd32(sc + 0x38) + g_rd32(cs + 4)) + 35.f;
    rt_log("[chooser] AI match: A clicks card %u at %.0f,%.0f\n", (unsigned)g_rd32(base), x, y);
    raw_mouse(x, y, WM_MOUSEMOVE, 0);
    raw_mouse(x, y, WM_LBUTTONDOWN, 1);
    raw_mouse(x, y, WM_LBUTTONUP, 0);
    rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
    return 1;
}

void pad_input_update(const PadIn pads[2], float dt)
{
    static int started;
    if (!started++) {
        if (getenv("RT_DESKTOP")) pad_desktop = 1;
        if (getenv("RT_TOUCH_FIRST")) pad_touch_first = 1;
        if (pad_desktop || pad_touch_first) touch_mode = 1; /* (the mouse, or the touch screen, plays first) */
    }
    pads_connected[0] = pads[0].connected, pads_connected[1] = pads[1].connected;
    if (p2_hold > 0 && (p2_hold -= dt) <= 0) key(p2_vk, 0); /* (player 2's finger: its key let go) */
    static uint32_t prev[2], prev_stick[2];
    static int was_versus, sel = -1;
    static float settle;
    if (!rt_main_hwnd) return;
    /* The hosts call this from the game's message pump, which checks the queue in a loop until it is
     * empty: reading the pads (and posting) at most every 8 ms lets that loop finish. Posting a mouse
     * move on every check while the stick was held kept the queue from ever emptying (the menu froze). */
    static float acc;
    acc += dt;
    if (acc < 0.008f) return;
    dt = acc > 0.1f ? 0.1f : acc;
    acc = 0;
    pad_poll_no++;
    click_release();
    hold_release(pads[0].connected ? pads[0].buttons : 0);
    pad_dave_prompt(0, 0, 0); /* Crazy Dave's literal, marked up before its first bubble */
    ov_clear(); /* rebuilt below, for this poll */
    n_occl = 0;
    if (board()) occlude_tooltips_of(board());
    settle += dt;
    if (settle > 1.f) {
        settle = 0;
        load_bindings();
    }
    /* the keyboard players while a level is being played (scene 3) or in versus; Crazy Dave's talk and
     * the other in-level screens take clicks, and the seed chooser has its own handling */
    uint32_t app_ = rt_app_object();
    /* an open dialog (LawnApp+0x330: SexyApp's dialog list, its size at +0x338), such as the pause menu,
     * always takes the pointer */
    int dialogs = valid(app_) ? (int)g_rd32(app_ + 0x338) : 0;
    if (dialogs < 0 || dialogs > 16) dialogs = 0;
    int playing = !dialogs && board() != 0 && valid(app_) && g_rd32(app_ + 0x818) == 3;
    int zen = 0; /* (the Zen Garden plays with the keyboard player too: zen_keys below) */
    int in_level = board() != 0, versus = (playing && !zen) || (is_versus() && !dialogs);
    {
        /* the DLC's keyboard players, set before any screen creates their cursors: versus = both players;
         * everything else = player 1 alone (NCOP +0x1164 = 0, P1KB +0x5cd4 = 1, TASO +0x5df4 = 1,
         * P2KB +0x5cd5 = 0) */
        uint32_t pi = player_info();
        int vs = is_versus();
        /* the PC's own setup (the desktop apps: versus always, the rest while the mouse plays): the game's keyboard
         * players, their pictures and names as the game has them; what the controller's play changed is put back */
        int pc = pad_desktop && (vs || touch_mode);
        /* Outside versus the DLC draws the keyboard cursor with player 2's images (its co-op design: the
         * keyboard player is P2). Single player is P1: point IMAGE_P2_1..3 (0x109418c/90/94) at IMAGE_P1_1..3
         * (0x1094180/84/88) and put them back for versus. */
        /* No "P1" over the single player's cursor: the DLC draws the P1/P2 image only while the name at
         * PlayerInfo+0x1184 (an MSVC std::string) is empty, else the
         * player's name as text (empty here). A single space outside versus draws nothing; versus gets "" back. */
        /* (this build's std::string: debug proxy +0, 16-byte buffer +4, size +20, capacity +24) */
        /* the two co-op names: +0x1184 labels the keyboard cursor, +0x1168 the mouse (touch) pointer */
        for (uint32_t str = 0x1168; str <= 0x1184 && pi; str += 0x1c) {
            if (g_rd32(pi + str + 24) != 15) continue;
            uint32_t sz = g_rd32(pi + str + 20);
            uint8_t c0 = RD8_G(pi + str + 4);
            /* (and versus on one touch screen: no "P1" by the mouse pointer, which is only where a finger last was) */
            int blank = (!vs && !pc) || (vs && str == 0x1168 && pad_touch_first && touch_mode);
            if (blank && sz == 0) { WR8_G(pi + str + 4, ' '); WR8_G(pi + str + 5, 0); g_wr32(pi + str + 20, 1); }
            if (!blank && sz == 1 && c0 == ' ') { WR8_G(pi + str + 4, 0); g_wr32(pi + str + 20, 0); }
        }
        uint32_t p1_0 = g_rd32(0x1094180u);
        if (valid(p1_0)) {
            if (!p2_orig[0])
                for (int i = 0; i < 3; i++) p2_orig[i] = g_rd32(0x109418cu + 4 * i);
            for (int i = 0; i < 3; i++) {
                uint32_t want = vs || pc ? p2_orig[i] : g_rd32(0x1094180u + 4 * i);
                if (g_rd32(0x109418cu + 4 * i) != want) g_wr32(0x109418cu + 4 * i, want);
            }
        }
        if (pi) {
            const uint32_t off[4] = {0x1164, 0x5cd4, 0x5df4, 0x5cd5};
            static uint8_t solo[4] = {0, 1, 1, 0};
            static int env_done;
            if (!env_done++ && getenv("RT_SOLO")) sscanf(getenv("RT_SOLO"), "%hhu,%hhu,%hhu,%hhu", &solo[0], &solo[1], &solo[2], &solo[3]);
            /* TASO (+0x5df4) is also Multiplayer's "AI Opponent" box (0 = the AI plays; the box, a Checkbox at
             * ChallengeScreen+0x424, ticked at +0x94, is made from it and writes it back as a map is chosen).
             * Outside versus it stays set (the keyboard player needs it), so the player's choice is kept here
             * and given back to the box while Multiplayer is open. */
            static uint32_t ai_pi;
            if (pi != ai_pi) ai_pi = pi, ai_off = RD8_G(pi + 0x5df4);
            if (vs) ai_off = RD8_G(pi + 0x5df4);
            ai_match = vs && !ai_off;
            static uint8_t was[4];
            static int forced;
            if (pc) {
                if (forced) /* back from the controller's play: the game's own values */
                    for (int i = 0; i < 4; i++) WR8_G(pi + off[i], was[i]);
                forced = 0;
            } else if (pad_desktop && !forced) {
                for (int i = 0; i < 4; i++) was[i] = RD8_G(pi + off[i]);
                forced = 1;
            }
            for (int i = 0; i < 4 && !pc; i++) {
                if (vs && i != 1 && i != 3) continue;
                uint8_t want = vs ? 1 : solo[i];
                if (vs && i == 3 && ai_match) want = 0; /* against the AI: no player 2 */
                /* touch play: no keyboard player (its cursor and bracket go), the mouse plays as on PC;
                 * the controller's next press brings it back */
                if (!vs && i == 1 && (touch_mode || (on_imit && seed_chooser()))) want = 0;
                if (!vs && i == 1 && game_mode() == 140) want = 0; /* (the Tree of Wisdom: no grid cursor) */
                /* versus on one touch screen (the Android app, touch in use): player 1 is the mouse (one finger),
                 * player 2 the keyboard player (the other finger: pad_touch_finger) */
                if (vs && i == 1 && pad_touch_first && touch_mode) want = 0;
                if (RD8_G(pi + off[i]) != want) WR8_G(pi + off[i], want);
            }
        }
    }
    if (versus != was_versus) {
        /* let go of whatever the other scheme was holding */
        for (int p = 0; p < 2; p++)
            for (int i = 0; i < 9; i++)
                if (prev[p] || prev_stick[p]) key(pkeys[p][i], 0);
        if (mouse_buttons & 1) { mouse_buttons &= ~1; mouse(WM_LBUTTONUP); }
        if (mouse_buttons & 2) { mouse_buttons &= ~2; mouse(WM_RBUTTONUP); }
        /* buttons still held stay "held": a + that opened the pause menu must not count again as a press
         * in the menu's scheme (it closed the menu at once, and holding + kept toggling it) */
        memset(prev_stick, 0, sizeof prev_stick);
        was_versus = versus;
        scheme_switched = 1;
        if (versus) {
            cursor_shown = 0; /* the game's own grid cursors from here */
        } else if (in_level) {
            /* a menu over a level: the pointer starts in the middle, shown */
            cur_x = DISPLAY_W / 2;
            cur_y = DISPLAY_H / 2;
            cursor_shown = 1;
            mouse(WM_MOUSEMOVE);
        }
        rt_log("[pad] %s controls (game mode %d)\n", versus ? "keyboard-player" : "menu", game_mode());
    }

    if (in_level && !is_versus() && !dialogs && chooser_update(&pads[0], dt)) return;
    if (zen_active && !(playing && game_mode() == 50)) { /* left the garden: let go of anything held */
        zen_active = 0;
        if (mouse_buttons & 1) { mouse_buttons &= ~1; mouse(WM_LBUTTONUP); }
        if (mouse_buttons & 2) { mouse_buttons &= ~2; mouse(WM_RBUTTONUP); }
    }

    /* outside versus the mouse is the co-op partner: its pointer (and the label the DLC draws next to it)
     * goes off screen while the controller plays, from the level's start (a level begun from a menu by the
     * controller left it mid-screen, its label with it); a touch brings it back (not in the Zen Garden: its
     * pointer rests on the cursor's cell, or on Main Menu / the Shop as they are pressed) */
    if (playing && !zen && !is_versus() && game_mode() != 50 && game_mode() != 140 && pads[0].connected &&
        (!touch_mode || pads[0].buttons || fabsf(pads[0].lx) > 0.3f || fabsf(pads[0].ly) > 0.3f)) {
        static int parked;
        if (hold_pb) parked = 0; /* (a button being pressed: the pointer rests on it until it is let go) */
        else if (!parked || rt_mouse_x != -400) {
            parked = 1;
            cursor_shown = 0;
            rt_mouse_x = rt_mouse_y = -400;
            rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
        }
    }

    if (is_versus()) versus_menu_corner();
    /* the versus pause menu's Main Menu (button 1) goes back to Multiplayer (versus_return_step): named for it */
    if (screen_kind == SCR_OPTIONS && is_versus() && screen_obj) {
        uint32_t mm = button_with_id(screen_obj, 1);
        char lb[24];
        if (mm && (game_string(mm + 0x90, lb, sizeof lb), !strcmp(lb, "Main Menu"))) game_string_set(mm + 0x90, "Lobby");
    }
    if (versus) {
        static const uint32_t slot_btn[9] = {PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_L, PB_R, PB_A, PB_B, PB_X};
        int solo = !is_versus();
        for (int p = 0; p < 2; p++) {
            uint32_t sd = level_stick(p, &pads[p]);
            uint32_t now = (pads[p].connected ? pads[p].buttons : 0) | sd;
            if (p == 1 && ai_match) now = 0; /* the AI plays player 2's side */
            uint32_t was = prev[p] | prev_stick[p];
            if (p == 0 && now) touch_mode = 0; /* the controller again: its cursor comes back */
            int tree = solo && game_mode() == 140;
            int garden = (solo && game_mode() == 50) || tree;
            /* on the lawn the D-pad steps the cursor a whole cell at once (lawn_dpad), not the DLC's glide with
             * its direction keys; the stick keeps those (any angle). The versus draft (not playing yet) and the
             * gardens keep the keys. */
            if (playing && !garden) {
                const uint32_t dirs = PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT;
                lawn_dpad(p, (p == 1 && ai_match) || !pads[p].connected ? 0 : pads[p].buttons & dirs, dt);
                now = (now & ~dirs) | sd;
            }
            /* Zombies vs. Zombies (mode 102): the keyboard player can't place the gravestone (the DLC's zombie
             * check, 0x915df0, leaves it out there, so its keyboard path takes it for a plant); A with it chosen
             * is the mouse's: a click on its packet (the bank's packets: +0x48 + 0x54 i, x +0x30 / y +0x34 from
             * the bank's own at +8 / +0xc), then on the cursor's cell */
            uint32_t a_mouse = 0;
            /* Vasebreaker (no seed bank shown: vase_mode): there is nothing for the keyboard player
             * to plant, so A is the mouse's click at the cursor's cell middle (a vase seed packet is picked up as the
             * cursor passes over it, the DLC's own, and A plants it; X breaks vases, x_action), and the pointer stays on
             * the cell while a seed is in hand
             * (Board +0x13c's type +0x30 is 2, a packet from a vase) so it and its ghost show there */
            if (solo && p == 0 && vase_mode()) {
                float cx, cy;
                if (cursor_cell_mid(&cx, &cy)) {
                    uint32_t bd = board(), co = bd ? g_rd32(bd + 0x13c) : 0;
                    int held = valid(co) && g_rd32(co + 0x30) == 2; /* (CURSOR_TYPE_PLANT_FROM_USABLE_COIN) */
                    static float hx, hy;
                    static int a_down;
                    /* (down with A, up with its release: as the game sees a click; down and up at once broke nothing) */
                    if ((pads[0].buttons & PB_A) && !(prev[0] & PB_A)) {
                        raw_mouse(cx, cy, WM_MOUSEMOVE, 0);
                        raw_mouse(cx, cy, WM_LBUTTONDOWN, 1);
                        a_down = 1;
                        hx = cx, hy = cy;
                    } else if (a_down && !(pads[0].buttons & PB_A)) {
                        raw_mouse(hx, hy, WM_LBUTTONUP, 0);
                        a_down = 0;
                        hx = -1; /* (the pointer back on the cell, with the seed) */
                    } else if (a_down) {
                    } else if (held && (cx != hx || cy != hy)) {
                        raw_mouse(cx, cy, WM_MOUSEMOVE, 0);
                        hx = cx, hy = cy;
                    } else if (!held && hx > -300) {
                        rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
                        hx = hy = -400;
                    }
                }
                a_mouse = PB_A;
            }
            if (solo && p == 0 && game_mode() == 102 && (pads[0].buttons & PB_A) && !(prev[0] & PB_A)) {
                uint32_t bd = board(), kc = bd ? g_rd32(bd + 0x178) : 0, bank = bd ? g_rd32(bd + 0x148) : 0;
                float kx, ky;
                if (valid(kc) && valid(bank) && kb_cursor(&kx, &ky)) {
                    uint32_t i = g_rd32(kc + 0x48), pk = bank + 0x54 * i;
                    if (i < 10 && g_rd32(pk + 0x5c) == 0x59) {
                        float px = (float)(int32_t)(g_rd32(bank + 8) + g_rd32(pk + 0x30)) + 25.f;
                        float py = (float)(int32_t)(g_rd32(bank + 0xc) + g_rd32(pk + 0x34)) + 35.f;
                        raw_mouse(px, py, WM_MOUSEMOVE, 0);
                        raw_mouse(px, py, WM_LBUTTONDOWN, 1);
                        raw_mouse(px, py, WM_LBUTTONUP, 0);
                        raw_mouse(kx, ky, WM_MOUSEMOVE, 0);
                        raw_mouse(kx, ky, WM_LBUTTONDOWN, 1);
                        raw_mouse(kx, ky, WM_LBUTTONUP, 0);
                        rt_post_message(rt_main_hwnd, WM_MOUSEMOVE, 0, ((uint32_t)(uint16_t)-400 << 16) | (uint16_t)-400);
                        a_mouse = PB_A;
                    }
                }
            }
            if (!solo && p == 0 && ai_match && (now & PB_A) && !(was & PB_A) && seed_chooser() && ai_chooser_pick())
                a_mouse = PB_A;
            static int b_direct[2]; /* versus: B went down as the Cancel key (not over a plant) */
            for (int i = 0; i < 9; i++) {
                /* single player: B digs when held (below), X is free (the DLC's 9th key pauses); in the Zen
                 * Garden only the directions are the keyboard player's (zen_keys does the rest) */
                if (solo && (slot_btn[i] & (PB_B | PB_X))) continue;
                /* versus: B over a plant (the plant player's) digs once held, with the meter, as in single
                 * player; elsewhere it is the Cancel key at once */
                if (!solo && slot_btn[i] == PB_B) {
                    if ((now & PB_B) && !(was & PB_B) && !(plant_under_cursor_p(p) && !bank_is_zombie(p))) {
                        key(pkeys[p][i], 1);
                        b_direct[p] = 1;
                    }
                    if (!(now & PB_B) && b_direct[p]) {
                        key(pkeys[p][i], 0);
                        b_direct[p] = 0;
                    }
                    continue;
                }
                if (garden && (i >= 4 || tree)) continue;
                if (slot_btn[i] & a_mouse) continue;
                if ((now & slot_btn[i]) && !(was & slot_btn[i])) key(pkeys[p][i], 1);
                if (!(now & slot_btn[i]) && (was & slot_btn[i])) key(pkeys[p][i], 0);
            }
            uint32_t b = now & ~sd, pressed = b & ~prev[p];
            if (garden) {
                if (p == 0) {
                    if (tree) tree_keys(b, pressed);
                    else zen_keys(b, pressed, b & ~pressed ? 0 : 0);
                }
                prev[p] = b;
                prev_stick[p] = sd;
                continue;
            }
            if (pressed & PB_PLUS) {
                /* versus has no Escape for its menu: the Menu button itself */
                if (solo) tap(hotkeys[HK_MENU]);
                else if (versus_chooser_menu()) versus_menu_press();
                else versus_button_press(0, PB_PLUS);
            }
            if (!solo && (pressed & PB_MINUS)) versus_button_press(1, PB_MINUS); /* the next one (pause / flag) */
            if (solo && p == 0) {
                int mode = game_mode(), last_stand = mode == 38 || mode == 184;
                if ((pressed & PB_Y) && !last_stand) tap(hotkeys[HK_SPEED]); /* fast forward (Y, as in PvZ Touch's mod) */
                /* Last Stand: Y presses Start / Continue Onslaught (Challenge::LastStandUpdate's stone button,
                 * 300,559 210x46 or 270,559 257x46), as PvZ Touch */
                if ((pressed & PB_Y) && last_stand) press_hold(PB_Y, 405.f, 582.f);
                x_action(b & PB_X, pressed & PB_X, mode);
                flash_look_update(&pads[0], lvl_on[0] || (pads[0].connected && (pads[0].buttons & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT))), dt);
                if (pressed & PB_MINUS) tap(hotkeys[HK_PAUSE]);
                dig_update(b & PB_B, dt);
            }
            if (!solo) dig_update_p(p, (b & PB_B) && !b_direct[p] && !bank_is_zombie(p), dt);
            /* ZR / ZL held: the sun (or brain) magnet, as PvZ Touch */
            if (!solo || p == 0) vacuum_update(p, (b & (PB_ZR | PB_ZL)) != 0, dt);
            prev[p] = b;
            prev_stick[p] = sd;
        }
        if (!(solo && (game_mode() == 50 || game_mode() == 140))) level_overlay(solo);
        return;
    }

    /* everything else (menus, dialogs, Crazy Dave): controller 1 moves a highlight over the screen's
     * buttons (menu_update); the pointer is only the touch screen's */
    if (imitater_update(&pads[0], dt)) return;
    menu_update(&pads[0], dt);
}

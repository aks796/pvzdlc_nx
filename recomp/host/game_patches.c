/* game_patches.c: fixes that run inside the lifted game code, called before the instructions listed
 * in tools/x86recomp/lift.py HOOKS (CPU state synced, so they can read and change registers). */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "rt.h"
#include "pad_input.h"

/* Wall-nut Bowling and the other conveyor levels: SeedBank::Draw clips the packets to the belt, clears
 * the clip, then (single player, PlayerInfo+0x1164 clear) draws every packet again for its press-pop
 * animation and the keyboard cursors' packets on top. Those second draws had no clip, so a packet still
 * arriving showed over the belt's right end instead of under it. Keep the belt's right edge for them
 * (only the right edge: popped and highlighted packets may still reach past the bank's other sides).
 * The Graphics (esi here) has its clip rect at +0x20: x, y, w, h. */
static uint32_t belt_clip[4], g_clip[4], g_patched;

int game_belt_level; /* the seed bank is a conveyor belt (it clips its packets): no shovel */

void game_hook_bank_drawn(CPU *c)
{
    for (int i = 0; i < 4; i++) belt_clip[i] = g_rd32(c->esi + 0x20 + 4 * i);
    game_belt_level = (int32_t)(belt_clip[0] + belt_clip[2]) < 700;
}

static int bank_keys_covered;
void game_hook_bank_redraw(CPU *c)
{
    uint32_t g = c->esi;
    bank_keys_covered = 1;
    for (int i = 0; i < 4; i++) g_clip[i] = g_rd32(g + 0x20 + 4 * i);
    int32_t right = (int32_t)(belt_clip[0] + belt_clip[2]), cur = (int32_t)(g_clip[0] + g_clip[2]);
    if (right < cur && right > (int32_t)g_clip[0]) {
        g_wr32(g + 0x28, (uint32_t)(right - (int32_t)g_clip[0]));
        g_patched = g;
    }
}

/* The keyboard cursors' packet and yellow bracket are drawn through a copy of the Graphics (ebp-0x118)
 * whose clip the DLC clears: give it the belt's right edge too while the redraw above is clipped. */
void game_hook_bank_cursor(CPU *c)
{
    if (!g_patched) return;
    uint32_t copy = c->ebp - 0x118;
    int32_t right = (int32_t)(belt_clip[0] + belt_clip[2]), x0 = (int32_t)g_rd32(copy + 0x20);
    if (right > x0 && right < x0 + (int32_t)g_rd32(copy + 0x28)) g_wr32(copy + 0x28, (uint32_t)(right - x0));
}

/* SeedBank::Draw (0x912c70: [esp + 4] = Graphics) starts: on the DLC's stages the board is drawn moved
 * (game_hook_board_draw_begin) while the plant select shows the street, and the bank (its frame, sun count)
 * moved with it, away from the chooser's cards, which sit where the bank is: drawn where it is */
static uint32_t bank_g;
static int32_t bank_dx;
static void bank_unpan(uint32_t g, int32_t dx)
{
    float t;
    memcpy(&t, G2H(g + 0x08), 4);
    t += (float)dx;
    memcpy(G2H(g + 0x08), &t, 4);
}
static int32_t board_pan_dx(void);
static int bank_keys;
void game_hook_bank_begin(CPU *c)
{
    bank_g = 0;
    bank_keys = bank_keys_covered = 0;
    int32_t dx = board_pan_dx();
    uint32_t g = g_rd32(c->esp + 4);
    if (!dx || !g) return;
    bank_g = g, bank_dx = dx;
    bank_unpan(g, -dx);
}

/* The DLC's "Keybind Hint" (LawnApp+0x656, Settings): SeedBank::Draw writes each packet's key (PlayerInfo +0x5b4c,
 * 0x1c a packet) over its top-left corner right after the packet (TodDrawStringMatrix, 0x9c98f0, cdecl: Graphics *,
 * Font *, const SexyMatrix3 &, const std::string &, const Color &, at the call 0x913154; the matrix holds screen
 * coordinates). Then the keyboard players' pass (0x913347: co-op, so always while a controller plays, and versus)
 * draws every packet again, over the keys: the keys kept here are written again after it, as on the PC without
 * co-op, where nothing covers them. */
#define TOD_DRAW_STRING_MATRIX 0x9c98f0u
#define GFX_PUSH_STATE 0xa2cf40u
#define GFX_POP_STATE 0xa2cba0u
#define GFX_CLEAR_CLIP 0xa006f0u
#define STR_CTOR 0x72a830u
#define STR_DTOR 0x72cd50u
static struct { uint32_t font, matrix[9], color[4]; char text[16]; } bank_key[16];
static uint32_t bank_key_g;

void game_hook_bank_label(CPU *c)
{
    uint32_t g = g_rd32(c->esp), font = g_rd32(c->esp + 4), m = g_rd32(c->esp + 8), s = g_rd32(c->esp + 12),
             col = g_rd32(c->esp + 16);
    if (bank_keys >= 16 || (bank_keys && g != bank_key_g)) return;
    /* (this build's std::string: debug proxy +0, 16-byte buffer +4 or a pointer there, size +20, capacity +24) */
    uint32_t n = g_rd32(s + 20), cap = g_rd32(s + 24), p = cap >= 16 ? g_rd32(s + 4) : s + 4;
    if (!n || n > 15) return;
    bank_key_g = g;
    memcpy(bank_key[bank_keys].text, G2H(p), n);
    bank_key[bank_keys].text[n] = 0;
    bank_key[bank_keys].font = font;
    for (int i = 0; i < 9; i++) bank_key[bank_keys].matrix[i] = g_rd32(m + 4 * i);
    for (int i = 0; i < 4; i++) bank_key[bank_keys].color[i] = g_rd32(col + 4 * i);
    bank_keys++;
}

static uint32_t bank_call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args)
{
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx;
    c->ecx = self;
    uint32_t r = rt_guest_call(c, fn + rt_img_delta, n, args);
    c->eax = eax, c->ecx = ecx, c->edx = edx;
    return r;
}

static void bank_keys_again(CPU *c)
{
    uint32_t g = bank_key_g, esp = c->esp;
    /* scratch under the stack: the matrix (36 bytes), the colour (16), a std::string (28) and its text */
    uint32_t base = (esp - 0x200) & ~15u, mat = base, col = base + 48, str = base + 64, txt = base + 96;
    c->esp = base;
    bank_call(c, GFX_PUSH_STATE, g, 0, NULL);
    bank_call(c, GFX_CLEAR_CLIP, g, 0, NULL);
    for (int k = 0; k < bank_keys; k++) {
        for (int i = 0; i < 9; i++) g_wr32(mat + 4 * i, bank_key[k].matrix[i]);
        for (int i = 0; i < 4; i++) g_wr32(col + 4 * i, bank_key[k].color[i]);
        memcpy(G2H(txt), bank_key[k].text, strlen(bank_key[k].text) + 1);
        bank_call(c, STR_CTOR, str, 1, &txt);
        uint32_t args[5] = {g, bank_key[k].font, mat, str, col};
        bank_call(c, TOD_DRAW_STRING_MATRIX, 0, 5, args);
        bank_call(c, STR_DTOR, str, 0, NULL);
    }
    bank_call(c, GFX_POP_STATE, g, 0, NULL);
    c->esp = esp;
}

void game_hook_bank_done(CPU *c)
{
    if (bank_keys && bank_keys_covered && bank_key_g == c->esi) bank_keys_again(c);
    bank_keys = bank_keys_covered = 0;
    if (bank_g && bank_g == c->esi) bank_unpan(bank_g, bank_dx);
    bank_g = 0;
    if (g_patched && g_patched == c->esi)
        for (int i = 0; i < 4; i++) g_wr32(g_patched + 0x20 + 4 * i, g_clip[i]);
    g_patched = 0;
}

/* The DLC's keyboard player (the controller in levels) refuses to plant from a conveyor belt holding
 * only one seed (it checks GetNumSeedsOnConveyorBelt() <= 1), so a lone Wall-nut could not be bowled
 * until the next one arrived. The mouse has no such rule: let one seed through (an empty belt stays
 * refused). Runs right after the count returns in eax. The rule also kept the belt from ever emptying
 * under the cursor, which two other paths rely on (below). */
void game_hook_kb_plant_count(CPU *c)
{
    if (rt_verbose) rt_log("[kb] player %d plants: packet %d, %d on the belt\n", (int)g_rd32(c->edi + 0x30), (int)g_rd32(c->edi + 0x48), (int)c->eax);
    if (c->eax == 1) c->eax = 2;
}

/* BOMB All Together! (game mode 100): Board +0x5824 counts the explosives still allowed (Board::AddPlant, 0x730500,
 * takes one for every plant but a Wall-nut or a Coffee Bean). At none left the bank greys those packets and the
 * mouse can't take one (SeedPacket's click, 0x91602a: the buzzer), but the keyboard plant (0x752520) never asks,
 * so the controller planted past the limit. Here, at its "may plant" test (al; esi = board, ebp = the cursor, its
 * packet index +0x48), it is refused as the mouse is, by the packet's own type: the buzzer. */
/* (verbose: the keyboard plant of a zombie, 0x7527e0: al = affordable and its packet ready (+0x48), else refused) */
void game_hook_kb_zombie_ok(CPU *c)
{
    uint32_t bank = g_rd32(c->esi + 0x148), cur = g_rd32(c->esp + 0x3c), idx = cur ? g_rd32(cur + 0x48) : 0;
    if (rt_verbose && bank && idx < 16)
        rt_log("[kb] zombie %x: may place %d (packet %u ready %d; brains %d)\n", (unsigned)c->edi, (int)(c->eax & 0xff),
               (unsigned)idx, (int)*(uint8_t *)G2H(bank + 0x54 * idx + 0x70), (int)g_rd32(c->esi + 0x5598));
}
/* (the keyboard plant's CanPlantAt answer, 0x7592e9: 0 plants, anything else is why not) */
void game_hook_kb_can_plant(CPU *c)
{
    if (rt_verbose) rt_log("[kb] CanPlantAt: %d\n", (int)c->eax);
}
void game_hook_kb_plant_ok(CPU *c)
{
    if (rt_verbose) rt_log("[kb] keyboard plant: seed %x, may plant %d\n", (unsigned)c->edi, (int)(c->eax & 0xff));
    uint32_t b = c->esi, app = g_rd32(b + 0x90), bank = g_rd32(b + 0x148);
    if (!app || !bank || g_rd32(app + 0x814) != 100 || (int32_t)g_rd32(b + 0x5824) > 0) return;
    uint32_t idx = g_rd32(c->ebp + 0x48);
    if (idx >= 10) return;
    uint32_t type = g_rd32(bank + 0x28 + 0x54 * idx + 0x34);
    if (type != 3 && type != 0x23 && (c->eax & 0xff)) {
        c->eax &= ~0xffu;
        rt_log("[kb] BOMB All Together: no explosive left, packet %u (seed 0x%x) refused\n", (unsigned)idx, (unsigned)type);
    }
}

/* The flashlight's light (0x737030: Adventure 7-5, game mode 108) is drawn at the mouse: its y in esi here
 * (0x7371d3), its x in edi at 0x7371f9. With the controller playing (the mouse parked off the screen) it is the
 * keyboard cursor's place (pad_light_point). */
void game_hook_light_y(CPU *c)
{
    int x, y;
    if (pad_light_point(&x, &y)) c->esi = (uint32_t)y;
}
void game_hook_light_x(CPU *c)
{
    int x, y;
    if (pad_light_point(&x, &y)) c->edi = (uint32_t)x;
}

/* Conveyor belts: after planting, the keyboard plant (0x752520) steps the player's packet index one to the left
 * (so it stays on a packet once the planted one leaves the belt), but it does so before marking the packet as
 * planted (0x912c50, then SeedPacket::WasPlanted 0x9179d0, which takes the packet at the index off the belt). The
 * packet left of the chosen one went, and the chosen one stayed. The step is skipped there (ebp = the cursor) and
 * made once the packet is marked. */
static uint32_t belt_cursor;
int game_hook_kb_belt_step(CPU *c)
{
    belt_cursor = c->ebp;
    return 1;
}
void game_hook_kb_planted(CPU *c)
{
    if (!belt_cursor) return;
    if (belt_cursor == c->ebp) {
        int32_t idx = (int32_t)g_rd32(c->ebp + 0x48);
        if (idx > 0) g_wr32(c->ebp + 0x48, (uint32_t)(idx - 1));
        rt_log("[kb] planted packet %d from the belt, now on %d\n", (int)idx, idx > 0 ? (int)idx - 1 : 0);
    }
    belt_cursor = 0;
}

/* With the last seed planted the belt is empty, and two keyboard-cursor paths read the selected slot
 * without checking it holds a seed: the P1 tooltip update (0x75f860) and the ghost plant drawn at the
 * cursor (in 0x792990). Both looked up SEED_NONE (an assertion, then a crash). Skip them for an empty
 * slot: the tooltip takes its own hide path, the ghost is not drawn. */
static int slot_empty(uint32_t board, uint32_t cursor)
{
    uint32_t bank = g_rd32(board + 0x148);
    if (!cursor || !bank) return 1;
    int32_t idx = (int32_t)g_rd32(cursor + 0x48);
    /* no such slot (the Zen Garden has no seed packets) is as empty as an empty one */
    if (idx < 0 || idx >= (int32_t)g_rd32(bank + 0x24)) return 1;
    uint32_t type = g_rd32(bank + 0x28 + 0x54 * (uint32_t)idx + 0x34);
    return type == 0xffffffffu || type >= 0x200u;
}

/* edi = board; the P1 cursor. Its tooltip (the packet's name over the seed bank) isn't shown at all: as on the
 * consoles, nothing covers the bank or the mini-games' prompts under it (the Slot Machine's "to pull handle") */
int game_hook_kb_slot_empty(CPU *c)
{
    (void)c;
    return 1;
}

/* DialogButton::Draw (0x886240, thiscall: ecx = the button, [esp + 4] = Graphics) starts, and reaches its one
 * exit (0x8865e0: edi = the button, ebx = the Graphics): the focused main-menu sign's pop (pad_input.c) */
extern void pad_button_draw(uint32_t btn, uint32_t g, int begin);
void game_hook_btn_draw(CPU *c) { pad_button_draw(c->ecx, g_rd32(c->esp + 4), 1); }
void game_hook_btn_draw_end(CPU *c) { pad_button_draw(c->edi, c->ebx, 0); }

/* ebx = the cursor object (board at +4, player at +0x30). Nor is a ghost drawn for a packet that isn't a plant
 * or a zombie: the Slot Machine's sun and diamond (its reels land on them) are past the plants
 * ReanimatorCache::DrawCachedPlant (0x7d0130) can draw (0..0xb2), and it asserts (then the game ends). */
#define IZ_GHOST_DX -60
#define IZ_GHOST_DY -85
int game_hook_kb_ghost_empty(CPU *c)
{
    uint32_t board = g_rd32(c->ebx + 4);
    if (!board) return 0;
    uint32_t cursor = g_rd32(board + 0x178 + 4 * g_rd32(c->ebx + 0x30));
    if (slot_empty(board, cursor)) return 1;
    /* (Vasebreaker: no bank shown, nothing of the keyboard player's to plant: its hidden slot's seed showed) */
    extern int vase_mode(void);
    if (vase_mode()) return 1;
    /* I, Zombie: the zombie's ghost (drawn at ebp-0x10 x / ebp-0x18 y from the cell, set by the DLC's I, Zombie
     * branch at 0x792ec6) stood half a cell right of and a cell below the bracket, where no zombie goes: moved by
     * IZ_GHOST_DX / DY onto where A puts the zombie (Challenge::IZombiePlaceZombie: the cell's x - 30) */
    extern int pad_izombie(void);
    if (pad_izombie()) {
        int dx = getenv("IZDX") ? atoi(getenv("IZDX")) : IZ_GHOST_DX, dy = getenv("IZDY") ? atoi(getenv("IZDY")) : IZ_GHOST_DY;
        g_wr32(c->ebp - 0x10, g_rd32(c->ebp - 0x10) + (uint32_t)dx);
        g_wr32(c->ebp - 0x18, g_rd32(c->ebp - 0x18) + (uint32_t)dy);
    }
    uint32_t bank = g_rd32(board + 0x148), idx = g_rd32(cursor + 0x48);
    uint32_t type = g_rd32(bank + 0x28 + 0x54 * idx + 0x34);
    if (type > 0xb2u) {
        static uint32_t logged;
        if (logged != type) logged = type, rt_log("[kb] no ghost for seed type 0x%x\n", (unsigned)type);
        return 1;
    }
    return 0;
}

/* The keyboard cursor's ghost is drawn by DrawSeedType (call at 0x793108: Graphics, type, imitater, variation,
 * float x, float y). The DLC's zombie check (0x915df0, IsZombieSeedType) only knows zombie types up to 0x89, so
 * Zombies vs. Zombies' own zombies (0x8a and up, game mode 102) got the plants' ghost place: a cell left and a
 * row down from the cursor. They are drawn where the mouse's ghost would be: KB_ZGHOST_DX / DY on. */
#define KB_ZGHOST_DX 72.f
#define KB_ZGHOST_DY -72.f
void game_hook_kb_ghost_pos(CPU *c)
{
    uint32_t app = rt_app_object(), type = g_rd32(c->esp + 4);
    if (!app || g_rd32(app + 0x814) != 102 || type <= 0x89 || type >= 0x200) return;
    float x, y;
    memcpy(&x, G2H(c->esp + 0x10), 4);
    memcpy(&y, G2H(c->esp + 0x14), 4);
    x += getenv("ZGDX") ? (float)atof(getenv("ZGDX")) : KB_ZGHOST_DX;
    y += getenv("ZGDY") ? (float)atof(getenv("ZGDY")) : KB_ZGHOST_DY;
    memcpy(G2H(c->esp + 0x10), &x, 4);
    memcpy(G2H(c->esp + 0x14), &y, 4);
}

/* ---- Board::Update (0x756f70) and 2x speed ----
 * At 2x speed Board::Update runs its body twice (it loops back while the byte it read from +0x5a33 is set,
 * clearing it). The keyboard players' "just pressed" table (+0x5933 = WidgetManager key down +0xf8 and not
 * down last pass, +0x5834) is rebuilt every pass, but +0x5834 was only refreshed after the loop, so the
 * second pass saw every press again: L / R stepped two packets, A planted two Wall-nuts. Refresh it at
 * the loop's end too, as the DLC does after the loop. The cursors also moved on both passes; the second
 * pass's move is dropped (below), so the cursor's speed does not change with the game's. */
static int second_pass;

void game_hook_board_update(CPU *c)
{
    extern void wide_bushes(CPU *c, uint32_t board);
    second_pass = 0;
    wide_bushes(c, c->ecx);
}

void game_hook_speed_pass(CPU *c)
{
    if (!*(uint8_t *)GPG(c->ebp - 0x229)) return; /* no second pass */
    uint32_t board = c->esi, app = g_rd32(board + 0x90), wm = app ? g_rd32(app + 0x320) : 0;
    if (!wm) return;
    for (uint32_t i = 0; i < 255; i++) *(uint8_t *)GPG(board + 0x5834 + i) = *(uint8_t *)GPG(wm + 0xf8 + i);
    second_pass = 1;
}

/* A keyboard cursor (edi; its player at +0x30) moves: xmm2 / xmm1 hold the unit direction from the held
 * keys (8 ways), times its speed (+0x44) next. With the stick tilted the port holds a direction key so
 * the DLC takes this path, and here the direction becomes the stick's, any angle, scaled by the tilt. */
void game_hook_kb_move(CPU *c)
{
    if (second_pass) {
        c->x[2].f[0] = c->x[1].f[0] = 0;
        return;
    }
    float sx, sy;
    if (pad_level_stick((int)g_rd32(c->edi + 0x30), &sx, &sy)) {
        c->x[2].f[0] = sx;
        c->x[1].f[0] = sy;
    }
}

/* TodStringListFind (cdecl: result, name) just returned the string for a key: console wording */
void game_hook_string_found(CPU *c)
{
    pad_string_found(c->eax, g_rd32(c->esp + 4));
}

/* Font::DrawString (ImageFont's, ecx = the font; g, x, y, const std::string &, ...) starts */
void game_hook_font_draw(CPU *c)
{
    uint32_t col = g_rd32(c->esp + 20), was = col;
    extern int pad_font_pushed(uint32_t str, uint32_t *col);
    int push = pad_font_pushed(g_rd32(c->esp + 16), &col);
    if (push) g_wr32(c->esp + 8, g_rd32(c->esp + 8) + (uint32_t)push), g_wr32(c->esp + 12, g_rd32(c->esp + 12) + (uint32_t)push);
    pad_font_draw(c->ecx, g_rd32(c->esp + 4), (int)g_rd32(c->esp + 8), (int)g_rd32(c->esp + 12), g_rd32(c->esp + 16), &col);
    if (col != was) g_wr32(c->esp + 20, col);
}

/* LawnApp::DrawCrazyDave made its "click to continue" string (edi = bubble x, ebx = bubble y; the text
 * goes at +139, +140 in FONT_PICO129, Sexy::FONT_PICO129 at 0x1093fc0) */
extern uint32_t rt_img_delta;
static uint32_t pico_font;

void game_hook_dave_prompt(CPU *c)
{
    /* FONT_PICO129 (ascent 10) was hard to read on the console: the line is drawn in the game's
     * 0x1093f80 font (ascent 14, the bubble's own Brianne style) instead, just for this draw */
    uint32_t big = g_rd32(0x1093f80u + rt_img_delta);
    pico_font = g_rd32(0x1093fc0u + rt_img_delta);
    if (big) g_wr32(0x1093fc0u + rt_img_delta, big);
    pad_dave_prompt((int)(c->edi + 0x8b), (int)(c->ebx + 0x8c), big ? big : pico_font);
}

/* ... and after that one TodDrawString, FONT_PICO129 is itself again */
void game_hook_dave_prompt_done(CPU *c)
{
    (void)c;
    if (pico_font) g_wr32(0x1093fc0u + rt_img_delta, pico_font);
    pico_font = 0;
}

/* The Zen Garden: the keyboard player's brackets sat high on the pots (the DLC places them for the lawn's
 * cells). They are drawn lower there, through the Graphics' translation (+0x0c mTransY) for that draw. */
#define ZEN_CURSOR_DY 18.f
static uint32_t kbc_g;
void game_hook_kbcursor_draw(CPU *c)
{
    uint32_t app = rt_app_object();
    kbc_g = 0;
    if (!app || g_rd32(app + 0x814) != 50) return;
    kbc_g = g_rd32(c->esp + 4);
    extern void pad_zen_selector(uint32_t g);
    pad_zen_selector(kbc_g);
    float t;
    memcpy(&t, G2H(kbc_g + 0x0c), 4);
    t += ZEN_CURSOR_DY;
    memcpy(G2H(kbc_g + 0x0c), &t, 4);
}
void game_hook_kbcursor_drawn(CPU *c)
{
    if (!kbc_g || g_rd32(c->ebp + 0xd0) != kbc_g) { kbc_g = 0; return; }
    float t;
    memcpy(&t, G2H(kbc_g + 0x0c), 4);
    t -= ZEN_CURSOR_DY;
    memcpy(G2H(kbc_g + 0x0c), &t, 4);
    kbc_g = 0;
}

/* Widescreen (rt_widescreen): the draw surface shows 133 px more on each side of the game's 800x600
 * window (hle_ddraw). The game clips everything to its window and to each widget, so a clip that reaches
 * the window's edge, asked for by a rect that reaches it too, runs on to the screen's edge. Graphics:
 * +0x08 mTransX, +0x0c mTransY (float), +0x20 mClipRect (x, y, w, h). */
#define WS_L (-133)
#define WS_R (800 + 133)
static int32_t clip_parent[4];
static float g_transf(uint32_t g, uint32_t off)
{
    float f;
    memcpy(&f, G2H(g + off), 4);
    return f;
}
/* a Graphics over an 800x600 image (the screen) starts with the whole screen as its clip */
void game_hook_graphics_made(CPU *c)
{
    if (!rt_widescreen) return;
    uint32_t g = c->edi;
    if (g_rd32(g + 0x20) == 0 && g_rd32(g + 0x24) == 0 && g_rd32(g + 0x28) == 800 && g_rd32(g + 0x2c) == 600) {
        g_wr32(g + 0x20, (uint32_t)WS_L);
        g_wr32(g + 0x28, (uint32_t)(WS_R - WS_L));
    }
}
/* A reanim track drawn with the whole 800x600 window as its clip takes a fast path that draws with no clip
 * through D3D directly (TodBltMatrix's, inlined in 0x921f50); the menu drawn under a popup comes that
 * way. Its clip is the whole screen instead, so it takes the ordinary path (DDImage::BltMatrix, which the
 * menu's widescreen pieces hang on). The clip Rect is at [esp+0x200] here. */
void game_hook_reanim_clip(CPU *c)
{
    if (!rt_widescreen) return;
    uint32_t r = g_rd32(c->esp + 0x200);
    if (g_rd32(r) == 0 && g_rd32(r + 4) == 0 && g_rd32(r + 8) == 800 && g_rd32(r + 12) == 600) {
        g_wr32(r, (uint32_t)WS_L);
        g_wr32(r + 8, (uint32_t)(WS_R - WS_L));
    }
}
void game_hook_cliprect_in(CPU *c)
{
    if (!rt_widescreen) return;
    for (int i = 0; i < 4; i++) clip_parent[i] = (int32_t)g_rd32(c->ecx + 0x20 + 4 * i);
}
void game_hook_cliprect_out(CPU *c)
{
    if (!rt_widescreen) return;
    uint32_t clip = c->esi, g = clip - 0x20;
    int32_t rx = (int32_t)g_rd32(c->esp + 0x28) + (int32_t)g_transf(g, 0x08), rw = (int32_t)g_rd32(c->esp + 0x30);
    int32_t pl = clip_parent[0], pr = clip_parent[0] + clip_parent[2];
    if (pl <= 0) pl = WS_L;      /* a parent at the window's edge is at the screen's */
    if (pr >= 800) pr = WS_R;
    int32_t l = (int32_t)g_rd32(clip), r = l + (int32_t)g_rd32(clip + 8);
    if (rx <= 0 && l <= 0) l = pl;
    if (rx + rw >= 800 && r >= 800) r = pr;
    if (r < l) r = l;
    g_wr32(clip, (uint32_t)l);
    g_wr32(clip + 8, (uint32_t)(r - l));
}
void game_hook_fillrect(CPU *c)
{
    if (!rt_widescreen) return;
    int32_t x = (int32_t)g_rd32(c->esp + 4), w = (int32_t)g_rd32(c->esp + 12);
    int32_t ax = x + (int32_t)g_transf(c->ecx, 0x08);
    if (ax <= 0 && ax + w >= 800 && rt_verbose) {
        static uint32_t callers[32];
        static int n;
        uint32_t ra = g_rd32(c->esp) - rt_img_delta;
        int k = 0;
        while (k < n && callers[k] != ra) k++;
        if (k == n && n < 32) {
            callers[n++] = ra;
            rt_log("[wide] fill from %08x: %d,%d %dx%d (trans %d) colour a %d\n", (unsigned)ra, x, (int)g_rd32(c->esp + 8), w,
                   (int)g_rd32(c->esp + 16), (int)g_transf(c->ecx, 0x08), (int)g_rd32(c->ecx + 0x3c));
        }
    }
    /* the award screens drawn wide (the menu's Help 2, the Multiplayer HELP 4, the letters 5): their fade in
     * (the fill at 0x7df6e7) was cut to 4:3 by the screen's clip: the clip (+0x20 x, +0x28 width) to the
     * screen's edges for it */
    extern int pad_award_type(void);
    if (ax <= 0 && ax + w >= 800 && g_rd32(c->esp) - rt_img_delta == 0x7df6e7u) {
        int t = pad_award_type();
        if ((t == 2 || t == 4 || t == 5) && (int32_t)g_rd32(c->ecx + 0x20) >= 0)
            g_wr32(c->ecx + 0x20, (uint32_t)WS_L), g_wr32(c->ecx + 0x28, (uint32_t)(WS_R - WS_L));
    }
    if (ax <= 0 && ax + w >= 800) { /* a fill over the whole window (fades, black-outs) covers the screen */
        g_wr32(c->esp + 4, (uint32_t)(x - (ax - WS_L)));
        g_wr32(c->esp + 12, (uint32_t)(WS_R - WS_L));
    }
}

/* Widescreen: the level's camera moves the board widget along its 1400-px background (pans to the street
 * at -380 = 800 - 1180, scrolled to the house at +220). With 133 more shown on each side those stops would
 * show past the picture's ends, so the board is drawn as if within 133 of them (-247 .. 87): Board::Draw (0x7359f0)
 * gets its Graphics' translation (+0x08) moved by the difference (its clip is the screen's), put back at its
 * returns. The
 * board's own place is left alone (the pan is the game's, and so are its hit tests). The original stages
 * have HenryJk's wider pictures and need none (the Moon too: its blood moon has a wide one). */
void game_frame_begin(void) {}
static uint32_t pan_g;
static int32_t pan_dx;
void game_hook_board_draw_begin(CPU *c)
{
    pan_g = 0;
    if (!rt_widescreen) return;
    uint32_t b = c->ecx, g = g_rd32(c->esp + 4);
    if (!b || !g || g_rd32(b + 0x5580) <= 5) return;
    int32_t x = (int32_t)g_rd32(b + 0x34), cx = x < -380 + 133 ? -380 + 133 : x > 220 - 133 ? 220 - 133 : x;
    if (cx == x) return;
    pan_g = g, pan_dx = cx - x;
    float t;
    memcpy(&t, G2H(g + 0x08), 4);
    t += (float)pan_dx;
    memcpy(G2H(g + 0x08), &t, 4);
}
static int32_t board_pan_dx(void) { return pan_g ? pan_dx : 0; }
void game_hook_board_draw_end(CPU *c)
{
    (void)c;
    if (!pan_g) return;
    float t;
    memcpy(&t, G2H(pan_g + 0x08), 4);
    t -= (float)pan_dx;
    memcpy(G2H(pan_g + 0x08), &t, 4);
    pan_g = 0;
}

/* The flashlight's darkness (0x737030: Adventure 7-5, game mode 108) is drawn on the screen during the level's
 * opening pan (scene 2), not on the lawn: it takes the board's place (+0x34) back off its Graphics' translation
 * (0x737116), then draws its layer (LawnApp +0x8e8) from 220 left of the screen. The widescreen pan above had moved
 * that translation by pan_dx more, so the layer began 133 right of the screen's left end and that strip was left
 * undimmed: pan_dx comes off too (ebp = the Graphics; the function pushes and pops its state). */
void game_hook_light_pan(CPU *c)
{
    int32_t dx = board_pan_dx();
    static int logged;
    if (rt_verbose && !logged++) {
        uint32_t app = g_rd32(c->ebx + 0x90), img = app ? g_rd32(app + 0x8e8) : 0;
        if (img) rt_log("[wide] flashlight darkness %dx%d, pan %d\n", (int)g_rd32(img + 0x24), (int)g_rd32(img + 0x28), (int)dx);
    }
    if (!dx) return;
    float t;
    memcpy(&t, G2H(c->ebp + 0x08), 4);
    t -= (float)dx;
    memcpy(G2H(c->ebp + 0x08), &t, 4);
}

/* ---------------- plant select speed ----------------
 * The DLC is a debug build: its seed packets look their seed's data up in a std::map<int, ...> through
 * operator[] (0x824d90) about 5000 times a frame on the plant select screen, each lookup a dozen
 * debug-checked compares and iterator bookkeeping (10-15 fps on the Switch). The answers (the address of the
 * key's value, a map node, which never moves) are cached per map and key; a map whose head or size changes
 * drops its entries. */
#define SEEDMAP_N 1024
static struct { uint32_t map, key, head, size, value; } seedmap[SEEDMAP_N];
static uint32_t seedmap_map, seedmap_key;
static unsigned seedmap_slot(uint32_t map, uint32_t key) { return ((map >> 4) * 31u + key * 2654435761u) & (SEEDMAP_N - 1); }

int game_hook_seedmap_get(CPU *c)
{
    uint32_t map = c->ecx, kp = g_rd32(c->esp + 4), key = g_rd32(kp);
    uint32_t head = g_rd32(map + 4), size = g_rd32(map + 8);
    unsigned s = seedmap_slot(map, key);
    if (seedmap[s].map == map && seedmap[s].key == key && seedmap[s].head == head && seedmap[s].size == size && seedmap[s].value) {
        c->eax = seedmap[s].value;
        return 1;
    }
    seedmap_map = map, seedmap_key = key;
    return 0;
}

void game_hook_seedmap_got(CPU *c)
{
    uint32_t map = seedmap_map;
    if (!map) return;
    unsigned s = seedmap_slot(map, seedmap_key);
    seedmap[s].map = map, seedmap[s].key = seedmap_key;
    seedmap[s].head = g_rd32(map + 4), seedmap[s].size = g_rd32(map + 8);
    seedmap[s].value = c->eax;
    seedmap_map = 0;
}

/* std::_Debug_lt_pred(pred, a, b): pred(a, b), and when true pred(b, a) again to check the ordering is
 * strict; the check is skipped (the answer stands) */
int game_hook_lt_checked(CPU *c)
{
    (void)c;
    return 1;
}

/* TodDrawString (0x9c97f0, cdecl: Graphics *, const std::string &, x, y, Font *, const Color &, justification):
 * a line well above or below the screen is not drawn. The DLC's credits page draws every line of its 7568 px
 * scroll each frame, measuring each (StringWidth, character by character through the debug STL). */
int game_hook_draw_string_cull(CPU *c)
{
    uint32_t g = g_rd32(c->esp + 4);
    /* nor the players' names over their cursors (PlayerInfo +0x1168 / +0x1184, typed on the Multiplayer
     * screen) outside versus: single player has no name over its cursor */
    uint32_t app = rt_app_object(), str = g_rd32(c->esp + 8);
    if (app) {
        uint32_t mode = g_rd32(app + 0x814), pi = g_rd32(app + 0x848);
        if (pi && (mode < 144 || mode > 151) && (str == pi + 0x1168 || str == pi + 0x1184)) return 1;
    }
    float ty;
    memcpy(&ty, G2H(g + 0x0c), 4);
    float y = (float)(int32_t)g_rd32(c->esp + 16) + ty;
    return y < -120.f || y > 720.f;
}

/* The debug STL's iterator tracking (std::_Iterator_base12, MSVC 14.44): every iterator made or copied was
 * linked into its container's list under a global lock (_Lockit 3: _Mtxlock / _Mtxunlock), and unlinked
 * again by a walk of that list, so a container's iterators could all be invalidated when it changes. On the
 * credits page and the plant select that was over a third of the frame (17 fps on the Switch). Iterators
 * now only point at their container's proxy (so the debug checks that compare containers still work) and are
 * never listed: _Adopt (0x7615d0 unlocked, 0x761590 locked) sets the proxy; _Orphan_me (0x761e90 unlocked,
 * 0x761e60 locked) clears it. The lists stay empty, so the containers' own invalidation walks find nothing.
 * Iterator layout: +0 _Myproxy, +4 _Mynextiter; a container's +0 is its proxy. */
int game_hook_iter_adopt(CPU *c)
{
    uint32_t it = c->ecx, parent = g_rd32(c->esp + 4);
    g_wr32(it, parent ? g_rd32(parent) : 0);
    g_wr32(it + 4, 0);
    return 1;
}
int game_hook_iter_orphan(CPU *c)
{
    g_wr32(c->ecx, 0);
    return 1;
}

/* std::_Mtxlock / _Mtxunlock (0xbab120 / 0xbab140, cdecl (CRITICAL_SECTION *)): the lock taken here directly */
int game_hook_mtxlock(CPU *c)
{
    extern void rt_cs_enter(uint32_t cs);
    rt_cs_enter(g_rd32(c->esp + 4));
    return 1;
}
int game_hook_mtxunlock(CPU *c)
{
    extern void rt_cs_leave(uint32_t cs);
    rt_cs_leave(g_rd32(c->esp + 4));
    return 1;
}

/* Versus against the AI (Multiplayer's "AI Opponent (Beta)": PlayerInfo +0x5df4 cleared, the save's TASO) turns
 * the DLC's keyboard players off: every keyboard-player path checks that byte too, so a controller (player 1, a
 * keyboard player) did nothing there, only the mouse played. At each of those checks that serve player 1's
 * keyboard cursor (its keys, update, drawing; not the AI's own checks or the bans), an AI match with player 1
 * a keyboard player takes the path the byte set would take, as a match without the AI does. Player 2 is the AI
 * (pad_input.c clears P2KB +0x5cd5 then). */
static int ai_kb(int site)
{
    static uint32_t mask = 0xffffffffu;
    static int env;
    if (!env++ && getenv("RT_AI_KB")) mask = (uint32_t)strtoul(getenv("RT_AI_KB"), NULL, 16);
    if (!(mask >> site & 1)) return 0;
    uint32_t app = rt_app_object();
    if (!app) return 0;
    int32_t mode = (int32_t)g_rd32(app + 0x814);
    uint32_t pi = g_rd32(app + 0x848);
    if (mode < 144 || mode > 151 || !pi) return 0;
    return *(volatile uint8_t *)G2H(pi + 0x5df4) == 0 && *(volatile uint8_t *)G2H(pi + 0x5cd4) != 0;
}
int game_hook_ai_kb0(CPU *c) { (void)c; return ai_kb(0); }
int game_hook_ai_kb1(CPU *c) { (void)c; return ai_kb(1); }
int game_hook_ai_kb2(CPU *c) { (void)c; return ai_kb(2); }
int game_hook_ai_kb3(CPU *c) { (void)c; return ai_kb(3); }
int game_hook_ai_kb4(CPU *c) { (void)c; return ai_kb(4); }
int game_hook_ai_kb5(CPU *c) { (void)c; return ai_kb(5); }
int game_hook_ai_kb6(CPU *c) { (void)c; return ai_kb(6); }
int game_hook_ai_kb7(CPU *c) { (void)c; return ai_kb(7); }
int game_hook_ai_kb8(CPU *c) { (void)c; return ai_kb(8); }
int game_hook_ai_kb9(CPU *c) { (void)c; return ai_kb(9); }
int game_hook_ai_kb10(CPU *c) { (void)c; return ai_kb(10); }
int game_hook_ai_kb11(CPU *c) { (void)c; return ai_kb(11); }
int game_hook_ai_kb12(CPU *c) { (void)c; return ai_kb(12); }
int game_hook_ai_kb13(CPU *c) { (void)c; return ai_kb(13); }
int game_hook_ai_kb14(CPU *c) { (void)c; return ai_kb(14); }


/* Versus on one touch screen: player 2's cursor isn't drawn while fingers play (pad_p2_cursor_hidden): Board::Draw's
 * call for it (0x73def9 -> 0x73df05), and SeedBank::Draw's bracket on its packet (the cursors' loop, ecx = the
 * player, 0x9134c0 -> 0x91386c) */
/* The Keybind Hint's keys (seed bank, shovel, the Menu and speed buttons, the Zen Garden's tools) skipped while a
 * controller plays (pad_keys_hint_hidden): each check of LawnApp+0x656 goes where the setting is off */
extern int pad_keys_hint_hidden(void);
int game_hook_hint_hidden(CPU *c) { (void)c; return pad_keys_hint_hidden(); }
extern int pad_p2_cursor_hidden(void);
int game_hook_p2_cursor_draw(CPU *c) { (void)c; return pad_p2_cursor_hidden(); }
int game_hook_p2_bank_cursor(CPU *c) { return c->ecx == 1 && pad_p2_cursor_hidden(); }

/* SeedChooserScreen::Draw draws its Menu button (GameButton::Draw, ecx = the button) at 0x909c18, done 0x909c1d */
extern void pad_chooser_menu_draw(uint32_t btn, int begin);
void game_hook_chmenu_draw(CPU *c) { pad_chooser_menu_draw(c->ecx, 1); }
void game_hook_chmenu_drawn(CPU *c) { (void)c; pad_chooser_menu_draw(0, 0); }

/* The DLC's credits page (ChallengeScreen page 11): the port's own section after the DLC team's (pad_input.c). Its
 * Draw has drawn the playtesters (0x801a33, edi = the Graphics, [ebp - 0x8c] = the page's scroll in pixels, every
 * line at its page y less that): the section is drawn, and the rest of the page (the special thanks, the logos,
 * the end) moves down CREDITS_PORT_H, as its background does (credits_wide.jpg / credits_43.jpg, that much taller:
 * tools/make_wide_credits.py). Update clamps the scroll to the page's limit (0x80f604: [esp+8], eax = the page,
 * ebx = the screen; +0x400 the limit kept): 695 for this page, now 695 + 22.4 (in tens of pixels). */
#define CREDITS_PORT_H 224
extern void pad_credits_port(uint32_t g, int32_t scroll);
void game_hook_credits_port(CPU *c)
{
    int32_t scroll = (int32_t)g_rd32(c->ebp - 0x8c);
    pad_credits_port(c->edi, scroll);
    g_wr32(c->ebp - 0x8c, (uint32_t)(scroll - CREDITS_PORT_H));
}
void game_hook_credits_max(CPU *c)
{
    if (c->eax != 0xb) return;
    float m = 695.f + CREDITS_PORT_H / 10.f;
    uint32_t u;
    memcpy(&u, &m, 4);
    g_wr32(c->esp + 8, u);
    g_wr32(c->ebx + 0x400, u);
}

/* Board::DrawShovel has the shovel slot's rect from Board::GetShovelButtonRect (0x73c8e4, [ebp+0x34]: x, y, w, h):
 * versus draws it in the wide screen's corner, off the lawn mower (pad_input.c pad_shovel_rect) */
extern void pad_shovel_rect(uint32_t rect);
void game_hook_shovel_rect(CPU *c) { pad_shovel_rect(c->ebp + 0x34); }

/* ImitaterDialog::Draw has drawn its cards (ebp = the Graphics), its tooltip next: the controller's bracket */
extern void pad_imitater_drawn(uint32_t g);
void game_hook_imit_drawn(CPU *c) { pad_imitater_drawn(c->ebp); }

/* SeedChooserScreen::Draw's end (esi = its Graphics): the controller's bracket on the Imitater */
extern void pad_chooser_drawn(uint32_t g);
void game_hook_chooser_drawn(CPU *c) { pad_chooser_drawn(c->esi); }

/* AlmanacDialog::Draw, its Close button next (ebx = the Graphics): the controller's bracket on the card */
extern void pad_almanac_drawn(uint32_t g);
void game_hook_almanac_drawn(CPU *c) { pad_almanac_drawn(c->ebx); }

/* Board::DrawZenButtons draws the next-garden arrow (Graphics::DrawImage(IMAGE_ZEN_NEXTGARDEN, 566, y), ecx = edi
 * the Graphics) at 0x73f02c, done 0x73f031: held by ZR, drawn pressed (as the game's buttons: darkened, 1 px down
 * and right) */
extern int pad_next_garden_held;
static uint32_t ng_g, ng_col[4];
static uint8_t ng_colorize;
void game_hook_nextgarden_draw(CPU *c)
{
    ng_g = 0;
    if (!pad_next_garden_held) return;
    ng_g = c->edi;
    for (int i = 0; i < 4; i++) ng_col[i] = g_rd32(ng_g + 0x30 + 4 * i);
    ng_colorize = *(uint8_t *)G2H(ng_g + 0x48);
    g_wr32(ng_g + 0x30, 150), g_wr32(ng_g + 0x34, 150), g_wr32(ng_g + 0x38, 150), g_wr32(ng_g + 0x3c, 255);
    *(uint8_t *)G2H(ng_g + 0x48) = 1;
    g_wr32(c->esp + 4, g_rd32(c->esp + 4) + 1);
    g_wr32(c->esp + 8, g_rd32(c->esp + 8) + 1);
}
void game_hook_nextgarden_drawn(CPU *c)
{
    (void)c;
    if (!ng_g) return;
    for (int i = 0; i < 4; i++) g_wr32(ng_g + 0x30 + 4 * i, ng_col[i]);
    *(uint8_t *)G2H(ng_g + 0x48) = ng_colorize;
    ng_g = 0;
}

/* GameSelector's overlay: one of Adventure's level digits (TodDrawImageCelF's args at esp): 1 = drawn popping */
extern int pad_level_digit_draw(uint32_t args);
int game_hook_lvlnum_cel(CPU *c) { return pad_level_digit_draw(c->esp); }

/* 3D acceleration always on (the DLC's Options, "3D Acceleration"). Off, the game draws in software into plain
 * memory and presents an 800x600 software display, which the Switch's deko3d renderer doesn't show: it kept the
 * last picture it had on the GPU (a player's photo, 2026-11-20: the shop's key tag over the whole screen, upside
 * down), and the port's widescreen art and effects are made for the 3D path on every platform. So:
 * SexyAppBase::Set3DAcclerated (0xa9ba50, thiscall: bool is3D, bool reinit) always gets is3D = true (unticking the
 * box changes nothing; it shows ticked again next time), and the registry's Is3D as read at start (0xa95345 loads
 * it into cl, tested at 0xa9534b) is taken as true, so a saved "off" is undone at the next start. */
void game_hook_force_3d_set(CPU *c)
{
    g_wr32(c->esp + 4, (g_rd32(c->esp + 4) & ~0xffu) | 1u);
}

void game_hook_force_3d_read(CPU *c)
{
    c->ecx = (c->ecx & ~0xffu) | 1u;
    *(uint8_t *)G2H(c->ebp - 0x145) = 1;
}

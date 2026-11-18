/* pad_input.h: game controllers -> the DLC's mouse and keyboard (pad_input.c), shared by the Switch
 * and desktop hosts. */
#ifndef PAD_INPUT_H
#define PAD_INPUT_H

#include <stddef.h>
#include <stdint.h>

enum {
    PB_A = 1 << 0, PB_B = 1 << 1, PB_X = 1 << 2, PB_Y = 1 << 3, PB_L = 1 << 4, PB_R = 1 << 5,
    PB_ZL = 1 << 6, PB_ZR = 1 << 7, PB_PLUS = 1 << 8, PB_MINUS = 1 << 9,
    PB_UP = 1 << 10, PB_DOWN = 1 << 11, PB_LEFT = 1 << 12, PB_RIGHT = 1 << 13,
};

typedef struct {
    int connected;
    uint32_t buttons;
    float lx, ly, rx, ry; /* -1..1, y up */
} PadIn;

/* Once per poll with both controllers' state and the seconds since the last call */
void pad_input_update(const PadIn pads[2], float dt);
/* A touch or mouse moved the pointer itself: the pad cursor follows it */
void pad_input_pointer(int x, int y);
/* The cursor the host draws, if a controller is driving it: 1 and its position */
int pad_input_cursor(int *x, int *y);
/* A level's keyboard player p (0/1) is steered by its stick: 1 and the direction (y down) scaled by the
 * tilt, 0 when the stick is centred (the D-pad then moves the DLC's own 8 ways) */
int pad_level_stick(int p, float *x, float *y);
int pad_light_point(int *x, int *y);
/* TodStringListFind found `result` for the key `name` (std::strings): the console wording */
void pad_string_found(uint32_t result, uint32_t name);
/* Font::DrawString is about to draw `str` at x, baseline y with Graphics g: its button pictures */
void pad_font_draw(uint32_t font, uint32_t g, int x, int y, uint32_t str, uint32_t *col); /* (col: the colour argument, may be pointed elsewhere) */
/* Crazy Dave's bubble is drawing its "click to continue" centred at x, baseline y, in font */
void pad_dave_prompt(int x, int y, uint32_t font);

int game_rtti_is(uint32_t obj, const char *want);
const char *game_rtti_name(uint32_t obj);
void game_widget_probe(void);
void game_widget_tree(void);
void game_image_probe(void);
void game_font_probe(void);
uint32_t game_top_widget(void);
/* the focused text box (0 if none), its text, and replacing it as if typed (then Enter) */
uint32_t game_focused_edit(void);
int game_held_packet(void);
int game_touch_on_shovel(int display_x, int display_y);
void game_edit_text(uint32_t edit, char *out, size_t n);
void game_edit_replace(uint32_t edit, const char *text);

#endif

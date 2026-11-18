/* overlay.h: pictures the port draws over the game's display (overlay.c). Positions and sizes are in
 * the game's 800x600 points; source rectangles in the picture's pixels. */
#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdint.h>

#include "../runtime/gpu.h"

enum {
    OV_HELP,         /* 546x42, 42 px cels: A B X Y + R L D-pad ZR ZL - RStick LStick */
    OV_HELP_SMALL,   /* 273x21, the same at half size */
    OV_METER_EMPTY,  /* 46x46: the dig meter's button */
    OV_METER_FULL,   /* 46x46: its ring */
    OV_WHITE,        /* 4x4 white, made in code: plain rectangles */
    OV_PAD,          /* 64x43: a controller (the versus screen's P1 / P2 marks) */
    OV_IMAGES
};
enum { HELP_A, HELP_B, HELP_X, HELP_Y, HELP_PLUS, HELP_R, HELP_L, HELP_DPAD, HELP_ZR, HELP_ZL, HELP_MINUS };

void ov_clear(void);
/* 1: the pictures that follow add light (SRCALPHA, ONE), as glows; 2: they multiply what is under them
 * (ZERO, SRCCOLOR), as a tint; 0: normal blending */
void ov_additive(int on);
/* 1: what follows goes in the list placed while the game draws its frame (cleared at its BeginScene) */
void ov_use_frame_list(int on);
int ov_active(void);
void ov_image(int img, int sx, int sy, int sw, int sh, float x, float y, float w, float h, uint32_t argb);
/* the same, centred on (cx, cy) and turned deg degrees clockwise */
void ov_image_rot(int img, int sx, int sy, int sw, int sh, float cx, float cy, float w, float h, float deg, uint32_t argb);
/* a picture made at run time (ARGB, w x h), kept under `key`; returns its image number for ov_image, or -1 */
/* port_art/<file> as RGBA bytes (free() them), port/<file> in the game folder first; NULL if neither */
uint8_t *ov_art_rgba(const char *file, int *w, int *h);
int ov_dynamic(uint32_t key, int w, int h, const uint32_t *argb);
int ov_dynamic_find(uint32_t key, int *w, int *h);
/* a plain rectangle (game points), and an outline frame t points thick */
void ov_rect(float x, float y, float w, float h, uint32_t argb);
void ov_frame(float x, float y, float w, float h, float t, uint32_t argb);
void ov_pie(int img, float cx, float cy, float r, float frac, uint32_t argb);
/* present (backends): src (w x h) with the list over it; returns the texture to show */
GpuTex *ov_compose(GpuTex *src, uint32_t w, uint32_t h, void (*draw)(const GpuState *, const GpuVtx *, uint32_t));

#endif

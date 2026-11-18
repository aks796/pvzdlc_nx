/* hle_gdi32.c: just enough GDI. DIB sections are real guest memory (the
 * framework draws into them itself); text and blits to the screen DC are
 * no-ops because the screen is DirectDraw's job. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

typedef struct {
    RtObj obj;
    int kind; /* 1 bitmap, 2 font, 3 dc, 4 region */
    uint32_t bits, w, h, bpp, height_px;
} GdiObj;

static uint32_t gdi_new(int kind)
{
    GdiObj *g = calloc(1, sizeof *g);
    g->obj.type = OBJ_OTHER;
    g->kind = kind;
    return rt_handle_new(&g->obj);
}
static GdiObj *gdi_of(uint32_t h) { return (GdiObj *)rt_handle_get(h, OBJ_OTHER); }

HLE(GetDeviceCaps)
{
    switch (ARG(1)) {
    case 8: RET(2, 1280);   /* HORZRES */
    case 10: RET(2, 720);   /* VERTRES */
    case 12: RET(2, 32);    /* BITSPIXEL */
    case 14: RET(2, 1);     /* PLANES */
    case 88: case 90: RET(2, 96); /* LOGPIXELSX/Y */
    case 116: RET(2, 60);   /* VREFRESH */
    default: RET(2, 0);
    }
}
HLE(GetStockObject) { RET(1, 0x00060000u + ARG(0)); }
HLE(CreateCompatibleDC) { RET(1, gdi_new(3)); }
HLE(DeleteDC) { RET(1, rt_handle_close(ARG(0))); }
HLE(CreateDIBSection)
{
    uint32_t bmi = ARG(1), out = ARG(3);
    int32_t w = (int32_t)g_rd32(bmi + 4), h = (int32_t)g_rd32(bmi + 8);
    uint32_t bpp = g_rd16(bmi + 14);
    if (h < 0) h = -h;
    uint32_t stride = (((uint32_t)w * bpp + 31) / 32) * 4;
    uint32_t bits = rt_vm_alloc(0, stride * (uint32_t)h);
    uint32_t hb = gdi_new(1);
    GdiObj *g = gdi_of(hb);
    g->bits = bits;
    g->w = (uint32_t)w;
    g->h = (uint32_t)h;
    g->bpp = bpp;
    if (out) g_wr32(out, bits);
    RET(6, hb);
}
HLE(SelectObject) { RET(2, 0x00060000u); }
HLE(DeleteObject)
{
    GdiObj *g = gdi_of(ARG(0));
    if (g && g->kind == 1 && g->bits) rt_vm_free(g->bits);
    if (g) rt_handle_close(ARG(0));
    RET(1, 1);
}
HLE(GetObjectA)
{
    GdiObj *g = gdi_of(ARG(0));
    uint32_t n = ARG(1), p = ARG(2);
    if (!g || !p) RET(3, 0);
    if (g->kind == 1 && n >= 24) {
        memset(G2H(p), 0, 24);
        g_wr32(p + 4, g->w);
        g_wr32(p + 8, g->h);
        g_wr32(p + 12, ((g->w * g->bpp + 31) / 32) * 4);
        g_wr16(p + 16, 1);
        g_wr16(p + 18, (uint16_t)g->bpp);
        g_wr32(p + 20, g->bits);
        RET(3, 24);
    }
    RET(3, 0);
}
HLE(CreateFontA)
{
    uint32_t h = gdi_new(2);
    GdiObj *g = gdi_of(h);
    int32_t px = (int32_t)ARG(0);
    g->height_px = (uint32_t)(px < 0 ? -px : px ? px : 16);
    RET(14, h);
}
HLE(CreateFontIndirectA)
{
    uint32_t h = gdi_new(2);
    GdiObj *g = gdi_of(h);
    int32_t px = (int32_t)g_rd32(ARG(0));
    g->height_px = (uint32_t)(px < 0 ? -px : px ? px : 16);
    RET(1, h);
}
HLE(GetTextExtentPoint32A)
{
    uint32_t n = ARG(2), sz = ARG(3);
    g_wr32(sz, n * 8);
    g_wr32(sz + 4, 16);
    RET(4, 1);
}
HLE(GetTextMetricsA)
{
    uint32_t p = ARG(1);
    memset(G2H(p), 0, 53);
    g_wr32(p, 16);      /* tmHeight */
    g_wr32(p + 4, 13);  /* tmAscent */
    g_wr32(p + 8, 3);   /* tmDescent */
    g_wr32(p + 20, 8);  /* tmAveCharWidth */
    g_wr32(p + 24, 16); /* tmMaxCharWidth */
    g_wr32(p + 28, 400);
    RET(2, 1);
}
HLE(SetBkMode) { RET(2, 1); }
HLE(SetTextColor) { RET(2, 0); }
HLE(TextOutA) { RET(5, 1); }
HLE(BitBlt) { RET(9, 1); }
HLE(StretchBlt) { RET(11, 1); }
HLE(GdiFlush) { RET(0, 1); }
HLE(Rectangle) { RET(5, 1); }
HLE(CreateRectRgn) { RET(4, gdi_new(4)); }
HLE(SelectClipRgn) { RET(2, 2); }       /* SIMPLEREGION */
HLE(IntersectClipRect) { RET(5, 2); }

const HleEntry hle_gdi32[] = {
#define G(n) {"gdi32.dll", #n, n}
    G(GetDeviceCaps), G(GetStockObject), G(CreateCompatibleDC), G(DeleteDC), G(CreateDIBSection), G(SelectObject),
    G(DeleteObject), G(GetObjectA), G(CreateFontA), G(CreateFontIndirectA), G(GetTextExtentPoint32A),
    G(GetTextMetricsA), G(SetBkMode), G(SetTextColor), G(TextOutA), G(BitBlt), G(StretchBlt), G(GdiFlush),
    G(Rectangle), G(CreateRectRgn), G(SelectClipRgn), G(IntersectClipRect),
#undef G
    {NULL, NULL, NULL}};

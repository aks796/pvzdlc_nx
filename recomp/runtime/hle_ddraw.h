/* hle_ddraw.h: DirectDraw surfaces, shared with the Direct3D 7 device. */
#ifndef HLE_DDRAW_H
#define HLE_DDRAW_H

#include "gpu.h"
#include "rt.h"

/* Always 32 bpp, X8R8G8B8 or (alpha set) A8R8G8B8, in guest memory */
typedef struct Surf {
    uint32_t obj;          /* guest COM object */
    int refs;
    uint32_t w, h, pitch, bits, caps;
    int alpha;
    int has_ck;
    uint32_t ck_lo, ck_hi;
    uint32_t clipper;
    struct Surf *back;     /* attached back buffer of a flipping primary */
    int primary;
    int d3d_target, d3d_texture; /* ever drawn into / sampled by Direct3D */
    /* GPU copy (rt_gpu): made when the GPU first needs the surface. cpu_ok/gpu_ok say which copy is
     * current; whoever writes one copy clears the other's flag. */
    GpuTex *gtex;
    int cpu_ok, gpu_ok;
    /* widescreen: the game's draw surface is wider than the game believes (vw); what it draws lands ox
     * pixels in, so the picture grows on both sides */
    int32_t ox;
    uint32_t vw;
} Surf;

/* Make the GPU copy current (uploading the CPU one if needed); 0 if there is no GPU */
int surf_gpu(Surf *s);
/* Make the CPU copy current (downloading the GPU one if needed) */
void surf_cpu(Surf *s);
static inline void surf_gpu_wrote(Surf *s) { s->cpu_ok = 0; s->gpu_ok = 1; }
static inline void surf_cpu_wrote(Surf *s) { s->gpu_ok = 0; s->cpu_ok = 1; }

/* -v: CPU operations that a GPU renderer would have to sync for, per 5 s */
extern uint32_t st_lock_target, st_blt_from_target, st_blt_to_target, st_lock_tex, st_blt_to_tex, st_fill_target;

extern Surf *primary;
extern int rt_widescreen; /* 1066x600 view (133 more on each side) instead of 800x600 */
#define WIDE_W 1066
#define WIDE_OX 133
Surf *S(uint32_t obj);
Surf *surf_new(uint32_t w, uint32_t h, uint32_t caps);
uint32_t d3d_device_create(Surf *target);
extern uint32_t vt_d3ddev;

#endif

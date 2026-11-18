/* vcdiff.h: applies a VCDIFF patch (RFC 3284: xdelta3's format, so the DLC Edition's Steam / EA / OG Patch.xdelta) */
#pragma once
#include <stddef.h>

typedef void (*VcdiffProgress)(const char *what, int permille);

/* 1 when path starts as a VCDIFF file does */
int vcdiff_is_patch(const char *path);
/* patch applied to source: target written (as target.part, renamed when whole); windows = 0 for all of it, n to
 * decode (and check) only the first n windows without writing anything (target unused). 0 when done; -1 and a
 * message in err when the patch is damaged; -2 when it doesn't fit source (another version of the file) */
int vcdiff_apply(const char *patch, const char *source, const char *target, int windows, VcdiffProgress progress,
                 char *err, size_t errlen);

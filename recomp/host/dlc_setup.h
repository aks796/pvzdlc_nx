/* dlc_setup.h: the first launch's game files, from the DLC Edition exe or from the DLC's patch + the player's PvZ exe */
#pragma once
#include <stddef.h>
#include "evb_unpack.h"

/* What the player put in dir: the DLC Edition exe (an .exe of 100 MB or more), or the DLC team's patch (Steam / EA /
 * OG Patch.xdelta, any of them) and the Plants vs. Zombies exe it is for; own = a file name to leave alone (the app's
 * own program), "" for none. The game is unpacked into dir. 0 when done; 1 when nothing usable is there; -1 when it
 * failed (err says why, for the player). */
int dlc_setup_from_dir(const char *dir, const char *own, EvbProgress progress, char *err, size_t errlen);
/* What the player picked: the DLC Edition exe, or the DLC's patch and then base, the Plants vs. Zombies exe it is for
 * (NULL when they picked the exe). The game is unpacked into dir. 0 when done, -1 when it failed (err). */
int dlc_setup_from_files(const char *exe_or_patch, const char *base, const char *dir, EvbProgress progress, char *err,
                         size_t errlen);
/* 1 when path is a VCDIFF (xdelta) patch */
int dlc_is_patch(const char *path);

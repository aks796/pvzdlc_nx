/* dlc_setup.c: the first launch's game files from what the player gives the port. The DLC Edition is shipped as the
 * DLC team's patches (Steam / EA / OG Patch.xdelta) for the player's own PlantsVsZombies.exe, applied with DeltaPatcher,
 * a Windows program; the port takes either the patched exe, or the patch and the exe it is for: vcdiff.c makes the DLC
 * Edition exe from them (byte for byte the same as DeltaPatcher's), evb_unpack.c unpacks it (and checks it is the
 * 2.7.2 build), and the made exe is deleted again. In a folder (the Switch's, or a desktop app's) with several patches
 * and exes (the DLC's download has all three patches and DeltaPatcher.exe), each patch is tried with each smaller exe
 * on its first window, whose checksum only matches the exe it was made for. */
#include "dlc_setup.h"
#include "vcdiff.h"
#include "../runtime/rt_compat.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MADE_EXE "DLC Edition v2.7.2.exe" /* (made in the game folder, deleted once unpacked) */
#define DLC_EXE_MIN (100ll << 20)         /* the DLC Edition exe is 213 MB; PvZ's own are 0.3-5 MB */
#define MAX_FILES 16

int dlc_is_patch(const char *path) { return vcdiff_is_patch(path); }

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), k = strlen(ext);
    return n > k && !strcasecmp(name + n - k, ext);
}

static int patch_and_unpack(const char *patch, const char *base, const char *dir, EvbProgress progress, char *err,
                            size_t errlen)
{
    char made[1200];
    snprintf(made, sizeof made, "%s/%s", dir, MADE_EXE);
    if (progress) progress("Patching the DLC Edition exe (first launch only)", 0);
    int r = vcdiff_apply(patch, base, made, 0, progress, err, errlen);
    if (r == -2)
        snprintf(err, errlen, "That isn't the Plants vs. Zombies exe this patch is for. Steam Patch is for the Steam "
                              "Game of the Year exe, EA Patch for the EA app's, OG Patch for the original 1.0.0.1051.");
    if (r) return -1;
    if (progress) progress("Unpacking the game (first launch only)", 0);
    r = evb_unpack(made, dir, progress, err, errlen);
    remove(made);
    return r ? -1 : 0;
}

int dlc_setup_from_files(const char *exe_or_patch, const char *base, const char *dir, EvbProgress progress, char *err,
                         size_t errlen)
{
    if (!dlc_is_patch(exe_or_patch)) {
        if (progress) progress("Unpacking the game (first launch only)", 0);
        return evb_unpack(exe_or_patch, dir, progress, err, errlen) ? -1 : 0;
    }
    if (!base || !base[0]) {
        snprintf(err, errlen, "With the DLC's patch, the Plants vs. Zombies exe it is for is needed too.");
        return -1;
    }
    return patch_and_unpack(exe_or_patch, base, dir, progress, err, errlen);
}

int dlc_setup_from_dir(const char *dir, const char *own, EvbProgress progress, char *err, size_t errlen)
{
    static char patches[MAX_FILES][1200], bases[MAX_FILES][1200]; /* (not on the stack) */
    char dlc[1200] = "";
    int np = 0, nb = 0;
    long long best = 0;
    DIR *d = opendir(dir);
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.' || !strcasecmp(n, "dlc_game.exe") || (own && own[0] && !strcasecmp(n, own))) continue;
        char p[1200];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, n);
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        if (has_ext(n, ".exe")) {
            if ((long long)st.st_size >= DLC_EXE_MIN) {
                if ((long long)st.st_size > best) best = (long long)st.st_size, snprintf(dlc, sizeof dlc, "%s", p);
            } else if (nb < MAX_FILES)
                snprintf(bases[nb++], sizeof bases[0], "%s", p);
        } else if ((has_ext(n, ".xdelta") || has_ext(n, ".vcdiff") || has_ext(n, ".delta")) && np < MAX_FILES &&
                   dlc_is_patch(p))
            snprintf(patches[np++], sizeof patches[0], "%s", p);
    }
    closedir(d);
    if (dlc[0]) {
        if (progress) progress("Unpacking the game (first launch only)", 0);
        return evb_unpack(dlc, dir, progress, err, errlen) ? -1 : 0;
    }
    if (!np) return 1;
    if (!nb) {
        snprintf(err, errlen, "The DLC's patch is here, but not the Plants vs. Zombies exe it is for: put your "
                              "PlantsVsZombies.exe in %s too.", dir);
        return -1;
    }
    for (int i = 0; i < np; i++)
        for (int j = 0; j < nb; j++) {
            char e1[300];
            if (vcdiff_apply(patches[i], bases[j], "", 1, NULL, e1, sizeof e1) == 0)
                return patch_and_unpack(patches[i], bases[j], dir, progress, err, errlen);
        }
    snprintf(err, errlen, "None of the DLC's patches here fits the Plants vs. Zombies exe here. Steam Patch is for the "
                          "Steam Game of the Year exe, EA Patch for the EA app's, OG Patch for the original 1.0.0.1051.");
    return -1;
}

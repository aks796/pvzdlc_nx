/* main_switch.c: run the recompiled game on the Nintendo Switch (libnx NRO).
 *
 * Everything lives in sdmc:/switch/pvzdlc/: the player puts the DLC Edition 2.7.2 exe there (any name: their
 * PlantsVsZombies.exe patched by the DLC's xdelta), and on the first launch its packed files are unpacked next to
 * it (evb_unpack.c: dlc_game.exe, main.pak, bass.dll, properties/), as in gamedata/ on the Mac.
 * A file named "verbose" in that folder turns on the -v log. The game needs ~810 MB of guest memory,
 * so it only runs with full application memory (launched over a game, holding R). */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <sys/iosupport.h>

#include <dirent.h>
#include <strings.h>

#include "rt.h"
#include "saltynx.h"
#include "evb_unpack.h"

#define GAME_DIR "sdmc:/switch/pvzdlc"

/* ---- the boot screen: PvZ Touch's green bar (the android32 ports' log_console_progress) on the text
 * console, from launch until the game opens its display (then rt_gpu_start closes it and deko3d takes
 * the window), where the game's own loading bar carries on ---- */
#define TITLE "Plants vs. Zombies: DLC Edition"
static pthread_mutex_t boot_lock = PTHREAD_MUTEX_INITIALIZER;
static int boot_console, boot_closed;

static int centre(int cols, int n) { return n < cols ? (cols - n) / 2 + 1 : 1; }

void rt_boot_progress(const char *what, int permille)
{
    static char last[96];
    static int last_pct = -1;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    int pct = permille / 10;
    pthread_mutex_lock(&boot_lock);
    if (strncmp(what, last, sizeof last - 1))
        rt_log("[boot +%.2fs] %s (%d%%)%s\n", armTicksToNs(armGetSystemTick()) / 1e9, what, pct, boot_closed ? " after close" : "");
    if (!boot_closed && (pct != last_pct || strncmp(what, last, sizeof last - 1))) {
        if (!boot_console) {
            consoleInit(NULL);
            boot_console = 1;
        }
        last_pct = pct;
        snprintf(last, sizeof last, "%s", what);
        enum { COLS = 80, BAR = 56 };
        int fill = permille * BAR / 1000;
        char bar[BAR + 1];
        for (int i = 0; i < BAR; i++) bar[i] = i < fill ? '#' : '-';
        bar[BAR] = 0;
        static const char note[] = "Loading (the first launch takes longer)";
        printf("\x1b[2J");
        printf("\x1b[18;%dH\x1b[32;1m%s\x1b[0m", centre(COLS, (int)strlen(TITLE)), TITLE);
        printf("\x1b[20;%dH%s", centre(COLS, (int)strlen(note)), note);
        printf("\x1b[23;%dH[\x1b[32m%s\x1b[0m] %3d%%", (COLS - BAR - 7) / 2 + 1, bar, pct);
        printf("\x1b[25;%dH%s", centre(COLS, (int)strlen(last)), last);
        fflush(stdout);
        consoleUpdate(NULL);
    }
    pthread_mutex_unlock(&boot_lock);
}

static ssize_t null_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r;
    (void)fd;
    (void)ptr;
    return (ssize_t)len;
}

/* The game is opening its display: hand the window over for good */
void rt_boot_screen_close(void)
{
    pthread_mutex_lock(&boot_lock);
    rt_log("[boot +%.2fs] screen closed\n", armTicksToNs(armGetSystemTick()) / 1e9);
    if (boot_console) consoleExit(NULL);
    boot_console = 0;
    boot_closed = 1;
    /* stdout / stderr stay pointed at the console device after consoleExit; a write there once deko3d owns
     * the display crashed in libnx's console renderer. From here on they go nowhere (the log is debug.log). */
    static devoptab_t null_dev;
    null_dev = *devoptab_list[STD_OUT];
    null_dev.name = "null";
    null_dev.write_r = null_write;
    devoptab_list[STD_OUT] = &null_dev;
    devoptab_list[STD_ERR] = &null_dev;
    pthread_mutex_unlock(&boot_lock);
}

/* Before the game has a display: a text console, then wait for + */
static void fail(const char *fmt, const char *arg)
{
    rt_boot_screen_close();
    consoleInit(NULL);
    printf("\n  Plants vs. Zombies: DLC Edition\n\n  ");
    printf(fmt, arg);
    printf("\n\n  Press + to exit.\n");
    consoleUpdate(NULL);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

static int game_result;
static void *game_main(void *arg)
{
    (void)arg;
    rt_platform_thread_start();
    game_result = rt_start();
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    snprintf(rt_game_dir, sizeof rt_game_dir, "%s", GAME_DIR);
    /* the port's pictures come with the NRO (romfs:/wide), unless a wide/ folder is next to it (testing art) */
    struct stat wst;
    if (R_SUCCEEDED(romfsInit()) && stat(GAME_DIR "/wide", &wst) != 0) snprintf(rt_wide_root, sizeof rt_wide_root, "romfs:");
    mkdir(GAME_DIR, 0777);
    struct stat st;
    if (stat(GAME_DIR "/verbose", &st) == 0) rt_verbose = 1;
    rt_widescreen = stat(GAME_DIR "/4x3", &st) != 0; /* 16:9 unless the game folder has a file named 4x3 */
    rt_platform_thread_start();
    rt_boot_progress("Starting", 0);

    AppletType at = appletGetAppletType();
    if (at != AppletType_Application && at != AppletType_SystemApplication) {
        fail("%s", "Not enough memory in applet mode.\n  Start it from a game instead: hold R while launching any game.");
        return 1;
    }
    if (stat(GAME_DIR "/dlc_game.exe", &st) != 0 || stat(GAME_DIR "/main.pak", &st) != 0) {
        /* the first launch: the DLC Edition's exe (the biggest .exe here, whatever its name) unpacked */
        char exe[300] = "", err[256];
        long long best = 0;
        DIR *d = opendir(GAME_DIR);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n < 5 || strcasecmp(e->d_name + n - 4, ".exe") || !strcasecmp(e->d_name, "dlc_game.exe")) continue;
            char p[300];
            snprintf(p, sizeof p, "%s/%s", GAME_DIR, e->d_name);
            if (stat(p, &st) == 0 && (long long)st.st_size > best) best = (long long)st.st_size, snprintf(exe, sizeof exe, "%s", p);
        }
        if (d) closedir(d);
        if (!exe[0]) {
            fail("%s", "Put the Plants vs. Zombies: DLC Edition 2.7.2 exe (made with the DLC's\n"
                       "  patch from your own copy of PvZ) in " GAME_DIR "/ next to pvzdlc.nro.");
            return 1;
        }
        rt_log("[boot] unpacking %s\n", exe);
        rt_boot_progress("Unpacking the game (first launch only)", 0);
        if (evb_unpack(exe, GAME_DIR, rt_boot_progress, err, sizeof err)) {
            rt_log("[boot] unpacking failed: %s\n", err);
            fail("%s", err);
            return 1;
        }
        rt_log("[boot] unpacked\n");
    }
    rt_boot_progress("Reserving memory", 30);
    if (rt_mem_init()) {
        fail("%s", "Couldn't reserve the guest address space.");
        return 1;
    }
    rt_hle_init();
    saltynx_init(); /* (the console overlays' FPS: saltynx.c) */
    rt_boot_progress("Loading dlc_game.exe", 80);
    if (rt_load_image(GAME_DIR "/dlc_game.exe")) {
        fail("%s", "dlc_game.exe didn't load (see " GAME_DIR "/debug.log).");
        return 1;
    }
    /* The game's main thread: recompiled code nests one host frame per guest call, deeper than the
     * NRO's own main thread stack allows, so it gets a thread of its own with a big stack */
    rt_boot_progress("Starting the game", 140);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    if (pthread_create(&t, &attr, game_main, NULL)) {
        fail("%s", "Couldn't start the game thread.");
        return 1;
    }
    pthread_join(t, NULL);
    return game_result;
}

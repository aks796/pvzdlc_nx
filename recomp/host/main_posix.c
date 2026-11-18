/* main_posix.c: run the recompiled game on a desktop host (macOS, Windows, Linux) or Android.
 *   pvzdlc_host GAME_EXE GAME_DIR [-v|-vv]     a test run (gamedata/)
 *   pvzdlc_host                                the Mac app: the game folder is ~/Library/Application Support/
 *     Plants vs. Zombies DLC Edition. On the first launch the player picks their DLC Edition exe (or puts it in that
 *     folder) and it is unpacked there (evb_unpack.c), as on the Switch; the port's pictures (wide/) come from the
 *     app's Resources. Options are empty files in the game folder (4x3, verbose), as on the Switch.
 *   the Windows app (no arguments): everything is in the app's own folder (the exe, SDL2.dll, wide/), and the game is
 *     unpacked next to it on the first launch from the DLC Edition exe put there or picked.
 *   Android (SDL_main): the Java setup screen (SetupActivity) has unpacked the game into the app's external files
 *     folder (android_jni.c) and copied the port's pictures into its internal files folder (wide/). */
#include <SDL.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <fcntl.h>
#include <locale.h>
#include <windows.h>
#include <commdlg.h>
#endif

#include "rt.h"
#include "evb_unpack.h"

#define APP_NAME "Plants vs. Zombies DLC Edition"
extern const char *rt_window_title;

#ifndef __ANDROID__
/* ---------------- the first launch's window: a bar while the exe is unpacked ---------------- */
static SDL_Window *boot_win;
static SDL_Renderer *boot_ren;

static void boot_progress(const char *what, int permille)
{
    if (!boot_win) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO)) return;
        boot_win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 64, 0);
        boot_ren = boot_win ? SDL_CreateRenderer(boot_win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
    }
    if (!boot_ren) return;
    SDL_SetWindowTitle(boot_win, what);
    /* the Switch's boot bar: green on dark, a thin frame */
    SDL_SetRenderDrawColor(boot_ren, 24, 24, 24, 255);
    SDL_RenderClear(boot_ren);
    SDL_Rect frame = {20, 22, 440, 20}, bar = {22, 24, (436 * (permille < 0 ? 0 : permille > 1000 ? 1000 : permille)) / 1000, 16};
    SDL_SetRenderDrawColor(boot_ren, 90, 90, 90, 255);
    SDL_RenderDrawRect(boot_ren, &frame);
    SDL_SetRenderDrawColor(boot_ren, 84, 176, 46, 255);
    SDL_RenderFillRect(boot_ren, &bar);
    SDL_RenderPresent(boot_ren);
}

static void boot_close(void)
{
    if (boot_ren) SDL_DestroyRenderer(boot_ren);
    if (boot_win) SDL_DestroyWindow(boot_win);
    boot_ren = NULL, boot_win = NULL;
}

#endif

static void fail(const char *msg)
{
#ifndef __ANDROID__
    boot_close();
#endif
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, msg, NULL);
    rt_log("[app] %s\n", msg);
}

static int exists(const char *dir, const char *name)
{
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return stat(p, &st) == 0;
}

#ifndef __ANDROID__
static char own_exe[260]; /* (Windows: the app's own exe, never the DLC's) */
/* the biggest .exe in dir (the player's DLC Edition exe, any name), "" none */
static void find_exe(const char *dir, char *out, size_t cap)
{
    out[0] = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    long long best = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".exe") || !strcasecmp(e->d_name, "dlc_game.exe") ||
            !strcasecmp(e->d_name, own_exe))
            continue;
        char p[PATH_MAX];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) == 0 && st.st_size > best) best = st.st_size, snprintf(out, cap, "%s", p);
    }
    closedir(d);
}

#ifdef __APPLE__
/* the system's file chooser (through AppleScript, which needs no extra frameworks); "" if cancelled */
static void choose_exe(char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = popen("osascript -e 'POSIX path of (choose file with prompt \"Choose your Plants vs. Zombies: DLC "
                    "Edition exe (v2.7.2)\")' 2>/dev/null",
                    "r");
    if (!f) return;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    pclose(f);
    out[strcspn(out, "\r\n")] = 0;
}
#elif defined(_WIN32)
/* the system's Open dialog; "" if cancelled */
static void choose_exe(char *out, size_t cap)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"Choose your Plants vs. Zombies: DLC Edition exe (v2.7.2)";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    out[0] = 0;
    if (GetOpenFileNameW(&o)) WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)cap, NULL, NULL);
}
#endif

/* The app's game folder and pictures, and the first launch's unpacking. 0 when the game can start. */
static int app_setup(void)
{
#ifdef _WIN32
    /* the app's own folder (UTF-8 file names throughout: the manifest's active code page, and the C library's) */
    setlocale(LC_ALL, ".UTF-8");
    wchar_t wself[MAX_PATH];
    char self[PATH_MAX];
    if (!GetModuleFileNameW(NULL, wself, MAX_PATH) || !WideCharToMultiByte(CP_UTF8, 0, wself, -1, self, sizeof self, NULL, NULL))
        return -1;
    char *slash = strrchr(self, '\\');
    if (!slash) return -1;
    *slash = 0;
    snprintf(own_exe, sizeof own_exe, "%s", slash + 1);
    snprintf(rt_game_dir, sizeof rt_game_dir, "%s", self);
#else
    const char *home = getenv("HOME");
    if (!home) return -1;
    snprintf(rt_game_dir, sizeof rt_game_dir, "%s/Library/Application Support/" APP_NAME, home);
    mkdir(rt_game_dir, 0755);
#endif
    rt_window_title = "Plants vs. Zombies: DLC Edition";
    extern int pad_desktop;
    pad_desktop = 1; /* (the PC game's own mouse and keyboard play first: pad_input.c) */
#ifdef __APPLE__
    /* Contents/MacOS/<exe> -> Contents/Resources (wide/), unless the game folder has its own wide/ (testing art) */
    char self[PATH_MAX], real[PATH_MAX];
    uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) == 0 && realpath(self, real)) {
        char *slash = strrchr(real, '/');
        if (slash) {
            *slash = 0;
            char res[PATH_MAX];
            snprintf(res, sizeof res, "%s/../Resources", real);
            if (exists(res, "wide") && !exists(rt_game_dir, "wide") && realpath(res, self))
                snprintf(rt_wide_root, sizeof rt_wide_root, "%s", self);
        }
    }
#endif
    if (exists(rt_game_dir, "verbose")) rt_verbose = 1;
    if (exists(rt_game_dir, "dlc_game.exe") && exists(rt_game_dir, "main.pak")) return 0;

    /* the first launch: the DLC Edition exe, put in the game folder or picked */
    char exe[PATH_MAX];
    find_exe(rt_game_dir, exe, sizeof exe);
#if defined(__APPLE__) || defined(_WIN32)
    if (!exe[0]) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, APP_NAME,
                                 "Choose your Plants vs. Zombies: DLC Edition v2.7.2 exe (the one the DLC team's patch "
                                 "makes from your own copy of Plants vs. Zombies).\n\nThe game is unpacked from it once; "
                                 "after that the exe isn't needed.",
                                 NULL);
        choose_exe(exe, sizeof exe);
    }
#endif
    if (!exe[0]) {
        char msg[1200];
        snprintf(msg, sizeof msg, "No DLC Edition exe. Put your Plants vs. Zombies: DLC Edition v2.7.2 exe in\n%s\nand "
                                  "start the game again.", rt_game_dir);
        fail(msg);
        return -1;
    }
    char err[400];
    boot_progress("Unpacking the game (first launch only)", 0);
    if (evb_unpack(exe, rt_game_dir, boot_progress, err, sizeof err)) {
        fail(err);
        return -1;
    }
    boot_close();
    return 0;
}
#endif

#ifdef __ANDROID__
static int android_setup(void)
{
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");        /* touches are handled as touches (platform_sdl.c) */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); /* Back = Esc, not "leave the app" */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    const char *ext = SDL_AndroidGetExternalStoragePath(), *in = SDL_AndroidGetInternalStoragePath();
    if (!ext || !in) {
        fail("The app's storage isn't available.");
        return -1;
    }
    snprintf(rt_game_dir, sizeof rt_game_dir, "%s", ext);
    if (exists(in, "wide") && !exists(rt_game_dir, "wide")) snprintf(rt_wide_root, sizeof rt_wide_root, "%s", in);
    rt_window_title = "Plants vs. Zombies: DLC Edition";
    extern int pad_touch_first;
    pad_touch_first = 1; /* (the touch screen plays first, the game's wording with tap: pad_input.c) */
    if (exists(rt_game_dir, "verbose")) rt_verbose = 1;
    if (!exists(rt_game_dir, "dlc_game.exe") || !exists(rt_game_dir, "main.pak")) {
        fail("The game isn't unpacked yet: start the app again and choose your DLC Edition exe.");
        return -1;
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    _set_fmode(_O_BINARY); /* (files are bytes: the C library's text mode turns CR LF into LF and stops at ^Z) */
#endif
    rt_platform_thread_start();
    char exe[PATH_MAX];
#ifdef __ANDROID__
    (void)argv;
    argc = 0;
    if (android_setup()) return 1;
    snprintf(exe, sizeof exe, "%s/dlc_game.exe", rt_game_dir);
#else
    if (argc >= 3) {
        snprintf(rt_game_dir, sizeof rt_game_dir, "%s", argv[2]);
        snprintf(exe, sizeof exe, "%s", argv[1]);
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "-v")) rt_verbose = 1;
            if (!strcmp(argv[i], "-vv")) rt_verbose = 2;
        }
    } else {
        if (app_setup()) return 1;
        snprintf(exe, sizeof exe, "%s/dlc_game.exe", rt_game_dir);
    }
#endif
    /* 16:9 unless the game folder has a file named 4x3 */
    char p43[PATH_MAX];
    snprintf(p43, sizeof p43, "%s/4x3", rt_game_dir);
    struct stat st43;
    rt_widescreen = stat(p43, &st43) != 0;
    if (rt_mem_init()) {
        fail("Can't reserve the game's 4 GiB memory window.");
        return 1;
    }
    rt_hle_init();
    if (rt_load_image(exe)) {
        fail("dlc_game.exe didn't load (see debug.log in the game folder).");
        return 1;
    }
    return rt_start();
}

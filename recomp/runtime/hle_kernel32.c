/* hle_kernel32.c: kernel32.dll for the recompiled game.
 * Arguments are read from the guest stack (ARG(n)); every function here is
 * stdcall, so RET(n, value) pops n arguments. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <fnmatch.h>
#endif
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "rt.h"

#define PROCESS_HEAP 0x00a00000u
#define PSEUDO_PROCESS 0xffffffffu
#define PSEUDO_THREAD 0xfffffffeu

void rt_set_last_error(uint32_t e)
{
    RtThread *t = rt_thread_self();
    if (t) {
        t->last_error = e;
        g_wr32(t->teb + 0x34, e);
    }
}
static uint32_t last_error(void)
{
    RtThread *t = rt_thread_self();
    return t ? g_rd32(t->teb + 0x34) : 0;
}

/* ---------------- modules ---------------- */
static const char *known_dlls[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll",
                                   "ole32.dll", "oleaut32.dll", "winmm.dll", "ws2_32.dll", "winhttp.dll",
                                   "ntdll.dll", "shfolder.dll", "version.dll", "ddraw.dll", "dsound.dll",
                                   "bass.dll", "kernelbase.dll", "imagehlp.dll", NULL};
#define MODULE_HANDLE(i) (0x70000000u + (uint32_t)(i) * 0x10000u)

static int dll_index(const char *name)
{
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char low[64];
    size_t n = 0;
    for (; base[n] && n < sizeof low - 5; n++) low[n] = (char)tolower((unsigned char)base[n]);
    low[n] = 0;
    if (!strchr(low, '.')) strcat(low, ".dll");
    for (int i = 0; known_dlls[i]; i++)
        if (!strcmp(known_dlls[i], low)) return i;
    /* API-set names resolve to kernel32 */
    if (!strncmp(low, "api-ms-win-", 11) || !strncmp(low, "ext-ms-", 7)) return 0;
    return -1;
}

static uint32_t module_for(const char *name)
{
    int i = dll_index(name);
    if (i < 0) {
        rt_log("[k32] module not available: %s\n", name);
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    return MODULE_HANDLE(i);
}

HLE(GetModuleHandleA)
{
    uint32_t n = ARG(0);
    if (!n) RET(1, rt_image.base);
    char s[260];
    rt_a2u(n, s, sizeof s);
    RET(1, module_for(s));
}
HLE(GetModuleHandleW)
{
    uint32_t n = ARG(0);
    if (!n) RET(1, rt_image.base);
    char s[260];
    rt_w2u(n, s, sizeof s);
    RET(1, module_for(s));
}
HLE(GetModuleHandleExW)
{
    uint32_t flags = ARG(0), name = ARG(1), out = ARG(2);
    uint32_t h;
    if (flags & 4) h = rt_image.base; /* FROM_ADDRESS: only our image exists */
    else if (!name) h = rt_image.base;
    else {
        char s[260];
        rt_w2u(name, s, sizeof s);
        h = module_for(s);
    }
    if (out) g_wr32(out, h);
    RET(3, h != 0);
}
HLE(LoadLibraryA)
{
    char s[260];
    rt_a2u(ARG(0), s, sizeof s);
    rt_log("[k32] LoadLibraryA(%s)\n", s);
    RET(1, module_for(s));
}
HLE(LoadLibraryW)
{
    char s[260];
    rt_w2u(ARG(0), s, sizeof s);
    rt_log("[k32] LoadLibraryW(%s)\n", s);
    RET(1, module_for(s));
}
HLE(LoadLibraryExW)
{
    char s[260];
    rt_w2u(ARG(0), s, sizeof s);
    if (rt_verbose) rt_log("[k32] LoadLibraryExW(%s)\n", s);
    RET(3, module_for(s));
}
HLE(FreeLibrary) { RET(1, 1); }

HLE(GetProcAddress)
{
    uint32_t mod = ARG(0), name = ARG(1);
    char s[128];
    if (name < 0x10000) snprintf(s, sizeof s, "#%u", (unsigned)name);
    else rt_a2u(name, s, sizeof s);
    uint32_t i = (mod - 0x70000000u) / 0x10000u;
    uint32_t r = 0;
    if (mod >= 0x70000000u && i < sizeof known_dlls / sizeof *known_dlls - 1) {
        const char *dll = known_dlls[i];
        if (!strcmp(dll, "kernelbase.dll")) dll = "kernel32.dll";
        r = rt_hle_lookup(dll, s);
        if (rt_verbose || !r) rt_log("[k32] GetProcAddress(%s, %s) -> %08x\n", known_dlls[i], s, (unsigned)r);
    } else {
        rt_log("[k32] GetProcAddress(%08x, %s): unknown module\n", (unsigned)mod, s);
    }
    if (!r) rt_set_last_error(ERROR_PROC_NOT_FOUND);
    RET(2, r);
}

static uint32_t put_a(const char *s, uint32_t buf, uint32_t n)
{
    size_t len = strlen(s);
    if (!n) return 0;
    if (len >= n) {
        rt_u2a(s, buf, n);
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    rt_u2a(s, buf, n);
    return (uint32_t)len;
}
static uint32_t put_w(const char *s, uint32_t buf, uint32_t n)
{
    size_t len = strlen(s);
    if (!n) return 0;
    rt_u2w(s, buf, n);
    if (len >= n) {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    return (uint32_t)len;
}

HLE(GetModuleFileNameA) { RET(3, put_a(RT_GUEST_EXE, ARG(1), ARG(2))); }
HLE(GetModuleFileNameW) { RET(3, put_w(RT_GUEST_EXE, ARG(1), ARG(2))); }

static uint32_t cmdline_a, cmdline_w;
HLE(GetCommandLineA)
{
    if (!cmdline_a) cmdline_a = rt_heap_strdup("\"" RT_GUEST_EXE "\"");
    RET(0, cmdline_a);
}
HLE(GetCommandLineW)
{
    if (!cmdline_w) cmdline_w = rt_heap_wstrdup("\"" RT_GUEST_EXE "\"");
    RET(0, cmdline_w);
}
HLE(GetStartupInfoW)
{
    uint32_t p = ARG(0);
    memset(G2H(p), 0, 68);
    g_wr32(p, 68);
    RET(1, 0);
}
HLE(GetEnvironmentStringsW)
{
    uint32_t p = rt_heap_alloc(4, 1);
    RET(0, p);
}
HLE(FreeEnvironmentStringsW) { RET(1, 1); }
HLE(SetEnvironmentVariableW) { RET(2, 1); }

/* ---------------- process / system ---------------- */
HLE(GetCurrentProcess) { RET(0, PSEUDO_PROCESS); }
HLE(GetCurrentProcessId) { RET(0, 0x1000); }
HLE(GetCurrentThread) { RET(0, PSEUDO_THREAD); }
HLE(GetCurrentThreadId) { RET(0, rt_thread_self()->tid); }
HLE(GetCurrentProcessorNumber) { RET(0, 0); }
HLE(ExitProcess)
{
    rt_log("[k32] ExitProcess(%u)\n", (unsigned)ARG(0));
    exit((int)ARG(0));
}
HLE(TerminateProcess)
{
    rt_log("[k32] TerminateProcess(%u)\n", (unsigned)ARG(1));
    exit((int)ARG(1));
}
HLE(IsDebuggerPresent) { RET(0, 0); }
HLE(IsProcessorFeaturePresent)
{
    uint32_t f = ARG(0);
    /* PF_FLOATING_POINT_PRECISION_ERRATA 0, MMX 3, XMMI 6, XMMI64 10, NX 12, CMPXCHG8B? */
    RET(1, f == 3 || f == 6 || f == 10 || f == 12);
}
HLE(SetUnhandledExceptionFilter) { RET(1, 0); }
HLE(UnhandledExceptionFilter)
{
    rt_log("[k32] UnhandledExceptionFilter\n");
    RET(1, 1);
}
HLE(SetErrorMode) { RET(1, 0); }
HLE(SetConsoleCtrlHandler) { RET(2, 1); }
HLE(GetLastError) { RET(0, last_error()); }
HLE(SetLastError)
{
    rt_set_last_error(ARG(0));
    RET(1, 0);
}
HLE(EncodePointer) { RET(1, ARG(0)); }
HLE(DecodePointer) { RET(1, ARG(0)); }
/* The host may follow the game's own progress messages (the Switch boot screen) */
__attribute__((weak)) void rt_game_debug_text(const char *s) { (void)s; }

/* A failed game assertion: log who called it */
static void debug_text_stack(CPU *c, const char *s)
{
    if (!strstr(s, "assertion failed")) return;
    /* the frame-pointer chain: each frame's saved ebp, return address above it */
    rt_log("[debug] callers:");
    for (uint32_t bp = c->ebp, k = 0; k < 24 && bp > c->esp && bp - c->esp < 0x100000u; bp = g_rd32(bp), k++)
        rt_log(" %06x", (unsigned)g_rd32(bp + 4));
    rt_log("\n");
}

HLE(OutputDebugStringA)
{
    char s[1024];
    rt_a2u(ARG(0), s, sizeof s);
    rt_log("[debug] %s%s", s, strchr(s, '\n') ? "" : "\n");
    debug_text_stack(c, s);
    rt_game_debug_text(s);
    RET(1, 0);
}
HLE(OutputDebugStringW)
{
    char s[1024];
    rt_w2u(ARG(0), s, sizeof s);
    rt_log("[debug] %s%s", s, strchr(s, '\n') ? "" : "\n");
    debug_text_stack(c, s);
    rt_game_debug_text(s);
    RET(1, 0);
}
HLE(FormatMessageA)
{
    uint32_t buf = ARG(4), n = ARG(5), flags = ARG(0);
    char s[64];
    snprintf(s, sizeof s, "Error %u", (unsigned)ARG(2));
    if (flags & 0x100) { /* ALLOCATE_BUFFER */
        uint32_t p = rt_heap_strdup(s);
        g_wr32(buf, p);
        RET(7, strlen(s));
    }
    RET(7, put_a(s, buf, n));
}
HLE(MulDiv)
{
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), d = (int32_t)ARG(2);
    if (!d) RET(3, -1);
    int64_t r = a * b;
    r = (r >= 0) == (d >= 0) ? (r + (d > 0 ? d : -d) / 2) / d : (r - (d > 0 ? d : -d) / 2) / d;
    RET(3, (uint32_t)(int32_t)r);
}

HLE(GetVersionExA)
{
    uint32_t p = ARG(0);
    uint32_t size = g_rd32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    g_wr32(p + 4, 6);   /* report Windows 7: what PvZ-era code expects */
    g_wr32(p + 8, 1);
    g_wr32(p + 12, 7601);
    g_wr32(p + 16, 2);  /* VER_PLATFORM_WIN32_NT */
    RET(1, 1);
}
static void system_info(uint32_t p)
{
    memset(G2H(p), 0, 36);
    g_wr16(p, 0);                       /* PROCESSOR_ARCHITECTURE_INTEL */
    g_wr32(p + 4, 0x1000);              /* page size */
    g_wr32(p + 8, 0x10000);
    g_wr32(p + 12, 0x7ffeffff);
    g_wr32(p + 16, 3);                  /* active processor mask */
    g_wr32(p + 20, 2);                  /* processors */
    g_wr32(p + 24, 586);
    g_wr32(p + 28, 0x10000);            /* allocation granularity */
    g_wr16(p + 32, 6);
    g_wr16(p + 34, 0x0f06);
}
HLE(GetSystemInfo) { system_info(ARG(0)); RET(1, 0); }
HLE(GetNativeSystemInfo) { system_info(ARG(0)); RET(1, 0); }
HLE(GetSystemDirectoryA) { RET(2, put_a("C:\\Windows\\System32", ARG(0), ARG(1))); }
HLE(GetWindowsDirectoryA) { RET(2, put_a("C:\\Windows", ARG(0), ARG(1))); }

/* ---------------- time ---------------- */
static uint64_t now_100ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;
}
/* time the host spent outside the game (a system applet over it: the controller screen, the keyboard),
 * left out of the game's clocks so its animations don't jump when it comes back (rt_time_away) */
static uint64_t away_ns;
static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec - away_ns;
}
void rt_time_away(uint64_t ns) { away_ns += ns; }
static uint64_t mono_base;
uint32_t rt_tick_ms(void)
{
    if (!mono_base) mono_base = mono_ns() - 10000000000ull; /* start at 10 s like a fresh Windows box */
    return (uint32_t)((mono_ns() - mono_base) / 1000000ull);
}
HLE(GetSystemTimeAsFileTime)
{
    uint64_t t = now_100ns();
    g_wr32(ARG(0), (uint32_t)t);
    g_wr32(ARG(0) + 4, (uint32_t)(t >> 32));
    RET(1, 0);
}
static void to_systemtime(const struct tm *tm, int ms, uint32_t p)
{
    g_wr16(p, (uint16_t)(tm->tm_year + 1900));
    g_wr16(p + 2, (uint16_t)(tm->tm_mon + 1));
    g_wr16(p + 4, (uint16_t)tm->tm_wday);
    g_wr16(p + 6, (uint16_t)tm->tm_mday);
    g_wr16(p + 8, (uint16_t)tm->tm_hour);
    g_wr16(p + 10, (uint16_t)tm->tm_min);
    g_wr16(p + 12, (uint16_t)tm->tm_sec);
    g_wr16(p + 14, (uint16_t)ms);
}
HLE(GetLocalTime)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t s = tv.tv_sec;
    localtime_r(&s, &tm);
    to_systemtime(&tm, (int)(tv.tv_usec / 1000), ARG(0));
    RET(1, 0);
}
HLE(FileTimeToSystemTime)
{
    uint64_t ft = g_rd32(ARG(0)) | ((uint64_t)g_rd32(ARG(0) + 4) << 32);
    time_t s = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&s, &tm);
    to_systemtime(&tm, (int)((ft / 10000) % 1000), ARG(1));
    RET(2, 1);
}
HLE(CompareFileTime)
{
    uint64_t a = g_rd32(ARG(0)) | ((uint64_t)g_rd32(ARG(0) + 4) << 32);
    uint64_t b = g_rd32(ARG(1)) | ((uint64_t)g_rd32(ARG(1) + 4) << 32);
    RET(2, a < b ? 0xffffffffu : a > b);
}
HLE(GetTimeZoneInformation)
{
    memset(G2H(ARG(0)), 0, 172);
    RET(1, 0);
}
HLE(GetTickCount) { RET(0, rt_tick_ms()); }
HLE(GetTickCount64)
{
    rt_tick_ms();
    uint64_t t = (mono_ns() - mono_base) / 1000000ull;
    c->edx = (uint32_t)(t >> 32);
    RET(0, (uint32_t)t);
}
HLE(QueryPerformanceCounter)
{
    uint64_t t = mono_ns() / 100; /* 10 MHz like modern Windows */
    g_wr32(ARG(0), (uint32_t)t);
    g_wr32(ARG(0) + 4, (uint32_t)(t >> 32));
    RET(1, 1);
}
HLE(QueryPerformanceFrequency)
{
    g_wr32(ARG(0), 10000000);
    g_wr32(ARG(0) + 4, 0);
    RET(1, 1);
}
/* Hosts can sleep more precisely than usleep (macOS: a real-time wake-up) */
__attribute__((weak)) void rt_platform_sleep_us(uint32_t us) { usleep(us); }

/* the game's own thread (the one that ran the entry point), for the frame stats; SexyApp's cursor
 * thread sleeps 10 ms in a loop and must not count */
static int on_main(CPU *c)
{
    return ((RtThread *)c->host)->tid == rt_main_tid;
}

HLE(Sleep)
{
    uint32_t ms = ARG(0);
    uint64_t t0 = rt_verbose ? mono_ns() : 0;
    if (ms) rt_platform_sleep_us(ms * 1000);
    else sched_yield();
    if (rt_verbose && on_main(c)) {
        uint64_t us = (mono_ns() - t0) / 1000;
        rt_stat_sleep_us += us;
        rt_stat_sleeps++;
        if (us > rt_stat_max_sleep_us) rt_stat_max_sleep_us = us;
    }
    RET(1, 0);
}
HLE(SwitchToThread)
{
    sched_yield();
    RET(0, 1);
}

/* ---------------- code pages & locale ---------------- */
HLE(GetACP) { RET(0, 1252); }
HLE(GetOEMCP) { RET(0, 437); }
HLE(GetConsoleOutputCP) { RET(0, 437); }
HLE(IsValidCodePage)
{
    uint32_t cp = ARG(0);
    RET(1, cp == 1252 || cp == 437 || cp == 65001 || cp == 20127 || cp == 28591);
}
HLE(GetCPInfo)
{
    uint32_t p = ARG(1);
    memset(G2H(p), 0, 20);
    g_wr32(p, ARG(0) == 65001 ? 4 : 1);
    *(uint8_t *)G2H(p + 4) = '?';
    RET(2, 1);
}

/* Code page conversion: UTF-8 for CP_UTF8 (65001), Latin-1 for everything else */
HLE(MultiByteToWideChar)
{
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4), dlen = ARG(5);
    int32_t slen = (int32_t)ARG(3);
    if (!src || !slen) {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(6, 0);
    }
    uint32_t n = slen < 0 ? (uint32_t)strlen(G2H(src)) + 1 : (uint32_t)slen;
    const uint8_t *s = G2H(src);
    uint32_t out = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t ch = s[i++];
        if (cp == 65001 && ch >= 0x80) {
            int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
            ch &= 0x3F >> extra;
            for (int k = 0; k < extra && i < n; k++) ch = (ch << 6) | (s[i++] & 0x3F);
        }
        uint32_t units = ch >= 0x10000 ? 2 : 1;
        if (dlen) {
            if (out + units > dlen) {
                rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
                RET(6, 0);
            }
            if (units == 2) {
                ch -= 0x10000;
                g_wr16(dst + 2 * out, (uint16_t)(0xD800 + (ch >> 10)));
                g_wr16(dst + 2 * out + 2, (uint16_t)(0xDC00 + (ch & 0x3FF)));
            } else {
                g_wr16(dst + 2 * out, (uint16_t)ch);
            }
        }
        out += units;
    }
    RET(6, out);
}
HLE(WideCharToMultiByte)
{
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4), dlen = ARG(5), used = ARG(7);
    int32_t slen = (int32_t)ARG(3);
    if (!src || !slen) {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(8, 0);
    }
    uint32_t n = slen < 0 ? (uint32_t)rt_wlen(src) + 1 : (uint32_t)slen;
    uint32_t out = 0;
    int defused = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ch = g_rd16(src + 2 * i);
        if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < n) {
            uint32_t lo = g_rd16(src + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) { ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        uint8_t tmp[4];
        uint32_t k;
        if (cp == 65001) {
            if (ch < 0x80) { tmp[0] = (uint8_t)ch; k = 1; }
            else if (ch < 0x800) { tmp[0] = (uint8_t)(0xC0 | (ch >> 6)); tmp[1] = (uint8_t)(0x80 | (ch & 0x3F)); k = 2; }
            else if (ch < 0x10000) { tmp[0] = (uint8_t)(0xE0 | (ch >> 12)); tmp[1] = (uint8_t)(0x80 | ((ch >> 6) & 0x3F)); tmp[2] = (uint8_t)(0x80 | (ch & 0x3F)); k = 3; }
            else { tmp[0] = (uint8_t)(0xF0 | (ch >> 18)); tmp[1] = (uint8_t)(0x80 | ((ch >> 12) & 0x3F)); tmp[2] = (uint8_t)(0x80 | ((ch >> 6) & 0x3F)); tmp[3] = (uint8_t)(0x80 | (ch & 0x3F)); k = 4; }
        } else {
            if (ch > 0xFF) { ch = '?'; defused = 1; }
            tmp[0] = (uint8_t)ch;
            k = 1;
        }
        if (dlen) {
            if (out + k > dlen) {
                rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
                RET(8, 0);
            }
            memcpy(G2H(dst + out), tmp, k);
        }
        out += k;
    }
    if (used) g_wr32(used, (uint32_t)defused);
    RET(8, out);
}

static uint32_t ctype1(uint32_t ch)
{
    uint32_t t = 0x200; /* C1_DEFINED */
    if (ch < 0x80) {
        if (isupper((int)ch)) t |= 0x1 | 0x100;
        if (islower((int)ch)) t |= 0x2 | 0x100;
        if (isdigit((int)ch)) t |= 0x4;
        if (isspace((int)ch)) t |= 0x8;
        if (ispunct((int)ch)) t |= 0x10;
        if (iscntrl((int)ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit((int)ch)) t |= 0x80;
    } else if (ch >= 0xC0 && ch <= 0xFF && ch != 0xD7 && ch != 0xF7) {
        t |= 0x100 | (ch < 0xDF ? 0x1 : 0x2);
    } else if (ch == 0xA0) {
        t |= 0x8 | 0x40;
    } else if (ch > 0xFF) {
        t |= 0x100;
    } else {
        t |= 0x10;
    }
    return t;
}
HLE(GetStringTypeW)
{
    uint32_t type = ARG(0), src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) n = (int32_t)rt_wlen(src) + 1;
    for (int32_t i = 0; i < n; i++) {
        uint32_t ch = g_rd16(src + 2 * (uint32_t)i);
        g_wr16(out + 2 * (uint32_t)i, (uint16_t)(type == 1 ? ctype1(ch) : 0));
    }
    RET(4, 1);
}
static uint32_t wlower(uint32_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? ch | 0x20 : ch;
}
static uint32_t wupper(uint32_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) ? ch & ~0x20u : ch;
}
static uint32_t lcmap(uint32_t flags, uint32_t src, int32_t n, uint32_t dst, int32_t dn)
{
    if (n < 0) n = (int32_t)rt_wlen(src) + 1;
    if (!dn) return (uint32_t)n;
    if (dn < n) {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t ch = g_rd16(src + 2 * (uint32_t)i);
        if (flags & 0x100) ch = wlower(ch);
        if (flags & 0x200) ch = wupper(ch);
        g_wr16(dst + 2 * (uint32_t)i, (uint16_t)ch);
    }
    return (uint32_t)n;
}
HLE(LCMapStringW) { RET(6, lcmap(ARG(1), ARG(2), (int32_t)ARG(3), ARG(4), (int32_t)ARG(5))); }
HLE(LCMapStringEx) { RET(9, lcmap(ARG(1), ARG(2), (int32_t)ARG(3), ARG(4), (int32_t)ARG(5))); }
static uint32_t wcompare(uint32_t flags, uint32_t a, int32_t na, uint32_t b, int32_t nb)
{
    if (na < 0) na = (int32_t)rt_wlen(a);
    if (nb < 0) nb = (int32_t)rt_wlen(b);
    for (int32_t i = 0;; i++) {
        if (i >= na || i >= nb) return na < nb ? 1 : na > nb ? 3 : 2;
        uint32_t x = g_rd16(a + 2 * (uint32_t)i), y = g_rd16(b + 2 * (uint32_t)i);
        if (flags & 1) { x = wlower(x); y = wlower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
}
HLE(CompareStringW) { RET(6, wcompare(ARG(1), ARG(2), (int32_t)ARG(3), ARG(4), (int32_t)ARG(5))); }
HLE(CompareStringEx) { RET(9, wcompare(ARG(1), ARG(2), (int32_t)ARG(3), ARG(4), (int32_t)ARG(5))); }

static const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *months[] = {"January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"};
static int locale_value(uint32_t type, char *out, size_t cap, uint32_t *num)
{
    type &= 0x0fffffff;
    *num = 0xffffffff;
    const char *s = NULL;
    switch (type) {
    case 0x01: s = "0409"; *num = 0x409; break;          /* ILANGUAGE */
    case 0x02: case 0x1001: s = "English"; break;         /* SLANGUAGE / SENGLANGUAGE */
    case 0x03: s = "ENU"; break;
    case 0x05: s = "1"; *num = 1; break;                  /* ICOUNTRY */
    case 0x06: case 0x1002: s = "United States"; break;
    case 0x07: s = "USA"; break;
    case 0x0B: s = "437"; *num = 437; break;              /* IDEFAULTCODEPAGE */
    case 0x1004: s = "1252"; *num = 1252; break;          /* IDEFAULTANSICODEPAGE */
    case 0x0C: s = ","; break;                            /* SLIST */
    case 0x0D: s = "1"; *num = 1; break;                  /* IMEASURE */
    case 0x0E: s = "."; break;
    case 0x0F: s = ","; break;
    case 0x10: s = "3;0"; break;
    case 0x11: s = "2"; *num = 2; break;
    case 0x12: s = "1"; *num = 1; break;                  /* ILZERO */
    case 0x14: s = "$"; break;
    case 0x15: s = "USD"; break;
    case 0x16: s = "."; break;
    case 0x17: s = ","; break;
    case 0x18: s = "3;0"; break;
    case 0x19: s = "2"; *num = 2; break;
    case 0x1B: s = "0"; *num = 0; break;
    case 0x1C: s = "0"; *num = 0; break;
    case 0x1D: s = "/"; break;
    case 0x1E: s = ":"; break;
    case 0x1F: s = "M/d/yyyy"; break;
    case 0x20: s = "dddd, MMMM d, yyyy"; break;
    case 0x1003: s = "h:mm:ss tt"; break;
    case 0x21: s = "0"; *num = 0; break;
    case 0x22: s = "0"; *num = 0; break;
    case 0x23: s = "0"; *num = 0; break;
    case 0x24: s = "0"; *num = 0; break;
    case 0x25: s = "0"; *num = 0; break;
    case 0x26: s = "0"; *num = 0; break;
    case 0x28: s = "AM"; break;
    case 0x29: s = "PM"; break;
    case 0x50: s = ""; break;                             /* SPOSITIVESIGN */
    case 0x51: s = "-"; break;
    case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: s = "1"; *num = 1; break;
    case 0x59: s = "en"; break;
    case 0x5A: s = "US"; break;
    case 0x5C: s = "en-US"; break;                        /* SNAME */
    case 0x1009: s = "1"; *num = 1; break;                /* ICALENDARTYPE */
    case 0x100C: s = "6"; *num = 6; break;                /* IFIRSTDAYOFWEEK */
    case 0x100D: s = "0"; *num = 0; break;
    case 0x1014: s = "0"; *num = 0; break;
    case 0x1016: s = "0"; *num = 0; break;
    case 0x1018: s = "M/d/yyyy"; break;
    default:
        if (type >= 0x2A && type <= 0x30) s = days[(type - 0x2A + 1) % 7];
        else if (type >= 0x31 && type <= 0x37) { snprintf(out, cap, "%.3s", days[(type - 0x31 + 1) % 7]); return 1; }
        else if (type >= 0x38 && type <= 0x43) s = months[type - 0x38];
        else if (type >= 0x44 && type <= 0x4F) { snprintf(out, cap, "%.3s", months[type - 0x44]); return 1; }
        else if (type == 0x100E) s = "";
        break;
    }
    if (!s) return 0;
    snprintf(out, cap, "%s", s);
    return 1;
}
static uint32_t get_locale_info(uint32_t type, uint32_t buf, uint32_t n)
{
    char s[128];
    uint32_t num;
    if (!locale_value(type, s, sizeof s, &num)) {
        rt_log("[k32] GetLocaleInfo: unknown type %x\n", (unsigned)type);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (type & 0x20000000) { /* LOCALE_RETURN_NUMBER */
        if (num == 0xffffffff) num = (uint32_t)atoi(s);
        if (n >= 2) g_wr32(buf, num);
        return 2;
    }
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (!n) return len;
    if (n < len) {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    rt_u2w(s, buf, n);
    return len;
}
HLE(GetLocaleInfoW) { RET(4, get_locale_info(ARG(1), ARG(2), ARG(3))); }
HLE(GetLocaleInfoEx) { RET(4, get_locale_info(ARG(1), ARG(2), ARG(3))); }
HLE(GetUserDefaultLCID) { RET(0, 0x409); }
HLE(IsValidLocale) { RET(2, 1); }
HLE(EnumSystemLocalesW) { RET(2, 1); }
static uint32_t put_w_len(const char *s, uint32_t buf, uint32_t n)
{
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (!n) return len;
    if (n < len) return 0;
    rt_u2w(s, buf, n);
    return len;
}
HLE(GetDateFormatW)
{
    uint32_t st = ARG(2);
    char s[64];
    if (st) snprintf(s, sizeof s, "%u/%u/%u", g_rd16(st + 2), g_rd16(st + 6), g_rd16(st));
    else snprintf(s, sizeof s, "1/1/2026");
    RET(6, put_w_len(s, ARG(4), ARG(5)));
}
HLE(GetTimeFormatW)
{
    uint32_t st = ARG(2);
    char s[64];
    if (st) snprintf(s, sizeof s, "%u:%02u:%02u", g_rd16(st + 8), g_rd16(st + 10), g_rd16(st + 12));
    else snprintf(s, sizeof s, "12:00:00");
    RET(6, put_w_len(s, ARG(4), ARG(5)));
}

/* ---------------- memory ---------------- */
HLE(GetProcessHeap) { RET(0, PROCESS_HEAP); }
HLE(HeapAlloc)
{
    uint32_t p = rt_heap_alloc(ARG(2), (ARG(1) & 8) != 0);
    if (!p) rt_log("[k32] HeapAlloc(%u) failed\n", (unsigned)ARG(2));
    RET(3, p);
}
HLE(HeapFree)
{
    rt_heap_free(ARG(2));
    RET(3, 1);
}
HLE(HeapReAlloc)
{
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
    if (flags & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
        if (rt_heap_size(p) >= size) RET(4, p);
        RET(4, 0);
    }
    RET(4, rt_heap_realloc(p, size, (flags & 8) != 0));
}
HLE(HeapSize) { RET(3, rt_heap_size(ARG(2))); }
HLE(HeapValidate) { RET(3, 1); }
HLE(HeapQueryInformation) { RET(5, 0); }
HLE(GlobalAlloc) { RET(2, rt_heap_alloc(ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0)); }
HLE(GlobalFree)
{
    rt_heap_free(ARG(0));
    RET(1, 0);
}
HLE(GlobalLock) { RET(1, ARG(0)); }
HLE(GlobalUnlock) { RET(1, 1); }
HLE(LocalFree)
{
    rt_heap_free(ARG(0));
    RET(1, 0);
}
HLE(VirtualProtect)
{
    if (ARG(3)) g_wr32(ARG(3), 0x04);
    RET(4, 1);
}
HLE(IsBadWritePtr) { RET(2, ARG(0) < 0x10000); }
HLE(VirtualQuery)
{
    uint32_t a = ARG(0), mbi = ARG(1);
    uint32_t base = 0, size = 0, prot = 0x04, type = 0x20000; /* MEM_PRIVATE */
    RtThread *t = rt_thread_self();
    if (a >= rt_image.base && a < rt_image.base + rt_image.size) {
        base = rt_image.base;
        size = rt_image.size;
        type = 0x1000000; /* MEM_IMAGE */
        prot = 0x20;
    } else if (t && a >= t->stack_lo && a < t->stack_hi) {
        base = t->stack_lo;
        size = t->stack_hi - t->stack_lo;
    } else if (!rt_vm_query(a, &base, &size)) {
        memset(G2H(mbi), 0, 28);
        g_wr32(mbi, a & ~0xfffu);
        g_wr32(mbi + 12, 0x1000);
        g_wr32(mbi + 16, 0x10000); /* MEM_FREE */
        g_wr32(mbi + 20, 1);
        RET(3, 28);
    }
    g_wr32(mbi, a & ~0xfffu);
    g_wr32(mbi + 4, base);
    g_wr32(mbi + 8, prot);
    g_wr32(mbi + 12, base + size - (a & ~0xfffu));
    g_wr32(mbi + 16, 0x1000); /* MEM_COMMIT */
    g_wr32(mbi + 20, prot);
    g_wr32(mbi + 24, type);
    RET(3, 28);
}

/* ---------------- guest-address-keyed host sync objects ---------------- */
typedef struct { uint32_t key; int kind; pthread_mutex_t m; pthread_cond_t cv; } KeyedObj;
#define KEYED_MAX 4096
static KeyedObj *keyed[KEYED_MAX];
static pthread_mutex_t keyed_lock = PTHREAD_MUTEX_INITIALIZER;

/* Slots are only ever filled (never freed), so finding an existing object needs no lock: critical
 * sections are entered thousands of times a frame. Only adding one takes keyed_lock. */
static KeyedObj *keyed_get(uint32_t key, int recursive)
{
    uint32_t h = (key >> 2) * 2654435761u % KEYED_MAX;
    for (uint32_t i = 0; i < KEYED_MAX; i++) {
        KeyedObj *o = __atomic_load_n(&keyed[(h + i) % KEYED_MAX], __ATOMIC_ACQUIRE);
        if (!o) break;
        if (o->key == key) return o;
    }
    pthread_mutex_lock(&keyed_lock);
    for (uint32_t i = 0; i < KEYED_MAX; i++) {
        KeyedObj *o = keyed[(h + i) % KEYED_MAX];
        if (!o) {
            o = calloc(1, sizeof *o);
            o->key = key;
            pthread_mutexattr_t at;
            pthread_mutexattr_init(&at);
            if (recursive) pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&o->m, &at);
            pthread_cond_init(&o->cv, NULL);
            __atomic_store_n(&keyed[(h + i) % KEYED_MAX], o, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&keyed_lock);
            return o;
        }
        if (o->key == key) {
            pthread_mutex_unlock(&keyed_lock);
            return o;
        }
    }
    pthread_mutex_unlock(&keyed_lock);
    rt_fatal("too many sync objects");
}

/* CRITICAL_SECTION: kept in the guest's own struct, as Windows does: +12 OwningThread (our thread id,
 * claimed with a compare-and-swap), +8 RecursionCount. The DLC enters and leaves critical sections
 * thousands of times a frame (25-40% of the game thread on hardware with a host mutex per call); the
 * uncontended path is now a couple of atomics. A thread that finds it taken yields until it is free
 * (the game's other threads hold them briefly). */
static __thread uint32_t cs_tid;

static uint32_t self_tid(void)
{
    if (!cs_tid) cs_tid = rt_thread_self()->tid;
    return cs_tid;
}

static void cs_init(uint32_t cs)
{
    memset(G2H(cs), 0, 24);
    g_wr32(cs + 4, 0xffffffffu);
}
HLE(InitializeCriticalSection)
{
    cs_init(ARG(0));
    RET(1, 0);
}
HLE(InitializeCriticalSectionAndSpinCount)
{
    cs_init(ARG(0));
    RET(2, 1);
}
HLE(InitializeCriticalSectionEx)
{
    cs_init(ARG(0));
    RET(3, 1);
}
/* the same locks taken from host code (the DLC's debug STL takes its _Lockit ~200k times a second) */
void rt_cs_enter(uint32_t cs)
{
    uint32_t me = self_tid(), *owner = (uint32_t *)G2H(cs + 12), free_ = 0;
    if (__atomic_load_n(owner, __ATOMIC_RELAXED) == me) {
        g_wr32(cs + 8, g_rd32(cs + 8) + 1);
        return;
    }
    for (int spins = 0;; spins++) {
        free_ = 0;
        if (__atomic_compare_exchange_n(owner, &free_, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (spins > 64) sched_yield();
    }
    g_wr32(cs + 8, 1);
}
void rt_cs_leave(uint32_t cs)
{
    uint32_t n = g_rd32(cs + 8);
    if (n > 1) {
        g_wr32(cs + 8, n - 1);
        return;
    }
    g_wr32(cs + 8, 0);
    __atomic_store_n((uint32_t *)G2H(cs + 12), 0, __ATOMIC_RELEASE);
}

HLE(EnterCriticalSection)
{
    uint32_t cs = ARG(0), me = self_tid();
    uint32_t *owner = (uint32_t *)G2H(cs + 12);
    if (__atomic_load_n(owner, __ATOMIC_RELAXED) == me) {
        g_wr32(cs + 8, g_rd32(cs + 8) + 1);
        RET(1, 0);
    }
    uint32_t free_ = 0;
    if (!__atomic_compare_exchange_n(owner, &free_, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        uint64_t t0 = rt_verbose ? mono_ns() : 0;
        for (int spins = 0;; spins++) {
            free_ = 0;
            if (__atomic_compare_exchange_n(owner, &free_, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            if (spins > 64) sched_yield();
        }
        if (rt_verbose && on_main(c)) rt_stat_lock_us += (mono_ns() - t0) / 1000;
    }
    g_wr32(cs + 8, 1);
    RET(1, 0);
}
HLE(LeaveCriticalSection)
{
    uint32_t cs = ARG(0), n = g_rd32(cs + 8);
    if (n > 1) {
        g_wr32(cs + 8, n - 1);
        RET(1, 0);
    }
    g_wr32(cs + 8, 0);
    __atomic_store_n((uint32_t *)G2H(cs + 12), 0, __ATOMIC_RELEASE);
    RET(1, 0);
}
HLE(DeleteCriticalSection) { RET(1, 0); }

/* SRW locks (exclusive only, as the STL uses them) + condition variables */
HLE(AcquireSRWLockExclusive)
{
    pthread_mutex_lock(&keyed_get(ARG(0), 0)->m);
    RET(1, 0);
}
HLE(ReleaseSRWLockExclusive)
{
    pthread_mutex_unlock(&keyed_get(ARG(0), 0)->m);
    RET(1, 0);
}
HLE(TryAcquireSRWLockExclusive) { RET(1, pthread_mutex_trylock(&keyed_get(ARG(0), 0)->m) == 0); }
HLE(SleepConditionVariableSRW)
{
    KeyedObj *cv = keyed_get(ARG(0), 0), *lk = keyed_get(ARG(1), 0);
    uint32_t ms = ARG(2);
    int r = 0;
    if (ms == 0xffffffffu) {
        pthread_cond_wait(&cv->cv, &lk->m);
    } else {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct timespec ts;
        uint64_t ns = (uint64_t)tv.tv_usec * 1000 + (uint64_t)ms * 1000000ull;
        ts.tv_sec = tv.tv_sec + (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        r = pthread_cond_timedwait(&cv->cv, &lk->m, &ts);
    }
    if (r) {
        rt_set_last_error(1460); /* ERROR_TIMEOUT */
        RET(4, 0);
    }
    RET(4, 1);
}
HLE(WakeConditionVariable)
{
    pthread_cond_signal(&keyed_get(ARG(0), 0)->cv);
    RET(1, 0);
}
HLE(WakeAllConditionVariable)
{
    pthread_cond_broadcast(&keyed_get(ARG(0), 0)->cv);
    RET(1, 0);
}
HLE(InitOnceExecuteOnce)
{
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t once = ARG(0), fn = ARG(1), param = ARG(2), ctx = ARG(3);
    uint32_t ok = 1;
    pthread_mutex_lock(&once_lock);
    if (g_rd32(once) != 2) {
        uint32_t args[3] = {once, param, ctx};
        ok = rt_guest_call(c, fn, 3, args) != 0;
        if (ok) g_wr32(once, 2);
    }
    pthread_mutex_unlock(&once_lock);
    RET(4, ok);
}

/* Interlocked singly linked lists (only push/flush are imported) */
static pthread_mutex_t slist_lock = PTHREAD_MUTEX_INITIALIZER;
HLE(InitializeSListHead)
{
    memset(G2H(ARG(0)), 0, 8);
    RET(1, 0);
}
HLE(InterlockedPushEntrySList)
{
    uint32_t head = ARG(0), e = ARG(1);
    pthread_mutex_lock(&slist_lock);
    uint32_t old = g_rd32(head);
    g_wr32(e, old);
    g_wr32(head, e);
    g_wr16(head + 4, (uint16_t)(g_rd16(head + 4) + 1));
    pthread_mutex_unlock(&slist_lock);
    RET(2, old);
}
HLE(InterlockedFlushSList)
{
    uint32_t head = ARG(0);
    pthread_mutex_lock(&slist_lock);
    uint32_t old = g_rd32(head);
    g_wr32(head, 0);
    g_wr16(head + 4, 0);
    pthread_mutex_unlock(&slist_lock);
    RET(1, old);
}
HLE(FlushProcessWriteBuffers)
{
    __sync_synchronize();
    RET(0, 0);
}

/* Kernel objects: events, mutexes, semaphores */
typedef struct {
    RtObj obj;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int manual, signaled;    /* event */
    uint32_t owner, count;   /* mutex: owner tid + recursion; semaphore: count */
    uint32_t max;
} SyncObj;
static SyncObj *sync_new(int type)
{
    SyncObj *s = calloc(1, sizeof *s);
    s->obj.type = type;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv, NULL);
    return s;
}
HLE(CreateEventExW)
{
    SyncObj *s = sync_new(OBJ_EVENT);
    s->manual = (ARG(2) & 1) != 0;
    s->signaled = (ARG(2) & 2) != 0;
    RET(4, rt_handle_new(&s->obj));
}
HLE(CreateMutexA)
{
    SyncObj *s = sync_new(OBJ_MUTEX);
    if (ARG(1)) {
        s->owner = rt_thread_self()->tid;
        s->count = 1;
    }
    rt_set_last_error(0);
    RET(3, rt_handle_new(&s->obj));
}
HLE(CreateSemaphoreExW)
{
    SyncObj *s = sync_new(OBJ_SEMAPHORE);
    s->count = ARG(1);
    s->max = ARG(2);
    RET(6, rt_handle_new(&s->obj));
}

/* Sleep on cv until signalled or until start_ms + ms (wall clock); callers re-check */
static void wait_until(pthread_cond_t *cv, pthread_mutex_t *m, uint32_t ms, uint64_t start_ms)
{
    if (ms == 0xffffffffu) {
        pthread_cond_wait(cv, m);
        return;
    }
    uint64_t end = start_ms + ms;
    struct timespec ts = {(time_t)(end / 1000), (long)(end % 1000) * 1000000L};
    pthread_cond_timedwait(cv, m, &ts);
}
static uint64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

uint32_t rt_wait_thread(uint32_t h, uint32_t ms);
static uint32_t wait_one(uint32_t h, uint32_t ms)
{
    RtObj *o = rt_handle_get(h, OBJ_NONE);
    if (!o) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        return 0xffffffffu;
    }
    if (o->type == OBJ_THREAD) return rt_wait_thread(h, ms);
    SyncObj *s = (SyncObj *)o;
    uint64_t start = wall_ms();
    uint32_t me = rt_thread_self()->tid;
    pthread_mutex_lock(&s->m);
    for (;;) {
        int ready = 0;
        if (o->type == OBJ_EVENT && s->signaled) {
            if (!s->manual) s->signaled = 0;
            ready = 1;
        } else if (o->type == OBJ_MUTEX && (!s->count || s->owner == me)) {
            s->owner = me;
            s->count++;
            ready = 1;
        } else if (o->type == OBJ_SEMAPHORE && s->count) {
            s->count--;
            ready = 1;
        }
        if (ready) {
            pthread_mutex_unlock(&s->m);
            return 0;
        }
        if (ms == 0 || (ms != 0xffffffffu && wall_ms() >= start + ms)) {
            pthread_mutex_unlock(&s->m);
            return 0x102; /* WAIT_TIMEOUT */
        }
        wait_until(&s->cv, &s->m, ms, start);
    }
}
HLE(WaitForSingleObject)
{
    uint64_t t0 = rt_verbose ? mono_ns() : 0;
    uint32_t r = wait_one(ARG(0), ARG(1));
    if (rt_verbose && on_main(c)) rt_stat_lock_us += (mono_ns() - t0) / 1000;
    RET(2, r);
}
HLE(WaitForSingleObjectEx) { RET(3, wait_one(ARG(0), ARG(1))); }

/* ---------------- TLS / FLS ---------------- */
static uint8_t tls_used[64];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
HLE(TlsAlloc)
{
    pthread_mutex_lock(&tls_lock);
    for (uint32_t i = 0; i < 64; i++) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            pthread_mutex_unlock(&tls_lock);
            RET(0, i);
        }
    }
    pthread_mutex_unlock(&tls_lock);
    RET(0, 0xffffffffu);
}
HLE(TlsFree)
{
    if (ARG(0) < 64) tls_used[ARG(0)] = 0;
    RET(1, 1);
}
HLE(TlsGetValue)
{
    rt_set_last_error(0);
    RET(1, ARG(0) < 64 ? g_rd32(rt_thread_self()->teb + 0xe10 + 4 * ARG(0)) : 0);
}
HLE(TlsSetValue)
{
    if (ARG(0) >= 64) RET(2, 0);
    g_wr32(rt_thread_self()->teb + 0xe10 + 4 * ARG(0), ARG(1));
    RET(2, 1);
}
/* FLS: same idea, slots at TEB+0x1000 (128 of them). Callbacks are ignored. */
static uint8_t fls_used[128];
HLE(FlsAlloc)
{
    pthread_mutex_lock(&tls_lock);
    for (uint32_t i = 1; i < 128; i++) {
        if (!fls_used[i]) {
            fls_used[i] = 1;
            pthread_mutex_unlock(&tls_lock);
            RET(1, i);
        }
    }
    pthread_mutex_unlock(&tls_lock);
    RET(1, 0xffffffffu);
}
HLE(FlsFree)
{
    if (ARG(0) < 128) fls_used[ARG(0)] = 0;
    RET(1, 1);
}
HLE(FlsGetValue) { RET(1, ARG(0) < 128 ? g_rd32(rt_thread_self()->teb + 0x1000 + 4 * ARG(0)) : 0); }
HLE(FlsSetValue)
{
    if (ARG(0) >= 128) RET(2, 0);
    g_wr32(rt_thread_self()->teb + 0x1000 + 4 * ARG(0), ARG(1));
    RET(2, 1);
}

/* ---------------- threads ---------------- */
typedef struct {
    RtObj obj;
    RtThread *t;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int started, suspended;
} ThreadObj;

static void *thread_main(void *arg)
{
    ThreadObj *to = arg;
    RtThread *t = to->t;
    rt_platform_thread_start();
    pthread_mutex_lock(&to->m);
    while (to->suspended) pthread_cond_wait(&to->cv, &to->m);
    pthread_mutex_unlock(&to->m);
    rt_thread_attach(t);
    rt_run_tls_callbacks(&t->cpu, 2);
    uint32_t args[1] = {t->param};
    uint32_t r = rt_guest_call(&t->cpu, t->start, 1, args);
    pthread_mutex_lock(&to->m);
    t->exit_code = r;
    t->finished = 1;
    pthread_cond_broadcast(&to->cv);
    pthread_mutex_unlock(&to->m);
    return NULL;
}

uint32_t rt_wait_thread(uint32_t h, uint32_t ms)
{
    ThreadObj *to = (ThreadObj *)rt_handle_get(h, OBJ_THREAD);
    if (!to) return 0xffffffffu;
    uint64_t start = wall_ms();
    pthread_mutex_lock(&to->m);
    while (!to->t->finished) {
        if (ms == 0 || (ms != 0xffffffffu && wall_ms() >= start + ms)) {
            pthread_mutex_unlock(&to->m);
            return 0x102;
        }
        wait_until(&to->cv, &to->m, ms, start);
    }
    pthread_mutex_unlock(&to->m);
    return 0;
}

HLE(CreateThread)
{
    uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), tid_out = ARG(5);
    ThreadObj *to = calloc(1, sizeof *to);
    to->obj.type = OBJ_THREAD;
    pthread_mutex_init(&to->m, NULL);
    pthread_cond_init(&to->cv, NULL);
    to->t = rt_thread_new(stack);
    to->t->start = start;
    to->t->param = param;
    to->suspended = (flags & 4) != 0;
    uint32_t h = rt_handle_new(&to->obj);
    to->t->handle = h;
    if (tid_out) g_wr32(tid_out, to->t->tid);
    rt_log("[k32] CreateThread(start=%08x, param=%08x) tid %u\n", (unsigned)start, (unsigned)param,
           (unsigned)to->t->tid);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20); /* recompiled code nests a host frame per guest call */
    pthread_create(&to->t->pt, &at, thread_main, to);
    RET(6, h);
}
HLE(ResumeThread)
{
    ThreadObj *to = (ThreadObj *)rt_handle_get(ARG(0), OBJ_THREAD);
    if (!to) RET(1, 0xffffffffu);
    pthread_mutex_lock(&to->m);
    uint32_t prev = (uint32_t)to->suspended;
    to->suspended = 0;
    pthread_cond_broadcast(&to->cv);
    pthread_mutex_unlock(&to->m);
    RET(1, prev);
}
/* A thread leaving through ExitThread (the CRT's _endthreadex) never returns to thread_main: mark it
 * finished here, or WaitForSingleObject on its handle waits forever */
static void exit_current_thread(uint32_t code)
{
    RtThread *t = rt_thread_self();
    ThreadObj *to = t && t->handle ? (ThreadObj *)rt_handle_get(t->handle, OBJ_THREAD) : NULL;
    if (to) {
        pthread_mutex_lock(&to->m);
        t->exit_code = code;
        t->finished = 1;
        pthread_cond_broadcast(&to->cv);
        pthread_mutex_unlock(&to->m);
    }
    pthread_exit(NULL);
}
HLE(ExitThread)
{
    rt_log("[k32] ExitThread(%u)\n", (unsigned)ARG(0));
    exit_current_thread(ARG(0));
}
HLE(FreeLibraryAndExitThread) { exit_current_thread(ARG(1)); }
HLE(GetExitCodeThread)
{
    ThreadObj *to = (ThreadObj *)rt_handle_get(ARG(0), OBJ_THREAD);
    uint32_t code = 259; /* STILL_ACTIVE */
    if (to && to->t->finished) code = to->t->exit_code;
    if (ARG(1)) g_wr32(ARG(1), code);
    RET(2, 1);
}
HLE(SetThreadPriority) { RET(2, 1); }
HLE(GetThreadPriority) { RET(1, 0); }
HLE(IsThreadAFiber) { RET(0, 0); }

/* ---------------- files ---------------- */
typedef struct {
    RtObj obj;
    int fd;
    int is_std;
    char path[512];
} FileObj;
typedef struct {
    RtObj obj;
    DIR *d;
    char dir[512];
    char pattern[256];
    int wide;
} FindObj;

static void file_destroy(RtObj *o)
{
    FileObj *f = (FileObj *)o;
    if (f->fd >= 0 && !f->is_std) close(f->fd);
    free(f);
}
static void find_destroy(RtObj *o)
{
    FindObj *f = (FindObj *)o;
    if (f->d) closedir(f->d);
    free(f);
}

static uint32_t std_handles[3];
static uint32_t std_handle(int which)
{
    if (!std_handles[which]) {
        FileObj *f = calloc(1, sizeof *f);
        f->obj.type = OBJ_FILE;
        f->obj.destroy = file_destroy;
        f->fd = which == 0 ? 0 : which;
        f->is_std = 1;
        std_handles[which] = rt_handle_new(&f->obj);
    }
    return std_handles[which];
}
HLE(GetStdHandle)
{
    int32_t n = (int32_t)ARG(0);
    RET(1, n == -10 ? std_handle(0) : n == -11 ? std_handle(1) : n == -12 ? std_handle(2) : 0xffffffffu);
}
HLE(SetStdHandle) { RET(2, 1); }
HLE(GetConsoleMode) { RET(2, 0); }
HLE(WriteConsoleW)
{
    char s[2048];
    uint32_t n = ARG(2);
    uint32_t tmp = rt_heap_alloc(2 * n + 2, 0);
    memcpy(G2H(tmp), G2H(ARG(1)), 2 * n);
    g_wr16(tmp + 2 * n, 0);
    rt_w2u(tmp, s, sizeof s);
    rt_heap_free(tmp);
    rt_log("%s", s);
    if (ARG(3)) g_wr32(ARG(3), n);
    RET(5, 1);
}
HLE(ReadConsoleW) { RET(5, 0); }

static uint32_t errno_to_win(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    default: return ERROR_ACCESS_DENIED;
    }
}

static uint32_t create_file(const char *gpath, uint32_t access, uint32_t disp)
{
    char hp[1024];
    rt_host_path(gpath, hp, sizeof hp);
    int flags = 0;
    int rd = (access & 0x80000000u) || (access & 1) || (access & 0x10000000u);
    int wr = (access & 0x40000000u) || (access & 2) || (access & 0x10000000u);
    flags = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;
    case 2: flags |= O_CREAT | O_TRUNC; break;
    case 4: flags |= O_CREAT; break;
    case 5: flags |= O_TRUNC; break;
    default: break;
    }
    if (!(flags & O_CREAT) && rt_fs_lookup(hp) == 0) {
        if (rt_verbose > 1) rt_log("[k32] CreateFile(%s) -> missing (cached)\n", gpath);
        rt_set_last_error(2); /* ERROR_FILE_NOT_FOUND */
        return 0xffffffffu;
    }
    struct stat st;
    int existed = stat(hp, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) flags = O_RDONLY;
    int fd = open(hp, flags, 0644);
    if (fd < 0 && (flags & O_CREAT) && errno == ENOENT) {
        /* create missing parent folders under the game dir (save data etc.) */
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", hp);
        for (char *p = tmp + strlen(rt_game_dir) + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        fd = open(hp, flags, 0644);
    }
    if (rt_verbose > (fd < 0 && errno == ENOENT ? 1 : 0))
        rt_log("[k32] CreateFile(%s, %08x, %u) -> %s\n", gpath, (unsigned)access, (unsigned)disp, fd < 0 ? strerror(errno) : "ok");
    if (fd >= 0 && (flags & O_CREAT)) rt_fs_note(hp, 1);
    if (fd < 0) {
        rt_set_last_error(errno_to_win(errno));
        return 0xffffffffu;
    }
    FileObj *f = calloc(1, sizeof *f);
    f->obj.type = OBJ_FILE;
    f->obj.destroy = file_destroy;
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", hp);
    rt_set_last_error((disp == 2 || disp == 4) && existed ? ERROR_ALREADY_EXISTS : 0);
    return rt_handle_new(&f->obj);
}
HLE(CreateFileA)
{
    char s[600];
    rt_a2u(ARG(0), s, sizeof s);
    RET(7, create_file(s, ARG(1), ARG(4)));
}
HLE(CreateFileW)
{
    char s[600];
    rt_w2u(ARG(0), s, sizeof s);
    RET(7, create_file(s, ARG(1), ARG(4)));
}
static FileObj *file_of(uint32_t h) { return (FileObj *)rt_handle_get(h, OBJ_FILE); }
HLE(ReadFile)
{
    FileObj *f = file_of(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), out = ARG(3);
    if (!f) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(5, 0);
    }
    ssize_t r = read(f->fd, G2H(buf), n);
    if (out) g_wr32(out, r > 0 ? (uint32_t)r : 0);
    RET(5, r >= 0);
}
HLE(WriteFile)
{
    FileObj *f = file_of(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), out = ARG(3);
    if (!f) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(5, 0);
    }
    ssize_t r = write(f->fd, G2H(buf), n);
    if (out) g_wr32(out, r > 0 ? (uint32_t)r : 0);
    RET(5, r >= 0);
}
HLE(SetFilePointerEx)
{
    FileObj *f = file_of(ARG(0));
    int64_t dist = (int64_t)((uint64_t)ARG(1) | ((uint64_t)ARG(2) << 32));
    uint32_t out = ARG(3), method = ARG(4);
    if (!f) RET(5, 0);
    off_t r = lseek(f->fd, (off_t)dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) RET(5, 0);
    if (out) {
        g_wr32(out, (uint32_t)r);
        g_wr32(out + 4, (uint32_t)((uint64_t)r >> 32));
    }
    RET(5, 1);
}
HLE(GetFileSize)
{
    FileObj *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(2, 0xffffffffu);
    if (ARG(1)) g_wr32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET(2, (uint32_t)st.st_size);
}
HLE(GetFileSizeEx)
{
    FileObj *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(2, 0);
    g_wr32(ARG(1), (uint32_t)st.st_size);
    g_wr32(ARG(1) + 4, (uint32_t)((uint64_t)st.st_size >> 32));
    RET(2, 1);
}
static void put_filetime(uint32_t p, time_t t)
{
    if (!p) return;
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    g_wr32(p, (uint32_t)ft);
    g_wr32(p + 4, (uint32_t)(ft >> 32));
}
HLE(GetFileTime)
{
    FileObj *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(4, 0);
    put_filetime(ARG(1), st.st_mtime);
    put_filetime(ARG(2), st.st_atime);
    put_filetime(ARG(3), st.st_mtime);
    RET(4, 1);
}
HLE(SetEndOfFile)
{
    FileObj *f = file_of(ARG(0));
    if (!f) RET(1, 0);
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    RET(1, ftruncate(f->fd, pos) == 0);
}
HLE(FlushFileBuffers) { RET(1, 1); }
HLE(GetFileType)
{
    FileObj *f = file_of(ARG(0));
    RET(1, !f ? 0 : f->is_std ? 2 : 1);
}
HLE(GetFileInformationByHandleEx)
{
    FileObj *f = file_of(ARG(0));
    uint32_t cls = ARG(1), buf = ARG(2);
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(4, 0);
    if (cls == 0) { /* FileBasicInfo */
        memset(G2H(buf), 0, 40);
        put_filetime(buf, st.st_mtime);
        put_filetime(buf + 8, st.st_atime);
        put_filetime(buf + 16, st.st_mtime);
        put_filetime(buf + 24, st.st_mtime);
        g_wr32(buf + 32, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        RET(4, 1);
    }
    if (cls == 1) { /* FileStandardInfo */
        memset(G2H(buf), 0, 24);
        g_wr32(buf, (uint32_t)st.st_size);
        g_wr32(buf + 8, (uint32_t)st.st_size);
        g_wr32(buf + 16, 1);
        *(uint8_t *)G2H(buf + 21) = S_ISDIR(st.st_mode);
        RET(4, 1);
    }
    rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(4, 0);
}
HLE(SetFileInformationByHandle) { RET(4, 1); }
HLE(CloseHandle)
{
    uint32_t h = ARG(0);
    if (h == PSEUDO_PROCESS || h == PSEUDO_THREAD) RET(1, 1);
    for (int i = 0; i < 3; i++)
        if (std_handles[i] == h) RET(1, 1);
    RET(1, rt_handle_close(h));
}
HLE(DeleteFileA)
{
    char s[600], hp[1024];
    rt_a2u(ARG(0), s, sizeof s);
    rt_host_path(s, hp, sizeof hp);
    if (unlink(hp)) {
        rt_set_last_error(errno_to_win(errno));
        RET(1, 0);
    }
    rt_fs_note(hp, 0);
    RET(1, 1);
}
static int mkdir_p(const char *hp)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", hp);
    for (char *p = tmp + strlen(rt_game_dir) + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    int r = mkdir(tmp, 0755);
    if (r == 0 || errno == EEXIST) rt_fs_note(tmp, 1);
    return r;
}
HLE(CreateDirectoryW)
{
    char s[600], hp[1024];
    rt_w2u(ARG(0), s, sizeof s);
    rt_host_path(s, hp, sizeof hp);
    if (mkdir_p(hp)) {
        rt_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
        RET(2, 0);
    }
    RET(2, 1);
}
HLE(RemoveDirectoryW)
{
    char s[600], hp[1024];
    rt_w2u(ARG(0), s, sizeof s);
    rt_host_path(s, hp, sizeof hp);
    int ok = rmdir(hp) == 0;
    if (ok) rt_fs_note(hp, 0);
    RET(1, ok);
}
static int fill_attr_data(const char *gpath, uint32_t p)
{
    char hp[1024];
    rt_host_path(gpath, hp, sizeof hp);
    struct stat st;
    if (rt_fs_lookup(hp) == 0) {
        rt_set_last_error(2);
        return 0;
    }
    if (stat(hp, &st)) {
        rt_set_last_error(errno_to_win(errno));
        return 0;
    }
    memset(G2H(p), 0, 36);
    g_wr32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    put_filetime(p + 4, st.st_mtime);
    put_filetime(p + 12, st.st_atime);
    put_filetime(p + 20, st.st_mtime);
    g_wr32(p + 32, (uint32_t)st.st_size);
    return 1;
}
HLE(GetFileAttributesExA)
{
    char s[600];
    rt_a2u(ARG(0), s, sizeof s);
    RET(3, fill_attr_data(s, ARG(2)));
}

/* FindFirstFile patterns match case-insensitively (newlib's fnmatch has no FNM_CASEFOLD) */
static int match_nocase(const char *pattern, const char *name)
{
    char p[260], n[260];
    size_t i;
    for (i = 0; pattern[i] && i < sizeof p - 1; i++) p[i] = (char)tolower((unsigned char)pattern[i]);
    p[i] = 0;
    for (i = 0; name[i] && i < sizeof n - 1; i++) n[i] = (char)tolower((unsigned char)name[i]);
    n[i] = 0;
#ifdef _WIN32
    /* (mingw has no fnmatch; FindFirstFile patterns only use * and ?) */
    const char *pp = p, *nn = n, *star = NULL, *mark = NULL;
    while (*nn) {
        if (*pp == '?' || *pp == *nn) pp++, nn++;
        else if (*pp == '*') star = pp++, mark = nn;
        else if (star) pp = star + 1, nn = ++mark;
        else return 0;
    }
    while (*pp == '*') pp++;
    return *pp == 0;
#else
    return fnmatch(p, n, 0) == 0;
#endif
}
HLE(CreateSymbolicLinkW) { RET(3, 0); }

/* FindFirstFile: host readdir + Windows wildcard match (case-insensitive) */
static int find_next(FindObj *f, uint32_t data)
{
    struct dirent *e;
    while (f->d && (e = readdir(f->d))) {
        if (!match_nocase(f->pattern, e->d_name)) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        uint32_t size = f->wide ? 592 : 320;
        memset(G2H(data), 0, size);
        g_wr32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        put_filetime(data + 4, st.st_mtime);
        put_filetime(data + 12, st.st_atime);
        put_filetime(data + 20, st.st_mtime);
        g_wr32(data + 32, (uint32_t)st.st_size);
        if (f->wide) rt_u2w(e->d_name, data + 44, 260);
        else rt_u2a(e->d_name, data + 44, 260);
        return 1;
    }
    rt_set_last_error(ERROR_NO_MORE_FILES);
    return 0;
}
static uint32_t find_first(const char *gpath, uint32_t data, int wide)
{
    char hp[1024];
    rt_host_path(gpath, hp, sizeof hp);
    FindObj *f = calloc(1, sizeof *f);
    f->obj.type = OBJ_FIND;
    f->obj.destroy = find_destroy;
    f->wide = wide;
    char *slash = strrchr(hp, '/');
    if (slash) {
        *slash = 0;
        snprintf(f->dir, sizeof f->dir, "%s", hp);
        snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1);
    }
    if (!strcmp(f->pattern, "*.*")) strcpy(f->pattern, "*");
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, data)) {
        find_destroy(&f->obj);
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        return 0xffffffffu;
    }
    return rt_handle_new(&f->obj);
}
HLE(FindFirstFileA)
{
    char s[600];
    rt_a2u(ARG(0), s, sizeof s);
    RET(2, find_first(s, ARG(1), 0));
}
HLE(FindFirstFileExW)
{
    char s[600];
    rt_w2u(ARG(0), s, sizeof s);
    RET(6, find_first(s, ARG(2), 1));
}
HLE(FindNextFileA)
{
    FindObj *f = (FindObj *)rt_handle_get(ARG(0), OBJ_FIND);
    RET(2, f ? find_next(f, ARG(1)) : 0);
}
HLE(FindNextFileW)
{
    FindObj *f = (FindObj *)rt_handle_get(ARG(0), OBJ_FIND);
    RET(2, f ? find_next(f, ARG(1)) : 0);
}
HLE(FindClose) { RET(1, rt_handle_close(ARG(0))); }

HLE(GetFullPathNameW)
{
    char s[600];
    rt_w2u(ARG(0), s, sizeof s);
    char full[1024];
    if (isalpha((unsigned char)s[0]) && s[1] == ':') snprintf(full, sizeof full, "%s", s);
    else if (s[0] == '\\' || s[0] == '/') snprintf(full, sizeof full, "C:%s", s);
    else snprintf(full, sizeof full, "%s\\%s", rt_get_cwd(), s);
    for (char *p = full; *p; p++)
        if (*p == '/') *p = '\\';
    uint32_t len = (uint32_t)strlen(full);
    if (ARG(1) <= len) RET(4, len + 1);
    rt_u2w(full, ARG(2), ARG(1));
    if (ARG(3)) {
        char *b = strrchr(full, '\\');
        g_wr32(ARG(3), b ? ARG(2) + 2 * (uint32_t)(b + 1 - full) : 0);
    }
    RET(4, len);
}
HLE(GetCurrentDirectoryW)
{
    const char *d = rt_get_cwd();
    uint32_t len = (uint32_t)strlen(d);
    if (ARG(0) <= len) RET(2, len + 1);
    rt_u2w(d, ARG(1), ARG(0));
    RET(2, len);
}
HLE(SetCurrentDirectoryW)
{
    char s[600];
    rt_w2u(ARG(0), s, sizeof s);
    rt_set_cwd(s);
    rt_log("[k32] SetCurrentDirectory(%s)\n", s);
    RET(1, 1);
}
HLE(GetTempPathW)
{
    const char *d = "C:\\PvZDLC\\temp\\";
    uint32_t len = (uint32_t)strlen(d);
    if (ARG(0) <= len) RET(2, len + 1);
    rt_u2w(d, ARG(1), ARG(0));
    RET(2, len);
}
HLE(GetDriveTypeW) { RET(1, 3); }
/* File mappings: anonymous (pagefile) mappings are guest memory, optionally
 * shared by name; file-backed ones are a private copy of the file. */
typedef struct {
    RtObj obj;
    char name[128];
    uint32_t mem, size;
} MapObj;
static MapObj *maps[64];

/* The whole range from a file offset (newlib has no pread): seek, then read until done */
static void read_all_at(int fd, void *buf, uint32_t size, off_t off)
{
    if (lseek(fd, off, SEEK_SET) < 0) return;
    uint8_t *p = buf;
    uint32_t total = size, chunk = 4u << 20;
    while (size) {
        ssize_t n = read(fd, p, size < chunk ? size : chunk);
        if (n <= 0) break;
        p += n;
        size -= (uint32_t)n;
        /* main.pak (205 MB) is read whole at start: 150..900 of the boot bar */
        if (total >= (16u << 20))
            rt_boot_progress("Reading the game data", 150 + (int)((uint64_t)(total - size) * 750 / total));
    }
}
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t create_mapping(uint32_t file, uint32_t size_hi, uint32_t size_lo, const char *name)
{
    pthread_mutex_lock(&map_lock);
    if (name[0]) {
        for (int i = 0; i < 64; i++) {
            if (maps[i] && !strcmp(maps[i]->name, name)) {
                maps[i]->obj.refs++;
                pthread_mutex_unlock(&map_lock);
                rt_set_last_error(ERROR_ALREADY_EXISTS);
                return rt_handle_new(&maps[i]->obj);
            }
        }
    }
    uint32_t size = size_lo;
    FileObj *f = file != 0xffffffffu ? file_of(file) : NULL;
    struct stat st;
    if (f && !size && !size_hi && !fstat(f->fd, &st)) size = (uint32_t)st.st_size;
    if (size_hi || !size) {
        pthread_mutex_unlock(&map_lock);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    MapObj *m = calloc(1, sizeof *m);
    m->obj.type = OBJ_MAPPING;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->size = size;
    m->mem = rt_vm_alloc(0, size);
    if (f) read_all_at(f->fd, G2H(m->mem), size, 0);
    for (int i = 0; i < 64; i++)
        if (!maps[i]) { maps[i] = m; break; }
    pthread_mutex_unlock(&map_lock);
    rt_set_last_error(0);
    return rt_handle_new(&m->obj);
}
HLE(CreateFileMappingA)
{
    char name[128] = "";
    if (ARG(5)) rt_a2u(ARG(5), name, sizeof name);
    RET(6, create_mapping(ARG(0), ARG(3), ARG(4), name));
}
HLE(CreateFileMappingW)
{
    char name[128] = "";
    if (ARG(5)) rt_w2u(ARG(5), name, sizeof name);
    RET(6, create_mapping(ARG(0), ARG(3), ARG(4), name));
}
HLE(MapViewOfFile)
{
    MapObj *m = (MapObj *)rt_handle_get(ARG(0), OBJ_MAPPING);
    if (!m || ARG(3) >= m->size) RET(5, 0);
    RET(5, m->mem + ARG(3));
}

const HleEntry hle_kernel32[] = {
#define K(n) {"kernel32.dll", #n, n}
    K(GetModuleHandleA), K(GetModuleHandleW), K(GetModuleHandleExW), K(LoadLibraryA), K(LoadLibraryW),
    K(LoadLibraryExW), K(FreeLibrary), K(GetProcAddress), K(GetModuleFileNameA), K(GetModuleFileNameW),
    K(GetCommandLineA), K(GetCommandLineW), K(GetStartupInfoW), K(GetEnvironmentStringsW),
    K(FreeEnvironmentStringsW), K(SetEnvironmentVariableW), K(GetCurrentProcess), K(GetCurrentProcessId),
    K(GetCurrentThread), K(GetCurrentThreadId), K(GetCurrentProcessorNumber), K(ExitProcess), K(TerminateProcess),
    K(IsDebuggerPresent), K(IsProcessorFeaturePresent), K(SetUnhandledExceptionFilter), K(UnhandledExceptionFilter),
    K(SetErrorMode), K(SetConsoleCtrlHandler), K(GetLastError), K(SetLastError), K(EncodePointer), K(DecodePointer),
    K(OutputDebugStringA), K(OutputDebugStringW), K(FormatMessageA), K(MulDiv), K(GetVersionExA), K(GetSystemInfo),
    K(GetNativeSystemInfo), K(GetSystemDirectoryA), K(GetWindowsDirectoryA), K(GetSystemTimeAsFileTime),
    K(GetLocalTime), K(FileTimeToSystemTime), K(CompareFileTime), K(GetTimeZoneInformation), K(GetTickCount),
    K(GetTickCount64), K(QueryPerformanceCounter), K(QueryPerformanceFrequency), K(Sleep), K(SwitchToThread),
    K(GetACP), K(GetOEMCP), K(GetConsoleOutputCP), K(IsValidCodePage), K(GetCPInfo), K(MultiByteToWideChar),
    K(WideCharToMultiByte), K(GetStringTypeW), K(LCMapStringW), K(LCMapStringEx), K(CompareStringW),
    K(CompareStringEx), K(GetLocaleInfoW), K(GetLocaleInfoEx), K(GetUserDefaultLCID), K(IsValidLocale),
    K(EnumSystemLocalesW), K(GetDateFormatW), K(GetTimeFormatW), K(GetProcessHeap), K(HeapAlloc), K(HeapFree),
    K(HeapReAlloc), K(HeapSize), K(HeapValidate), K(HeapQueryInformation), K(GlobalAlloc), K(GlobalFree),
    K(GlobalLock), K(GlobalUnlock), K(LocalFree), K(VirtualProtect), K(IsBadWritePtr), K(VirtualQuery),
    K(InitializeCriticalSection), K(InitializeCriticalSectionAndSpinCount), K(InitializeCriticalSectionEx),
    K(EnterCriticalSection), K(LeaveCriticalSection), K(DeleteCriticalSection), K(AcquireSRWLockExclusive),
    K(ReleaseSRWLockExclusive), K(TryAcquireSRWLockExclusive), K(SleepConditionVariableSRW),
    K(WakeConditionVariable), K(WakeAllConditionVariable), K(InitOnceExecuteOnce), K(InitializeSListHead),
    K(InterlockedPushEntrySList), K(InterlockedFlushSList), K(FlushProcessWriteBuffers), K(CreateEventExW),
    K(CreateMutexA), K(CreateSemaphoreExW), K(WaitForSingleObject), K(WaitForSingleObjectEx), K(TlsAlloc),
    K(TlsFree), K(TlsGetValue), K(TlsSetValue), K(FlsAlloc), K(FlsFree), K(FlsGetValue), K(FlsSetValue),
    K(CreateThread), K(ResumeThread), K(ExitThread), K(FreeLibraryAndExitThread), K(GetExitCodeThread),
    K(SetThreadPriority), K(GetThreadPriority), K(IsThreadAFiber), K(GetStdHandle), K(SetStdHandle),
    K(GetConsoleMode), K(WriteConsoleW), K(ReadConsoleW), K(CreateFileA), K(CreateFileW), K(ReadFile),
    K(WriteFile), K(SetFilePointerEx), K(GetFileSize), K(GetFileSizeEx), K(GetFileTime), K(SetEndOfFile),
    K(FlushFileBuffers), K(GetFileType), K(GetFileInformationByHandleEx), K(SetFileInformationByHandle),
    K(CloseHandle), K(DeleteFileA), K(CreateDirectoryW), K(RemoveDirectoryW), K(GetFileAttributesExA),
    K(CreateSymbolicLinkW), K(FindFirstFileA), K(FindFirstFileExW), K(FindNextFileA), K(FindNextFileW),
    K(FindClose), K(GetFullPathNameW), K(GetCurrentDirectoryW), K(SetCurrentDirectoryW), K(GetTempPathW),
    K(GetDriveTypeW), K(CreateFileMappingA), K(CreateFileMappingW), K(MapViewOfFile),
#undef K
    {NULL, NULL, NULL}};

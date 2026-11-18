/* rt_sys.c: logging, string conversion, guest<->host paths, handle table. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.h"

int rt_verbose;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;

void rt_log(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&log_lock);
    if (!log_file) {
        char p[600];
        snprintf(p, sizeof p, "%s/debug.log", rt_game_dir);
        log_file = fopen(p, "w");
#ifdef __SWITCH__
        if (log_file) setvbuf(log_file, NULL, _IOFBF, 64 << 10);
#endif
    }
#ifndef __SWITCH__
    /* (the Switch has no terminal: stderr is the boot screen's text console while it is up, and writing
     * to it after the console handed the display to deko3d crashed in libnx's console renderer) */
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
#endif
    if (log_file) {
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        va_end(ap);
#ifdef __SWITCH__
        /* the SD card is slow: flush once a second, and at once for the lines that come before a crash */
        static time_t last_flush;
        time_t now = time(NULL);
        if (now != last_flush || strstr(fmt, "[fault]") || strstr(fmt, "[fatal]") || strstr(fmt, "[trap]")) {
            fflush(log_file);
            last_flush = now;
        }
#else
        fflush(log_file);
#endif
    }
    pthread_mutex_unlock(&log_lock);
}

void rt_log_flush(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_file) fflush(log_file);
    pthread_mutex_unlock(&log_lock);
}

void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("[fatal] %s\n", buf);
    RtThread *t = rt_thread_self();
    if (t) rt_dump(&t->cpu);
    rt_log_flush();
    abort();
}

/* ---- strings ---- */
size_t rt_wlen(uint32_t a)
{
    size_t n = 0;
    while (g_rd16(a + 2 * n)) n++;
    return n;
}

static size_t put_utf8(uint32_t cp, char *out, size_t cap, size_t n)
{
    char tmp[4];
    size_t k;
    if (cp < 0x80) { tmp[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); k = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); k = 4;
    }
    if (n + k < cap) memcpy(out + n, tmp, k);
    return k;
}

size_t rt_w2u(uint32_t a, char *out, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = 0;; i++) {
        uint32_t w = g_rd16(a + 2 * i);
        if (!w) break;
        if (w >= 0xD800 && w < 0xDC00) {
            uint32_t lo = g_rd16(a + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) {
                w = 0x10000 + ((w - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        n += put_utf8(w, out, cap, n);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

size_t rt_a2u(uint32_t a, char *out, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = 0;; i++) {
        uint8_t ch = *(uint8_t *)G2H(a + i);
        if (!ch) break;
        n += put_utf8(ch, out, cap, n); /* Latin-1 is close enough to 1252 for paths */
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

static uint32_t next_cp(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t cp = *s++;
    if (cp >= 0xF0 && s[0] && s[1] && s[2]) { cp = ((cp & 7) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); s += 3; }
    else if (cp >= 0xE0 && s[0] && s[1]) { cp = ((cp & 15) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; }
    else if (cp >= 0xC0 && s[0]) { cp = ((cp & 31) << 6) | (s[0] & 0x3F); s += 1; }
    *ps = (const char *)s;
    return cp;
}

size_t rt_u2w(const char *s, uint32_t out, size_t cap)
{
    size_t n = 0;
    while (*s && n + 1 < cap) {
        uint32_t cp = next_cp(&s);
        if (cp >= 0x10000 && n + 2 < cap) {
            cp -= 0x10000;
            g_wr16(out + 2 * n++, (uint16_t)(0xD800 + (cp >> 10)));
            g_wr16(out + 2 * n++, (uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            g_wr16(out + 2 * n++, (uint16_t)cp);
        }
    }
    if (cap) g_wr16(out + 2 * n, 0);
    return n;
}

size_t rt_u2a(const char *s, uint32_t out, size_t cap)
{
    size_t n = 0;
    while (*s && n + 1 < cap) {
        uint32_t cp = next_cp(&s);
        *(uint8_t *)G2H(out + n++) = cp < 0x100 ? (uint8_t)cp : '?';
    }
    if (cap) *(uint8_t *)G2H(out + n) = 0;
    return n;
}

/* ---- paths ---- */
char rt_game_dir[512];
static char cwd[512] = RT_GUEST_DIR;
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Absolute, normalised guest path (backslashes, no . or ..) */
static void full_guest(const char *in, char *out, size_t cap)
{
    char tmp[1024];
    if (isalpha((unsigned char)in[0]) && in[1] == ':') {
        snprintf(tmp, sizeof tmp, "%s", in);
    } else if (in[0] == '\\' || in[0] == '/') {
        snprintf(tmp, sizeof tmp, "C:%s", in);
    } else {
        pthread_mutex_lock(&cwd_lock);
        snprintf(tmp, sizeof tmp, "%s\\%s", cwd, in);
        pthread_mutex_unlock(&cwd_lock);
    }
    for (char *p = tmp; *p; p++)
        if (*p == '/') *p = '\\';
    /* split into components and resolve . and .. */
    char *parts[128];
    int np = 0;
    char *save = NULL;
    char drive[3] = {(char)toupper((unsigned char)tmp[0]), ':', 0};
    for (char *tok = strtok_r(tmp + 2, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (np) np--; continue; }
        if (np < 128) parts[np++] = tok;
    }
    size_t n = (size_t)snprintf(out, cap, "%s", drive);
    for (int i = 0; i < np && n < cap; i++) n += (size_t)snprintf(out + n, cap - n, "\\%s", parts[i]);
    if (np == 0 && n < cap) snprintf(out + n, cap - n, "\\");
}

/* where the game folder's wide/ really is ("" the game folder; the Switch: "romfs:", the NRO's own) */
char rt_wide_root[1024];

void rt_host_path(const char *guest, char *out, size_t cap)
{
    char g[1024];
    full_guest(guest, g, sizeof g);
    size_t root = strlen(RT_GUEST_DIR);
    const char *rest;
    char prefix[1100];
    if (!strncasecmp(g, RT_GUEST_DIR, root) && (g[root] == '\\' || g[root] == 0)) {
        rest = g + root;
        snprintf(prefix, sizeof prefix, "%s", rt_wide_root[0] && !strncasecmp(rest, "\\wide\\", 6) ? rt_wide_root : rt_game_dir);
    } else {
        rest = g + 2; /* anything else lives under <game>/_drive_<letter> */
        snprintf(prefix, sizeof prefix, "%s/_drive_%c", rt_game_dir, tolower((unsigned char)g[0]));
    }
    size_t n = (size_t)snprintf(out, cap, "%s", prefix);
    for (const char *p = rest; *p && n + 1 < cap; p++) out[n++] = *p == '\\' ? '/' : *p;
    out[n < cap ? n : cap - 1] = 0;
}

void rt_set_cwd(const char *guest)
{
    char g[512];
    full_guest(guest, g, sizeof g);
    pthread_mutex_lock(&cwd_lock);
    snprintf(cwd, sizeof cwd, "%s", g);
    size_t n = strlen(cwd);
    if (n > 3 && cwd[n - 1] == '\\') cwd[n - 1] = 0;
    pthread_mutex_unlock(&cwd_lock);
}

const char *rt_get_cwd(void) { return cwd; }

/* ---- file-existence cache ----
 * The DLC looks for every image as a loose file under several names (.tga .jpg .png .gif, with _ variants)
 * before reading it from main.pak: thousands of misses at startup, each a slow SD-card stat on the Switch.
 * One walk of the game folder answers them from memory. Names compare case-insensitively (FAT); the HLE
 * keeps the set current when the game creates, deletes or renames something. */
#include <dirent.h>
#include <sys/stat.h>
#define FS_SLOTS 16384
static char *fs_set[FS_SLOTS]; /* lower-case host paths; "\x01" = deleted */
static int fs_ready = -1;
static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t fs_hash(const char *p)
{
    uint32_t h = 2166136261u;
    for (; *p; p++) h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
    return h;
}
static int fs_eq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }
static void fs_put(const char *p, int present)
{
    uint32_t i = fs_hash(p) % FS_SLOTS;
    for (uint32_t n = 0; n < FS_SLOTS; n++, i = (i + 1) % FS_SLOTS) {
        if (!fs_set[i]) {
            if (present) fs_set[i] = strdup(p);
            return;
        }
        if (fs_set[i][0] != 1 && fs_eq(fs_set[i], p)) {
            if (!present) { free(fs_set[i]); fs_set[i] = strdup("\x01"); }
            return;
        }
    }
    fs_ready = 0; /* full: stop answering */
}
static int fs_has(const char *p)
{
    uint32_t i = fs_hash(p) % FS_SLOTS;
    for (uint32_t n = 0; n < FS_SLOTS && fs_set[i]; n++, i = (i + 1) % FS_SLOTS)
        if (fs_set[i][0] != 1 && fs_eq(fs_set[i], p)) return 1;
    return 0;
}
static int fs_walk(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        fs_put(p, 1);
        n++;
#ifdef _WIN32
        struct stat st; /* (no d_type) */
        if (depth < 12 && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) n += fs_walk(p, depth + 1);
#else
        if (depth < 12 && e->d_type == DT_DIR) n += fs_walk(p, depth + 1);
#endif
    }
    closedir(d);
    return n;
}

/* -1 unknown (not under the game folder, or no cache), 0 missing, 1 present */
int rt_fs_lookup(const char *hp)
{
    size_t n = strlen(rt_game_dir);
    if (strncasecmp(hp, rt_game_dir, n) || hp[n] != '/') return -1;
    pthread_mutex_lock(&fs_lock);
    if (fs_ready < 0) {
        fs_put(rt_game_dir, 1);
        int c = fs_walk(rt_game_dir, 0);
        if (fs_ready < 0) fs_ready = 1;
        rt_log("[fs] %d entries cached under %s\n", c, rt_game_dir);
    }
    int r = fs_ready ? fs_has(hp) : -1;
    pthread_mutex_unlock(&fs_lock);
    return r;
}

/* the game made (1) or removed (0) this path */
void rt_fs_note(const char *hp, int present)
{
    pthread_mutex_lock(&fs_lock);
    if (fs_ready > 0) {
        fs_put(hp, present);
        if (present) {
            /* its parent folders exist too */
            char t[1024];
            snprintf(t, sizeof t, "%s", hp);
            size_t root = strlen(rt_game_dir);
            for (char *q = t + strlen(t); q > t + root; q--)
                if (*q == '/') { *q = 0; fs_put(t, 1); }
        }
    }
    pthread_mutex_unlock(&fs_lock);
}

/* ---- handles ---- */
#define MAX_HANDLES 8192
static RtObj *handles[MAX_HANDLES];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t rt_handle_new(RtObj *o)
{
    pthread_mutex_lock(&handle_lock);
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = o;
            o->refs = 1;
            pthread_mutex_unlock(&handle_lock);
            return 0x1000 + i * 4;
        }
    }
    pthread_mutex_unlock(&handle_lock);
    rt_log("[handles] table full\n");
    return 0;
}

RtObj *rt_handle_get(uint32_t h, int type)
{
    if (h < 0x1000 || (h & 3)) return NULL;
    uint32_t i = (h - 0x1000) / 4;
    if (i >= MAX_HANDLES) return NULL;
    pthread_mutex_lock(&handle_lock);
    RtObj *o = handles[i];
    pthread_mutex_unlock(&handle_lock);
    if (o && type != OBJ_NONE && o->type != type) return NULL;
    return o;
}

int rt_handle_close(uint32_t h)
{
    if (h < 0x1000 || (h & 3)) return 0;
    uint32_t i = (h - 0x1000) / 4;
    if (i >= MAX_HANDLES) return 0;
    pthread_mutex_lock(&handle_lock);
    RtObj *o = handles[i];
    handles[i] = NULL;
    pthread_mutex_unlock(&handle_lock);
    if (!o) return 0;
    if (--o->refs <= 0 && o->destroy) o->destroy(o);
    return 1;
}

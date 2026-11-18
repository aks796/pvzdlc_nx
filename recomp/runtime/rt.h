/* rt.h: runtime internals shared by the loader, guest memory and the HLE
 * (Windows API) layer. Recompiled code only sees x86rt.h. */
#ifndef RT_H
#define RT_H

#include "rt_compat.h"
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "x86rt.h"

/* ---- logging ---- */
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
extern int rt_verbose;

/* ---- guest memory ----
 * The guest's 4 GiB address space is one host window at rt_membase. */
#define G2H(a) ((void *)GPG(a))
static inline uint32_t H2G(const void *p) { return (uint32_t)((const uint8_t *)p - rt_membase); }
static inline uint32_t g_rd32(uint32_t a) { uint32_t v; __builtin_memcpy(&v, GPG(a), 4); return v; }
static inline float g_rdf32(uint32_t a) { float v; __builtin_memcpy(&v, GPG(a), 4); return v; }
static inline uint16_t g_rd16(uint32_t a) { uint16_t v; __builtin_memcpy(&v, GPG(a), 2); return v; }
static inline void g_wr32(uint32_t a, uint32_t v) { __builtin_memcpy(GPG(a), &v, 4); }
static inline void g_wr16(uint32_t a, uint16_t v) { __builtin_memcpy(GPG(a), &v, 2); }

#define RT_PAGE 0x1000u
#define RT_ALLOC_GRAN 0x10000u
#define RT_HLE_BASE 0xF0000000u   /* fake addresses for host functions */

int rt_mem_init(void);
/* VirtualAlloc-style: page-granular, 64 KiB-aligned reservations. addr 0 = anywhere. */
uint32_t rt_vm_alloc(uint32_t addr, uint32_t size);
int rt_vm_free(uint32_t addr);
int rt_vm_query(uint32_t addr, uint32_t *base, uint32_t *size);
void rt_vm_usage(uint64_t *now, uint64_t *peak);
/* Host memory backing the guest window (the Switch maps it in chunks; elsewhere the whole window) */
uint64_t rt_mem_backed(void);
void rt_report_fault(const char *what, void *addr);
void rt_log_flush(void);
/* The host's boot progress screen (Switch: PvZ's green bar), permille 0..1000; no-op elsewhere */
void rt_boot_progress(const char *what, int permille);
uint32_t rt_app_object(void);
/* The process heap (HeapAlloc/malloc for HLE scratch). Thread-safe. */
uint32_t rt_heap_alloc(uint32_t size, int zero);
void rt_heap_free(uint32_t p);
uint32_t rt_heap_realloc(uint32_t p, uint32_t size, int zero);
uint32_t rt_heap_size(uint32_t p);
uint32_t rt_heap_strdup(const char *s);
uint32_t rt_heap_wstrdup(const char *utf8);
/* Called at the start of every guest thread (and the main one); the host marks it interactive */
void rt_platform_thread_start(void);
/* The game's Sleep(): the host may make the wake-up punctual */
void rt_platform_sleep_us(uint32_t us);
/* -v frame statistics, reset by the frame timer in hle_ddraw.c */
extern uint64_t rt_stat_sleep_us, rt_stat_draw_us, rt_stat_max_sleep_us, rt_stat_lock_us, rt_stat_vblank_us;
extern uint32_t rt_stat_sleeps, rt_stat_draws;
/* Run fn over rows [y0, y1) split between worker threads (rt_pool.c); rows must be independent */
void rt_parallel_rows(int y0, int y1, void (*fn)(void *ctx, int y0, int y1), void *ctx);

/* ---- strings ---- */
size_t rt_wlen(uint32_t a);
/* guest UTF-16 -> host UTF-8 (returns bytes written, excluding NUL) */
size_t rt_w2u(uint32_t a, char *out, size_t cap);
/* guest ANSI (Windows-1252) -> host UTF-8 */
size_t rt_a2u(uint32_t a, char *out, size_t cap);
/* host UTF-8 -> guest UTF-16 buffer of cap wchars; returns wchars written excluding NUL */
size_t rt_u2w(const char *s, uint32_t out, size_t cap);
size_t rt_u2a(const char *s, uint32_t out, size_t cap);

/* ---- paths ----
 * The game believes it lives in C:\PvZDLC; that folder is rt_game_dir. */
extern char rt_game_dir[512];
extern char rt_wide_root[1024]; /* the port's pictures (wide/) when not in the game folder: "romfs:" on the Switch, the
                                 * Mac app's Resources, the Android app's files */
extern int rt_widescreen; /* 16:9: the draw surface is 1066 wide, the game in it at x 133 (hle_ddraw) */
#define RT_GUEST_DIR "C:\\PvZDLC"
#define RT_GUEST_EXE "C:\\PvZDLC\\PlantsVsZombies.exe"
void rt_host_path(const char *guest_utf8, char *out, size_t cap);
/* file-existence cache for the game folder: -1 unknown, 0 missing, 1 present; rt_fs_note on changes */
int rt_fs_lookup(const char *hp);
void rt_fs_note(const char *hp, int present);
void rt_set_cwd(const char *guest_utf8);
const char *rt_get_cwd(void);

/* ---- threads ---- */
typedef struct RtThread {
    CPU cpu;
    uint32_t teb, stack_lo, stack_hi, tls_block;
    uint32_t tid, handle;
    uint32_t start, param;
    uint32_t last_error;
    uint32_t exit_code;
    int finished;
    pthread_t pt;
} RtThread;
RtThread *rt_thread_self(void);
RtThread *rt_thread_new(uint32_t stack_size);
void rt_thread_attach(RtThread *t);
/* Call a guest function (stdcall or cdecl) with args; returns eax. */
uint32_t rt_guest_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
void rt_dump(CPU *c);
extern uint32_t rt_main_tid;
/* a guest address the host can read without faulting */
int rt_vm_mapped(uint32_t a);
/* RT_PROF builds: sample the game thread's current function (rt_prof.c) */
void rt_prof_start(CPU *main_cpu);

/* ---- loader ---- */
typedef struct {
    uint32_t base, size, entry;
    uint32_t stack_reserve;
    uint32_t tls_start, tls_end, tls_zero, tls_index_addr, tls_callbacks;
    uint32_t peb;
} RtImage;
extern RtImage rt_image;
int rt_load_image(const char *exe_path);
void rt_run_tls_callbacks(CPU *c, uint32_t reason);
int rt_start(void);

/* ---- HLE ---- */
typedef void (*hle_fn)(CPU *c);
typedef struct { const char *dll, *name; hle_fn fn; } HleEntry;
/* Each hle_*.c defines a NULL-terminated table and registers it here. */
void rt_hle_register(const HleEntry *table);
void rt_hle_init(void);
uint32_t rt_hle_lookup(const char *dll, const char *name); /* fake address or 0 */
uint32_t rt_hle_addr(hle_fn fn, const char *label);        /* fake address for a host function */

#define HLE(name) static void name(CPU *c)
#define ARG(n) g_rd32(c->esp + 4 + 4 * (n))
/* stdcall return: pop the return address and n arguments */
#define RET(nargs, v) do { c->eax = (uint32_t)(v); c->esp += 4 + 4 * (nargs); return; } while (0)
#define RET_CDECL(v) do { c->eax = (uint32_t)(v); c->esp += 4; return; } while (0)

void rt_set_last_error(uint32_t e);

/* ---- handles ---- */
enum { OBJ_NONE, OBJ_FILE, OBJ_FIND, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_THREAD, OBJ_HEAP, OBJ_MAPPING,
       OBJ_REGKEY, OBJ_OTHER };
typedef struct RtObj {
    int type;
    int refs;
    void (*destroy)(struct RtObj *);
} RtObj;
uint32_t rt_handle_new(RtObj *o);
RtObj *rt_handle_get(uint32_t h, int type);
int rt_handle_close(uint32_t h);

/* Windows error codes we use */
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_HANDLE_EOF 38
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_ALREADY_EXISTS 183
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_MOD_NOT_FOUND 126

#endif

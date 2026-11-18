/* rt_pool.c: a few worker threads that split one job's rows between them.
 *
 * Used for work whose rows are independent (a big triangle, a full-screen
 * stretch blit): every row belongs to exactly one worker, so the result is the
 * same as doing it on one thread. The caller works too and returns only when
 * every row is done. Jobs come from one thread at a time (the game's render
 * thread); other callers just run the rows themselves. */
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "rt.h"

/* weak default: hosts that care about timer precision override it */
__attribute__((weak)) void rt_platform_thread_start(void) {}
__attribute__((weak)) void rt_boot_progress(const char *what, int permille) { (void)what; (void)permille; }

uint64_t rt_stat_sleep_us, rt_stat_draw_us, rt_stat_max_sleep_us, rt_stat_lock_us, rt_stat_vblank_us;
uint32_t rt_stat_sleeps, rt_stat_draws;

#define MAX_WORKERS 7
#define CHUNK 4 /* rows handed out at a time: small, so one slow worker can't hold the frame up long */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;
static int nworkers = -1, busy_workers;
static unsigned generation;
static void (*job_fn)(void *, int, int);
static void *job_ctx;
static int job_end, job_next; /* job_next is taken with atomics */
static pthread_mutex_t submit = PTHREAD_MUTEX_INITIALIZER;

static void run_chunks(void)
{
    for (;;) {
        int y = __atomic_fetch_add(&job_next, CHUNK, __ATOMIC_RELAXED);
        if (y >= job_end) return;
        int e = y + CHUNK < job_end ? y + CHUNK : job_end;
        job_fn(job_ctx, y, e);
    }
}

static void *worker(void *arg)
{
    (void)arg;
    unsigned seen = 0;
    rt_platform_thread_start();
    pthread_mutex_lock(&lock);
    for (;;) {
        while (generation == seen) pthread_cond_wait(&start, &lock);
        seen = generation;
        pthread_mutex_unlock(&lock);
        run_chunks();
        pthread_mutex_lock(&lock);
        if (--busy_workers == 0) pthread_cond_signal(&done);
    }
    return NULL;
}

static void pool_init(void)
{
    /* the caller is one thread; leave a couple of fast cores for the game's other threads, the
     * audio and the rest of the system. On Apple silicon only the performance cores count: a row on
     * an efficiency core finishes last and the frame waits for it. */
#ifdef __SWITCH__
    long n = 3 + 2; /* a game gets cores 0-2: two workers next to the caller (n - 3 below) */
#else
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long n = (long)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
#endif
#ifdef __APPLE__
    int perf = 0;
    size_t len = sizeof perf;
    if (sysctlbyname("hw.perflevel0.logicalcpu", &perf, &len, NULL, 0) == 0 && perf > 0) n = perf;
#endif
    n -= 3;
    const char *env = getenv("RT_THREADS");
    if (env) n = atoi(env) - 1;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    if (n < 0) n = 0;
    nworkers = 0;
    for (long i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) nworkers++;
    }
    rt_log("[pool] %d render worker threads\n", nworkers);
}

void rt_parallel_rows(int y0, int y1, void (*fn)(void *ctx, int y0, int y1), void *ctx)
{
    if (y1 <= y0) return;
    if (nworkers < 0) {
        pthread_mutex_lock(&lock);
        if (nworkers < 0) pool_init();
        pthread_mutex_unlock(&lock);
    }
    if (!nworkers || y1 - y0 <= CHUNK || pthread_mutex_trylock(&submit)) {
        fn(ctx, y0, y1);
        return;
    }
    pthread_mutex_lock(&lock);
    job_fn = fn;
    job_ctx = ctx;
    job_end = y1;
    __atomic_store_n(&job_next, y0, __ATOMIC_RELAXED);
    busy_workers = nworkers;
    generation++;
    pthread_cond_broadcast(&start);
    pthread_mutex_unlock(&lock);
    run_chunks();
    pthread_mutex_lock(&lock);
    while (busy_workers) pthread_cond_wait(&done, &lock);
    pthread_mutex_unlock(&lock);
    pthread_mutex_unlock(&submit);
}

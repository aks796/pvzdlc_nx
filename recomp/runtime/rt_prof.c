/* rt_prof.c: a sampling profiler for hardware builds (RT_PROF). Every lifted function stores its guest
 * address on entry (RT_ENTER -> CPU.prof_va, one store); a thread reads the game thread's value every
 * millisecond and, with -v, logs the most frequent ones every 10 s:
 *   [prof] 10.0 s, 9874 samples: 12.3% 009c1a70, 8.1% 0080c520, ...
 * Calls into the Windows API layer show as the import's or COM method's name.
 * It is the function entered last, so a hot leaf shows up as itself and a hot loop body as the last
 * callee it makes: good enough to find what to make native. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "x86rt.h"

#ifdef RT_PROF
void rt_platform_sleep_us(uint32_t us);
const char *rt_hle_label(uint32_t target);

#define SLOTS 4096
static struct { uint32_t va, n; } hist[SLOTS];
static CPU *prof_cpu;

static void count(uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 20;
    for (uint32_t k = 0; k < SLOTS; k++) {
        uint32_t i = (h + k) & (SLOTS - 1);
        if (hist[i].va == va) { hist[i].n++; return; }
        if (!hist[i].va) { hist[i].va = va; hist[i].n = 1; return; }
    }
}

static void report(uint32_t total)
{
    uint32_t top[24] = {0};
    for (int r = 0; r < 24; r++) {
        uint32_t best = 0, bi = SLOTS;
        for (uint32_t i = 0; i < SLOTS; i++) {
            if (hist[i].n <= best) continue;
            int used = 0;
            for (int q = 0; q < r; q++) used |= top[q] == i + 1;
            if (!used) { best = hist[i].n; bi = i; }
        }
        if (bi == SLOTS) break;
        top[r] = bi + 1;
    }
    char line[1024];
    int len = snprintf(line, sizeof line, "[prof] 10 s, %u samples:", (unsigned)total);
    for (int r = 0; r < 24 && top[r] && len < (int)sizeof line - 24; r++) {
        uint32_t i = top[r] - 1, va = hist[i].va;
        if (va >= 0xF0000000u)
            len += snprintf(line + len, sizeof line - len, " %.1f%% %s", 100.0 * hist[i].n / total, rt_hle_label(va));
        else
            len += snprintf(line + len, sizeof line - len, " %.1f%% %06x", 100.0 * hist[i].n / total, (unsigned)va);
    }
    rt_log("%s\n", line);
}

#ifdef RT_COUNT
/* call counts per function (RT_ENTER), the top ones logged per 10 s with the profile */
uint32_t rt_call_count[0x200000];
static void count_report(void)
{
    static uint32_t prev[0x200000];
    uint32_t top[80] = {0}, topn[80] = {0};
    for (uint32_t i = 0; i < 0x200000; i++) {
        uint32_t n = rt_call_count[i] - prev[i];
        prev[i] = rt_call_count[i];
        if (n <= topn[79]) continue;
        int k = 79;
        while (k > 0 && topn[k - 1] < n) { topn[k] = topn[k - 1]; top[k] = top[k - 1]; k--; }
        topn[k] = n, top[k] = i << 2;
    }
    char line[3000];
    int len = snprintf(line, sizeof line, "[calls] 10 s:");
    for (int k = 0; k < 80 && topn[k]; k++)
        len += snprintf(line + len, sizeof line - len, " %06x:%u", (unsigned)(top[k] + 0x400000u) , (unsigned)topn[k]);
    rt_log("%s\n", line);
}
#endif

static void *prof_thread(void *arg)
{
    (void)arg;
    uint32_t total = 0, ticks = 0;
    for (;;) {
        rt_platform_sleep_us(1000);
        uint32_t va = prof_cpu->prof_va;
        if (va) { count(va); total++; }
        if (++ticks >= 10000) {
            if (total && rt_verbose) report(total);
#ifdef RT_COUNT
            if (rt_verbose) count_report();
#endif
            memset(hist, 0, sizeof hist);
            total = ticks = 0;
        }
    }
    return NULL;
}

void rt_prof_start(CPU *main_cpu)
{
    prof_cpu = main_cpu;
    pthread_t t;
    pthread_create(&t, NULL, prof_thread, NULL);
}
#endif

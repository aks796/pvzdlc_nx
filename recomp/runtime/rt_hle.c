/* rt_hle.c: the bridge from guest calls to host (HLE) functions.
 *
 * Every host function the guest can reach (imports, GetProcAddress results,
 * COM methods, thread-exit trampolines) gets a fake guest address
 * RT_HLE_BASE + 16 * slot. rt_call sends those here. A slot without a
 * function stands for an import we haven't written yet: calling it stops the
 * game with its name, which is how the next piece of work gets chosen. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "rt.h"

#define MAX_SLOTS 16384
#define MAX_TABLES 32

typedef struct {
    hle_fn fn;
    char label[96];
} Slot;

static Slot slots[MAX_SLOTS];
static uint32_t nslots = 1; /* slot 0 unused */
static const HleEntry *tables[MAX_TABLES];
static int ntables;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void rt_hle_register(const HleEntry *t)
{
    if (ntables < MAX_TABLES) tables[ntables++] = t;
}

uint32_t rt_hle_addr(hle_fn fn, const char *label)
{
    pthread_mutex_lock(&lock);
    for (uint32_t i = 1; i < nslots; i++) {
        if (slots[i].fn == fn && !strcmp(slots[i].label, label)) {
            pthread_mutex_unlock(&lock);
            return RT_HLE_BASE + 16 * i;
        }
    }
    if (nslots >= MAX_SLOTS) rt_fatal("out of HLE slots");
    uint32_t i = nslots++;
    slots[i].fn = fn;
    snprintf(slots[i].label, sizeof slots[i].label, "%s", label);
    pthread_mutex_unlock(&lock);
    return RT_HLE_BASE + 16 * i;
}

uint32_t rt_hle_lookup(const char *dll, const char *name)
{
    for (int t = 0; t < ntables; t++) {
        for (const HleEntry *e = tables[t]; e->name; e++) {
            if (!strcasecmp(e->dll, dll) && !strcmp(e->name, name)) {
                char label[160];
                snprintf(label, sizeof label, "%s!%s", e->dll, e->name);
                return rt_hle_addr(e->fn, label);
            }
        }
    }
    return 0;
}

void rt_hle_call(CPU *c, uint32_t target)
{
    uint32_t i = (target - RT_HLE_BASE) / 16;
    if (i == 0 || i >= nslots || (target & 15)) rt_trap(c, target, "bad HLE address");
    Slot *s = &slots[i];
    if (!s->fn) {
        rt_log("[hle] not implemented yet: %s (return address %08x)\n", s->label, (unsigned)g_rd32(c->esp));
        rt_trap(c, g_rd32(c->esp), "missing import");
    }
    if (rt_verbose > 1) rt_log("[hle] %s\n", s->label);
    s->fn(c);
}

/* the import or COM method behind a fake address, for the profiler */
const char *rt_hle_label(uint32_t target)
{
    uint32_t i = (target - RT_HLE_BASE) / 16;
    return i && i < nslots && slots[i].label ? slots[i].label : "hle";
}

/* Each HLE source file provides its table; hle_tables.h lists them. */
#define HLE_TABLE(x) extern const HleEntry x[];
#include "hle_tables.h"
#undef HLE_TABLE

void rt_hle_init(void)
{
#define HLE_TABLE(x) rt_hle_register(x);
#include "hle_tables.h"
#undef HLE_TABLE
}

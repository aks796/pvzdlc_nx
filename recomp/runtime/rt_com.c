/* rt_com.c: COM objects whose methods are host functions.
 *
 * A guest-visible object is 8 bytes of guest memory: [vtable][impl id].
 * The vtable is a guest array of HLE fake addresses, built once per
 * interface; slots without a host function are named stubs, so calling one
 * stops with e.g. "IDirectDrawSurface7::UpdateOverlay". Several guest
 * objects (interface views) can share one host impl. */
#include <stdio.h>
#include <string.h>

#include "rt.h"
#include "rt_com.h"

#define MAX_IMPL 65536
static void *impls[MAX_IMPL];
static uint32_t next_impl = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t com_vtable(const char *iface, const ComMethod *m, int n)
{
    uint32_t vt = rt_heap_alloc(4 * (uint32_t)n, 1);
    for (int i = 0; i < n; i++) {
        char label[128];
        snprintf(label, sizeof label, "%s::%s", iface, m[i].name ? m[i].name : "?");
        g_wr32(vt + 4 * (uint32_t)i, rt_hle_addr(m[i].fn, label));
    }
    return vt;
}

uint32_t com_wrap(uint32_t vtable, void *impl)
{
    pthread_mutex_lock(&lock);
    uint32_t id = 0;
    for (uint32_t i = 1; i < MAX_IMPL; i++) {
        uint32_t k = (next_impl + i) % MAX_IMPL;
        if (k && !impls[k]) { id = k; break; }
    }
    if (!id) rt_fatal("COM object table full");
    impls[id] = impl;
    next_impl = id;
    pthread_mutex_unlock(&lock);
    uint32_t obj = rt_heap_alloc(8, 0);
    g_wr32(obj, vtable);
    g_wr32(obj + 4, id);
    return obj;
}

void *com_impl(uint32_t obj)
{
    if (!obj) return NULL;
    uint32_t id = g_rd32(obj + 4);
    return id < MAX_IMPL ? impls[id] : NULL;
}

void com_unwrap(uint32_t obj)
{
    uint32_t id = g_rd32(obj + 4);
    pthread_mutex_lock(&lock);
    if (id < MAX_IMPL) impls[id] = NULL;
    pthread_mutex_unlock(&lock);
    g_wr32(obj, 0);
    rt_heap_free(obj);
}

int com_guid_eq(uint32_t guid, const uint8_t want[16])
{
    return guid && !memcmp(G2H(guid), want, 16);
}

void com_guid_str(uint32_t guid, char *out, size_t cap)
{
    const uint8_t *g = G2H(guid);
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    snprintf(out, cap, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", (unsigned)d1, d2, d3, g[8], g[9], g[10],
             g[11], g[12], g[13], g[14], g[15]);
}

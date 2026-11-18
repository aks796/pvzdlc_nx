/* rt_com.h: helpers for COM objects implemented on the host (see rt_com.c). */
#ifndef RT_COM_H
#define RT_COM_H

#include "rt.h"

typedef struct {
    const char *name;
    hle_fn fn; /* NULL: a named stub that stops the game if called */
} ComMethod;

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u

uint32_t com_vtable(const char *iface, const ComMethod *m, int n);
uint32_t com_wrap(uint32_t vtable, void *impl);
void *com_impl(uint32_t obj);
void com_unwrap(uint32_t obj);
int com_guid_eq(uint32_t guid, const uint8_t want[16]);
void com_guid_str(uint32_t guid, char *out, size_t cap);

/* GUID bytes as stored in memory: Data1 LE, Data2 LE, Data3 LE, Data4 */
#define GUID_BYTES(d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7)                                            \
    {(uint8_t)(d1), (uint8_t)((d1) >> 8), (uint8_t)((d1) >> 16), (uint8_t)((d1) >> 24), (uint8_t)(d2),    \
     (uint8_t)((d2) >> 8), (uint8_t)(d3), (uint8_t)((d3) >> 8), b0, b1, b2, b3, b4, b5, b6, b7}

#endif

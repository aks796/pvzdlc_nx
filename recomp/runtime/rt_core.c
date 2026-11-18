/* rt_core.c: dispatch, traps and the CPU-identity instructions for
 * recompiled code. The Windows API layer (HLE) lives elsewhere. */
#include <stdio.h>
#include <stdlib.h>

#include "x86rt.h"

uint32_t rt_img_delta;
#ifndef RT_FLAT32
uint8_t *rt_membase __attribute__((weak)); /* rt_mem.c owns it in full builds */
#endif

/* Fake addresses handed out for imported functions, COM methods and
 * GetProcAddress results: rt_call routes them to the HLE layer. */
#define RT_HLE_BASE 0xF0000000u

void rt_hle_call(CPU *c, uint32_t target) __attribute__((weak));
void rt_hle_call(CPU *c, uint32_t target)
{
    uint32_t i = (target - RT_HLE_BASE) / 16;
    if (i < rt_imports_count)
        fprintf(stderr, "[rt] unimplemented import %s!%s\n", rt_imports[i].dll, rt_imports[i].name);
    rt_trap(c, target, "HLE call");
}

static rt_fn lookup_slow(uint32_t va, uint32_t *index)
{
    uint32_t lo = 0, hi = rt_funcs_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rt_funcs[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    *index = lo;
    return (lo < rt_funcs_count && rt_funcs[lo].va == va) ? rt_funcs[lo].fn : NULL;
}

/* Indirect calls (vtables, function pointers) are hot: a direct-mapped cache in front of the binary
 * search. Each slot is one 64-bit word, guest address << 32 | index into rt_funcs, so threads racing on
 * a slot only ever see a whole entry (a miss at worst). */
#define CALL_CACHE_BITS 14
static uint64_t call_cache[1u << CALL_CACHE_BITS];

static inline rt_fn lookup(uint32_t va)
{
    uint32_t h = (va ^ (va >> CALL_CACHE_BITS)) & ((1u << CALL_CACHE_BITS) - 1);
    uint64_t e = __atomic_load_n(&call_cache[h], __ATOMIC_RELAXED);
    if ((uint32_t)(e >> 32) == va && e) return rt_funcs[(uint32_t)e].fn;
    uint32_t i;
    rt_fn fn = lookup_slow(va, &i);
    if (fn) __atomic_store_n(&call_cache[h], (uint64_t)va << 32 | i, __ATOMIC_RELAXED);
    return fn;
}

void rt_call(CPU *c, uint32_t target)
{
    if (target >= RT_HLE_BASE) {
#ifdef RT_PROF
        c->prof_va = target; /* rt_prof.c: time in the Windows API layer, by import */
#endif
        rt_hle_call(c, target);
        return;
    }
    rt_fn fn = lookup(target - rt_img_delta);
    if (!fn) {
        rt_resume(c, target - rt_img_delta); /* an exception continuation: doesn't return if it is one */
        rt_trap(c, target - rt_img_delta, "call to unknown code");
    }
    fn(c);
}

void rt_dump(CPU *c) __attribute__((weak));
/* The runtime's log, when it is linked (rt_loader.c): the reason, the guest state, flushed */
void rt_trap_report(CPU *c, uint32_t va, const char *what) __attribute__((weak));

void rt_trap(CPU *c, uint32_t va, const char *what)
{
    if (rt_trap_report) {
        rt_trap_report(c, va, what);
        abort();
    }
    if (rt_dump) {
        fprintf(stderr, "[rt] trap at %08x: %s\n", (unsigned)va, what);
        rt_dump(c);
        abort();
    }
    fprintf(stderr, "[rt] trap at %08x: %s (eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x)\n",
            (unsigned)va, what, (unsigned)c->eax, (unsigned)c->ecx, (unsigned)c->edx, (unsigned)c->ebx,
            (unsigned)c->esp, (unsigned)c->ebp, (unsigned)c->esi, (unsigned)c->edi);
    abort();
}

void rt_unimpl(CPU *c, uint32_t va, const char *text)
{
    fprintf(stderr, "[rt] unimplemented instruction at %08x: %s\n", (unsigned)va, text);
    rt_trap(c, va, "unimplemented");
}

/* Report a plain SSE2 CPU so the CRT and STL never pick their AVX/SSE4 paths */
void rt_cpuid(CPU *c)
{
    switch (c->eax) {
    case 0:
        c->eax = 1;
        c->ebx = 0x756e6547; /* "GenuineIntel" */
        c->edx = 0x49656e69;
        c->ecx = 0x6c65746e;
        break;
    case 1:
        c->eax = 0x000006f6; /* family 6 model 15 */
        c->ebx = 0;
        c->ecx = 0;                       /* no SSE3+ */
        c->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26);
        break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
        break;
    }
}

void rt_rdtsc(CPU *c) __attribute__((weak));
void rt_rdtsc(CPU *c)
{
    static uint64_t t;
    t += 1000;
    c->eax = (uint32_t)t;
    c->edx = (uint32_t)(t >> 32);
}

void rt_xgetbv(CPU *c)
{
    c->eax = 3; /* x87 + SSE state, no AVX */
    c->edx = 0;
}

void rt_fxam(CPU *c)
{
    double v = c->fst[c->ftop & 7];
    uint32_t sw = c->fsw & ~0x4700u;
    if (__builtin_signbit(v)) sw |= 0x200u;           /* C1 */
    if (v != v) sw |= 0x100u;                          /* NaN: C0 */
    else if (__builtin_isinf(v)) sw |= 0x500u;         /* C2|C0 */
    else if (v == 0.0) sw |= 0x4000u;                  /* C3 */
    else sw |= 0x400u;                                 /* normal: C2 */
    c->fsw = sw;
}

/* FNSAVE/FRSTOR: 108-byte image (control/status/tag + 8 x 80-bit registers) */
void rt_fnsave(CPU *c, uint32_t a)
{
    RT_MEMBASE_LOCAL
    WR32(a, c->fcw);
    WR32(a + 4, (c->fsw & ~0x3800u) | ((c->ftop & 7) << 11));
    WR32(a + 8, 0);
    for (int i = 12; i < 28; i += 4) WR32(a + i, 0);
    for (int i = 0; i < 8; i++) WRF80(a + 28 + 10 * i, c->fst[(c->ftop + i) & 7]);
    c->fcw = 0x37f;
    c->fsw = 0;
    c->ftop = 0;
}

void rt_frstor(CPU *c, uint32_t a)
{
    RT_MEMBASE_LOCAL
    c->fcw = RD16(a);
    c->fsw = RD16(a + 4);
    c->ftop = (c->fsw >> 11) & 7;
    for (int i = 0; i < 8; i++) c->fst[(c->ftop + i) & 7] = RDF80(a + 28 + 10 * i);
}

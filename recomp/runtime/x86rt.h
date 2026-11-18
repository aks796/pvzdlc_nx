/* x86rt.h: what recompiled x86-32 code needs from the runtime.
 *
 * Guest code runs on guest memory addressed by 32-bit guest addresses. On the
 * AArch32 target a guest address IS a host pointer (RT_MEMBASE 0); a 64-bit
 * test host can set RT_MEMBASE to a reserved 4 GiB window.
 *
 * Guest GPRs are C locals in every recompiled function (LOCALS); STORE/LOAD
 * sync them with the CPU struct whenever control leaves the function.
 * Flags are lazy (fl_op/fl_a/fl_b/fl_r/fl_c, see the f_* helpers).
 */
#ifndef X86RT_H
#define X86RT_H

#include <stdint.h>

typedef union X128 {
    uint8_t b[16];
    uint16_t w[8];
    uint32_t u[4];
    uint64_t q[2];
    float f[4];
    double d[2];
} X128;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;       /* direction flag */
    uint32_t fs_base;  /* guest address of this thread's TEB */
    uint32_t ftop, fsw, fcw, mxcsr;
    double fst[8];     /* x87 stack, kept as double */
    X128 x[8];
    void *host;        /* runtime's per-thread data */
    uint32_t trace_pos;
    uint32_t trace[64]; /* last functions entered (RT_TRACE builds) */
    void *rj_top;       /* innermost live resume point (exceptions) */
    uint32_t resume_va; /* continuation being resumed */
    volatile uint32_t prof_va; /* RT_PROF: the function entered last (rt_prof.c samples it) */
} CPU;

/* A resume point: functions owning exception continuations push one at entry. Clang has no __builtin_setjmp on
 * AArch64 (the Android build): there it is the C library's _setjmp, which suits the generated code (after a resume
 * it reloads the guest registers from the CPU struct, LOAD, rather than trusting locals). */
#if defined(__clang__) && defined(__aarch64__)
#include <setjmp.h>
#define __builtin_setjmp(b) _setjmp(b)
#define __builtin_longjmp(b, v) _longjmp(b, v)
#define RT_JMP_BUF jmp_buf buf
#else
#define RT_JMP_BUF void *buf[5] /* __builtin_setjmp buffer */
#endif
typedef struct RtJmp {
    RT_JMP_BUF;
    struct RtJmp *prev;
    uint32_t func, esp0;
} RtJmp;
#define RJ_POP (c->rj_top = rj_.prev)

#ifdef RT_TRACE
#define RT_ENTER(va) (c->trace[c->trace_pos++ & 63] = (va))
#elif defined(RT_COUNT)
extern uint32_t rt_call_count[0x200000];
#define RT_ENTER(va) (c->prof_va = (va), rt_call_count[(((va) - 0x400000u) >> 2) & 0x1fffff]++)
#elif defined(RT_PROF)
#define RT_ENTER(va) (c->prof_va = (va))
#else
#define RT_ENTER(va) ((void)0)
#endif

/* Image relocation: generated code writes relocated constants as IMG(va). */
extern uint32_t rt_img_delta;
#define IMG(v) ((uint32_t)(v) + rt_img_delta)

/* RT_FLAT32 (AArch32 host): guest address == host pointer. Otherwise
 * (AArch64 host) guest memory is a 4 GiB window at rt_membase; every
 * function keeps the base in a local (M_) so it stays in a register. */
#ifdef RT_FLAT32
#define RT_MEMBASE_LOCAL
#define GP(a) ((uint8_t *)(uintptr_t)(uint32_t)(a))
#define GPG(a) GP(a)
#else
extern uint8_t *rt_membase;
#define RT_MEMBASE_LOCAL uint8_t *const M_ = rt_membase; (void)M_;
#define GP(a) (M_ + (uint32_t)(a))
#define GPG(a) (rt_membase + (uint32_t)(a))
#endif

/* Unaligned guest accesses go through memcpy: one load/store on ARMv8, and it
 * avoids under-aligned typedefs (GCC 15 ICEs passing them as arguments). */
#define RT_LD(T, p) ({ T v__; __builtin_memcpy(&v__, (p), sizeof(T)); v__; })
#define RT_ST(T, p, v) do { T v__ = (T)(v); __builtin_memcpy((p), &v__, sizeof(T)); } while (0)

#define RD8(a) ((uint32_t)*GP(a))
#define RD16(a) ((uint32_t)RT_LD(uint16_t, GP(a)))
#define RD32(a) RT_LD(uint32_t, GP(a))
#define RD64(a) RT_LD(uint64_t, GP(a))
#define RDF32(a) RT_LD(float, GP(a))
#define RDF64(a) RT_LD(double, GP(a))
#define WR8(a, v) (*GP(a) = (uint8_t)(v))
#define WR16(a, v) RT_ST(uint16_t, GP(a), v)
#define WR32(a, v) RT_ST(uint32_t, GP(a), v)
#define WR64(a, v) RT_ST(uint64_t, GP(a), v)
#define WRF32(a, v) RT_ST(float, GP(a), v)
#define WRF64(a, v) RT_ST(double, GP(a), v)

static inline X128 RDX128(uint32_t a) { X128 v; __builtin_memcpy(&v, GPG(a), 16); return v; }
static inline X128 RDX64(uint32_t a) { X128 v = {{0}}; __builtin_memcpy(&v, GPG(a), 8); return v; }
static inline X128 RDX32(uint32_t a) { X128 v = {{0}}; __builtin_memcpy(&v, GPG(a), 4); return v; }
static inline void WRX128(uint32_t a, X128 v) { __builtin_memcpy(GPG(a), &v, 16); }
static inline X128 X128_U32(uint32_t u) { X128 v = {{0}}; v.u[0] = u; return v; }

/* ---- registers ---- */
#define LOCALS                                                                    \
    RT_MEMBASE_LOCAL                                                              \
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, \
             ebp = c->ebp, esi = c->esi, edi = c->edi;                            \
    uint32_t fl_op = 0, fl_a = 0, fl_b = 0, fl_r = 0, fl_c = 0
#define STORE (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp, \
               c->ebp = ebp, c->esi = esi, c->edi = edi)
#define LOAD (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, \
              ebp = c->ebp, esi = c->esi, edi = c->edi)

/* ---- lazy flags ----
 * fl_op = kind * 4 + width (0: 8-bit, 1: 16-bit, 2: 32-bit). Kinds match
 * K in lift.py. fl_c is the carry-in (ADC/SBB), the preserved CF (INC/DEC)
 * or the tested bit (BT). MUL keeps CF=OF in fl_b. EXPL keeps EFLAGS in fl_r. */
enum { K_ADD, K_SUB, K_ADC, K_SBB, K_LOGIC, K_INC, K_DEC, K_NEG, K_SHL, K_SHR, K_SAR,
       K_MUL, K_EXPL, K_ROL, K_ROR, K_BT };

#define FK(op) ((op) >> 2)
#define FWID(op) ((op) & 3)
static inline uint32_t f_m(uint32_t op) { return FWID(op) == 0 ? 0xffu : FWID(op) == 1 ? 0xffffu : 0xffffffffu; }
static inline uint32_t f_bits(uint32_t op) { return FWID(op) == 0 ? 8 : FWID(op) == 1 ? 16 : 32; }
static inline uint32_t f_msb(uint32_t op, uint32_t v) { return (v >> (f_bits(op) - 1)) & 1; }
static inline int32_t f_sx(uint32_t op, uint32_t v)
{
    return FWID(op) == 0 ? (int8_t)v : FWID(op) == 1 ? (int16_t)v : (int32_t)v;
}

static inline uint32_t f_cf(uint32_t op, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    uint32_t m = f_m(op);
    switch (FK(op)) {
    case K_ADD: return (r & m) < (a & m);
    case K_SUB: return (a & m) < (b & m);
    case K_ADC: return c ? (r & m) <= (a & m) : (r & m) < (a & m);
    case K_SBB: return (uint64_t)(a & m) < (uint64_t)(b & m) + c;
    case K_INC: case K_DEC: case K_BT: return c;
    case K_NEG: return (a & m) != 0;
    case K_SHL: return b <= f_bits(op) ? (a >> (f_bits(op) - b)) & 1 : 0;
    case K_SHR: return (a >> (b - 1)) & 1;
    case K_SAR: return (uint32_t)(f_sx(op, a) >> (b - 1 < 31 ? b - 1 : 31)) & 1;
    case K_MUL: return b;
    case K_EXPL: case K_ROL: return r & 1;
    case K_ROR: return f_msb(op, r);
    default: return 0;
    }
}
static inline uint32_t f_zf(uint32_t op, uint32_t r)
{
    return FK(op) == K_EXPL ? (r >> 6) & 1 : (r & f_m(op)) == 0;
}
static inline uint32_t f_sf(uint32_t op, uint32_t r)
{
    return FK(op) == K_EXPL ? (r >> 7) & 1 : f_msb(op, r);
}
static inline uint32_t f_of(uint32_t op, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    uint32_t m = f_m(op), sign = 1u << (f_bits(op) - 1);
    switch (FK(op)) {
    case K_ADD: case K_ADC: return f_msb(op, (a ^ r) & (b ^ r));
    case K_SUB: case K_SBB: return f_msb(op, (a ^ b) & (a ^ r));
    case K_INC: return (r & m) == sign;
    case K_DEC: case K_NEG: return (a & m) == sign;
    case K_SHL: return f_msb(op, r) ^ f_cf(op, a, b, r, c);
    case K_SHR: return f_msb(op, a);
    case K_MUL: return b;
    case K_EXPL: return (r >> 11) & 1;
    case K_ROL: return f_msb(op, r) ^ (r & 1);
    case K_ROR: return f_msb(op, r) ^ ((r >> (f_bits(op) - 2)) & 1);
    default: return 0;
    }
}
static inline uint32_t f_pf(uint32_t op, uint32_t r)
{
    return FK(op) == K_EXPL ? (r >> 2) & 1 : !__builtin_parity(r & 0xffu);
}
static inline uint32_t f_eflags(uint32_t op, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    return f_cf(op, a, b, r, c) | (f_pf(op, r) << 2) | (f_zf(op, r) << 6) | (f_sf(op, r) << 7) |
           (f_of(op, a, b, r, c) << 11) | 0x202u;
}
static inline int cc_be(uint32_t op, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    if (FK(op) == K_SUB) return (a & f_m(op)) <= (b & f_m(op));
    return f_cf(op, a, b, r, c) | f_zf(op, r);
}
static inline int cc_l(uint32_t op, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    if (FK(op) == K_SUB) return f_sx(op, a) < f_sx(op, b);
    if (FK(op) == K_LOGIC) return f_msb(op, r);
    return f_sf(op, r) != f_of(op, a, b, r, c);
}
static inline int cc_le(uint32_t op, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    if (FK(op) == K_SUB) return f_sx(op, a) <= f_sx(op, b);
    if (FK(op) == K_LOGIC) return (r & f_m(op)) == 0 || f_msb(op, r);
    return f_zf(op, r) || f_sf(op, r) != f_of(op, a, b, r, c);
}
#define FL fl_op, fl_a, fl_b, fl_r, fl_c
#define CC_B ((int)f_cf(FL))
#define CC_E ((int)f_zf(fl_op, fl_r))
#define CC_BE cc_be(FL)
#define CC_S ((int)f_sf(fl_op, fl_r))
#define CC_O ((int)f_of(FL))
#define CC_P ((int)f_pf(fl_op, fl_r))
#define CC_L cc_l(FL)
#define CC_LE cc_le(FL)
#define EFLAGS f_eflags(FL)

/* ---- x87 (as double) ---- */
#define ST(i) (c->fst[(c->ftop + (i)) & 7])
#define FPUSH(v) do { double v__ = (v); c->ftop = (c->ftop - 1) & 7; c->fst[c->ftop] = v__; } while (0)
#define FPOP (c->ftop = (c->ftop + 1) & 7)
#define FSW ((uint16_t)((c->fsw & ~0x3800u) | ((c->ftop & 7) << 11)))
#define FCOM(a, b) do { double a__ = (a), b__ = (b); c->fsw &= ~0x4500u;                  \
        if (a__ != a__ || b__ != b__) c->fsw |= 0x4500u;                                 \
        else if (a__ < b__) c->fsw |= 0x100u; else if (a__ == b__) c->fsw |= 0x4000u; } while (0)
#define FCOMI(a, b) do { double a__ = (a), b__ = (b); fl_op = K_EXPL * 4 + 2;            \
        fl_r = (a__ != a__ || b__ != b__) ? 0x45u : a__ < b__ ? 1u : a__ == b__ ? 0x40u : 0u; } while (0)
#define FCOMI_SSE FCOMI
#define FXAM rt_fxam(c)
#define FROUND(x) rt_fround(c, (x))

#define rt_sqrt __builtin_sqrt
#define rt_sqrtf __builtin_sqrtf
#define rt_trunc __builtin_trunc
#define rt_rint __builtin_rint
#define rt_sin __builtin_sin
#define rt_cos __builtin_cos
#define rt_atan2 __builtin_atan2
#define rt_exp2 __builtin_exp2
#define rt_log2 __builtin_log2
#define rt_fmod __builtin_fmod
#define rt_remainder __builtin_remainder
#define rt_ldexp __builtin_ldexp

static inline double rt_fround(CPU *c, double x)
{
    switch ((c->fcw >> 10) & 3) {
    case 0: return __builtin_rint(x);
    case 1: return __builtin_floor(x);
    case 2: return __builtin_ceil(x);
    default: return __builtin_trunc(x);
    }
}
/* x86 float->int: out of range or NaN gives the "integer indefinite" value */
static inline int32_t FTOI32(double x)
{
    return (x > -2147483649.0 && x < 2147483648.0) ? (int32_t)x : INT32_MIN;
}
static inline int16_t FTOI16(double x)
{
    return (x > -32769.0 && x < 32768.0) ? (int16_t)x : INT16_MIN;
}
static inline int64_t FTOI64(double x)
{
    return (x >= -9223372036854775808.0 && x < 9223372036854775808.0) ? (int64_t)x : INT64_MIN;
}
static inline double RDF80(uint32_t a)
{
    RT_MEMBASE_LOCAL
    uint64_t mant = RD64(a);
    uint32_t se = RD16(a + 8), e = se & 0x7fff;
    double v;
    if (e == 0 && mant == 0) v = 0.0;
    else if (e == 0x7fff) v = (mant << 1) ? __builtin_nan("") : __builtin_inf();
    else v = __builtin_ldexp((double)mant, (int)e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static inline void WRF80(uint32_t a, double v)
{
    RT_MEMBASE_LOCAL
    uint64_t bits;
    __builtin_memcpy(&bits, &v, 8);
    uint32_t sign = (uint32_t)(bits >> 63), e = (uint32_t)(bits >> 52) & 0x7ff;
    uint64_t frac = bits & 0xfffffffffffffull, mant;
    uint32_t e80;
    if (e == 0 && frac == 0) { e80 = 0; mant = 0; }
    else if (e == 0x7ff) { e80 = 0x7fff; mant = 0x8000000000000000ull | (frac << 11); }
    else if (e == 0) { /* denormal: normalise */
        int sh = __builtin_clzll(frac) - 11;
        mant = (frac << (sh + 11)) | 0x8000000000000000ull;
        e80 = (uint32_t)(15360 + 1 - sh);
    } else { e80 = e - 1023 + 16383; mant = 0x8000000000000000ull | (frac << 11); }
    WR64(a, mant);
    WR16(a + 8, (sign << 15) | e80);
}

/* ---- SSE helpers ---- */
static inline uint32_t rt_pmovmskb(X128 v)
{
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) m |= (uint32_t)(v.b[i] >> 7) << i;
    return m;
}
static inline uint8_t rt_sat_u8(int16_t v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static inline int8_t rt_sat_s8(int16_t v) { return v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }
static inline X128 rt_packuswb(X128 a, X128 b)
{
    X128 r;
    for (int i = 0; i < 8; i++) { r.b[i] = rt_sat_u8((int16_t)a.w[i]); r.b[8 + i] = rt_sat_u8((int16_t)b.w[i]); }
    return r;
}
static inline X128 rt_packsswb(X128 a, X128 b)
{
    X128 r;
    for (int i = 0; i < 8; i++) {
        r.b[i] = (uint8_t)rt_sat_s8((int16_t)a.w[i]);
        r.b[8 + i] = (uint8_t)rt_sat_s8((int16_t)b.w[i]);
    }
    return r;
}

/* ---- runtime entry points ---- */
void rt_call(CPU *c, uint32_t target);
void rt_trap(CPU *c, uint32_t va, const char *what) __attribute__((noreturn));
void rt_unimpl(CPU *c, uint32_t va, const char *text) __attribute__((noreturn));
void rt_cpuid(CPU *c);
void rt_rdtsc(CPU *c);
void rt_xgetbv(CPU *c);
void rt_fxam(CPU *c);
void rt_fnsave(CPU *c, uint32_t a);
void rt_frstor(CPU *c, uint32_t a);

typedef void (*rt_fn)(CPU *c);
typedef struct { uint32_t va; rt_fn fn; } RtFunc;
typedef struct { uint32_t iat; const char *dll; const char *name; } RtImport;
typedef struct { uint32_t va, func; } RtCont;
extern const RtCont rt_conts[];
extern const uint32_t rt_conts_count;
void rt_resume(CPU *c, uint32_t va);
extern const RtFunc rt_funcs[];
extern const uint32_t rt_funcs_count;
extern const RtImport rt_imports[];
extern const uint32_t rt_imports_count;

#endif

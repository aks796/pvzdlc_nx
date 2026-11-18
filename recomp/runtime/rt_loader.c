/* rt_loader.c: maps the game exe into the guest window, wires its imports
 * to the HLE layer, and runs guest threads (TEB, TLS, stack, entry). */
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "rt.h"

RtImage rt_image;
uint32_t rt_main_tid;
static pthread_key_t self_key;
static uint32_t next_tid = 0x100;
static pthread_mutex_t tid_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

int rt_load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        rt_log("[loader] can't open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)len);
    if (fread(file, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(file);
        return -1;
    }
    fclose(f);

    const uint8_t *nt = file + rd32(file + 0x3c);
    const uint8_t *fh = nt + 4, *oh = nt + 24;
    uint32_t nsec = rd16(fh + 2), opt_size = rd16(fh + 16);
    RtImage *im = &rt_image;
    im->entry = rd32(oh + 16);
    im->base = rd32(oh + 28);
    im->size = rd32(oh + 56);
    uint32_t hdr_size = rd32(oh + 60);
    im->stack_reserve = rd32(oh + 72);
    const uint8_t *dd = oh + 96;
    uint32_t imp_rva = rd32(dd + 8), tls_rva = rd32(dd + 9 * 8);
    im->entry += im->base;

    /* The window is ours, so the image goes at its preferred base: no relocation */
    if (im->base < 0x10000 || im->base + im->size > 0x02000000u) {
        rt_log("[loader] unexpected image range %08x+%x\n", (unsigned)im->base, (unsigned)im->size);
        return -1;
    }
    rt_img_delta = 0;
    memcpy(G2H(im->base), file, hdr_size);
    const uint8_t *sec = oh + opt_size;
    for (uint32_t i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = rd32(sec + 8), va = rd32(sec + 12), rsize = rd32(sec + 16), roff = rd32(sec + 20);
        uint32_t n = rsize < vsize ? rsize : vsize;
        if (!vsize) n = rsize;
        if (n && roff + n <= (uint32_t)len) memcpy(G2H(im->base + va), file + roff, n);
        if (rt_verbose) rt_log("[loader] %-8.8s %08x %08x\n", (const char *)sec, (unsigned)(im->base + va), (unsigned)vsize);
    }
    free(file);

    /* Imports: every IAT slot gets the fake address of its HLE function (or a stub that names it) */
    int missing = 0, total = 0;
    for (uint32_t d = im->base + imp_rva; g_rd32(d + 16); d += 20) {
        char dll[64];
        snprintf(dll, sizeof dll, "%s", (const char *)G2H(im->base + g_rd32(d + 12)));
        for (char *p = dll; *p; p++) *p = (char)tolower((unsigned char)*p);
        uint32_t names = g_rd32(d) ? g_rd32(d) : g_rd32(d + 16);
        for (uint32_t k = 0;; k++) {
            uint32_t th = g_rd32(im->base + names + 4 * k);
            if (!th) break;
            char name[128];
            if (th & 0x80000000u) snprintf(name, sizeof name, "#%u", (unsigned)(th & 0xffff));
            else snprintf(name, sizeof name, "%s", (const char *)G2H(im->base + th + 2));
            uint32_t fake = rt_hle_lookup(dll, name);
            if (!fake) {
                char label[200];
                snprintf(label, sizeof label, "%s!%s", dll, name);
                fake = rt_hle_addr(NULL, label);
                missing++;
            }
            g_wr32(im->base + g_rd32(d + 16) + 4 * k, fake);
            total++;
        }
    }
    rt_log("[loader] %d imports, %d without an HLE implementation yet\n", total, missing);

    if (tls_rva) {
        uint32_t t = im->base + tls_rva;
        im->tls_start = g_rd32(t);
        im->tls_end = g_rd32(t + 4);
        im->tls_index_addr = g_rd32(t + 8);
        im->tls_callbacks = g_rd32(t + 12);
        im->tls_zero = g_rd32(t + 16);
        if (im->tls_index_addr) g_wr32(im->tls_index_addr, 0);
    }
    return 0;
}

/* ---- threads ---- */
RtThread *rt_thread_self(void) { return pthread_getspecific(self_key); }

static void setup_peb(void)
{
    uint32_t peb = rt_vm_alloc(0, 0x1000);
    rt_image.peb = peb;
    g_wr32(peb + 0x08, rt_image.base);  /* ImageBaseAddress */
    g_wr32(peb + 0x10, peb + 0x800);    /* ProcessParameters: zeroed RTL_USER_PROCESS_PARAMETERS */
    g_wr32(peb + 0x18, 0x00a00000u);    /* ProcessHeap (handle value, see GetProcessHeap) */
    g_wr32(peb + 0x64, 2);              /* NumberOfProcessors */
    g_wr32(peb + 0xa4, 10);             /* OSMajorVersion */
    g_wr32(peb + 0xa8, 0);
    g_wr16(peb + 0xac, 19041);
    g_wr32(peb + 0xb0, 2);              /* OSPlatformId */
}

RtThread *rt_thread_new(uint32_t stack_size)
{
    RtThread *t = calloc(1, sizeof *t);
    if (!stack_size) stack_size = rt_image.stack_reserve ? rt_image.stack_reserve : 0x100000;
    if (stack_size < 0x40000) stack_size = 0x40000;
    t->stack_lo = rt_vm_alloc(0, stack_size);
    t->stack_hi = t->stack_lo + stack_size;
    pthread_mutex_lock(&tid_lock);
    t->tid = next_tid += 4;
    pthread_mutex_unlock(&tid_lock);

    /* TEB + TLS pointer array + implicit TLS block in one 64 KiB unit */
    uint32_t teb = rt_vm_alloc(0, 0x10000);
    t->teb = teb;
    g_wr32(teb + 0x00, 0xffffffffu);     /* SEH chain end */
    g_wr32(teb + 0x04, t->stack_hi);
    g_wr32(teb + 0x08, t->stack_lo);
    g_wr32(teb + 0x18, teb);
    g_wr32(teb + 0x20, 0x1000);          /* pid */
    g_wr32(teb + 0x24, t->tid);
    g_wr32(teb + 0x2c, teb + 0x2000);    /* ThreadLocalStoragePointer */
    g_wr32(teb + 0x30, rt_image.peb);
    uint32_t tls_len = rt_image.tls_end - rt_image.tls_start;
    if (rt_image.tls_start) {
        uint32_t blk = teb + 0x3000;
        if (tls_len + rt_image.tls_zero > 0xd000) blk = rt_heap_alloc(tls_len + rt_image.tls_zero, 1);
        memcpy(G2H(blk), G2H(rt_image.tls_start), tls_len);
        t->tls_block = blk;
        g_wr32(teb + 0x2000, blk);
    }

    CPU *c = &t->cpu;
    c->esp = t->stack_hi - 16;
    c->fs_base = teb;
    c->fcw = 0x27f; /* MSVC CRT default: 53-bit precision, round to nearest */
    c->mxcsr = 0x1f80;
    c->host = t;
    return t;
}

void rt_thread_attach(RtThread *t)
{
    pthread_setspecific(self_key, t);
}

uint32_t rt_guest_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t save = c->esp;
    for (int i = nargs - 1; i >= 0; i--) {
        c->esp -= 4;
        g_wr32(c->esp, args[i]);
    }
    c->esp -= 4;
    g_wr32(c->esp, RT_HLE_BASE - 16); /* return address nobody returns to */
    rt_call(c, fn);
    c->esp = save;
    return c->eax;
}

void rt_run_tls_callbacks(CPU *c, uint32_t reason)
{
    if (!rt_image.tls_callbacks) return;
    for (uint32_t p = rt_image.tls_callbacks; g_rd32(p); p += 4) {
        uint32_t args[3] = {rt_image.base, reason, 0};
        rt_guest_call(c, g_rd32(p), 3, args);
    }
}

void rt_dump(CPU *c)
{
    rt_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", (unsigned)c->eax,
           (unsigned)c->ecx, (unsigned)c->edx, (unsigned)c->ebx, (unsigned)c->esp, (unsigned)c->ebp,
           (unsigned)c->esi, (unsigned)c->edi);
    rt_log("  recent functions (newest last):");
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t k = (c->trace_pos + i) & 63;
        if (c->trace[k]) rt_log(" %08x", (unsigned)c->trace[k]);
    }
    rt_log("\n  stack:");
    for (uint32_t i = 0; i < 24; i++) {
        uint32_t a = c->esp + 4 * i;
        if (a < 0x10000 || a >= RT_HLE_BASE) break;
        rt_log(" %08x", (unsigned)g_rd32(a));
    }
    rt_log("\n");
}

void rt_trap_report(CPU *c, uint32_t va, const char *what)
{
    rt_log("[trap] at %08x: %s\n", (unsigned)va, what);
    rt_dump(c);
    rt_log_flush();
}

/* A host fault (SIGSEGV here, the libnx exception handler on Switch): say where and dump the guest CPU */
void rt_report_fault(const char *what, void *addr)
{
    uint8_t *a = addr;
    RtThread *t = rt_thread_self();
    if (a >= rt_membase && a < rt_membase + 0x100000000ull)
        rt_log("[fault] %s at guest address %08x\n", what, (unsigned)(a - rt_membase));
    else
        rt_log("[fault] %s at host address %p\n", what, (void *)a);
    if (t) rt_dump(&t->cpu);
}

#ifdef _WIN32
/* (only access violations: C++ exceptions, libopenmpt's, are raised as SEH exceptions too and go on their way) */
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    rt_report_fault("access violation", (void *)e->ExceptionRecord->ExceptionInformation[1]);
    rt_log_flush();
    _exit(3);
}
#elif !defined(__SWITCH__)
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    rt_report_fault(sig == SIGBUS ? "SIGBUS" : "SIGSEGV", si->si_addr);
    _exit(3);
}
#endif

int rt_start(void)
{
    pthread_key_create(&self_key, NULL);
#ifdef _WIN32
    AddVectoredExceptionHandler(0, on_fault);
#elif !defined(__SWITCH__)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif

    setup_peb();
    RtThread *t = rt_thread_new(0);
    t->pt = pthread_self();
    rt_main_tid = t->tid;
#ifdef RT_PROF
    rt_prof_start(&t->cpu);
#endif
    rt_thread_attach(t);
    rt_run_tls_callbacks(&t->cpu, 1);
    rt_log("[loader] calling entry %08x\n", (unsigned)rt_image.entry);
    uint32_t r = rt_guest_call(&t->cpu, rt_image.entry, 0, NULL);
    rt_log("[loader] entry returned %u\n", (unsigned)r);
    return (int)r;
}

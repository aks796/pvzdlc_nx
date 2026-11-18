/* rt_mem.c: the guest address space.
 *
 * One 4 GiB host window holds the guest's whole 32-bit address space, so a
 * guest pointer is just an offset from rt_membase. The window is mapped
 * read/write up front (lazily backed by the host); only the null area and the
 * fake HLE range are left inaccessible so stray accesses fault loudly.
 *
 * Layout: 0x00400000 image, 0x02000000-0xE0000000 VirtualAlloc ranges
 * (64 KiB granularity), heap arenas carved from those.
 */
#include <stdlib.h>
#include <string.h>
#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "rt.h"

uint8_t *rt_membase;

#define VM_LO 0x02000000u
#define VM_HI 0xE0000000u
#define UNITS (0x100000000ull / RT_ALLOC_GRAN)

static uint32_t unit_len[UNITS];  /* reservation length (units) at its first unit, else 0 */
static uint8_t unit_used[UNITS];
static uint8_t unit_dirty[UNITS]; /* handed out before: zero it again on reuse */
static uint32_t vm_rover = VM_LO / RT_ALLOC_GRAN;
static uint32_t vm_committed, vm_peak; /* units: guest memory in use, and its high-water mark */

/* Guest memory in use and its peak, in bytes (-v statistics; the Switch backs every byte) */
void rt_vm_usage(uint64_t *now, uint64_t *peak)
{
    *now = (uint64_t)vm_committed * RT_ALLOC_GRAN;
    *peak = (uint64_t)vm_peak * RT_ALLOC_GRAN;
}
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef __SWITCH__
/* Horizon has no demand paging and no fixed-address mmap. The window is a reservation in the code
 * memory region, and guest memory appears in it 32 MiB at a time: zeroed heap pages that
 * svcMapProcessCodeMemory moves into the window, made read/write (the layout driftdroid and the NSMBW
 * port use on hardware). Nothing is ever unmapped: a freed range keeps its pages for reuse. */
#define CHUNK (32u << 20)
#define HEAP_TAIL_SLACK 0x1000 /* newlib's next-chunk header sits past a block; keep it out of the mapping */
static uint8_t chunk_backed[0x100000000ull / CHUNK];
static Handle own_process = INVALID_HANDLE;

/* hbloader hands the NRO its process handle; an emulator that starts the NRO itself doesn't, and the
 * process-memory calls refuse the pseudo-handle. Sending the pseudo-handle to ourselves over IPC as a
 * copied handle gives a real one. */
static void receive_own_handle(void *arg)
{
    Handle session = (Handle)(uintptr_t)arg;
    void *tls = armGetTls();
    hipcMakeRequestInline(tls);
    s32 index = 0;
    if (R_SUCCEEDED(svcReplyAndReceive(&index, &session, 1, INVALID_HANDLE, UINT64_MAX))) {
        HipcParsedRequest req = hipcParseRequest(tls);
        if (req.meta.num_copy_handles == 1) own_process = req.data.copy_handles[0];
    }
    svcCloseHandle(session);
}
static Handle get_own_process(void)
{
    if (own_process != INVALID_HANDLE) return own_process;
    if ((own_process = envGetOwnProcessHandle()) != INVALID_HANDLE) return own_process;
    Handle server = INVALID_HANDLE, client = INVALID_HANDLE;
    if (R_SUCCEEDED(svcCreateSession(&server, &client, 0, 0))) {
        Thread th;
        if (R_SUCCEEDED(threadCreate(&th, receive_own_handle, (void *)(uintptr_t)server, NULL, 0x1000, 0x20, -2))) {
            threadStart(&th);
            hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] = CUR_PROCESS_HANDLE;
            svcSendSyncRequest(client);
            threadWaitForExit(&th);
            threadClose(&th);
        }
        svcCloseHandle(client);
    }
    if (own_process == INVALID_HANDLE) own_process = CUR_PROCESS_HANDLE;
    return own_process;
}

/* Back guest [addr, addr+size) with memory (caller holds vm_lock or is single-threaded) */
static int commit(uint32_t addr, uint64_t size)
{
    for (uint64_t c = addr / CHUNK; c * CHUNK < (uint64_t)addr + size; c++) {
        if (chunk_backed[c]) continue;
        uint8_t *heap = memalign(0x1000, CHUNK + HEAP_TAIL_SLACK);
        if (!heap) {
            rt_log("[mem] out of memory backing guest %08x (%u MiB backed so far)\n", (unsigned)(c * CHUNK),
                   (unsigned)(rt_mem_backed() >> 20));
            return -1;
        }
        memset(heap, 0, CHUNK);
        Result rc = svcMapProcessCodeMemory(get_own_process(), (u64)(rt_membase + c * CHUNK), (u64)heap, CHUNK);
        if (R_SUCCEEDED(rc)) rc = svcSetProcessMemoryPermission(get_own_process(), (u64)(rt_membase + c * CHUNK), CHUNK, Perm_Rw);
        if (R_FAILED(rc)) {
            rt_log("[mem] mapping guest %08x failed (rc=%x)\n", (unsigned)(c * CHUNK), (unsigned)rc);
            return -1;
        }
        chunk_backed[c] = 1;
    }
    return 0;
}

uint64_t rt_mem_backed(void)
{
    uint64_t n = 0;
    for (uint32_t c = 0; c < sizeof chunk_backed; c++) n += chunk_backed[c];
    return n * CHUNK;
}

static int reserve_window(void)
{
    virtmemLock();
    void *p = virtmemFindCodeMemory(0x100000000ull + 0x10000, 0);
    VirtmemReservation *r = p ? virtmemAddReservation(p, 0x100000000ull + 0x10000) : NULL;
    virtmemUnlock();
    if (!r) return -1;
    rt_membase = p;
    /* the image, PEB/TEB and the loader's fixed addresses live below VM_LO */
    return commit(0, VM_LO);
}
#elif defined(_WIN32)
/* Windows: the window is reserved, and guest memory is committed into it 32 MiB at a time, as on the Switch (a
 * 4 GiB commit up front would count against the page file); rt_vm_mapped answers from the same table. The null
 * area stays inaccessible, as with mmap elsewhere. */
#define CHUNK (32u << 20)
static uint8_t chunk_backed[0x100000000ull / CHUNK];

static int commit(uint32_t addr, uint64_t size)
{
    for (uint64_t c = addr / CHUNK; c * CHUNK < (uint64_t)addr + size; c++) {
        if (chunk_backed[c]) continue;
        if (!VirtualAlloc(rt_membase + c * CHUNK, CHUNK, MEM_COMMIT, PAGE_READWRITE)) {
            rt_log("[mem] committing guest %08x failed (error %lu)\n", (unsigned)(c * CHUNK), (unsigned long)GetLastError());
            return -1;
        }
        if (c == 0) {
            DWORD old;
            VirtualProtect(rt_membase, 0x10000, PAGE_NOACCESS, &old);
        }
        chunk_backed[c] = 1;
    }
    return 0;
}

uint64_t rt_mem_backed(void)
{
    uint64_t n = 0;
    for (uint32_t c = 0; c < sizeof chunk_backed; c++) n += chunk_backed[c];
    return n * CHUNK;
}

static int reserve_window(void)
{
    void *p = VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return -1;
    rt_membase = p;
    /* the image, PEB/TEB and the loader's fixed addresses live below VM_LO */
    return commit(0, VM_LO);
}
#else
static int commit(uint32_t addr, uint64_t size) { (void)addr; (void)size; return 0; }
uint64_t rt_mem_backed(void) { return 0x100000000ull; }

static int reserve_window(void)
{
    void *p = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    rt_membase = p;
    mprotect(rt_membase, 0x10000, PROT_NONE);
    mprotect(rt_membase + RT_HLE_BASE, 0x10000000u, PROT_NONE);
    return 0;
}
#endif

/* a guest address that can be read without faulting (the host code that looks through the game's
 * objects checks with this: on the Switch only mapped chunks are, elsewhere the whole window is) */
int rt_vm_mapped(uint32_t a)
{
    if (a < 0x10000u || a >= 0xF0000000u) return 0;
#if defined(__SWITCH__) || defined(_WIN32)
    return chunk_backed[a / CHUNK] != 0;
#else
    return 1;
#endif
}

int rt_mem_init(void)
{
    if (reserve_window()) return -1;
    /* The image range is claimed by the loader; keep the allocator out of it */
    for (uint32_t u = 0; u < VM_LO / RT_ALLOC_GRAN; u++) unit_used[u] = 1;
    for (uint32_t u = VM_HI / RT_ALLOC_GRAN; u < UNITS; u++) unit_used[u] = 1;
    return 0;
}

static int range_free(uint32_t u, uint32_t n)
{
    if (u + n > UNITS) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (unit_used[u + i]) return 0;
    return 1;
}

static uint32_t claim(uint32_t u, uint32_t n)
{
    if (commit(u * RT_ALLOC_GRAN, (uint64_t)n * RT_ALLOC_GRAN)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        unit_used[u + i] = 1;
        if (unit_dirty[u + i]) {
            memset(rt_membase + (uint64_t)(u + i) * RT_ALLOC_GRAN, 0, RT_ALLOC_GRAN);
        }
        unit_dirty[u + i] = 1;
    }
    unit_len[u] = n;
    vm_committed += n;
    if (vm_committed > vm_peak) vm_peak = vm_committed;
    return u * RT_ALLOC_GRAN;
}

uint32_t rt_vm_alloc(uint32_t addr, uint32_t size)
{
    uint32_t n = (size + RT_ALLOC_GRAN - 1) / RT_ALLOC_GRAN;
    if (!n) n = 1;
    uint32_t r = 0;
    pthread_mutex_lock(&vm_lock);
    if (addr) {
        uint32_t u = addr / RT_ALLOC_GRAN;
        if (range_free(u, n)) r = claim(u, n);
    } else {
        uint32_t lo = VM_LO / RT_ALLOC_GRAN, hi = VM_HI / RT_ALLOC_GRAN;
        for (int pass = 0; pass < 2 && !r; pass++) {
            uint32_t u = pass ? lo : vm_rover;
            while (u + n <= hi) {
                if (range_free(u, n)) {
                    r = claim(u, n);
                    vm_rover = u + n;
                    break;
                }
                u++;
                while (u < hi && unit_used[u]) u += unit_len[u] ? unit_len[u] : 1;
            }
        }
    }
    pthread_mutex_unlock(&vm_lock);
    return r;
}

int rt_vm_free(uint32_t addr)
{
    uint32_t u = addr / RT_ALLOC_GRAN;
    pthread_mutex_lock(&vm_lock);
    uint32_t n = unit_len[u];
    if (!n || addr % RT_ALLOC_GRAN) {
        pthread_mutex_unlock(&vm_lock);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) unit_used[u + i] = 0;
    unit_len[u] = 0;
    vm_committed -= n;
    pthread_mutex_unlock(&vm_lock);
    return 1;
}

int rt_vm_query(uint32_t addr, uint32_t *base, uint32_t *size)
{
    uint32_t u = addr / RT_ALLOC_GRAN;
    pthread_mutex_lock(&vm_lock);
    uint32_t s = u;
    while (s > 0 && !unit_len[s] && unit_used[s]) s--;
    int ok = unit_used[u] && unit_len[s] && s + unit_len[s] > u;
    if (ok) {
        *base = s * RT_ALLOC_GRAN;
        *size = unit_len[s] * RT_ALLOC_GRAN;
    }
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

/* ---- heap ----
 * Size classes up to 256 KiB from 1 MiB arenas; bigger blocks get their own
 * VM range. Every block has a 16-byte header: size, class, magic. */
#define HDR 16u
#define MAGIC 0x48454150u
#define BIG 0xffu
static const uint32_t classes[] = {16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768,
                                   896, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152,
                                   65536, 98304, 131072, 196608, 262144};
#define NCLASS (sizeof(classes) / sizeof(classes[0]))
static uint32_t free_list[NCLASS];
static uint32_t arena_cur, arena_end;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static int class_of(uint32_t total)
{
    for (unsigned i = 0; i < NCLASS; i++)
        if (classes[i] >= total) return (int)i;
    return -1;
}

uint32_t rt_heap_alloc(uint32_t size, int zero)
{
    uint32_t total = size + HDR;
    int k = class_of(total);
    uint32_t blk;
    if (k < 0) {
        blk = rt_vm_alloc(0, total);
        if (!blk) return 0;
        g_wr32(blk, size);
        g_wr32(blk + 4, BIG);
        g_wr32(blk + 8, MAGIC);
        return blk + HDR;
    }
    pthread_mutex_lock(&heap_lock);
    blk = free_list[k];
    if (blk) {
        free_list[k] = g_rd32(blk + HDR);
    } else {
        if (arena_cur + classes[k] > arena_end) {
            arena_cur = rt_vm_alloc(0, 0x100000);
            if (!arena_cur) {
                pthread_mutex_unlock(&heap_lock);
                return 0;
            }
            arena_end = arena_cur + 0x100000;
        }
        blk = arena_cur;
        arena_cur += classes[k];
    }
    pthread_mutex_unlock(&heap_lock);
    g_wr32(blk, size);
    g_wr32(blk + 4, (uint32_t)k);
    g_wr32(blk + 8, MAGIC);
    if (zero) memset(G2H(blk + HDR), 0, size);
    return blk + HDR;
}

static int header_ok(uint32_t p)
{
    return p >= HDR && g_rd32(p - HDR + 8) == MAGIC;
}

void rt_heap_free(uint32_t p)
{
    if (!p) return;
    if (!header_ok(p)) {
        rt_log("[heap] free of bad pointer %08x\n", (unsigned)p);
        return;
    }
    uint32_t blk = p - HDR, k = g_rd32(blk + 4);
    g_wr32(blk + 8, 0);
    if (k == BIG) {
        rt_vm_free(blk);
        return;
    }
    pthread_mutex_lock(&heap_lock);
    g_wr32(p, free_list[k]);
    free_list[k] = blk;
    pthread_mutex_unlock(&heap_lock);
}

uint32_t rt_heap_size(uint32_t p)
{
    return header_ok(p) ? g_rd32(p - HDR) : 0xffffffffu;
}

uint32_t rt_heap_realloc(uint32_t p, uint32_t size, int zero)
{
    if (!p) return rt_heap_alloc(size, zero);
    if (!header_ok(p)) return 0;
    uint32_t blk = p - HDR, old = g_rd32(blk), k = g_rd32(blk + 4);
    uint32_t cap = k == BIG ? 0 : classes[k] - HDR;
    if (k != BIG && size <= cap) {
        if (zero && size > old) memset(G2H(p + old), 0, size - old);
        g_wr32(blk, size);
        return p;
    }
    uint32_t q = rt_heap_alloc(size, 0);
    if (!q) return 0;
    memcpy(G2H(q), G2H(p), old < size ? old : size);
    if (zero && size > old) memset(G2H(q + old), 0, size - old);
    rt_heap_free(p);
    return q;
}

uint32_t rt_heap_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t p = rt_heap_alloc((uint32_t)n, 0);
    memcpy(G2H(p), s, n);
    return p;
}

uint32_t rt_heap_wstrdup(const char *utf8)
{
    size_t n = strlen(utf8) + 1;
    uint32_t p = rt_heap_alloc((uint32_t)(n * 2), 0);
    rt_u2w(utf8, p, n);
    return p;
}

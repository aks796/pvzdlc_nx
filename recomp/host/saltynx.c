/* saltynx.c: SaltyNX support for this homebrew program: Status Monitor Overlay (and its forks, such as Horizon OC
 * Monitor) shows its FPS and resolution.
 *
 * A C port of NFSMW-NX's SaltyNX template (StevensND/nfsmw-nx, extras/saltynx-template/switch_saltynx.cpp,
 * GPL-3.0; see docs/console-overlays.md there), with its ReverseNX-RT part left out (this port draws the same
 * docked and handheld, so a mode switch would change nothing).
 *
 * The overlays don't measure the FPS themselves: they read it from a shared page SaltyNX hands out, filled in by
 * the NX-FPS plugin it loads into games. SaltyNX never loads into a homebrew program, so the program writes the
 * block itself: SaltyNX's struct NxFpsSharedBlock (174 packed bytes, magic 0x465053), which the overlay finds by
 * scanning the page 4 bytes at a time.
 *  - The page comes from the "SaltySD" port (or "InjectServ"): command 7 gives its handle, command 6 reserves
 *    room in it (only when there is no block yet: the page isn't reset for homebrew, and reserving on every
 *    launch would fill it), command 0 ends the session. Each port takes one session at a time, so ours is
 *    closed right after.
 *  - A forwarder's process may have only one session, which libnx uses for "sm:" (0x10801): the session limit
 *    is raised first (SVC 0x7E, when the loader allows it), else "sm:" is let go for a moment.
 *  - Every present: plugin_active (the overlay clears it and waits 100 ms for it to come back), the frame-time
 *    ring ticks[10] (its average; all zero shows "inf"), the frame number and the resolution (render[0] /
 *    viewport[0]; their call count must not be 0xFFFF, the overlay's question mark).
 *  - Once a second: the frame rate and its 10 s average, the block checked (and found again if lost, so the
 *    overlay can be opened at any time).
 * Messages go to the debug log, as "[saltynx] ...". */
#include <math.h>
#include <string.h>
#include <switch.h>

#include "../runtime/rt.h"
#include "saltynx.h"

#define MAGIC_FPS 0x465053u
#define SHARED_SIZE 0x1000u
#define API_NVN 1 /* deko3d is NVN's level */

typedef struct __attribute__((packed)) {
    uint16_t width, height, calls;
} ResCalls;

/* SaltyNX's struct NxFpsSharedBlock */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t fps;
    float fps_average;
    uint8_t plugin_active;
    uint8_t fps_locked, fps_mode, zero_sync, patch_applied, api;
    uint32_t ticks[10];
    uint8_t buffers, set_buffers, active_buffers, set_active_buffers, display_sync;
    ResCalls render[8], viewport[8];
    uint8_t force_original_refresh_rate, dont_force_60_in_docked, force_suspend, current_refresh_rate;
    float read_speed_per_second;
    uint8_t fps_locked_docked;
    uint64_t frame_number;
    int8_t expected_set_buffers;
} FpsBlock;
_Static_assert(sizeof(FpsBlock) == 174, "the overlay expects 174 bytes");

static SharedMemory shm;
static int mapped, attempts, logged_ok, logged_fail;
static uint8_t *base;
static FpsBlock *block;
static uint8_t last_fps;
static float last_avg;
static uint16_t last_w, last_h;
static uint64_t frames, prev_tick;
static unsigned tick_pos;

static Result reserve(Service *s, uint64_t size, uint64_t *off) { return serviceDispatchInOut(s, 6, size, *off, .in_send_pid = true); }
static Result memory_handle(Service *s, Handle *out)
{
    return serviceDispatch(s, 7, .in_send_pid = true, .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = out);
}
static Result end_session(Service *s)
{
    const uint64_t zero = 0;
    return serviceDispatchIn(s, 0, zero, .in_send_pid = true);
}

static FpsBlock *find_block(void)
{
    if (!base) return NULL;
    for (uint32_t off = 0; off + 4 <= SHARED_SIZE; off += 4) {
        uint32_t v;
        memcpy(&v, base + off, 4);
        if (v == MAGIC_FPS) return (FpsBlock *)(base + off);
    }
    return NULL;
}

static int valid(const FpsBlock *b) { return b && b->magic == MAGIC_FPS; }

/* everything the overlay's FPS and RES rows need, right away */
static void seed(FpsBlock *b)
{
    b->plugin_active = 1;
    b->api = API_NVN;
    b->fps = last_fps;
    b->fps_average = last_avg;
    b->frame_number = frames;
    if (last_w && last_h) {
        ResCalls r = {last_w, last_h, last_fps ? last_fps : 1};
        b->render[0] = r;
        b->viewport[0] = r;
    }
    if (last_fps && b->ticks[0] == 0) /* (by index: ticks[] sits at an odd offset in the packed block) */
        for (int i = 0; i < 10; i++) b->ticks[i] = (uint32_t)(armGetSystemTickFreq() / last_fps);
}

static Handle resource_limit(void)
{
    uint64_t raw = 0;
    if (R_SUCCEEDED(svcGetInfo(&raw, InfoType_ResourceLimit, INVALID_HANDLE, 0))) return (Handle)raw;
    if (R_SUCCEEDED(svcGetInfo(&raw, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) return (Handle)raw;
    return INVALID_HANDLE;
}
static int session_room(Handle limit)
{
    s64 used = 0, max = 0;
    if (R_FAILED(svcGetResourceLimitCurrentValue(&used, limit, LimitableResource_Sessions)) ||
        R_FAILED(svcGetResourceLimitLimitValue(&max, limit, LimitableResource_Sessions)))
        return 1;
    return used < max;
}
/* room for one more session: the limit raised (once, for good), else "sm:" let go (*sm_off: bring it back) */
static int make_room(int *sm_off, int allow_sm)
{
    *sm_off = 0;
    Handle limit = resource_limit();
    if (limit == INVALID_HANDLE) return 1;
    int room = session_room(limit);
    if (!room && envIsSyscallHinted(0x7E)) {
        s64 max = 0;
        svcGetResourceLimitLimitValue(&max, limit, LimitableResource_Sessions);
        Result rc = svcSetResourceLimitLimitValue(limit, LimitableResource_Sessions, (u64)(max + 4));
        rt_log("[saltynx] session limit %lld -> %lld: %x\n", (long long)max, (long long)(max + 4), (unsigned)rc);
        room = session_room(limit);
    }
    if (!room && allow_sm) {
        smExit();
        if (session_room(limit)) {
            *sm_off = 1;
            room = 1;
        } else {
            smInitialize();
        }
    }
    svcCloseHandle(limit);
    return room;
}

void saltynx_init(void)
{
    if (valid(block)) return;
    block = NULL;
    if (mapped && (block = find_block()) != NULL) {
        seed(block);
        return;
    }
    int sm_off = 0;
    int room = make_room(&sm_off, attempts >= 3 && (attempts < 15 || attempts % 60 == 0));
    int passes = sm_off ? 1 : attempts == 0 ? 20 : 2, done = 0;
    uint64_t off = 0;
    int reserved = 0;
    Result rc_port = 0;
    static const char *const ports[2] = {"SaltySD", "InjectServ"};
    for (int i = 0; i < passes && !done && room; i++) {
        for (int p = 0; p < 2 && !done; p++) {
            Handle h = INVALID_HANDLE;
            rc_port = svcConnectToNamedPort(&h, ports[p]);
            if (R_FAILED(rc_port)) continue;
            Service s;
            memset(&s, 0, sizeof s);
            s.session = h;
            if (!mapped) {
                Handle mem = INVALID_HANDLE;
                if (R_SUCCEEDED(memory_handle(&s, &mem))) {
                    shmemLoadRemote(&shm, mem, SHARED_SIZE, Perm_Rw);
                    if (R_SUCCEEDED(shmemMap(&shm))) {
                        base = shmemGetAddr(&shm);
                        mapped = 1;
                    }
                }
            }
            if (mapped) {
                done = 1;
                if (!find_block()) reserved = R_SUCCEEDED(reserve(&s, sizeof(FpsBlock), &off));
                if (!logged_ok) rt_log("[saltynx] connected through %s\n", ports[p]);
            }
            end_session(&s);
            svcCloseHandle(h); /* (the port takes one session: the overlay needs it too) */
        }
        if (!done) svcSleepThread(10000000);
    }
    if (sm_off) smInitialize();
    if (!mapped) {
        attempts++;
        if (!logged_fail && attempts >= 5) {
            logged_fail = 1;
            rt_log("[saltynx] no SaltyNX (last error %x; f201 = not installed, 10801 = no free session)\n", (unsigned)rc_port);
        }
        return;
    }
    block = find_block();
    if (!block && reserved) {
        block = (FpsBlock *)(base + off);
        memset(block, 0, sizeof *block);
        block->magic = MAGIC_FPS;
    }
    if (block) {
        seed(block);
        attempts = 0;
        if (!logged_ok) {
            logged_ok = 1;
            rt_log("[saltynx] FPS block at offset %x\n", (unsigned)((uint8_t *)block - base));
        }
    } else {
        attempts++;
    }
}

void saltynx_present(uint32_t width, uint32_t height)
{
    static uint64_t second_start;
    static uint32_t in_second;
    static float history[10];
    static int n_hist;
    uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq();
    if (width && height) last_w = (uint16_t)width, last_h = (uint16_t)height;
    in_second++;
    if (!second_start) second_start = now;
    if (now - second_start >= freq) {
        /* once a second: the rate, its 10 s average, and the block found again if it went */
        float fps = (float)in_second * (float)freq / (float)(now - second_start);
        history[n_hist++ % 10] = fps;
        float sum = 0;
        int n = n_hist < 10 ? n_hist : 10;
        for (int i = 0; i < n; i++) sum += history[i];
        last_fps = (uint8_t)fminf(255.f, fps + 0.5f);
        last_avg = sum / (float)n;
        in_second = 0;
        second_start = now;
        if (!valid(block)) saltynx_init();
        else seed(block);
    }
    FpsBlock *b = block;
    if (!valid(b)) return;
    b->plugin_active = 1;
    b->api = API_NVN;
    static const FpsBlock *seen;
    if (seen != b) seen = b, prev_tick = 0; /* (a block found just now: no frame time from before it) */
    if (prev_tick) {
        uint64_t d = now - prev_tick;
        b->ticks[tick_pos] = (uint32_t)(d > 0xffffffffull ? 0xffffffffull : d);
        tick_pos = (tick_pos + 1) % 10;
        frames++;
        b->frame_number = frames;
    }
    prev_tick = now;
    if (last_w && last_h) {
        ResCalls r = {last_w, last_h, b->fps ? b->fps : 1};
        b->render[0] = r;
        b->viewport[0] = r;
    }
}

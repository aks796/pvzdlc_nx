/* rt_eh.c: Win32 structured exceptions for recompiled code.
 *
 * The game's own (recompiled) MSVC runtime does the C++ work: _CxxThrowException
 * calls RaiseException, which walks the fs:[0] registration chain and calls
 * each handler (__CxxFrameHandler3, _except_handler4). A handler that finds a
 * catch unwinds the frames in between with RtlUnwind, runs the catch funclet
 * and then jumps to the continuation address with ebp/esp set to the parent
 * frame. That jump arrives in rt_call as a target that is not a function:
 * rt_resume finds the live parent function's resume point (lift.py emits one
 * in every function that owns a continuation) and longjmps there. */
#include <stdio.h>
#include <string.h>

#include "rt.h"

#define EH_NONCONTINUABLE 0x1u
#define EH_UNWINDING 0x2u
#define EH_EXIT_UNWIND 0x4u
#define ExceptionContinueExecution 0u
#define ExceptionContinueSearch 1u
#define CHAIN_END 0xffffffffu

/* Is fva one of the functions that owns continuation va? */
static int owns(uint32_t va, uint32_t fva)
{
    uint32_t lo = 0, hi = rt_conts_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rt_conts[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    for (uint32_t i = lo; i < rt_conts_count && rt_conts[i].va == va; i++)
        if (rt_conts[i].func == fva) return 1;
    return 0;
}

void rt_resume(CPU *c, uint32_t va)
{
    /* the innermost live function that owns va (a continuation can belong to a parent and its catch
     * funclets: whichever is still running resumes); of several live frames of it (recursion), the one
     * whose ebp is the target's: an EBP frame has ebp = (esp at entry) - 4 after push ebp */
    RtJmp *best = NULL;
    for (RtJmp *j = c->rj_top; j; j = j->prev) {
        if (!owns(va, j->func)) continue;
        if (best && j->func != best->func) break;
        if (c->ebp == j->esp0 - 4) {
            best = j;
            break;
        }
        if (!best) best = j;
    }
    if (!best) return; /* caller traps */
    if (rt_verbose) rt_log("[eh] resuming at %08x in %08x\n", (unsigned)va, (unsigned)best->func);
    c->resume_va = va;
    __builtin_longjmp(best->buf, 1);
}

/* EXCEPTION_RECORD (80 bytes) + a small CONTEXT, built on the guest stack */
static uint32_t make_record(CPU *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args, uint32_t addr,
                            uint32_t *ctx_out)
{
    c->esp -= 0x400;
    uint32_t rec = c->esp, ctx = rec + 0x60;
    memset(G2H(rec), 0, 0x400);
    g_wr32(rec, code);
    g_wr32(rec + 4, flags);
    g_wr32(rec + 12, addr);
    if (nargs > 15) nargs = 15;
    g_wr32(rec + 16, nargs);
    for (uint32_t i = 0; i < nargs; i++) g_wr32(rec + 20 + 4 * i, g_rd32(args + 4 * i));
    /* CONTEXT: ContextFlags, then integer regs at their x86 offsets */
    g_wr32(ctx, 0x10007);
    g_wr32(ctx + 0x9c, c->edi);
    g_wr32(ctx + 0xa0, c->esi);
    g_wr32(ctx + 0xa4, c->ebx);
    g_wr32(ctx + 0xa8, c->edx);
    g_wr32(ctx + 0xac, c->ecx);
    g_wr32(ctx + 0xb0, c->eax);
    g_wr32(ctx + 0xb4, c->ebp);
    g_wr32(ctx + 0xb8, addr);
    g_wr32(ctx + 0xc4, c->esp);
    *ctx_out = ctx;
    return rec;
}

static void describe(uint32_t rec)
{
    uint32_t code = g_rd32(rec);
    rt_log("[eh] exception %08x at %08x", (unsigned)code, (unsigned)g_rd32(rec + 12));
    if (code == 0xE06D7363u && g_rd32(rec + 16) >= 3) {
        /* MSVC C++: [magic, object, ThrowInfo]; show std::exception::what() if the type looks like one */
        uint32_t obj = g_rd32(rec + 24), ti = g_rd32(rec + 28);
        rt_log(" (C++ object %08x, ThrowInfo %08x)", (unsigned)obj, (unsigned)ti);
        if (obj) {
            uint32_t what = g_rd32(obj + 4);
            if (what > 0x10000 && what < RT_HLE_BASE) {
                char s[200];
                rt_a2u(what, s, sizeof s);
                int ok = s[0] != 0;
                for (char *p = s; *p; p++) if ((unsigned char)*p < 32) ok = 0;
                if (ok) rt_log(" what=\"%s\"", s);
            }
        }
    }
    rt_log("\n");
}

static uint32_t call_handler(CPU *c, uint32_t frame, uint32_t rec, uint32_t ctx)
{
    uint32_t handler = g_rd32(frame + 4);
    uint32_t disp = c->esp - 16;
    uint32_t args[4] = {rec, frame, ctx, disp};
    return rt_guest_call(c, handler, 4, args);
}

HLE(RaiseException)
{
    uint32_t code = ARG(0), flags = ARG(1), nargs = ARG(2), args = ARG(3), ret = g_rd32(c->esp);
    uint32_t save_esp = c->esp, ctx;
    uint32_t rec = make_record(c, code, flags & EH_NONCONTINUABLE, nargs, args, ret, &ctx);
    describe(rec);
    uint32_t teb = c->fs_base;
    for (uint32_t frame = g_rd32(teb); frame != CHAIN_END && frame; frame = g_rd32(frame)) {
        if (frame < c->esp || frame >= ((RtThread *)c->host)->stack_hi) {
            rt_log("[eh] bad registration %08x\n", (unsigned)frame);
            break;
        }
        uint32_t r = call_handler(c, frame, rec, ctx);
        if (r == ExceptionContinueExecution) {
            if (flags & EH_NONCONTINUABLE) rt_fatal("handler continued a non-continuable exception");
            c->esp = save_esp;
            RET(4, 0);
        }
    }
    rt_log("[eh] nobody caught it\n");
    describe(rec);
    rt_fatal("unhandled exception %08x", (unsigned)code);
}

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): call every
 * handler above TargetFrame with EH_UNWINDING, unlinking each, then return
 * (the CRT's TargetIp is always the instruction after the call). */
HLE(RtlUnwind)
{
    uint32_t target = ARG(0), rec_in = ARG(2), retval = ARG(3);
    uint32_t save_esp = c->esp, ctx;
    uint32_t rec;
    if (rec_in) {
        rec = rec_in;
        make_record(c, 0, 0, 0, 0, g_rd32(save_esp), &ctx);
    } else {
        rec = make_record(c, 0xC0000027u, 0, 0, 0, g_rd32(save_esp), &ctx); /* STATUS_UNWIND */
    }
    g_wr32(rec + 4, g_rd32(rec + 4) | EH_UNWINDING | (target ? 0 : EH_EXIT_UNWIND));
    uint32_t teb = c->fs_base;
    for (uint32_t frame = g_rd32(teb); frame != CHAIN_END && frame && frame != target; frame = g_rd32(teb)) {
        call_handler(c, frame, rec, ctx);
        g_wr32(teb, g_rd32(frame)); /* unlink */
    }
    c->esp = save_esp;
    RET(4, retval);
}

const HleEntry hle_seh[] = {
    {"kernel32.dll", "RaiseException", RaiseException},
    {"kernel32.dll", "RtlUnwind", RtlUnwind},
    {"ntdll.dll", "RtlUnwind", RtlUnwind},
    {NULL, NULL, NULL}};

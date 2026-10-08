/*
 * cdl-flow.c - shadow call stack for the CDL recorder
 *
 * Every call (JSR / JSL / JSR (a,X)) and interrupt pushes a frame with the
 * address it should return to and S after the push. A return (RTS / RTL / RTI)
 * is matched to a frame by S, not by address, so the outcome says what the
 * code really did:
 *   - S matches the top frame          normal return (or a modified return address)
 *   - S matches a deeper frame         the frames above were dropped (PLA:PLA, longjmp)
 *   - S below every frame              return through an address the code pushed
 *                                      itself (PEA/PHA + RTS dispatch): an edge
 *                                      FLOW_RETURN from the RTS to its target
 * One record per (function entry, return instruction, entry M/X, exit M/X)
 * gives snesrecomp its exit widths; dropped frames name the call site.
 *
 * Exports:
 *   cdlDrainRets() -> count, records [entry, retPc, mx << 8 | flags] at cdlOutPtr()
 *     mx: bit0 entry M8, bit1 entry X8, bit2 exit M8, bit3 exit X8
 *     flags: RET_NORMAL, RET_MODIFIED, RET_DROPPED (retPc = call site), RET_INTERRUPT
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"
#include "cdl-optable.h"
#include "cdl-table.h"

#if EVS_CDL

#define FRAMES_MAX 128
#define NO_RETURN  0xFFFFFFFFu

typedef struct {
    uint32_t entry;     /* callee bus address */
    uint32_t ret;       /* expected return target, NO_RETURN for interrupts */
    uint32_t site;      /* call instruction, 0 for interrupts */
    uint16_t s;         /* S after the push */
    uint8_t  mx;        /* entry M8 | X8 << 1 */
    uint8_t  interrupt;
} Frame;

static Frame   frames[FRAMES_MAX];
static int     depth;
static Table   rets;
static uint8_t prevMx;

bool CDL_FlowAlloc(void)
{
    depth = 0;
    return tableAlloc(&rets, 1 << 12, false);
}

void CDL_FlowFree(void)
{
    tableFree(&rets);
    depth = 0;
}

static void push(uint32_t entry, uint32_t ret, uint32_t site, uint16_t s, uint8_t mx, bool interrupt)
{
    if (depth == FRAMES_MAX) {                       /* runaway: keep the newest half */
        memmove(frames, frames + FRAMES_MAX / 2, sizeof(Frame) * (FRAMES_MAX / 2));
        depth = FRAMES_MAX / 2;
    }
    frames[depth].entry = entry;
    frames[depth].ret = ret;
    frames[depth].site = site;
    frames[depth].s = s;
    frames[depth].mx = mx;
    frames[depth].interrupt = interrupt;
    depth++;
}

static void record(uint32_t entry, uint32_t retPc, uint8_t mx, uint8_t flags)
{
    tableOr(&rets, (1ULL << 63) | ((uint64_t)(mx & 0x0F) << 48) | ((uint64_t)(entry & 0xFFFFFF) << 24) | (retPc & 0xFFFFFF), flags);
}

static uint32_t callLength(uint8_t op)
{
    return op == 0x22 ? 4 : 3;   /* JSL long, JSR abs / JSR (abs,X) */
}

/* Called for every instruction before cdl.prev* are updated. Returns CDL bits for pc. */
uint8_t CDL_FlowExec(uint32_t pc, uint16_t s, bool m8, bool x8)
{
    uint8_t mx = (uint8_t)((m8 ? 1 : 0) | (x8 ? 2 : 0));
    uint8_t mark = 0;

    if (cdl.inInterrupt) {
        push(pc, NO_RETURN, 0, s, mx, true);
    } else if (cdl.havePrev && (CDL_OpFlow[cdl.prevOp] & 1)) {
        uint32_t ret = (cdl.prevPC & 0xFF0000) | ((cdl.prevPC + callLength(cdl.prevOp)) & 0xFFFF);
        push(pc, ret, cdl.prevPC, s, mx, false);
    } else if (cdl.havePrev && (CDL_OpInfo[cdl.prevOp] & CDL_OP_RET)) {
        uint8_t  op = cdl.prevOp;
        uint16_t pulled = op == 0x60 ? 2 : op == 0x6B ? 3 : (CheckEmulation() ? 3 : 4);
        uint16_t before = (uint16_t)(s - pulled);
        int i;
        for (i = depth - 1; i >= 0 && frames[i].s != before; i--) {}
        if (i < 0) {
            if (depth == 0 || before < frames[depth - 1].s) {
                CDL_RecordEdge(cdl.prevPC, pc, FLOW_RETURN);
                mark = CDL_JUMP_TARGET;
            } else {
                depth = 0;                           /* stack was switched */
            }
        } else {
            Frame* f = &frames[i];
            int j;
            uint8_t flags = f->interrupt ? RET_INTERRUPT : (f->ret == pc ? RET_NORMAL : RET_MODIFIED);
            for (j = depth - 1; j > i; j--) {
                if (!frames[j].interrupt) record(frames[j].entry, frames[j].site, frames[j].mx, RET_DROPPED);
            }
            record(f->entry, cdl.prevPC, (uint8_t)(f->mx | (prevMx << 2)), flags);
            if (flags == RET_MODIFIED) {
                CDL_RecordEdge(cdl.prevPC, pc, FLOW_RETURN);
                mark = CDL_JUMP_TARGET;
            }
            depth = i;
        }
    }
    prevMx = mx;
    return mark;
}

EMSCRIPTEN_KEEPALIVE
uint32_t cdlDrainRets(void)
{
    uint32_t n, d;
    uint32_t* out;
    if (!rets.keys) return 0;
    out = CDL_Out(rets.dcount * 3 + 1);
    if (!out) return 0;
    for (n = 0, d = 0; d < rets.dcount; d++, n++) {
        uint32_t i = rets.dlist[d];
        uint64_t k = rets.keys[i];
        rets.dirty[i] = 0;
        out[n * 3]     = (uint32_t)(k >> 24) & 0xFFFFFF;
        out[n * 3 + 1] = (uint32_t)k & 0xFFFFFF;
        out[n * 3 + 2] = (uint32_t)(((k >> 48) & 0x0F) << 8) | rets.vals[i];
    }
    rets.dcount = 0;
    return n;
}

#endif /* EVS_CDL */

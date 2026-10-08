/*
 * cdl-wram.c - WRAM access map + script attribution for the CDL recorder
 *
 * wflags: 1 byte per WRAM byte (read / write / byte / word / executed / touched
 * by a script / read as a jump pointer); merges with OR like rom.cdl.
 *
 * Script context: the host names the ROM offset of the script interpreter's
 * opcode fetch and the WRAM address of its bytecode pointer. When that fetch
 * runs, the pointer (= address of the script instruction) becomes the context;
 * WRAM accesses are attributed to it until the dispatcher's stack frame is left.
 * Interrupts suspend the context; excluded ranges (scratch, the slot table)
 * are never attributed.
 *
 * Code executed from WRAM: its bytes are kept in wcode (first bytes seen) and
 * wcodeState marks WCODE_SEEN / WCODE_CHANGED (executed again with other bytes:
 * self-modifying or reused buffer), so the export can emit snesrecomp ram_routines.
 *
 * Exports:
 *   cdlWcodePtr() / cdlWcodeStatePtr() / cdlWcodeDirtyPtr()   uint8[32] per 4 KB
 *   cdlSetScriptContext(fetchRomOff, ptrWram)   (-1 disables)
 *   cdlClearScriptExcludes() / cdlAddScriptExclude(lo, hi)   WRAM offsets, inclusive
 *   cdlWflagPtr() / cdlWflagFlushDirtyPtr() / cdlWflagViewDirtyPtr()   uint8[32] per 4 KB
 *   cdlDrainScriptXrefs() -> count, records [scriptAddr, wramAddr, flags] at cdlOutPtr()
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"
#include "cdl-table.h"

#if EVS_CDL

#define WRAM_SIZE    0x20000
#define WFLAG_CHUNKS 32
#define EXCLUDE_MAX  8

#define WCODE_SEEN    0x01
#define WCODE_CHANGED 0x02

static uint8_t* wflags;
static uint8_t* wcode;
static uint8_t* wcodeState;
static uint8_t  wcodeDirty[WFLAG_CHUNKS];
static uint8_t  wflagFlushDirty[WFLAG_CHUNKS];
static uint8_t  wflagViewDirty[WFLAG_CHUNKS];
static Table    scriptXrefs;

static int32_t  ctxFetchOff = -1;
static uint32_t ctxPtrAddr;
static uint32_t exclLo[EXCLUDE_MAX], exclHi[EXCLUDE_MAX];
static int      exclCount;
static bool     ctxActive, ctxSuspended;
static uint32_t ctxAddr;
static uint16_t ctxS, suspendS;

static inline void markW(uint32_t a, uint8_t f)
{
    if (a >= WRAM_SIZE) return;
    if ((wflags[a] | f) != wflags[a]) {
        wflags[a] |= f;
        wflagFlushDirty[a >> 12] = 1;
        wflagViewDirty[a >> 12]  = 1;
    }
}

void CDL_WramMarkRange(uint32_t addr, int32_t count, uint8_t wf)
{
    int32_t i;
    if (!wflags) return;
    for (i = 0; i < count; i++) markW((addr + (uint32_t)i) & (WRAM_SIZE - 1), wf);
}

bool CDL_WramAlloc(void)
{
    wflags = calloc(WRAM_SIZE, 1);
    wcode = calloc(WRAM_SIZE, 1);
    wcodeState = calloc(WRAM_SIZE, 1);
    memset(wcodeDirty, 0, sizeof(wcodeDirty));
    memset(wflagFlushDirty, 0, sizeof(wflagFlushDirty));
    memset(wflagViewDirty, 1, sizeof(wflagViewDirty));
    ctxActive = ctxSuspended = false;
    return wflags && wcode && wcodeState && tableAlloc(&scriptXrefs, 1 << 12, false);
}

void CDL_WramFree(void)
{
    free(wflags); free(wcode); free(wcodeState);
    wflags = wcode = wcodeState = NULL;
    tableFree(&scriptXrefs);
    ctxActive = false;
}

void CDL_WramExec(int32_t romOff, const uint8_t* p, uint32_t len)
{
    uint16_t s = ICPU.Registers.S.W;
    uint32_t i;

    if (ctxActive) {
        if (cdl.inInterrupt && !ctxSuspended) { ctxSuspended = true; suspendS = (uint16_t)(s + 1); }
        else if (ctxSuspended && s >= suspendS) ctxSuspended = false;
    }
    if (romOff >= 0 && romOff == ctxFetchOff) {
        const uint8_t* r = Memory.RAM + ctxPtrAddr;
        ctxAddr = r[0] | (r[1] << 8) | (r[2] << 16);
        ctxActive = true;
        ctxSuspended = false;
        ctxS = s;
    } else if (ctxActive && !ctxSuspended && s > ctxS) {
        ctxActive = false;
    }

    if (romOff < 0 && p >= Memory.RAM && p < Memory.RAM + WRAM_SIZE) {
        uint32_t a = (uint32_t)(p - Memory.RAM);
        for (i = 0; i < len && a + i < WRAM_SIZE; i++) {
            uint32_t w = a + i;
            uint8_t  b = p[i];
            markW(w, WF_EXEC);
            if (!(wcodeState[w] & WCODE_SEEN)) { wcode[w] = b; wcodeState[w] = WCODE_SEEN; wcodeDirty[w >> 12] = 1; }
            else if (wcode[w] != b && !(wcodeState[w] & WCODE_CHANGED)) { wcodeState[w] |= WCODE_CHANGED; wcodeDirty[w >> 12] = 1; }
        }
    }
}

static bool scriptExcluded(uint32_t addr)
{
    int i;
    for (i = 0; i < exclCount; i++) if (addr >= exclLo[i] && addr <= exclHi[i]) return true;
    /* the script's own bytecode when it lives in WRAM */
    if ((ctxAddr >> 16) == 0x7E || (ctxAddr >> 16) == 0x7F) {
        uint32_t code = ctxAddr - 0x7E0000;
        if (addr >= code && addr < code + 32) return true;
    }
    return false;
}

void CDL_WramAccess(uint32_t addr, uint8_t flags, uint32_t width)
{
    uint8_t f = ((flags & XR_READ) ? WF_READ : 0) | ((flags & XR_WRITE) ? WF_WRITE : 0)
              | (width == 2 ? WF_WORD : WF_BYTE) | ((flags & XR_POINTER) ? WF_POINTER : 0);
    bool script = ctxActive && !ctxSuspended && !scriptExcluded(addr);
    if (!wflags) return;
    if (script) f |= WF_SCRIPT;
    markW(addr, f);
    if (width == 2) markW(addr + 1, f);
    if (script)
        tableOr(&scriptXrefs, (1ULL << 63) | ((uint64_t)ctxAddr << 32) | addr,
                flags & (XR_READ | XR_WRITE | XR_BYTE | XR_WORD));
}

EMSCRIPTEN_KEEPALIVE
void cdlSetScriptContext(int32_t fetchRomOff, uint32_t ptrWram)
{
    ctxFetchOff = (ptrWram + 3 <= WRAM_SIZE) ? fetchRomOff : -1;
    ctxPtrAddr  = ptrWram;
    ctxActive   = false;
}

EMSCRIPTEN_KEEPALIVE void cdlClearScriptExcludes(void) { exclCount = 0; }

EMSCRIPTEN_KEEPALIVE
void cdlAddScriptExclude(uint32_t lo, uint32_t hi)
{
    if (exclCount >= EXCLUDE_MAX) return;
    exclLo[exclCount] = lo;
    exclHi[exclCount] = hi;
    exclCount++;
}

EMSCRIPTEN_KEEPALIVE uint8_t* cdlWflagPtr(void)           { return wflags; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWcodePtr(void)           { return wcode; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWcodeStatePtr(void)      { return wcodeState; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWcodeDirtyPtr(void)      { return wcodeDirty; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWflagFlushDirtyPtr(void) { return wflagFlushDirty; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWflagViewDirtyPtr(void)  { return wflagViewDirty; }

EMSCRIPTEN_KEEPALIVE
uint32_t cdlDrainScriptXrefs(void)
{
    uint32_t n, d;
    uint32_t* out;
    if (!scriptXrefs.keys) return 0;
    out = CDL_Out(scriptXrefs.dcount * 3 + 1);
    if (!out) return 0;
    for (n = 0, d = 0; d < scriptXrefs.dcount; d++, n++) {
        uint32_t i = scriptXrefs.dlist[d];
        uint64_t k = scriptXrefs.keys[i];
        scriptXrefs.dirty[i] = 0;
        out[n * 3]     = (uint32_t)(k >> 32) & 0xFFFFFF;
        out[n * 3 + 1] = (uint32_t)k & 0x1FFFF;
        out[n * 3 + 2] = scriptXrefs.vals[i];
    }
    scriptXrefs.dcount = 0;
    return n;
}

#endif /* EVS_CDL */

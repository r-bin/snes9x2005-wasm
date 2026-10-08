/*
 * cdl.c - Code/Data Logger + cross-reference recorder (see cdl.h)
 *
 * Exported Module API:
 *   cdlEnable() -> 1 ok / 0 no ROM     cdlDisable()      cdlIsEnabled()
 *   cdlRomSize()   cdlRomPtr()   cdlExtPtr()   cdlWvalPtr()
 *   cdlFlushDirtyPtr() / cdlViewDirtyPtr()  : uint8[128], one per 64 KB ROM chunk
 *   cdlWvalDirtyPtr()                       : uint8[512], one per 256 WRAM bytes
 *   (WRAM access map and script attribution: cdl-wram.c; hit counters: cdl-count.c)
 *   cdlDrainXrefs() / cdlDrainEdges() / cdlDrainStats() -> record count, records at cdlOutPtr()
 *     xref  : [pc, space<<24 | addr, flags]
 *     edge  : [from, to, kind]
 *     stats : [space<<24 | pc, count, min, max, flags]
 * Nothing here touches the disk; the host merges drained deltas.
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"
#include "cdl-optable.h"
#include "cdl-table.h"

#if EVS_CDL

#define WRAM_SIZE      0x20000
#define WVAL_BYTES     32
#define ROM_CHUNKS     128
#define WVAL_CHUNKS    512
#define XREFS_PER_PC   128
#define RECENT_SIZE    8192

CDLState cdl;

static uint8_t* romCdl;
static uint8_t* romExt;
static uint8_t* wvals;
static uint32_t romSize;
static uint8_t  flushDirty[ROM_CHUNKS];
static uint8_t  viewDirty[ROM_CHUNKS];
static uint8_t  wvalDirty[WVAL_CHUNKS];

/* ------------------------------------------------------------------ */
/* Recording                                                          */
/* ------------------------------------------------------------------ */

static Table xrefs, edges, stats;

static uint64_t recentKey[RECENT_SIZE];
static uint8_t  recentVal[RECENT_SIZE];

static inline void markRom(uint32_t off, uint8_t c, uint8_t e)
{
    if (off >= romSize) return;
    if ((romCdl[off] | c) != romCdl[off] || (romExt[off] | e) != romExt[off]) {
        romCdl[off] |= c;
        romExt[off] |= e;
        flushDirty[off >> 16] = 1;
        viewDirty[off >> 16]  = 1;
    }
}

static inline int32_t romOffset(const uint8_t* p)
{
    if (!romCdl || p < Memory.ROM || p >= Memory.ROM + romSize) return -1;
    return (int32_t)(p - Memory.ROM);
}

static void recordEdge(uint32_t from, uint32_t to, uint8_t kind)
{
    tableOr(&edges, (1ULL << 63) | ((uint64_t)from << 24) | to, kind);
}

void CDL_Exec(void)
{
    uint8_t* p   = CPU.PCAtOpcodeStart;
    uint32_t pc  = ICPU.ShiftedPB | (uint32_t)(p - CPU.PCBase);
    uint8_t  op  = *p;
    bool     m8  = CheckMemory() || CheckEmulation();
    bool     x8  = CheckIndex()  || CheckEmulation();
    int32_t  off = romOffset(p);
    uint8_t  entry = 0;

    if (cdl.inInterrupt) {
        recordEdge(0, pc, FLOW_INTERRUPT);
        entry = CDL_SUB_ENTRY;
    } else if (cdl.havePrev && CDL_OpFlow[cdl.prevOp]) {
        uint8_t kind = CDL_OpFlow[cdl.prevOp];
        uint32_t next = (cdl.prevPC & 0xFF0000) | ((cdl.prevPC + 2) & 0xFFFF);
        if (!(kind & 4) || pc != next) {
            recordEdge(cdl.prevPC, pc, kind);
            entry = (kind & 1) ? CDL_SUB_ENTRY : CDL_JUMP_TARGET;
        }
    }

    uint8_t  info = CDL_OpInfo[op];
    uint32_t len  = 1 + (info & CDL_OP_LEN_MASK);
    if ((info & CDL_OP_M16) && !m8) len++;
    if ((info & CDL_OP_X16) && !x8) len++;
    CDL_WramExec(off, p, len);

    if (off >= 0) {
        uint32_t i;
        markRom((uint32_t)off, CDL_CODE | entry | (m8 ? CDL_ACC_8 : 0) | (x8 ? CDL_IDX_8 : 0),
                EXT_OPCODE_HEAD | (m8 ? 0 : EXT_SEEN_M16) | (x8 ? 0 : EXT_SEEN_X16));
        for (i = 1; i < len; i++) markRom((uint32_t)off + i, CDL_CODE, 0);
        CDL_CountRom((uint32_t)off);
        cdl.curLen = len;
    }
    cdl.curRomOff = off;

    cdl.inInterrupt = false;
    cdl.havePrev = true;
    cdl.prevOp = op;
    cdl.prevPC = pc;
    cdl.curOp  = op;
    cdl.curPC  = pc;
    cdl.seq++;
}

static void recordXref(uint32_t space, uint32_t addr, uint8_t flags)
{
    uint64_t key = (1ULL << 63) | ((uint64_t)cdl.curPC << 32) | (space << 24) | addr;
    uint32_t h = hash64(key) & (RECENT_SIZE - 1);
    uint64_t skey;
    uint32_t si, xi;

    if (recentKey[h] == key && (recentVal[h] | flags) == recentVal[h]) return;
    recentKey[h] = key;
    recentVal[h] |= flags;

    skey = (1ULL << 63) | ((uint64_t)space << 24) | cdl.curPC;
    si = tableSlot(&stats, skey, true);
    xi = tableSlot(&xrefs, key, false);
    if (xi == UINT32_MAX && si != UINT32_MAX && stats.count[si] < XREFS_PER_PC) {
        xi = tableSlot(&xrefs, key, true);
        if (xi != UINT32_MAX) stats.count[si]++;
    }
    if (xi != UINT32_MAX && (xrefs.vals[xi] | flags) != xrefs.vals[xi]) {
        xrefs.vals[xi] |= flags;
        markDirty(&xrefs, xi);
    }
    if (si != UINT32_MAX) {
        bool fresh = stats.vals[si] == 0;
        bool changed = fresh || (stats.vals[si] | flags) != stats.vals[si]
                    || addr < stats.lo[si] || addr > stats.hi[si];
        if (fresh) { stats.lo[si] = addr; stats.hi[si] = addr; }
        if (addr < stats.lo[si]) stats.lo[si] = addr;
        if (addr > stats.hi[si]) stats.hi[si] = addr;
        stats.vals[si] |= flags;
        if (changed) markDirty(&stats, si);
    }
}

static inline void seeValue(uint32_t off, uint8_t v)
{
    uint8_t* b = &wvals[off * WVAL_BYTES + (v >> 3)];
    uint8_t bit = (uint8_t)(1 << (v & 7));
    if (!(*b & bit)) { *b |= bit; wvalDirty[off >> 8] = 1; }
}

void CDL_Access(uint32_t address, uint8_t* block, uint8_t flags, uint16_t value)
{
    uint32_t space, addr;
    uint32_t width = (flags & XR_WORD) ? 2 : 1;
    if (CDL_OpInfo[cdl.curOp] & CDL_OP_IJUMP) flags |= XR_POINTER;

    if (block >= (uint8_t*) MAP_LAST) {
        uint8_t* p = block + (address & 0xffff);
        int32_t off = romOffset(p);
        if (p >= Memory.RAM && p < Memory.RAM + WRAM_SIZE) {
            space = SPACE_WRAM;
            addr  = (uint32_t)(p - Memory.RAM);
            if ((cdl.inInterrupt || (CDL_OpInfo[cdl.curOp] & CDL_OP_STACK)) && addr < 0x2000) {
                int32_t d = (int32_t)addr - (int32_t)ICPU.Registers.S.W;
                if (d >= -4 && d <= 4) return;
            }
            CDL_WramAccess(addr, flags, width);
            CDL_CountWram(addr, (flags & XR_WRITE) != 0);
            if ((flags & XR_WRITE) && wvals) {
                seeValue(addr, (uint8_t)value);
                if (width == 2 && addr + 1 < WRAM_SIZE) seeValue(addr + 1, (uint8_t)(value >> 8));
            }
        } else if (off >= 0) {
            /* some opcodes fetch their own operand through the bus: not data */
            if (off >= cdl.curRomOff && (uint32_t)(off - cdl.curRomOff) < cdl.curLen && cdl.curRomOff >= 0) return;
            space = SPACE_ROM;
            addr  = (uint32_t)off;
            if (flags & XR_READ) {
                uint8_t e = ((flags & XR_POINTER) ? EXT_POINTER : 0) | (width == 2 ? EXT_DATA_WORD : 0);
                markRom(addr, CDL_DATA, e);
                CDL_CountRom(addr);
                if (width == 2) markRom(addr + 1, CDL_DATA, e & EXT_POINTER);
                cdl.lastRomRead = addr + width - 1;
                cdl.lastRomReadSeq = cdl.seq;
            }
        } else {
            space = SPACE_BUS;
            addr  = address & 0xFFFFFF;
        }
    } else if ((intptr_t) block == MAP_PPU || (intptr_t) block == MAP_CPU || (intptr_t) block == MAP_DSP) {
        space = SPACE_IO;
        addr  = address & 0xFFFF;
        if ((flags & XR_WRITE) && addr >= 0x2140 && addr <= 0x2143 && cdl.seq - cdl.lastRomReadSeq <= 4)
            markRom(cdl.lastRomRead, 0, EXT_APU_SOURCE);
    } else {
        space = SPACE_BUS;
        addr  = address & 0xFFFFFF;
    }
    recordXref(space, addr, flags);
}

void CDL_Dma(uint8_t bAddress, uint32_t source, int32_t count)
{
    uint8_t* base;
    int32_t off, i, room;
    uint8_t flags = XR_DMA | XR_READ;
    if (!cdl.enabled || !romCdl || count <= 0) return;
    base = GetBasePointer(source);
    if (!base || base < (uint8_t*) MAP_LAST) return;
    off = romOffset(base + (source & 0xffff));
    if (off < 0) return;
    room = 0x10000 - (int32_t)(source & 0xffff);   /* DMA wraps inside the bank */
    if (count > room) count = room;
    for (i = 0; i < count; i++) markRom((uint32_t)(off + i), CDL_DATA, EXT_DMA_SOURCE);
    if (bAddress == 0x18 || bAddress == 0x19) flags |= XR_DMA_VRAM;
    else if (bAddress == 0x22) flags |= XR_DMA_CGRAM;
    recordXref(SPACE_ROM, (uint32_t)off, flags);
}

/* ------------------------------------------------------------------ */
/* Exports                                                            */
/* ------------------------------------------------------------------ */

static uint32_t* outBuf;
static uint32_t  outCap;

static bool ensureOut(uint32_t words)
{
    if (words <= outCap) return true;
    uint32_t* grown = realloc(outBuf, words * sizeof(uint32_t));
    if (!grown) return false;
    outBuf = grown;
    outCap = words;
    return true;
}

uint32_t* CDL_Out(uint32_t words) { return ensureOut(words) ? outBuf : NULL; }

EMSCRIPTEN_KEEPALIVE
void cdlDisable(void)
{
    cdl.enabled = false;
    cdl.active  = false;
    free(romCdl); free(romExt); free(wvals);
    romCdl = romExt = wvals = NULL;
    romSize = 0;
    tableFree(&xrefs); tableFree(&edges); tableFree(&stats);
    CDL_WramFree();
    CDL_CountFree();
}

EMSCRIPTEN_KEEPALIVE
int cdlEnable(void)
{
    if (cdl.enabled) return 1;
    if (!Memory.ROM || !Memory.CalculatedSize) return 0;
    romSize = Memory.CalculatedSize;
    if (romSize > ROM_CHUNKS * 0x10000) romSize = ROM_CHUNKS * 0x10000;
    romCdl = calloc(romSize, 1);
    romExt = calloc(romSize, 1);
    wvals  = calloc(WRAM_SIZE * WVAL_BYTES, 1);
    if (!romCdl || !romExt || !wvals
        || !tableAlloc(&xrefs, 1 << 16, false) || !tableAlloc(&edges, 1 << 14, false)
        || !tableAlloc(&stats, 1 << 15, true) || !CDL_WramAlloc() || !CDL_CountAlloc(romSize)) {
        cdlDisable();
        return 0;
    }
    memset(flushDirty, 0, sizeof(flushDirty));
    memset(viewDirty, 1, sizeof(viewDirty));
    memset(wvalDirty, 0, sizeof(wvalDirty));
    memset(recentKey, 0, sizeof(recentKey));
    memset(recentVal, 0, sizeof(recentVal));
    cdl.havePrev = false;
    cdl.inInterrupt = false;
    cdl.enabled = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE int      cdlIsEnabled(void)     { return cdl.enabled; }
EMSCRIPTEN_KEEPALIVE uint32_t cdlRomSize(void)       { return romSize; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlRomPtr(void)        { return romCdl; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlExtPtr(void)        { return romExt; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWvalPtr(void)       { return wvals; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlFlushDirtyPtr(void) { return flushDirty; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlViewDirtyPtr(void)  { return viewDirty; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWvalDirtyPtr(void)  { return wvalDirty; }
EMSCRIPTEN_KEEPALIVE uint32_t* cdlOutPtr(void)       { return outBuf; }

static uint32_t drain(Table* t, uint32_t words, int kind)
{
    uint32_t n = 0, d;
    if (!t->keys || !ensureOut(t->dcount * words + 1)) return 0;
    for (d = 0; d < t->dcount; d++) {
        uint32_t i = t->dlist[d];
        uint64_t k = t->keys[i];
        uint32_t* o = &outBuf[n * words];
        t->dirty[i] = 0;
        if (kind == 0) {        /* xref */
            o[0] = (uint32_t)(k >> 32) & 0xFFFFFF;
            o[1] = (uint32_t)k & 0x3FFFFFF;
            o[2] = t->vals[i];
        } else if (kind == 1) { /* edge */
            o[0] = (uint32_t)(k >> 24) & 0xFFFFFF;
            o[1] = (uint32_t)k & 0xFFFFFF;
            o[2] = t->vals[i];
        } else {                /* stats */
            o[0] = (uint32_t)k & 0x3FFFFFF;
            o[1] = t->count[i];
            o[2] = t->lo[i];
            o[3] = t->hi[i];
            o[4] = t->vals[i];
        }
        n++;
    }
    t->dcount = 0;
    return n;
}

EMSCRIPTEN_KEEPALIVE uint32_t cdlDrainXrefs(void) { return drain(&xrefs, 3, 0); }
EMSCRIPTEN_KEEPALIVE uint32_t cdlDrainEdges(void) { return drain(&edges, 3, 1); }
EMSCRIPTEN_KEEPALIVE uint32_t cdlDrainStats(void) { return drain(&stats, 5, 2); }

#endif /* EVS_CDL */

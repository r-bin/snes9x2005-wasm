/*
 * cdl-regs.c - register context per instruction for the CDL recorder
 *
 *   DB and D at every ROM opcode head: a per-byte "last value" array filters
 *   repeats, so the hash table only sees a value the first time it differs.
 *   Pointer accesses ((dp), (dp,X), (dp),Y, [dp], [dp],Y, (sr,S),Y): the
 *   pointer is read before the instruction runs, so each access splits into
 *   base (the struct / buffer) and Y (the field offset). The pointer's own
 *   WRAM bytes are flagged WF_POINTER.
 *
 * Exports:
 *   cdlDrainRegs()  -> count, records [pc, kind << 16 | value] at cdlOutPtr()
 *                      kind: REG_DB, REG_D, REG_Y (Y at a pointer access)
 *   cdlDrainBases() -> count, records [pc, base24, flags] (BASE_* flags)
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"
#include "cdl-optable.h"
#include "cdl-table.h"

#if EVS_CDL

#define PER_PC_MAX 32
#define RECENT     4096   /* direct-mapped "seen lately" filter in front of the hash tables */

static uint8_t*  lastDb;
static uint16_t* lastD;
static uint8_t*  seen;          /* bit0 DB recorded once, bit1 D recorded once */
static uint32_t  regRomSize;
static Table     regs, bases, perPc;
static uint64_t  recent[RECENT];

/* true when key was seen lately (and remembers it otherwise) */
static inline bool seenLately(uint64_t key)
{
    uint32_t h = hash64(key) & (RECENT - 1);
    if (recent[h] == key) return true;
    recent[h] = key;
    return false;
}

bool CDL_RegsAlloc(uint32_t romSize)
{
    regRomSize = romSize;
    lastDb = calloc(romSize, 1);
    lastD  = calloc(romSize, sizeof(uint16_t));
    seen   = calloc(romSize, 1);
    memset(recent, 0, sizeof(recent));
    return lastDb && lastD && seen
        && tableAlloc(&regs, 1 << 12, false) && tableAlloc(&bases, 1 << 12, false)
        && tableAlloc(&perPc, 1 << 12, true);
}

void CDL_RegsFree(void)
{
    free(lastDb); free(lastD); free(seen);
    lastDb = NULL; lastD = NULL; seen = NULL;
    regRomSize = 0;
    tableFree(&regs); tableFree(&bases); tableFree(&perPc);
}

/* At most PER_PC_MAX distinct values per (pc, kind); true when this one may be added. */
static bool roomFor(uint32_t pc, uint32_t kind, uint64_t key, Table* t)
{
    uint32_t c;
    if (tableSlot(t, key, false) != UINT32_MAX) return true;
    c = tableSlot(&perPc, (1ULL << 63) | ((uint64_t)kind << 24) | pc, true);
    if (c == UINT32_MAX || perPc.count[c] >= PER_PC_MAX) return false;
    perPc.count[c]++;
    return true;
}

static void addReg(uint32_t pc, uint32_t kind, uint32_t value)
{
    uint64_t key = (1ULL << 63) | ((uint64_t)pc << 24) | (kind << 16) | (value & 0xFFFF);
    if (seenLately(key)) return;
    if (roomFor(pc, kind, key, &regs)) tableOr(&regs, key, 1);
}

static inline uint32_t bank0Word(uint32_t a)
{
    a &= 0xFFFF;
    if (a + 1 >= 0x2000) return UINT32_MAX;         /* direct page / stack outside low WRAM */
    return Memory.RAM[a] | (Memory.RAM[a + 1] << 8);
}

void CDL_RegsExec(int32_t off, const uint8_t* p, uint32_t pc)
{
    uint8_t  db = ICPU.Registers.DB, kind = CDL_OpPtr[p[0]];
    uint16_t d = ICPU.Registers.D.W;

    if (off >= 0 && (uint32_t)off < regRomSize) {
        if (!(seen[off] & 1) || lastDb[off] != db) { seen[off] |= 1; lastDb[off] = db; addReg(pc, REG_DB, db); }
        if (!(seen[off] & 2) || lastD[off] != d)   { seen[off] |= 2; lastD[off] = d;   addReg(pc, REG_D, d); }
    }

    if (kind && kind != CDL_PTR_SR) {
        uint32_t loc, lo, base, y = ICPU.Registers.Y.W;
        uint8_t  flags = 0;
        if (kind == CDL_PTR_ISRY) { loc = ICPU.Registers.S.W + p[1]; flags |= BASE_STACK; }
        else if (kind == CDL_PTR_IDPX) loc = d + p[1] + ICPU.Registers.X.W;
        else loc = d + p[1];
        loc &= 0xFFFF;
        lo = bank0Word(loc);
        if (lo == UINT32_MAX) return;
        if (kind == CDL_PTR_ILDP || kind == CDL_PTR_ILDPY) {
            if (loc + 2 >= 0x2000) return;
            base = lo | (Memory.RAM[loc + 2] << 16);
            flags |= BASE_LONG;
        } else {
            base = lo | ((uint32_t)db << 16);
        }
        if (kind == CDL_PTR_IDPY || kind == CDL_PTR_ILDPY || kind == CDL_PTR_ISRY) {
            flags |= BASE_Y;
            addReg(pc, REG_Y, y);
        }
        {
            uint64_t key = (1ULL << 63) | (1ULL << 62) | ((uint64_t)pc << 24) | (base & 0xFFFFFF);
            if (seenLately(key)) return;
            CDL_WramAccess(loc, XR_POINTER, 2);
            if (flags & BASE_LONG) CDL_WramAccess(loc + 2, XR_POINTER, 1);
            key &= ~(1ULL << 62);
            if (roomFor(pc, 3, key, &bases)) tableOr(&bases, key, flags);
        }
    }
}

static uint32_t drainPairs(Table* t, int baseKind)
{
    uint32_t n, d, words = baseKind ? 3 : 2;
    uint32_t* out;
    if (!t->keys) return 0;
    out = CDL_Out(t->dcount * words + 1);
    if (!out) return 0;
    for (n = 0, d = 0; d < t->dcount; d++, n++) {
        uint32_t i = t->dlist[d];
        uint64_t k = t->keys[i];
        t->dirty[i] = 0;
        out[n * words] = (uint32_t)(k >> 24) & 0xFFFFFF;
        if (baseKind) { out[n * words + 1] = (uint32_t)k & 0xFFFFFF; out[n * words + 2] = t->vals[i]; }
        else out[n * words + 1] = (uint32_t)k & 0xFFFFF;
    }
    t->dcount = 0;
    return n;
}

EMSCRIPTEN_KEEPALIVE uint32_t cdlDrainRegs(void)  { return drainPairs(&regs, 0); }
EMSCRIPTEN_KEEPALIVE uint32_t cdlDrainBases(void) { return drainPairs(&bases, 1); }

#endif /* EVS_CDL */

/*
 * cdl-count.c - hit counters for the CDL recorder
 *
 * romHits : 1 uint32 per ROM byte - executions at an opcode head, reads at a data byte
 * wReads / wWrites : 1 uint32 per WRAM byte (a word access counts at its low byte)
 *
 * Counts are deltas: a drain returns every non-zero counter of the dirty chunks
 * and zeroes it, so the host sums drained deltas and never sees one twice.
 *
 * Exports:
 *   cdlDrainRomHits()  -> count, records [romOff, hits] at cdlOutPtr()
 *   cdlDrainWramHits() -> count, records [wramAddr, reads, writes] at cdlOutPtr()
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"

#if EVS_CDL

#define WRAM_SIZE   0x20000
#define ROM_CHUNKS  128
#define WHIT_CHUNKS 32

static uint32_t* romHits;
static uint32_t* wReads;
static uint32_t* wWrites;
static uint32_t  hitRomSize;
static uint8_t   romHitDirty[ROM_CHUNKS];
static uint8_t   wHitDirty[WHIT_CHUNKS];

bool CDL_CountAlloc(uint32_t romSize)
{
    hitRomSize = romSize;
    romHits = calloc(romSize, sizeof(uint32_t));
    wReads  = calloc(WRAM_SIZE, sizeof(uint32_t));
    wWrites = calloc(WRAM_SIZE, sizeof(uint32_t));
    memset(romHitDirty, 0, sizeof(romHitDirty));
    memset(wHitDirty, 0, sizeof(wHitDirty));
    return romHits && wReads && wWrites;
}

void CDL_CountFree(void)
{
    free(romHits); free(wReads); free(wWrites);
    romHits = wReads = wWrites = NULL;
    hitRomSize = 0;
}

void CDL_CountRom(uint32_t off)
{
    if (off >= hitRomSize) return;
    romHits[off]++;
    romHitDirty[off >> 16] = 1;
}

void CDL_CountWram(uint32_t addr, bool write)
{
    if (addr >= WRAM_SIZE) return;
    if (write) wWrites[addr]++; else wReads[addr]++;
    wHitDirty[addr >> 12] = 1;
}

EMSCRIPTEN_KEEPALIVE
uint32_t cdlDrainRomHits(void)
{
    uint32_t n = 0, c, i, need = 0;
    uint32_t* out;
    if (!romHits) return 0;
    for (c = 0; c < ROM_CHUNKS; c++) {
        uint32_t lo = c << 16, hi = lo + 0x10000;
        if (!romHitDirty[c]) continue;
        if (hi > hitRomSize) hi = hitRomSize;
        for (i = lo; i < hi; i++) if (romHits[i]) need++;
    }
    out = CDL_Out(need * 2 + 1);
    if (!out) return 0;
    for (c = 0; c < ROM_CHUNKS; c++) {
        uint32_t lo = c << 16, hi = lo + 0x10000;
        if (!romHitDirty[c]) continue;
        romHitDirty[c] = 0;
        if (hi > hitRomSize) hi = hitRomSize;
        for (i = lo; i < hi; i++) {
            if (!romHits[i]) continue;
            out[n * 2] = i;
            out[n * 2 + 1] = romHits[i];
            romHits[i] = 0;
            n++;
        }
    }
    return n;
}

EMSCRIPTEN_KEEPALIVE
uint32_t cdlDrainWramHits(void)
{
    uint32_t n = 0, c, i, need = 0;
    uint32_t* out;
    if (!wReads) return 0;
    for (c = 0; c < WHIT_CHUNKS; c++) {
        if (!wHitDirty[c]) continue;
        for (i = c << 12; i < (c + 1) << 12; i++) if (wReads[i] | wWrites[i]) need++;
    }
    out = CDL_Out(need * 3 + 1);
    if (!out) return 0;
    for (c = 0; c < WHIT_CHUNKS; c++) {
        if (!wHitDirty[c]) continue;
        wHitDirty[c] = 0;
        for (i = c << 12; i < (c + 1) << 12; i++) {
            if (!(wReads[i] | wWrites[i])) continue;
            out[n * 3] = i;
            out[n * 3 + 1] = wReads[i];
            out[n * 3 + 2] = wWrites[i];
            wReads[i] = wWrites[i] = 0;
            n++;
        }
    }
    return n;
}

#endif /* EVS_CDL */

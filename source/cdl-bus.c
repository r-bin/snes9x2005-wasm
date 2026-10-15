/*
 * cdl-bus.c - which bus address form reached each ROM byte (CDL recorder)
 *
 * The ROM is mirrored: one byte has up to four bus addresses. rom.cdl and xrefs
 * keep only the ROM offset, so the form a program used is lost. This stream keeps it,
 * for ports that change what the mirrors mean (ExHiROM, SA-1):
 *
 *   forms[off]  low nibble  = forms the byte was EXECUTED through,
 *               high nibble = forms it was READ through (data read, DMA source)
 *     form bits: 1 = $C0-$FF   2 = $80-$BF   4 = $00-$3F   8 = $40-$7D
 *   sites       one row per (instruction PC, form) with the lowest / highest bus
 *               address that instruction used in that form: where the code that
 *               has to be rewritten lives. Only data reads and DMA sources;
 *               control flow is already in edges (the `to` address keeps its bank).
 *               The CPU's own vector fetches ($00:FFE0-FFFF, made when an interrupt
 *               is taken) are not the interrupted instruction's reads: they are
 *               recorded under pc 0, which reads "the CPU, not code".
 *
 * Merges are OR / min / max, so sessions combine in any order.
 *
 * Exports:
 *   cdlBusPtr() / cdlBusDirtyPtr()   uint8[romSize] / uint8[128] (one per 64 KB chunk)
 *   cdlDrainBusSites() -> count, records [formIdx << 24 | pc, flags, lo, hi] at cdlOutPtr()
 *                         formIdx 0..3 = the data form bit 1,2,4,8
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"
#include "cdl-table.h"

#if EVS_CDL

#define BUS_CHUNKS 128
#define RECENT     2048

static uint8_t* forms;
static uint32_t busRomSize;
static uint8_t  busDirty[BUS_CHUNKS];
static Table    sites;
static uint64_t recentKey[RECENT];
static uint32_t recentLo[RECENT], recentHi[RECENT];

/* bit 1/2/4/8 for $C0-$FF / $80-$BF / $00-$3F / $40-$7D */
static inline uint8_t formOf(uint32_t bus)
{
    uint32_t bank = (bus >> 16) & 0xFF;
    if (bank >= 0xC0) return 1;
    if (bank >= 0x80) return 2;
    if (bank >= 0x40) return 8;
    return 4;
}

bool CDL_BusAlloc(uint32_t romSize)
{
    busRomSize = romSize;
    forms = calloc(romSize, 1);
    memset(busDirty, 0, sizeof(busDirty));
    memset(recentKey, 0, sizeof(recentKey));
    return forms && tableAlloc(&sites, 1 << 12, true);
}

void CDL_BusFree(void)
{
    free(forms);
    forms = NULL;
    busRomSize = 0;
    tableFree(&sites);
}

static inline void mark(uint32_t off, uint32_t len, uint8_t bits)
{
    uint32_t i;
    for (i = 0; i < len && off + i < busRomSize; i++) {
        if ((forms[off + i] | bits) != forms[off + i]) {
            forms[off + i] |= bits;
            busDirty[(off + i) >> 16] = 1;
        }
    }
}

/* An instruction of len bytes at ROM offset off, fetched from bus address pc. */
void CDL_BusExec(uint32_t off, uint32_t len, uint32_t pc)
{
    if (!forms) return;
    mark(off, len, formOf(pc));
}

static void site(uint32_t pc, uint8_t form, uint32_t bus, uint8_t flags)
{
    uint32_t idx = form == 1 ? 0 : form == 2 ? 1 : form == 4 ? 2 : 3;
    uint64_t key = (1ULL << 63) | ((uint64_t)idx << 24) | (pc & 0xFFFFFF);
    uint32_t h = hash64(key) & (RECENT - 1);
    uint32_t i;
    if (recentKey[h] == key && bus >= recentLo[h] && bus <= recentHi[h]) return;
    i = tableSlot(&sites, key, true);
    if (i == UINT32_MAX) return;
    if (sites.vals[i] == 0 && sites.lo[i] == 0 && sites.hi[i] == 0) { sites.lo[i] = bus; sites.hi[i] = bus; }
    if (bus < sites.lo[i] || bus > sites.hi[i] || (sites.vals[i] | flags) != sites.vals[i]) {
        if (bus < sites.lo[i]) sites.lo[i] = bus;
        if (bus > sites.hi[i]) sites.hi[i] = bus;
        sites.vals[i] |= flags;
        markDirty(&sites, i);
    }
    recentKey[h] = key; recentLo[h] = sites.lo[i]; recentHi[h] = sites.hi[i];
}

/* A data read of width bytes at ROM offset off through bus address bus, by the instruction at pc. */
void CDL_BusData(uint32_t off, uint32_t width, uint32_t bus, uint32_t pc, uint8_t flags)
{
    uint8_t f;
    if (!forms) return;
    f = formOf(bus);
    mark(off, width, (uint8_t)(f << 4));
    if ((bus & 0xFFFFE0) == 0x00FFE0) pc = 0;   /* interrupt vector fetch */
    site(pc, f, bus & 0xFFFFFF, flags);
}

EMSCRIPTEN_KEEPALIVE uint8_t* cdlBusPtr(void)      { return forms; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlBusDirtyPtr(void) { return busDirty; }

EMSCRIPTEN_KEEPALIVE
uint32_t cdlDrainBusSites(void)
{
    uint32_t n = 0, d;
    uint32_t* out;
    if (!sites.keys) return 0;
    out = CDL_Out(sites.dcount * 4 + 1);
    if (!out) return 0;
    for (d = 0; d < sites.dcount; d++) {
        uint32_t i = sites.dlist[d];
        uint64_t k = sites.keys[i];
        sites.dirty[i] = 0;
        out[n * 4]     = (uint32_t)k & 0x3FFFFFF;
        out[n * 4 + 1] = sites.vals[i];
        out[n * 4 + 2] = sites.lo[i];
        out[n * 4 + 3] = sites.hi[i];
        n++;
    }
    sites.dcount = 0;
    return n;
}

#endif /* EVS_CDL */

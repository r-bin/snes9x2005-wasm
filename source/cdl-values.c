/*
 * cdl-values.c - which values the program wrote where: I/O registers, SRAM access, 16-bit WRAM words
 *
 * ioVals: 256-bit "values written" bitmap per I/O register the CPU wrote ($2100-$21FF, then $4200-$437F; includes
 *   the SPC ports $2140-$2143 and the DMA channel registers). 640 registers x 32 bytes. OR-merged.
 * sramFlags: one byte per SRAM byte, the wflags bits (read / write / byte / word). OR-merged.
 * wordRanges: for every WRAM address a 16-bit store (STA/STX/... in 16-bit mode) hit, in two sets (0 = natural,
 *   1 = while the host says a cheat or tool wrote memory recently): minimum, maximum and, while there are at
 *   most 16 distinct values, the sorted value list. 40-byte records, 128 K addresses per set (10 MB for both):
 *     +0 u16 min   +2 u16 max   +4 u8 n (0 none, 1..16 list length, 17 more than 16 distinct)   +6 u16 vals[16]
 *   Merge (min / max / list union, more than 16 -> 17) is commutative, associative and idempotent.
 *
 * Exports:
 *   cdlIoValPtr() / cdlIoValDirtyPtr()          uint8[640*32] / uint8[1]
 *   cdlSramFlagPtr() / cdlSramFlagDirtyPtr()    uint8[0x20000] / uint8[32] (4 KB each)
 *   cdlWordRangePtr(set) / cdlWordRangeDirtyPtr(set)   uint8[0x20000*40] / uint8[512] (256 addresses each)
 *   cdlSetValueSet(set)                         which word-range set CPU stores go to
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"

#if EVS_CDL

#define IO_REGS      640
#define IO_BYTES     32
#define SRAM_MAX     0x20000
#define VR_ADDRS     0x20000
#define VR_REC       40
#define VR_PER_CHUNK 256
#define VR_OVER      17

static uint8_t* ioVals;
static uint8_t  ioDirty[1];
static uint8_t* sramFlags;
static uint8_t  sramDirty[SRAM_MAX / 4096];
static uint8_t* ranges[2];
static uint8_t  rangeDirty[2][VR_ADDRS / VR_PER_CHUNK];
static uint8_t  valueSet;

bool CDL_ValuesAlloc(void)
{
    ioVals = calloc(IO_REGS * IO_BYTES, 1);
    sramFlags = calloc(SRAM_MAX, 1);
    ranges[0] = calloc((size_t)VR_ADDRS * VR_REC, 1);
    ranges[1] = calloc((size_t)VR_ADDRS * VR_REC, 1);
    ioDirty[0] = 0;
    memset(sramDirty, 0, sizeof(sramDirty));
    memset(rangeDirty, 0, sizeof(rangeDirty));
    valueSet = 0;
    return ioVals && sramFlags && ranges[0] && ranges[1];
}

void CDL_ValuesFree(void)
{
    free(ioVals); free(sramFlags); free(ranges[0]); free(ranges[1]);
    ioVals = sramFlags = ranges[0] = ranges[1] = NULL;
}

void CDL_IoValue(uint32_t reg, uint8_t v)
{
    uint32_t i;
    uint8_t* b;
    uint8_t bit = (uint8_t)(1 << (v & 7));
    if (!ioVals) return;
    if (reg >= 0x2100 && reg <= 0x21FF) i = reg - 0x2100;
    else if (reg >= 0x4200 && reg <= 0x437F) i = 256 + reg - 0x4200;
    else return;
    b = &ioVals[i * IO_BYTES + (v >> 3)];
    if (!(*b & bit)) { *b |= bit; ioDirty[0] = 1; }
}

void CDL_SramAccess(uint32_t off, uint8_t flags, uint32_t width)
{
    uint32_t i;
    uint8_t f = ((flags & XR_READ) ? WF_READ : 0) | ((flags & XR_WRITE) ? WF_WRITE : 0) | (width == 2 ? WF_WORD : WF_BYTE);
    if (!sramFlags) return;
    for (i = 0; i < width && off + i < SRAM_MAX; i++) {
        if ((sramFlags[off + i] | f) != sramFlags[off + i]) { sramFlags[off + i] |= f; sramDirty[(off + i) >> 12] = 1; }
    }
}

void CDL_WordValue(uint32_t addr, uint16_t v)
{
    uint8_t* r;
    uint16_t mn, mx, *vals;
    uint8_t n;
    bool changed = false;
    int i;
    if (!ranges[0] || addr >= VR_ADDRS) return;
    r = ranges[valueSet] + (size_t)addr * VR_REC;
    vals = (uint16_t*)(r + 6);
    n = r[4];
    if (n == 0) {
        r[0] = (uint8_t)v; r[1] = (uint8_t)(v >> 8); r[2] = (uint8_t)v; r[3] = (uint8_t)(v >> 8);
        r[4] = 1; vals[0] = v;
        rangeDirty[valueSet][addr / VR_PER_CHUNK] = 1;
        return;
    }
    mn = (uint16_t)(r[0] | (r[1] << 8));
    mx = (uint16_t)(r[2] | (r[3] << 8));
    if (v < mn) { r[0] = (uint8_t)v; r[1] = (uint8_t)(v >> 8); changed = true; }
    if (v > mx) { r[2] = (uint8_t)v; r[3] = (uint8_t)(v >> 8); changed = true; }
    if (n <= 16) {
        for (i = 0; i < n; i++) if (vals[i] == v) break;
        if (i == n) {
            if (n == 16) { r[4] = VR_OVER; memset(r + 6, 0, 32); }
            else {
                int j = n;
                while (j > 0 && vals[j - 1] > v) { vals[j] = vals[j - 1]; j--; }
                vals[j] = v;
                r[4] = (uint8_t)(n + 1);
            }
            changed = true;
        }
    }
    if (changed) rangeDirty[valueSet][addr / VR_PER_CHUNK] = 1;
}

EMSCRIPTEN_KEEPALIVE uint8_t* cdlIoValPtr(void)            { return ioVals; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlIoValDirtyPtr(void)       { return ioDirty; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlSramFlagPtr(void)         { return sramFlags; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlSramFlagDirtyPtr(void)    { return sramDirty; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWordRangePtr(int32_t s)      { return ranges[s & 1]; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlWordRangeDirtyPtr(int32_t s) { return rangeDirty[s & 1]; }
EMSCRIPTEN_KEEPALIVE void     cdlSetValueSet(int32_t s)       { valueSet = (uint8_t)(s & 1); }

#endif /* EVS_CDL */

/*
 * cdl-spc.c - SPC700 coverage for the CDL recorder (ARAM, 64 KB)
 *
 * aram: 1 byte per ARAM byte, OR-merged like rom.cdl:
 *   ARAM_EXEC 01 opcode head, ARAM_OPERAND 02 operand byte (bytes between two
 *   consecutive sequential opcode fetches), ARAM_READ 04, ARAM_WRITE 08
 * Hooks sit in apu_blargg.c (opcode fetch, spc_cpu_read / spc_cpu_write, the
 * direct-page fast paths). They use cdl.enabled, not cdl.active: the APU catches
 * up outside the CPU's main loop.
 *
 * Exports: cdlAramPtr() / cdlAramDirtyPtr() (uint8[16], one per 4 KB)
 */

#include <stdlib.h>
#include <string.h>
#include "emscripten.h"
#include "cdl.h"

#if EVS_CDL

#define ARAM_SIZE   0x10000
#define ARAM_CHUNKS 16

uint8_t*        cdlAram;
int32_t         cdlSpcPrev = -1;
static uint8_t  aramDirty[ARAM_CHUNKS];

bool CDL_SpcAlloc(void)
{
    cdlAram = calloc(ARAM_SIZE, 1);
    memset(aramDirty, 0, sizeof(aramDirty));
    cdlSpcPrev = -1;
    return cdlAram != NULL;
}

void CDL_SpcFree(void)
{
    free(cdlAram);
    cdlAram = NULL;
}

void CDL_SpcMark(uint32_t addr, uint8_t f)
{
    addr &= 0xFFFF;
    if ((cdlAram[addr] | f) != cdlAram[addr]) { cdlAram[addr] |= f; aramDirty[addr >> 12] = 1; }
}

void CDL_SpcExec(uint32_t addr)
{
    int32_t a = (int32_t)(addr & 0xFFFF), i;
    if (cdlSpcPrev >= 0 && a > cdlSpcPrev && a - cdlSpcPrev <= 3)
        for (i = cdlSpcPrev + 1; i < a; i++) CDL_SpcMark((uint32_t)i, ARAM_OPERAND);
    CDL_SpcMark((uint32_t)a, ARAM_EXEC);
    cdlSpcPrev = a;
}

EMSCRIPTEN_KEEPALIVE uint8_t* cdlAramPtr(void)      { return cdlAram; }
EMSCRIPTEN_KEEPALIVE uint8_t* cdlAramDirtyPtr(void) { return aramDirty; }

#endif /* EVS_CDL */

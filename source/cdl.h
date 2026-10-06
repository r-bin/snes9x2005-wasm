/*
 * cdl.h - Code/Data Logger + cross-reference recorder
 *
 * Off by default. While enabled it records, in memory only:
 *   - rom.cdl  : 1 byte per ROM byte, Mesen-S / BizHawk bit layout
 *   - rom.ext  : 1 companion byte per ROM byte (opcode head, M/X seen, DMA, APU)
 *   - edges    : (from PC, to PC, kind) for calls, jumps and taken branches
 *   - xrefs    : (instruction PC, accessed address, R/W + width) per data access
 *   - pcstats  : per (PC, space) access count + address range; caps xrefs per PC
 *   - wvals    : 256-bit "values seen" bitmap per WRAM byte (enum detection)
 * The host drains only what changed since the last drain (see cdl.c exports).
 */

#ifndef CDL_H
#define CDL_H

/* Compile-time switch for everything CDL. Constant on; build with -DEVS_CDL=0
 * to strip the recorder and every hook site from the core. */
#ifndef EVS_CDL
#define EVS_CDL 1
#endif

#include "my_types.h"
#include "snes9x.h"
#include "cpuexec.h"
#include "memmap.h"

#if EVS_CDL

/* rom.cdl */
#define CDL_CODE        0x01
#define CDL_DATA        0x02
#define CDL_JUMP_TARGET 0x04
#define CDL_SUB_ENTRY   0x08
#define CDL_IDX_8       0x10
#define CDL_ACC_8       0x20

/* rom.ext */
#define EXT_DMA_SOURCE  0x01
#define EXT_APU_SOURCE  0x02
#define EXT_OPCODE_HEAD 0x04
#define EXT_SEEN_M16    0x08
#define EXT_SEEN_X16    0x10
#define EXT_POINTER     0x20
#define EXT_DATA_WORD   0x40

/* xref flags */
#define XR_READ      0x01
#define XR_WRITE     0x02
#define XR_BYTE      0x04
#define XR_WORD      0x08
#define XR_POINTER   0x10
#define XR_DMA       0x20
#define XR_DMA_VRAM  0x40
#define XR_DMA_CGRAM 0x80

/* address spaces (top byte of an encoded address) */
#define SPACE_WRAM 0
#define SPACE_ROM  1
#define SPACE_BUS  2
#define SPACE_IO   3

/* edge kinds beyond opcodes.js FLOW bits */
#define FLOW_INTERRUPT 0x10

typedef struct {
    bool     enabled;      /* user switched recording on */
    bool     active;       /* enabled AND inside S9xMainLoop (excludes debugger reads) */
    bool     inInterrupt;  /* NMI/IRQ entry pushed; cleared at the next instruction */
    bool     havePrev;
    uint8_t  prevOp;
    uint8_t  curOp;
    uint32_t prevPC;
    uint32_t curPC;
    uint32_t seq;          /* instruction counter */
    int32_t  curRomOff;    /* ROM offset of the current instruction, -1 outside ROM */
    uint32_t curLen;
    uint32_t lastRomRead;
    uint32_t lastRomReadSeq;
} CDLState;

extern CDLState cdl;

void CDL_Exec(void);
void CDL_Access(uint32_t address, uint8_t* block, uint8_t flags, uint16_t value);
void CDL_Dma(uint8_t bAddress, uint32_t source, int32_t count);

static inline void CDL_OnExec(void)
{
    if (__builtin_expect(cdl.active, 0)) CDL_Exec();
}

#define CDL_ON_ACCESS(addr, block, flags, value) \
    do { if (__builtin_expect(cdl.active, 0) && !CPU.InDMA) CDL_Access((addr), (block), (flags), (value)); } while (0)

#endif /* EVS_CDL */

#endif /* CDL_H */

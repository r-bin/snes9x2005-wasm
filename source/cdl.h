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
 *   - hits     : execution / read counts per ROM byte, read / write counts per WRAM byte (cdl-count.c)
 *   - rets     : shadow call stack - exit M/X, dropped frames, pushed-address returns (cdl-flow.c)
 *   - regs     : DB / D per instruction, pointer bases + Y of indirect accesses (cdl-regs.c)
 *   - wcode    : bytes of code executed from WRAM, and whether they changed (cdl-wram.c)
 *   - aram     : SPC700 exec / operand / read / write per ARAM byte (cdl-spc.c)
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
#define EXT_HDMA        0x80

/* xref flags */
#define XR_READ      0x01
#define XR_WRITE     0x02
#define XR_BYTE      0x04
#define XR_WORD      0x08
#define XR_POINTER   0x10
#define XR_DMA       0x20
#define XR_DMA_VRAM  0x40
#define XR_DMA_CGRAM 0x80

/* wflags: 1 byte per WRAM byte (cdl-wram.c) */
#define WF_READ    0x01
#define WF_WRITE   0x02
#define WF_BYTE    0x04
#define WF_WORD    0x08
#define WF_EXEC    0x10
#define WF_SCRIPT  0x20
#define WF_POINTER 0x40
#define WF_DMA     0x80   /* read or written by DMA / HDMA / $2180 */

/* rets flags (cdl-flow.c) */
#define RET_NORMAL    0x01
#define RET_MODIFIED  0x02   /* S matched, target differs: return address adjusted (inline data) */
#define RET_DROPPED   0x04   /* frame never returned; record's retPc is the call site */
#define RET_INTERRUPT 0x08

/* regs kinds / base flags (cdl-regs.c) */
#define REG_DB 0
#define REG_D  1
#define REG_Y  2
#define BASE_Y     0x01
#define BASE_LONG  0x02
#define BASE_STACK 0x04

/* aram (cdl-spc.c) */
#define ARAM_EXEC    0x01
#define ARAM_OPERAND 0x02
#define ARAM_READ    0x04
#define ARAM_WRITE   0x08

/* address spaces (top byte of an encoded address) */
#define SPACE_WRAM 0
#define SPACE_ROM  1
#define SPACE_BUS  2
#define SPACE_IO   3

/* edge kinds beyond opcodes.js FLOW bits */
#define FLOW_INTERRUPT 0x10
#define FLOW_RETURN    0x20   /* return to an address the code pushed or adjusted itself */

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
void CDL_Dma(uint8_t bAddress, uint32_t source, int32_t count, bool toA, bool fixed);
void CDL_HdmaStart(uint32_t bus);
void CDL_HdmaBus(uint32_t bus, int32_t count);
void CDL_HdmaBytes(const uint8_t* p, int32_t count);
void CDL_RecordEdge(uint32_t from, uint32_t to, uint8_t kind);
uint32_t* CDL_Out(uint32_t words);   /* shared drain buffer */
extern uint32_t cdlDropped;          /* table inserts lost because a table was full */

/* cdl-flow.c */
bool CDL_FlowAlloc(void);
void CDL_FlowFree(void);
uint8_t CDL_FlowExec(uint32_t pc, uint16_t s, bool m8, bool x8);

/* cdl-regs.c */
bool CDL_RegsAlloc(uint32_t romSize);
void CDL_RegsFree(void);
void CDL_RegsExec(int32_t off, const uint8_t* p, uint32_t pc);

/* cdl-spc.c */
extern uint8_t* cdlAram;
extern int32_t  cdlSpcPrev;          /* last SPC opcode address, for operand bytes */
bool CDL_SpcAlloc(void);
void CDL_SpcFree(void);
void CDL_SpcMark(uint32_t addr, uint8_t f);
void CDL_SpcExec(uint32_t addr);

/* cdl-count.c */
bool CDL_CountAlloc(uint32_t romSize);
void CDL_CountFree(void);
void CDL_CountRom(uint32_t off);
void CDL_CountWram(uint32_t addr, bool write);

/* cdl-wram.c */
bool CDL_WramAlloc(void);
void CDL_WramFree(void);
void CDL_WramExec(int32_t romOff, const uint8_t* p, uint32_t len);
void CDL_WramAccess(uint32_t addr, uint8_t flags, uint32_t width);
void CDL_WramMarkRange(uint32_t addr, int32_t count, uint8_t wf);

static inline void CDL_OnExec(void)
{
    if (__builtin_expect(cdl.active, 0)) CDL_Exec();
}

#define CDL_ON_ACCESS(addr, block, flags, value) \
    do { if (__builtin_expect(cdl.active, 0) && !CPU.InDMA) CDL_Access((addr), (block), (flags), (value)); } while (0)

/* Fast path inline: only a byte that gains a flag calls into cdl-spc.c. */
#define CDL_SPC_EXEC(addr) \
    do { if (__builtin_expect(cdl.enabled, 0) && cdlAram) { uint32_t a_ = (uint32_t)(addr) & 0xFFFF; \
        if (cdlAram[a_] & ARAM_EXEC) cdlSpcPrev = (int32_t)a_; else CDL_SpcExec(a_); } } while (0)
#define CDL_SPC_ACCESS(addr, f) \
    do { if (__builtin_expect(cdl.enabled, 0) && cdlAram) { uint32_t a_ = (uint32_t)(addr) & 0xFFFF; \
        if ((cdlAram[a_] | (f)) != cdlAram[a_]) CDL_SpcMark(a_, (f)); } } while (0)

#else

#define CDL_SPC_EXEC(addr)      do { } while (0)
#define CDL_SPC_ACCESS(addr, f) do { } while (0)

#endif /* EVS_CDL */

#endif /* CDL_H */

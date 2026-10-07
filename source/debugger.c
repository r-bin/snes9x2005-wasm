/*
 * debugger.c — Breakpoint system + Emscripten JS bridge
 *
 * Exported Module API (all callable as Module._fnName() from JS):
 *
 *   addExecBreakpoint(addr)      — add 24-bit PC exec breakpoint
 *   removeExecBreakpoint(addr)   — remove exec breakpoint
 *   addWriteBreakpoint(addr)     — add WRAM write watchpoint (16-bit offset)
 *   removeWriteBreakpoint(addr)  — remove write watchpoint
 *   pauseEmulation()             — pause the emulator
 *   resumeEmulation()            — resume the emulator
 *   getCPUState()                — returns pointer to uint32[8] buffer
 *   readMemory(addr)             — read one byte from SNES bus
 *   writeMemory(addr, value)     — write one byte to SNES bus
 *   readMemoryRange(addr, size)  — returns pointer to static read buffer
 */

#include "emscripten.h"
#include "debugger.h"
#include "snes9x.h"
#include "cpuexec.h"
#include "memmap.h"
#include "evs-tas.h"

/* ------------------------------------------------------------------ */
/* Global debugger state                                               */
/* ------------------------------------------------------------------ */

DebuggerState dbg = {
    .hasExecBP   = false,
    .hasWriteBP  = false,
    .paused      = false,
    .execBPCount  = 0,
    .writeBPCount = 0,
};

/* ------------------------------------------------------------------ */
/* Internal: fire breakpoint, pause, notify JS                        */
/* ------------------------------------------------------------------ */

void DBG_FireBreakpoint(uint8_t type, uint32_t addr, uint8_t value, uint32_t pc)
{
    /*
     * Ask the JS frontend (Module.onBreakpointHit) first: returning false
     * means "not interested, keep running" — a conditional breakpoint. The
     * VS Code debugger uses it to hook the script interpreter's opcode fetch
     * and stop only on the script addresses it wants. Anything else pauses.
     */
    int pause = EM_ASM_INT({
        var cb = Module['onBreakpointHit'];
        if (typeof cb !== 'function') return 1;
        var result = cb({
            type    : $0 ? 'write' : 'exec',
            address : $1 >>> 0,
            value   : $2,
            pc      : $3 >>> 0
        });
        return result === false ? 0 : 1;
    }, (int)type, (int)addr, (int)value, (int)pc);
    if (!pause) return;

    /* Pause the emulation loop — mainLoop() checks this flag */
    dbg.paused = true;

    /*
     * Break out of the CPU inner do-while cleanly.
     * SCAN_KEYS_FLAG is the same mechanism used for frame boundaries —
     * it breaks the inner loop, lets S9xPackStatus run, then the loop
     * terminates normally.  The flag is cleared after the loop exits.
     */
    CPU.Flags |= SCAN_KEYS_FLAG;
}

/* ------------------------------------------------------------------ */
/* Exec breakpoints                                                    */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
void addExecBreakpoint(uint32_t addr)
{
    uint8_t i;
    /* Deduplicate */
    for (i = 0; i < dbg.execBPCount; i++)
        if (dbg.execBP[i] == addr) return;
    if (dbg.execBPCount >= DBG_MAX_EXEC_BP) return;
    dbg.execBP[dbg.execBPCount++] = addr;
    dbg.hasExecBP = true;
}

EMSCRIPTEN_KEEPALIVE
void removeExecBreakpoint(uint32_t addr)
{
    uint8_t i;
    for (i = 0; i < dbg.execBPCount; i++) {
        if (dbg.execBP[i] == addr) {
            /* Swap with last, shrink */
            dbg.execBP[i] = dbg.execBP[--dbg.execBPCount];
            dbg.hasExecBP = (dbg.execBPCount > 0);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* WRAM write watchpoints                                              */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
void addWriteBreakpoint(uint32_t wramOffset)
{
    uint8_t i;
    for (i = 0; i < dbg.writeBPCount; i++)
        if (dbg.writeBP[i] == wramOffset) return;
    if (dbg.writeBPCount >= DBG_MAX_WRITE_BP) return;
    dbg.writeBP[dbg.writeBPCount++] = wramOffset;
    dbg.hasWriteBP = true;
}

EMSCRIPTEN_KEEPALIVE
void removeWriteBreakpoint(uint32_t wramOffset)
{
    uint8_t i;
    for (i = 0; i < dbg.writeBPCount; i++) {
        if (dbg.writeBP[i] == wramOffset) {
            dbg.writeBP[i] = dbg.writeBP[--dbg.writeBPCount];
            dbg.hasWriteBP = (dbg.writeBPCount > 0);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pause / Resume                                                      */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
void pauseEmulation(void)
{
    dbg.paused = true;
    CPU.Flags |= SCAN_KEYS_FLAG;
}

EMSCRIPTEN_KEEPALIVE
void resumeEmulation(void)
{
    dbg.paused = false;
}

/* Paused by pauseEmulation() or a breakpoint: the webview idles while this is set. */
EMSCRIPTEN_KEEPALIVE
int isEmulationPaused(void)
{
    return dbg.paused ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* CPU state inspection                                                */
/* ------------------------------------------------------------------ */

/*
 * getCPUState — fills and returns a pointer to a static uint32[8] buffer.
 *
 * Layout:  [0] pc (24-bit)
 *          [1] A
 *          [2] X
 *          [3] Y
 *          [4] SP
 *          [5] P (status flags, packed)
 *          [6] DB (data bank)
 *          [7] PB (program bank)
 *
 * JS access:
 *   const ptr = Module._getCPUState() >>> 2;   // byte → uint32 index
 *   const state = {
 *     pc: Module.HEAPU32[ptr+0],
 *     a:  Module.HEAPU32[ptr+1],
 *     x:  Module.HEAPU32[ptr+2],
 *     y:  Module.HEAPU32[ptr+3],
 *     sp: Module.HEAPU32[ptr+4],
 *     p:  Module.HEAPU32[ptr+5],
 *     db: Module.HEAPU32[ptr+6],
 *     pb: Module.HEAPU32[ptr+7],
 *   };
 */
static uint32_t cpuStateBuf[8];

EMSCRIPTEN_KEEPALIVE
uint32_t* getCPUState(void)
{
    S9xPackStatus();
    cpuStateBuf[0] = ICPU.ShiftedPB | (uint32_t)(CPU.PC - CPU.PCBase);
    cpuStateBuf[1] = ICPU.Registers.A.W;
    cpuStateBuf[2] = ICPU.Registers.X.W;
    cpuStateBuf[3] = ICPU.Registers.Y.W;
    cpuStateBuf[4] = ICPU.Registers.S.W;
    cpuStateBuf[5] = ICPU.Registers.P.W;
    cpuStateBuf[6] = ICPU.Registers.DB;
    cpuStateBuf[7] = ICPU.Registers.PB;
    return cpuStateBuf;
}

/* ------------------------------------------------------------------ */
/* Memory access                                                       */
/* ------------------------------------------------------------------ */

#if EVS_TAS
/* Debugger reads must not disturb emulation: S9xGetByte charges CPU cycles,
 * sets the idle-loop WaitAddress and runs I/O register handlers (latches,
 * NMI/IRQ flag clears). The host polls memory at wall-clock times, so going
 * through it made every session (and input movie replay) non-reproducible.
 * Peek instead: memory directly, I/O registers as their last written value. */
static uint8_t dbgPeekByte(uint32_t addr)
{
    int32_t block = (addr >> MEMMAP_SHIFT) & MEMMAP_MASK;
    uint8_t* p = Memory.Map[block];

    if (p >= (uint8_t*) MAP_LAST)
        return p[addr & 0xffff];

    switch ((intptr_t) p)
    {
    case MAP_PPU:
    case MAP_CPU:
        return Memory.FillRAM[addr & 0x7fff];
    case MAP_SA1RAM:
    case MAP_LOROM_SRAM:
        return Memory.SRAM[(((addr & 0xFF0000) >> 1) | (addr & 0x7FFF)) & Memory.SRAMMask];
    case MAP_RONLY_SRAM:
    case MAP_HIROM_SRAM:
        return Memory.SRAM[((addr & 0x7fff) - 0x6000 + ((addr & 0xf0000) >> 3)) & Memory.SRAMMask];
    case MAP_BWRAM:
        return Memory.BWRAM[(addr & 0x7fff) - 0x6000];
    default:
        return 0;
    }
}

#else
#define dbgPeekByte(addr) S9xGetByte(addr)
#endif

EMSCRIPTEN_KEEPALIVE
uint8_t readMemory(uint32_t addr)
{
    return dbgPeekByte(addr);
}

EMSCRIPTEN_KEEPALIVE
void writeMemory(uint32_t addr, uint8_t value)
{
    S9xSetByte(value, addr);
}

/* readMemoryRange — returns a pointer to a static buffer of up to 4096 bytes */
#define DBG_READ_BUF_SIZE 4096
static uint8_t readBuf[DBG_READ_BUF_SIZE];

EMSCRIPTEN_KEEPALIVE
uint8_t* readMemoryRange(uint32_t addr, uint32_t size)
{
    uint32_t i;
    if (size > DBG_READ_BUF_SIZE) size = DBG_READ_BUF_SIZE;
    for (i = 0; i < size; i++)
        readBuf[i] = dbgPeekByte(addr + i);
    return readBuf;
}

EMSCRIPTEN_KEEPALIVE
void writeRomByte(uint32_t offset, uint8_t value)
{
    if (Memory.ROM && offset < MAX_ROM_SIZE)
        Memory.ROM[offset] = value;
}

EMSCRIPTEN_KEEPALIVE
uint8_t readRomByte(uint32_t offset)
{
    if (Memory.ROM && offset < MAX_ROM_SIZE)
        return Memory.ROM[offset];
    return 0;
}

/*
 * debugger.h — Minimal SNES breakpoint / watchpoint support
 *
 * Design goals:
 *   - Zero overhead when no breakpoints are set (single bool guard)
 *   - Exec breakpoints:  pause before the instruction at a 24-bit PC fires
 *   - WRAM write watchpoints: pause after a WRAM byte is written
 *   - JS notification via Module.onBreakpointHit callback
 *
 * All public symbols are prefixed DBG_ to avoid collisions.
 */

#ifndef DEBUGGER_H
#define DEBUGGER_H

#include "my_types.h"
#include "snes9x.h"    /* SCPUState CPU, SCAN_KEYS_FLAG */
#include "cpuexec.h"   /* SICPU ICPU, ShiftedPB */
#include "memmap.h"    /* CMemory Memory */

/* Maximum simultaneous breakpoints (small arrays — cache-friendly, no malloc) */
#define DBG_MAX_EXEC_BP   64
#define DBG_MAX_WRITE_BP  64

/* Breakpoint event types */
#define DBG_TYPE_EXEC   0
#define DBG_TYPE_WRITE  1

typedef struct {
    /* Fast guards — checked on every instruction / every WRAM write */
    bool     hasExecBP;
    bool     hasWriteBP;
    /* Pause state */
    bool     paused;
    /* Breakpoint address tables */
    uint32_t execBP [DBG_MAX_EXEC_BP];
    uint32_t writeBP[DBG_MAX_WRITE_BP];
    uint8_t  execBPCount;
    uint8_t  writeBPCount;
} DebuggerState;

extern DebuggerState dbg;

/*
 * DBG_FireBreakpoint — called when a breakpoint actually hits.
 * Sets dbg.paused, breaks the CPU inner loop, fires JS callback.
 */
void DBG_FireBreakpoint(uint8_t type, uint32_t addr, uint8_t value, uint32_t pc);

/*
 * DBG_CheckExecBreakpoint — inline fast path for execute breakpoints.
 * Call this after CPU.PCAtOpcodeStart = CPU.PC and before dispatch.
 * fullPC = ICPU.ShiftedPB | (CPU.PCAtOpcodeStart - CPU.PCBase)
 */
static inline void DBG_CheckExecBreakpoint(void)
{
    if (__builtin_expect(!dbg.hasExecBP, 1)) return;
    uint32_t fullPC = ICPU.ShiftedPB | (uint32_t)(CPU.PCAtOpcodeStart - CPU.PCBase);
    uint8_t i;
    for (i = 0; i < dbg.execBPCount; i++) {
        if (dbg.execBP[i] == fullPC) {
            DBG_FireBreakpoint(DBG_TYPE_EXEC, fullPC, 0, fullPC);
            return;
        }
    }
}

/*
 * DBG_CheckWriteBreakpoint — inline fast path for WRAM write watchpoints.
 * wramOffset: byte offset into Memory.RAM (0x0000..0x1FFFF)
 * value:      the byte that was just written
 * Call this AFTER the write but before returning from S9xSetByte.
 */
static inline void DBG_CheckWriteBreakpoint(uint32_t wramOffset, uint8_t value)
{
    if (__builtin_expect(!dbg.hasWriteBP, 1)) return;
    uint8_t i;
    for (i = 0; i < dbg.writeBPCount; i++) {
        if (dbg.writeBP[i] == wramOffset) {
            uint32_t pc = ICPU.ShiftedPB | (uint32_t)(CPU.PCAtOpcodeStart - CPU.PCBase);
            DBG_FireBreakpoint(DBG_TYPE_WRITE, wramOffset, value, pc);
            return;
        }
    }
}

#endif /* DEBUGGER_H */

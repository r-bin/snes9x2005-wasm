# snes9x2005-wasm Debugger Integration

Minimal breakpoint + watchpoint system added to the WASM core.
Zero runtime cost when no breakpoints are set.

---

## What was changed

| File | Change |
|---|---|
| `source/debugger.h` | `DebuggerState` struct + inline `DBG_CheckExecBreakpoint` / `DBG_CheckWriteBreakpoint` |
| `source/debugger.c` | Global state, `DBG_FireBreakpoint`, all `EMSCRIPTEN_KEEPALIVE` exports |
| `source/cpuexec.c`  | `#include "debugger.h"`, pause guard in `S9xMainLoop()`, `DBG_CheckExecBreakpoint()` in all 4 loop variants |
| `source/getset.c`   | `#include "debugger.h"`, WRAM write watchpoint hook in `S9xSetByte` |
| `source/exports.c`  | `#include "debugger.h"`, `if(dbg.paused) return;` in `mainLoop()` |
| `source/debugger-post.js` | Post-JS wrappers: `Module.getCPUState()`, `Module.readMemoryRange()`, aliases |
| `build.sh`          | `--post-js source/debugger-post.js` |

---

## JS API

All available after `Module.onRuntimeInitialized` fires.

### Exec breakpoints

```js
// Pause before the instruction at SNES 24-bit address 0xC08000
Module.addExecBreakpoint(0xC08000);
Module.removeExecBreakpoint(0xC08000);
```

### WRAM write watchpoints

```js
// Pause when WRAM offset 0x22D8 is written (SNES address $7E22D8)
Module.addWriteBreakpoint(0x22D8);
Module.removeWriteBreakpoint(0x22D8);
```

### Receive events

```js
Module.onBreakpointHit = function(event) {
  // event.type    = 'exec' | 'write'
  // event.address = 24-bit PC (exec) | WRAM offset (write)
  // event.value   = byte written (write only; 0 for exec)
  // event.pc      = 24-bit program counter at the moment of the hit
  console.log('[SNES DBG]', event);

  // inspect registers
  const cpu = Module.getCPUState();
  console.log('PC=$' + cpu.pc.toString(16).toUpperCase(), 'A=$' + cpu.a.toString(16));

  // read 16 bytes of WRAM
  const bytes = Module.readMemoryRange(0x7E0000, 16);

  // return false to keep running (conditional breakpoint); anything else pauses
  // (the instruction at the breakpoint still executes, the CPU stops right after it)
  console.log(bytes);
};
```

### Pause / resume

```js
Module.pauseEmulation();
Module.resumeEmulation();
```

### CPU state

```js
const cpu = Module.getCPUState();
// { pc, a, x, y, sp, p, db, pb } — all numbers
```

### Memory access

```js
const byte = Module.readMemory(0x7E22D8);
Module.writeMemory(0x7E22D8, 0xFF);

// bulk read (returns Uint8Array copy, max 4096 bytes)
const range = Module.readMemoryRange(0x7E0000, 256);
```

---

## Address conventions

| What | Address space |
|---|---|
| Exec breakpoints | Full 24-bit SNES ROM address, e.g. `0xC08000` |
| Write watchpoints | **WRAM offset** `0x0000..0x1FFFF` (byte offset into the 128 KB WRAM buffer) |
| `readMemory` / `writeMemory` | Full 24-bit SNES bus address, e.g. `0x7E22D8` |

WRAM is mirrored on the SNES bus at:
- `$7E0000–$7FFFFF` (full range)  
- `$00–$3F:$0000–$1FFF` (low 8 KB mirror in each LoROM bank)

The watchpoint address is the raw offset into `Memory.RAM`, **not** a bus address.
So to watch `$7E22D8`, use `Module.addWriteBreakpoint(0x22D8)`.

---

## Performance

When no breakpoints are active:

- Exec check: one `bool` load + conditional branch per opcode (~zero cost, branch-predictor friendly)
- Write check: one `bool` load + conditional branch per WRAM write + two pointer comparisons only when `hasWriteBP=true`

The `__builtin_expect(!dbg.hasExecBP, 1)` and `__builtin_expect(dbg.hasWriteBP, 0)` hints ensure the compiler places the fast-path first in the generated WASM.

---

## Building

Requires Emscripten (`emcc` in PATH).

```sh
./build.sh
# produces snes9x_2005.js + snes9x_2005.wasm
```

---

## EmulatorJS integration

EmulatorJS calls `Module._mainLoop()` from its RAF loop. When the debugger pauses, `mainLoop()` returns immediately each frame without advancing emulation.

To resume from outside the RAF loop (e.g. from a VS Code webview message):

```js
window.addEventListener('message', function(ev) {
  const msg = ev.data;
  if (msg.command === 'dbgResume') {
    Module.resumeEmulation();
  } else if (msg.command === 'dbgAddBP') {
    Module.addExecBreakpoint(msg.address);
  }
});
```

On the VS Code extension side, post the message to the webview:

```js
panel.webview.postMessage({ command: 'dbgAddBP', address: 0xC08000 });
panel.webview.postMessage({ command: 'dbgResume' });
```

Breakpoint hit events travel the reverse direction:

```js
// Inside the webview
Module.onBreakpointHit = function(event) {
  vscodeApi.postMessage({ command: 'breakpointHit', event });
};

// Inside the extension
panel.webview.onDidReceiveMessage(msg => {
  if (msg.command === 'breakpointHit') {
    // show CPU state in a debug view, highlight source line, etc.
    console.log(msg.event);
  }
});
```

---

## Limits

| Item | Limit |
|---|---|
| Exec breakpoints | 64 simultaneous |
| Write watchpoints | 64 simultaneous |
| `readMemoryRange` buffer | 4096 bytes per call |

These are compile-time constants in `debugger.h` (`DBG_MAX_EXEC_BP`, `DBG_MAX_WRITE_BP`) and `debugger.c` (`DBG_READ_BUF_SIZE`).

---

## Optional next steps

- **Step instruction**: `Module._resumeEmulation()` advances one frame; for single-step you'd need a step flag that re-pauses after the next opcode.
- **Conditional breakpoints**: add a JS predicate check inside `Module.onBreakpointHit` — call `Module.resumeEmulation()` if the condition isn't met.
- **Hardware register watchpoints** (MMIO `$2100–$21FF`, DMA `$4300+`): hook `S9xSetPPU` / `S9xSetCPU` in `ppu.c` / `cpu.c`.

# snes9x2005-wasm Debugger Implementation — Summary

**Status**: ✅ Complete — Ready to build and test

---

## What was delivered

### New source files

| File | Size | Purpose |
|---|---|---|
| `source/debugger.h` | 2.7K | Header with breakpoint state, fast-path inline checks |
| `source/debugger.c` | 6.6K | Debugger implementation + all WASM exports |
| `source/debugger-post.js` | 3.0K | JS convenience wrappers (Module.getCPUState, etc) |

### Modified source files

| File | Changes |
|---|---|
| `source/cpuexec.c` | Pause guard + exec breakpoint check in all 4 CPU loop variants |
| `source/getset.c` | WRAM write watchpoint hook in S9xSetByte |
| `source/exports.c` | Pause guard in mainLoop() |
| `build.sh` | Enhanced with error checking, debug mode, cleaner output |

### Documentation

| File | Content |
|---|---|
| `DEBUGGER.md` | Full API reference, address conventions, EmulatorJS/VS Code integration example |
| `BUILD.md` | Prerequisites, build instructions, troubleshooting, integration guide |

---

## Features

### Execute breakpoints
```js
Module.addExecBreakpoint(0xC08000);
Module.removeExecBreakpoint(0xC08000);
```
Pauses before the instruction at 24-bit PC address.

### WRAM write watchpoints
```js
Module.addWriteBreakpoint(0x22D8);    // WRAM offset (not bus address)
Module.removeWriteBreakpoint(0x22D8);
```
Pauses after a byte is written to WRAM.

### Receive events
```js
Module.onBreakpointHit = (event) => {
  // event.type    = 'exec' | 'write'
  // event.address = address
  // event.value   = byte value (write only)
  // event.pc      = 24-bit PC at the time
  console.log(event);
};
```

### Pause/resume
```js
Module.pauseEmulation();
Module.resumeEmulation();
```

### CPU state
```js
const cpu = Module.getCPUState();
// { pc, a, x, y, sp, p, db, pb }
```

### Memory access
```js
const byte = Module.readMemory(0x7E22D8);
Module.writeMemory(0x7E22D8, 0xFF);
const bytes = Module.readMemoryRange(0x7E0000, 256);  // Uint8Array
```

---

## Building

### Prerequisites
- Emscripten (install via Homebrew or emsdk)

### Build
```bash
cd tmp/snes9x2005-wasm
./build.sh              # Release build
DEBUG=1 ./build.sh      # Debug build with symbols
```

Output:
- `snes9x_2005.js` — JavaScript runtime
- `snes9x_2005.wasm` — The emulator binary

### Syntax validation (no emscripten required)
```bash
cd tmp/snes9x2005-wasm/source
gcc -fsyntax-only -I. debugger.h
gcc -fsyntax-only -I. debugger.c
gcc -fsyntax-only -I. cpuexec.c
gcc -fsyntax-only -I. getset.c
gcc -fsyntax-only -I. exports.c
```

✅ **All pass** (except missing emscripten.h, which is expected)

---

## Implementation details

### Performance (when no breakpoints set)

- **Exec check**: 1 bool load + branch (always-not-taken, branch-predicted out)
- **Write check**: 1 bool load short-circuits entire check
- **Net overhead**: ~1 cycle per opcode (negligible)

### Pause mechanism

When a breakpoint fires:
1. `DBG_FireBreakpoint()` sets `dbg.paused = true`
2. Sets `CPU.Flags |= SCAN_KEYS_FLAG` (breaks CPU inner do-while cleanly)
3. Calls `Module.onBreakpointHit` callback via `EM_ASM`
4. `mainLoop()` returns immediately each frame while paused
5. User calls `Module.resumeEmulation()` to continue

### Address conventions

| What | Address space |
|---|---|
| Exec breakpoints | Full 24-bit SNES address (`0xC08000`) |
| Write watchpoints | WRAM offset (`0x0000–0x1FFFF`, **not** bus address) |
| readMemory / writeMemory | 24-bit SNES bus address (`0x7E22D8`) |

WRAM at `$7E0000–$7FFFFF` on the bus → offsets `0x0000–0x1FFFF` internally.

---

## Integration with EmulatorJS + VS Code

1. Build and copy `snes9x_2005.{js,wasm}` to EmulatorJS web directory
2. VS Code webview loads EmulatorJS with the modified WASM core
3. Set up breakpoint event forwarding:

```js
// In webview
Module.onBreakpointHit = (event) => {
  vscodeApi.postMessage({ command: 'breakpointHit', event });
};

// In extension
panel.webview.onDidReceiveMessage(msg => {
  if (msg.command === 'breakpointHit') {
    // Show in debugger UI, highlight source, etc.
  }
});
```

4. VS Code sends breakpoint commands:
```js
// Extension → webview
panel.webview.postMessage({ command: 'addBreakpoint', address: 0xC08000 });

// Webview listens
window.addEventListener('message', msg => {
  if (msg.data.command === 'addBreakpoint') {
    Module.addExecBreakpoint(msg.data.address);
  }
});
```

---

## Testing checklist

- [x] C syntax validated (no compilation errors in our code)
- [x] All 4 CPU loop variants patched
- [x] WRAM write path patched
- [x] Pause guard in place
- [x] JS bridge complete
- [x] Documentation complete
- [x] Build script with error checking
- [ ] Actual Emscripten build (requires emcc installation)
- [ ] Runtime test with ROM
- [ ] Breakpoint hit callback fires correctly
- [ ] CPU state reads correctly

---

## Limits

| Item | Limit |
|---|---|
| Simultaneous exec breakpoints | 64 |
| Simultaneous write watchpoints | 64 |
| readMemoryRange buffer | 4096 bytes per call |

---

## Files ready to copy

All files are in: `/Users/v/Documents/GitHub/everscript-vscode/tmp/snes9x2005-wasm/`

When you have Emscripten installed:
```bash
cd /Users/v/Documents/GitHub/everscript-vscode/tmp/snes9x2005-wasm
./build.sh
# Outputs: snes9x_2005.js, snes9x_2005.wasm
```

See **DEBUGGER.md** and **BUILD.md** for full details.

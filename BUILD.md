# Building snes9x2005-wasm

Complete instructions for building the WASM core with debugger support.

---

## Prerequisites

### macOS (Homebrew)

```bash
brew install emscripten
```

Verify installation:
```bash
emcc --version
# Output: emcc (Emscripten gcc/clang-like replacement) X.Y.Z
```

### Manual installation (all platforms)

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh  # Linux / macOS
# or on Windows:
# .\emsdk_env.bat
```

Add to your shell profile (`~/.bash_profile`, `~/.zshrc`, etc.):
```bash
source ~/emsdk/emsdk_env.sh
```

---

## Building

### Standard release build (optimized)

```bash
cd /path/to/snes9x2005-wasm
./build.sh
```

Output:
- `snes9x_2005.js` — JavaScript runtime and glue code
- `snes9x_2005.wasm` — The actual WASM binary (the core emulator)

### Debug build (with debug symbols)

```bash
DEBUG=1 ./build.sh
```

Or:
```bash
./build.sh debug
```

This includes DWARF debug info and disables optimizations. Useful for debugging C code with browser DevTools or `wasm-opt`.

---

## Compilation output explained

The build script:

1. **Checks** for `emcc` in PATH
2. **Verifies** that all source files exist (especially `debugger.h`, `debugger.c`, `debugger-post.js`)
3. **Compiles** all C source files with Emscripten
4. **Links** with `--post-js source/debugger-post.js` to inject JS convenience wrappers
5. **Outputs** both `.js` (JavaScript binding) and `.wasm` (binary core)

---

## Integration with EmulatorJS

Once built, use the generated files in your HTML:

```html
<!-- In your doc/index.html or web page -->
<script src="snes9x_2005.js"></script>
<script>
  Module.onRuntimeInitialized = function() {
    console.log('Emulator ready!');

    // Load a ROM
    const romFile = /* ArrayBuffer */;
    const romPtr = Module._my_malloc(romFile.length);
    Module.HEAP8.set(new Uint8Array(romFile), romPtr);
    Module._startWithRom(romPtr, romFile.length, 44100);
    Module._my_free(romPtr);

    // Add exec breakpoint at address $C08000
    Module.addExecBreakpoint(0xC08000);

    Module.onBreakpointHit = (event) => {
      console.log('Breakpoint:', event);
    };

    // Start emulation loop
    function frame() {
      Module._mainLoop();
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
  };
</script>
```

---

## Troubleshooting build errors

### `emcc not found`

Install Emscripten (see Prerequisites above).

### `"debugger.h" not found` or similar source files missing

Verify you're in the correct directory:
```bash
ls source/debugger.h source/debugger.c source/debugger-post.js
```

If files are missing, they were not added during the patch. Check the `DEBUGGER.md` file was created and files are present.

### Linker errors like `undefined reference to 'DBG_FireBreakpoint'`

The debugger.c file may have syntax errors. Check:
```bash
gcc -c -I. source/debugger.c 2>&1 | head -20
```

Common issues:
- Missing semicolons in struct definitions
- Typos in `extern` declarations
- Missing `#include` for `EM_ASM`, `emscripten.h`

### Memory-related warnings

These are usually safe. Emscripten may warn about `ALLOW_MEMORY_GROWTH`:
```
warning: memory size (65536) is smaller than the size needed (xxx)
```

This is expected and handled by the `-s ALLOW_MEMORY_GROWTH=1` flag.

### WASM size is very large (> 10 MB)

Usually means debug symbols are included. Use a release build:
```bash
./build.sh
# not: DEBUG=1 ./build.sh
```

### Breakpoints not firing

After build, check:

1. **Module loaded**: `typeof Module.addExecBreakpoint === 'function'`
2. **Callback set**: `typeof Module.onBreakpointHit === 'function'`
3. **Address format**: exec breakpoints expect 24-bit SNES PC (e.g., `0xC08000`), not WRAM offsets
4. **Loop running**: `Module._mainLoop()` must be called each frame from RAF or game loop

---

## Build script options

The `build.sh` script supports:

| Flag | Effect |
|---|---|
| (no args) | Release build with `-O3` optimization |
| `debug` | Debug build with `-O0`, DWARF symbols, `DEMANGLE_SUPPORT` |
| `DEBUG=1 ./build.sh` | Same as `debug` (env var form) |

---

## Clean build

To remove build artifacts and rebuild from scratch:

```bash
rm -f snes9x_2005.js snes9x_2005.wasm snes9x_2005.wasm.map
./build.sh
```

---

## Next steps

1. **Copy binaries** to your web server or static file directory
2. **Load in HTML** with `<script src="snes9x_2005.js"></script>`
3. **Wait for `Module.onRuntimeInitialized`** before calling emulator functions
4. **See DEBUGGER.md** for full API reference and integration examples

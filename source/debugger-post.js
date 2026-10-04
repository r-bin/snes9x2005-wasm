/**
 * debugger-post.js — convenience wrappers over the WASM debugger exports
 *
 * Appended to the generated JS by --post-js at build time.
 * All functions below are available as Module.xxx() after the module loads.
 *
 * Low-level entry points (Module._xxx) are still available as-is.
 */
(function () {
    /**
     * getCPUState() — returns a plain object with all CPU registers.
     *
     * Reads from the static uint32[8] buffer written by the C getCPUState().
     * Layout: [pc, a, x, y, sp, p, db, pb]
     */
    Module['getCPUState'] = function () {
        var bytePtr = Module._getCPUState();
        var idx     = bytePtr >>> 2;          // byte offset → uint32 index
        var h       = HEAPU32;
        return {
            pc : h[idx + 0],   // 24-bit program counter
            a  : h[idx + 1],
            x  : h[idx + 2],
            y  : h[idx + 3],
            sp : h[idx + 4],
            p  : h[idx + 5],   // status flags (packed)
            db : h[idx + 6],   // data bank
            pb : h[idx + 7]    // program bank
        };
    };

    /**
     * readMemoryRange(addr, size) — read a contiguous range from the SNES bus.
     * Returns a Uint8Array copy (up to 4096 bytes per call, C-side limit).
     */
    Module['readMemoryRange'] = function (addr, size) {
        var bytePtr = Module._readMemoryRange(addr >>> 0, size >>> 0);
        var cap     = Math.min(size, 4096);
        return new Uint8Array(HEAPU8.buffer, bytePtr, cap).slice();
    };

    /* Convenience aliases — identical to calling Module._xxx() directly,
     * but using camelCase names that match the spec. */
    Module['addExecBreakpoint']    = function (addr)  { Module._addExecBreakpoint(addr >>> 0);    };
    Module['removeExecBreakpoint'] = function (addr)  { Module._removeExecBreakpoint(addr >>> 0); };
    Module['addWriteBreakpoint']   = function (addr)  { Module._addWriteBreakpoint(addr >>> 0);   };
    Module['removeWriteBreakpoint']= function (addr)  { Module._removeWriteBreakpoint(addr >>> 0);};
    Module['pauseEmulation']       = function ()      { Module._pauseEmulation();                  };
    Module['resumeEmulation']      = function ()      { Module._resumeEmulation();                 };
    Module['readMemory']           = function (addr)  { return Module._readMemory(addr >>> 0);     };
    Module['writeMemory']          = function (addr, v){ Module._writeMemory(addr >>> 0, v & 0xFF);};
    Module['writeRomByte']         = function (off, v) { Module._writeRomByte(off >>> 0, v & 0xFF); };
    Module['readRomByte']          = function (off)    { return Module._readRomByte(off >>> 0);     };

    /**
     * onBreakpointHit — set this to receive breakpoint events:
     *
     *   Module.onBreakpointHit = function(event) {
     *     // event.type    = 'exec' | 'write'
     *     // event.address = SNES address (24-bit for exec, WRAM offset for write)
     *     // event.value   = byte value (write only; 0 for exec)
     *     // event.pc      = 24-bit program counter at the time of the hit
     *     console.log(event);
     *   };
     *
     * Default: no-op.
     */
    if (typeof Module['onBreakpointHit'] === 'undefined') {
        Module['onBreakpointHit'] = null;
    }
}());

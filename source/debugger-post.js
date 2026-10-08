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
    Module['isEmulationPaused']    = function ()      {
        return typeof Module._isEmulationPaused === 'function' && Module._isEmulationPaused() === 1;
    };

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

/**
 * CDL recorder (cdl.c). Off until cdlEnable(); everything lives in WASM memory.
 *
 *   Module.cdlEnable() / cdlDisable() / cdlIsEnabled()
 *   Module.cdlSeed(cdlBytes, extBytes, wflagBytes) - start from what the library already knows
 *   Module.cdlSetScriptContext(fetchRomOff, ptrWram, excludes[[lo, hi], ...])
 *   Module.cdlDrain() -> { size, chunks:[{index, cdl, ext}], wvals:[{index, data}],
 *                          wflags:[{index, data}], xrefs, edges, stats, scriptXrefs,
 *                          romHits [off, n], wramHits [addr, reads, writes],
 *                          rets [entry, retPc, mx<<8|flags], regs [pc, kind<<16|value],
 *                          bases [pc, base24, flags] (Uint32Array),
 *                          wcode:[{index, code, state}] (4 KB), aram:[{index, data}] (4 KB), dropped }
 *     Only what changed since the previous drain; hit counts are deltas (zeroed by the drain).
 *   Module.cdlView(peek) -> { cdl, ext, dirty, wflags, wdirty } live views for the display
 *     (dirty / wdirty = chunks changed since the last non-peek call; peek leaves them set).
 */
(function () {
    var CHUNK = 0x10000;

    function copy(ptr, len) { return new Uint8Array(HEAPU8.buffer, ptr, len).slice(); }

    function drainList(fn, words) {
        var n = fn();
        if (!n) return new Uint32Array(0);
        var p = Module._cdlOutPtr() >>> 2;
        return HEAPU32.slice(p, p + n * words);
    }

    // false when the core was built with -DEVS_CDL=0
    Module['cdlEnable']    = function () { return typeof Module._cdlEnable === 'function' && Module._cdlEnable() === 1; };
    Module['cdlDisable']   = function () { Module._cdlDisable(); };
    Module['cdlIsEnabled'] = function () { return Module._cdlIsEnabled() === 1; };

    Module['cdlSeed'] = function (cdlBytes, extBytes, wflagBytes) {
        var size = Module._cdlRomSize();
        if (!size) return;
        if (cdlBytes) HEAPU8.set(cdlBytes.subarray(0, size), Module._cdlRomPtr());
        if (extBytes) HEAPU8.set(extBytes.subarray(0, size), Module._cdlExtPtr());
        if (wflagBytes && wflagBytes.length) HEAPU8.set(wflagBytes.subarray(0, 0x20000), Module._cdlWflagPtr());
        HEAPU8.fill(1, Module._cdlViewDirtyPtr(), Module._cdlViewDirtyPtr() + 128);
        HEAPU8.fill(1, Module._cdlWflagViewDirtyPtr(), Module._cdlWflagViewDirtyPtr() + 32);
    };

    Module['cdlSetScriptContext'] = function (fetchRomOff, ptrWram, excludes) {
        Module._cdlClearScriptExcludes();
        (excludes || []).forEach(function (r) { Module._cdlAddScriptExclude(r[0] >>> 0, r[1] >>> 0); });
        Module._cdlSetScriptContext(fetchRomOff | 0, ptrWram >>> 0);
    };

    Module['cdlDrain'] = function () {
        var size = Module._cdlRomSize();
        var out = { size: size, chunks: [], wvals: [] };
        if (!size) return out;
        var dirty = Module._cdlFlushDirtyPtr();
        var romP = Module._cdlRomPtr(), extP = Module._cdlExtPtr();
        for (var i = 0; i * CHUNK < size; i++) {
            if (!HEAPU8[dirty + i]) continue;
            HEAPU8[dirty + i] = 0;
            var len = Math.min(CHUNK, size - i * CHUNK);
            out.chunks.push({ index: i, cdl: copy(romP + i * CHUNK, len), ext: copy(extP + i * CHUNK, len) });
        }
        var wd = Module._cdlWvalDirtyPtr(), wp = Module._cdlWvalPtr();
        for (var j = 0; j < 512; j++) {
            if (!HEAPU8[wd + j]) continue;
            HEAPU8[wd + j] = 0;
            out.wvals.push({ index: j, data: copy(wp + j * 256 * 32, 256 * 32) });
        }
        out.wflags = [];
        var fd = Module._cdlWflagFlushDirtyPtr(), fp = Module._cdlWflagPtr();
        for (var k = 0; k < 32; k++) {
            if (!HEAPU8[fd + k]) continue;
            HEAPU8[fd + k] = 0;
            out.wflags.push({ index: k, data: copy(fp + k * 4096, 4096) });
        }
        out.xrefs = drainList(Module._cdlDrainXrefs, 3);
        out.scriptXrefs = drainList(Module._cdlDrainScriptXrefs, 3);
        out.edges = drainList(Module._cdlDrainEdges, 3);
        out.stats = drainList(Module._cdlDrainStats, 5);
        out.romHits = typeof Module._cdlDrainRomHits === 'function' ? drainList(Module._cdlDrainRomHits, 2) : new Uint32Array(0);
        out.wramHits = typeof Module._cdlDrainWramHits === 'function' ? drainList(Module._cdlDrainWramHits, 3) : new Uint32Array(0);
        var opt = function (name, words) { return typeof Module[name] === 'function' ? drainList(Module[name], words) : new Uint32Array(0); };
        out.rets = opt('_cdlDrainRets', 3);
        out.regs = opt('_cdlDrainRegs', 2);
        out.bases = opt('_cdlDrainBases', 3);
        out.wcode = [];
        if (typeof Module._cdlWcodeDirtyPtr === 'function') {
            var cd = Module._cdlWcodeDirtyPtr(), cp = Module._cdlWcodePtr(), cs = Module._cdlWcodeStatePtr();
            for (var c = 0; c < 32; c++) {
                if (!HEAPU8[cd + c]) continue;
                HEAPU8[cd + c] = 0;
                out.wcode.push({ index: c, code: copy(cp + c * 4096, 4096), state: copy(cs + c * 4096, 4096) });
            }
        }
        out.aram = [];
        if (typeof Module._cdlAramDirtyPtr === 'function') {
            var ad = Module._cdlAramDirtyPtr(), ap = Module._cdlAramPtr();
            for (var q = 0; q < 16; q++) {
                if (!HEAPU8[ad + q]) continue;
                HEAPU8[ad + q] = 0;
                out.aram.push({ index: q, data: copy(ap + q * 4096, 4096) });
            }
        }
        out.dropped = typeof Module._cdlDroppedCount === 'function' ? Module._cdlDroppedCount() : 0;
        return out;
    };

    Module['cdlView'] = function (peek) {
        var size = Module._cdlRomSize();
        if (!size) return null;
        var d = Module._cdlViewDirtyPtr(), wd = Module._cdlWflagViewDirtyPtr();
        var dirty = HEAPU8.slice(d, d + 128), wdirty = HEAPU8.slice(wd, wd + 32);
        if (!peek) { HEAPU8.fill(0, d, d + 128); HEAPU8.fill(0, wd, wd + 32); }
        return {
            size: size,
            cdl: new Uint8Array(HEAPU8.buffer, Module._cdlRomPtr(), size),
            ext: new Uint8Array(HEAPU8.buffer, Module._cdlExtPtr(), size),
            wflags: new Uint8Array(HEAPU8.buffer, Module._cdlWflagPtr(), 0x20000),
            dirty: dirty,
            wdirty: wdirty,
        };
    };
}());

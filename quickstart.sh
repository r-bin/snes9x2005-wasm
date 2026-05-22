#!/bin/bash

# Quick-start: Build snes9x2005-wasm with debugger support
# Copy this to your terminal and run

cd /Users/v/Documents/GitHub/everscript-vscode/tmp/snes9x2005-wasm

# 1. Check if Emscripten is installed
if ! command -v emcc &> /dev/null; then
    echo "Emscripten not found. Install it first:"
    echo "  brew install emscripten"
    exit 1
fi

echo "=========================================="
echo "snes9x2005-wasm Debugger Build"
echo "=========================================="
echo ""

# 2. Show what we're building
echo "[1/3] Checking prerequisites..."
echo "  - Emscripten: $(emcc -v 2>&1 | head -1)"
echo "  - Source files: OK ($(ls source/debugger.{h,c} | wc -l) debugger files)"
echo ""

# 3. Build
echo "[2/3] Building (this may take 30-60 seconds)..."
chmod +x build.sh
./build.sh
BUILD_RESULT=$?

# 4. Summary
echo ""
if [ $BUILD_RESULT -eq 0 ]; then
    echo "[3/3] Build successful!"
    echo ""
    echo "Generated files:"
    ls -lh snes9x_2005.js snes9x_2005.wasm 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
    echo ""
    echo "Next steps:"
    echo "  1. Read DEBUGGER.md for the full JS API"
    echo "  2. Read BUILD.md for integration details"
    echo "  3. Copy snes9x_2005.{js,wasm} to your web directory"
    echo "  4. Load in HTML: <script src=\"snes9x_2005.js\"></script>"
    echo ""
    echo "Quick API test (in browser console):"
    echo "  Module.addExecBreakpoint(0xC08000);"
    echo "  Module.onBreakpointHit = console.log;"
    echo ""
else
    echo "[3/3] Build FAILED"
    echo "See output above for errors."
    exit 1
fi

#!/bin/bash

# Build script for snes9x2005-wasm
# Compiles the SNES emulator core + debugger system via Emscripten

set -e
cd "$(dirname "$0")"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}[snes9x2005-wasm]${NC} Build script"

# ============================================================================
# Check Emscripten installation
# ============================================================================

if ! command -v emcc &> /dev/null; then
    echo -e "${RED}ERROR${NC}: emcc not found in PATH"
    echo ""
    echo "Emscripten is required to build snes9x2005-wasm."
    echo ""
    echo "Installation (macOS with Homebrew):"
    echo "  brew install emscripten"
    echo ""
    echo "Or from source:"
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk && ./emsdk install latest && ./emsdk activate latest"
    echo "  source ./emsdk_env.sh"
    echo ""
    exit 1
fi

EMCC_VERSION=$(emcc -v 2>&1 | head -1)
echo -e "${GREEN}✓${NC} Found: $EMCC_VERSION"

# ============================================================================
# Check source files exist
# ============================================================================

if [ ! -f source/debugger.h ] || [ ! -f source/debugger.c ]; then
    echo -e "${RED}ERROR${NC}: debugger files not found"
    echo "Expected: source/debugger.h, source/debugger.c, source/debugger-post.js"
    exit 1
fi

if [ ! -f source/debugger-post.js ]; then
    echo -e "${RED}ERROR${NC}: source/debugger-post.js not found"
    exit 1
fi

echo -e "${GREEN}✓${NC} Source files verified"

# ============================================================================
# Build configuration
# ============================================================================

OPTIMIZE="-O3"
DEBUG_FLAGS=""

# Optional: enable debug mode with -g flag or DEBUG=1 env var
if [ "$DEBUG" = "1" ] || [ "$1" = "debug" ]; then
    OPTIMIZE="-O0"
    DEBUG_FLAGS="-g4 -s DEMANGLE_SUPPORT=1"
    echo -e "${YELLOW}[Debug mode]${NC} Building with debug symbols..."
else
    echo -e "${YELLOW}[Release mode]${NC} Building optimized..."
fi

# ============================================================================
# Compilation
# ============================================================================

OUTFILE="snes9x_2005.js"
OUTFILE_WASM="${OUTFILE%.js}.wasm"

echo -e "${YELLOW}[Compiling]${NC} source/*.c → ${OUTFILE}"
echo ""

# Emscripten flags:
#   -O3                          : aggressive optimizations (or -O0 for debug)
#   -s WASM=1                    : generate WebAssembly (not asm.js)
#   -s EXPORTED_RUNTIME_METHODS='["cwrap"]' : allow JS to call C functions (Emscripten 5.0+)
#   -s ALLOW_MEMORY_GROWTH=1     : allow heap to grow dynamically
#   -g4 (debug only)             : generate DWARF debug info
#   --post-js debugger-post.js   : append JS convenience wrappers
#   source/*.c                   : all C source files
#   -o snes9x_2005.js            : output JS + WASM

emcc $OPTIMIZE $DEBUG_FLAGS \
    -s WASM=1 \
    -s EXPORTED_RUNTIME_METHODS='["cwrap"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    --post-js source/debugger-post.js \
    source/*.c \
    -o "$OUTFILE"

BUILD_EXIT=$?

# ============================================================================
# Verify output
# ============================================================================

echo ""

if [ $BUILD_EXIT -eq 0 ]; then
    if [ -f "$OUTFILE" ] && [ -f "$OUTFILE_WASM" ]; then
        JS_SIZE=$(ls -lh "$OUTFILE" | awk '{print $5}')
        WASM_SIZE=$(ls -lh "$OUTFILE_WASM" | awk '{print $5}')
        echo -e "${GREEN}✓ Build successful!${NC}"
        echo -e "  ${OUTFILE}       : ${JS_SIZE}"
        echo -e "  ${OUTFILE_WASM}  : ${WASM_SIZE}"
    else
        echo -e "${RED}ERROR${NC}: Output files not created"
        exit 1
    fi
else
    echo -e "${RED}ERROR${NC}: Compilation failed (exit code: $BUILD_EXIT)"
    exit $BUILD_EXIT
fi

echo ""
echo -e "${GREEN}Next steps:${NC}"
echo "  1. Copy snes9x_2005.js and snes9x_2005.wasm to your web directory"
echo "  2. Load snes9x_2005.js in your HTML"
echo "  3. Set up breakpoints via Module.addExecBreakpoint(0xNNNNNN)"
echo ""
echo "See DEBUGGER.md for the full API and examples."
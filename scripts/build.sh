#!/bin/bash
# doom_se30 build script
# Cross-compiles with Retro68 and deploys to Basilisk II shared folder
#
# Usage:
#   bash scripts/build.sh src           # debug build (Retro68 console enabled)
#   bash scripts/build.sh src release   # release build (no console window, DOOM_RELEASE_BUILD=1)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RETRO68_BUILD="$PROJECT_DIR/Retro68-build"
TOOLCHAIN="$RETRO68_BUILD/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake"
SHARED_DIR="$PROJECT_DIR/emulator/shared"
BUILD_DIR="$PROJECT_DIR/build"

# Which sub-project to build (default: hello_world)
TARGET="${1:-hello_world}"
TARGET_DIR="$PROJECT_DIR/$TARGET"

# Release mode: pass 'release' as second arg
MODE="${2:-debug}"
if [ "$MODE" = "release" ]; then
    RELEASE_FLAG="-DDOOM_RELEASE_BUILD=1"
    BUILD_LABEL="RELEASE"
    BUILD_TARGET_DIR="$BUILD_DIR/${TARGET}_release"
else
    RELEASE_FLAG=""
    BUILD_LABEL="DEBUG"
    BUILD_TARGET_DIR="$BUILD_DIR/$TARGET"
fi

if [ ! -f "$TOOLCHAIN" ]; then
    echo "ERROR: Retro68 toolchain not found at $TOOLCHAIN"
    echo "Has the Retro68 build completed? Check with:"
    echo "  tail -f $PROJECT_DIR/Retro68-build/../build.log"
    exit 1
fi

if [ ! -d "$TARGET_DIR" ]; then
    echo "ERROR: Target directory not found: $TARGET_DIR"
    exit 1
fi

echo "=== doom_se30 build ==="
echo "Target:    $TARGET"
echo "Mode:      $BUILD_LABEL"
echo "Toolchain: $TOOLCHAIN"
echo "Output:    $SHARED_DIR"
echo ""

# Configure (always re-configure for release to ensure DOOM_RELEASE_BUILD propagates;
# for debug, only configure if Makefile is missing)
mkdir -p "$BUILD_TARGET_DIR"

if [ "$MODE" = "release" ] || [ ! -f "$BUILD_TARGET_DIR/Makefile" ]; then
    echo "--- Configuring with CMake ($BUILD_LABEL) ---"
    cmake -S "$TARGET_DIR" \
          -B "$BUILD_TARGET_DIR" \
          -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
          $RELEASE_FLAG \
          2>&1
fi

# Build
echo "--- Building ($BUILD_LABEL) ---"
cmake --build "$BUILD_TARGET_DIR" 2>&1

# Find the output binary (Retro68 produces .bin or the app directly)
echo ""
echo "--- Build output ---"
ls -lh "$BUILD_TARGET_DIR/"*.bin 2>/dev/null || ls -lh "$BUILD_TARGET_DIR/"* 2>/dev/null | grep -v "CMake\|cmake\|Makefile\|\.a\|\.o" | head -20

# Deploy to shared folder
mkdir -p "$SHARED_DIR"
# Pick the main .bin (app MacBinary), not .rsrc.bin or .code.bin
BIN_FILE=$(ls "$BUILD_TARGET_DIR/"*.bin 2>/dev/null | grep -v '\.rsrc\.bin' | grep -v '\.code\.bin' | head -1)
if [ -n "$BIN_FILE" ]; then
    cp "$BIN_FILE" "$SHARED_DIR/"
    echo ""
    echo "--- Deployed ($BUILD_LABEL) ---"
    echo "  $(basename "$BIN_FILE") → $SHARED_DIR/"
    echo ""
    echo "File is now available in the Basilisk II shared folder."
    echo "In the emulator: open the 'Unix' volume and double-click the app."
else
    echo ""
    echo "WARNING: No .bin output found. Check build output above."
    echo "Build directory contents:"
    ls -la "$BUILD_TARGET_DIR/"
fi

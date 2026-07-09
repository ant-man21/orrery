#!/usr/bin/env bash
# =============================================================================
# build.sh — EDK2 firmware builder
# Usage: ./build.sh [-r|-d] [-c CHIP] [-C]
#
#   -r          Release build         (default)
#   -d          Debug build
#   -c CHIP     Chip target           (default: q35)
#   -C          Clean before build
#   -h          Show this help
#
# Chip configs live in:  chips/<CHIP>/chip.conf
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2_DIR="$(cd "$SCRIPT_DIR/../edk2" && pwd)"

# ---------- defaults ----------------------------------------------------------
BUILD_TYPE="RELEASE"
CHIP="q35"
CLEAN=0

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}

while getopts ":rdc:Ch" opt; do
    case $opt in
        r) BUILD_TYPE="RELEASE" ;;
        d) BUILD_TYPE="DEBUG"   ;;
        c) CHIP="$OPTARG"       ;;
        C) CLEAN=1              ;;
        h) usage ;;
        :) echo "ERROR: -$OPTARG requires an argument." >&2; exit 1 ;;
       \?) echo "ERROR: Unknown flag -$OPTARG" >&2; exit 1 ;;
    esac
done

# ---------- load chip config --------------------------------------------------
CHIP_CONF="$SCRIPT_DIR/chips/$CHIP/chip.conf"
if [[ ! -f "$CHIP_CONF" ]]; then
    echo "ERROR: No chip config found at $CHIP_CONF"
    echo "Available chips:"
    ls "$SCRIPT_DIR/chips/" 2>/dev/null | sed 's/^/  /'
    exit 1
fi

# shellcheck source=/dev/null
source "$CHIP_CONF"
# Expected variables:
#   ARCH, TOOLCHAIN, DSC, OUTPUT_DIR

# ---------- sanity checks -----------------------------------------------------
if [[ ! -f "$EDK2_DIR/edksetup.sh" ]]; then
    echo "ERROR: edksetup.sh not found in $EDK2_DIR"
    exit 1
fi

echo "============================================================"
echo "  Chip      : $CHIP"
echo "  Arch      : $ARCH"
echo "  Toolchain : $TOOLCHAIN"
echo "  DSC       : $DSC"
echo "  Build     : $BUILD_TYPE"
echo "  Clean     : $CLEAN"
echo "============================================================"

# ---------- build setup -------------------------------------------------------
cd "$EDK2_DIR"

export EDK_TOOLS_PATH="$EDK2_DIR/BaseTools"
export PACKAGES_PATH="$EDK2_DIR:$(dirname "$EDK2_DIR")"
export PYTHON_COMMAND=python3

set +u
source "$EDK2_DIR/edksetup.sh" --reconfig
set -u

export WORKSPACE="$(dirname "$EDK2_DIR")"

# ---------- clean -------------------------------------------------------------
if [[ "$CLEAN" -eq 1 ]]; then
    CLEAN_DIR="$WORKSPACE/$OUTPUT_DIR"

    echo "🧹 Cleaning target build directory:"
    echo "   $CLEAN_DIR"

    if [[ -d "$CLEAN_DIR" ]]; then
        rm -rf "$CLEAN_DIR"
        echo "✓ Clean complete"
    else
        echo "Nothing to clean (directory does not exist)"
    fi

    echo "Exiting (clean-only mode)"
    exit 0
fi

# ---------- build -------------------------------------------------------------
build \
    -a "$ARCH" \
    -t "$TOOLCHAIN" \
    -b "$BUILD_TYPE" \
    -p "$DSC" \
    -n "$(nproc)"

echo ""
echo "✓ Build complete → $EDK2_DIR/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/FV/"
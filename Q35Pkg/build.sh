#!/usr/bin/env bash
# =============================================================================
# build.sh — EDK2 firmware builder
# Usage: ./build.sh [-r|-d] [-c CHIP]
#
#   -r          Release build         (default)
#   -d          Debug build
#   -c CHIP     Chip target           (default: q35)
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

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}

while getopts ":rdc:h" opt; do
    case $opt in
        r) BUILD_TYPE="RELEASE" ;;
        d) BUILD_TYPE="DEBUG"   ;;
        c) CHIP="$OPTARG"       ;;
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
# Expected variables from chip.conf:
#   ARCH        e.g. X64
#   TOOLCHAIN   e.g. GCC5
#   DSC         e.g. OvmfPkg/OvmfPkgX64.dsc
#   OUTPUT_DIR  e.g. Build/OvmfX64

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
echo "============================================================"

# ---------- build -------------------------------------------------------------
cd "$EDK2_DIR"
export PYTHON_COMMAND=python3
# edksetup.sh uses unbound vars internally; suspend strict mode around it
set +u
source edksetup.sh --reconfig
set -u

build \
    -a "$ARCH" \
    -t "$TOOLCHAIN" \
    -b "$BUILD_TYPE" \
    -p "$DSC" \
    -n "$(nproc)"

echo ""
echo "✓ Build complete → $EDK2_DIR/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/FV/"
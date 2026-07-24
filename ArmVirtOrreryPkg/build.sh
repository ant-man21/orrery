#!/usr/bin/env bash
# =============================================================================
# build.sh — EDK2 firmware builder for ArmVirtOrreryPkg (QEMU virt, AArch64)
# Usage: ./build.sh [-r|-d] [-C]
#
#   -r          Release build         (default)
#   -d          Debug build
#   -C          Clean before build
#   -h          Show this help
#
# Build-verification target only for now — no qemu.sh/run flow here yet.
# TpmProvisionApp/TpmVerifyBootApp (from OrreryPkg) still hardcode an
# X64/OVMF flash address (see their source), so this platform isn't
# bootable as-is.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2_DIR="$(cd "$SCRIPT_DIR/../edk2" && pwd)"

# ---------- platform (fixed — this script only builds ArmVirtOrreryPkg) -------
ARCH="AARCH64"
TOOLCHAIN="GCC"
DSC="ArmVirtOrreryPkg/ArmVirtOrreryPkg.dsc"
OUTPUT_DIR="Build/ArmVirtQemu-AArch64"
export GCC_AARCH64_PREFIX=/usr/bin/aarch64-linux-gnu-

# ---------- defaults ------------------------------------------------------
BUILD_TYPE="RELEASE"
CLEAN=0

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
while getopts ":rdCh" opt; do
    case $opt in
        r) BUILD_TYPE="RELEASE" ;;
        d) BUILD_TYPE="DEBUG"   ;;
        C) CLEAN=1              ;;
        h) usage ;;
        :) echo "ERROR: -$OPTARG requires an argument." >&2; exit 1 ;;
       \?) echo "ERROR: Unknown flag -$OPTARG" >&2; exit 1 ;;
    esac
done

# ---------- sanity checks -----------------------------------------------------
if [[ ! -f "$EDK2_DIR/edksetup.sh" ]]; then
    echo "ERROR: edksetup.sh not found in $EDK2_DIR"
    exit 1
fi

echo "============================================================"
echo "  Platform  : ArmVirt (QEMU virt / AArch64)"
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
    -n "$(nproc)" \
    -D TPM2_ENABLE=TRUE

echo ""
echo "✓ Build complete → $WORKSPACE/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/FV/"

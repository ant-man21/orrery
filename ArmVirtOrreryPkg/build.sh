#!/usr/bin/env bash
# =============================================================================
# build.sh — EDK2 firmware builder for ArmVirtOrreryPkg (QEMU virt, AArch64)
# Usage: ./build.sh [-r|-d] [-C] [-s|-S]
#
#   -r          Release build         (default)
#   -d          Debug build
#   -C          Clean before build
#   -h          Show this help
#   -s          Sync .efi outputs → shared/apps/ + shared.img after build  (default: on)
#   -S          Skip sync
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
SYNC=1          # on by default; -S turns it off

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
        s) SYNC=1               ;;
        S) SYNC=0               ;;
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

# ---------- uefi shell image (fs0:) --------------------------------------------
# uefi-shell.img is gitignored — build it on first run (or if it's missing)
# so it's always there without a manual step.
UEFI_SHELL_IMG="$SCRIPT_DIR/uefi-shell.img"
if [[ ! -f "$UEFI_SHELL_IMG" ]]; then
    "$SCRIPT_DIR/../tools/make-uefi-shell-img.sh" -a
else 
    echo "✓ $SCRIPT_DIR/uefi-shell.img already generated"
fi

# ---------- post-build sync ---------------------------------------------------
# Copy application .efi files (not firmware blobs) to shared/apps/ so the
# next ./run.sh --reset-shared picks them up automatically on fs1:\apps\.
#
# Excluded by name:  OVMF*.fd, *.fd (firmware volumes)
# Excluded by path:  anything under .../FV/  (firmware volume outputs)
if [[ "$SYNC" -eq 1 ]]; then
    APPS_DIR="$SCRIPT_DIR/shared/apps"
    mkdir -p "$APPS_DIR"

    BUILD_OUT="$WORKSPACE/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/$ARCH"
    SYNC_LIST="$SCRIPT_DIR/shared/.sync"

    if [[ ! -f "$SYNC_LIST" ]]; then
        echo "  (no shared/.sync found — skipping EFI sync)"
        echo "  Create shared/.sync with one .efi basename per line to enable"
    else
        mapfile -t EFI_NAMES < <(grep -v '^\s*#' "$SYNC_LIST" | grep -v '^\s*$')

        echo ""
        echo "→ Syncing ${#EFI_NAMES[@]} .efi file(s) from shared/.sync:"
        for name in "${EFI_NAMES[@]}"; do
            hit=$(find "$BUILD_OUT" -name "$name" ! -path "*/FV/*" 2>/dev/null | head -1)
            if [[ -n "$hit" ]]; then
                cp "$hit" "$APPS_DIR/$name"
                echo "  ✓ $name"
            else
                echo "  ✗ $name  (not found in build output — built yet?)"
            fi
        done
    fi

    # ---------- push into shared.img in place --------------------------------
    SHARED_IMG="$SCRIPT_DIR/shared.img"
    SHARED_SIZE_MB=256
    mkdir -p "$SCRIPT_DIR/shared/data"

    if [[ ! -f "$SHARED_IMG" ]]; then
        echo "→ shared.img not found — creating fresh (${SHARED_SIZE_MB} MB FAT32)..."
        dd if=/dev/zero of="$SHARED_IMG" bs=1M count="$SHARED_SIZE_MB" status=none
        mformat -i "$SHARED_IMG" -F -v SHARED ::
        mcopy -i "$SHARED_IMG" -s "$SCRIPT_DIR/shared"/* ::
        echo "✓ shared.img created → $SHARED_IMG"
    else
        shopt -s nullglob
        apps=("$APPS_DIR"/*)
        shopt -u nullglob
        if [[ ${#apps[@]} -gt 0 ]]; then
            echo "→ Pushing into shared.img (existing files preserved)..."
            mcopy -o -i "$SHARED_IMG" "${apps[@]}" ::apps/
            echo "✓ shared.img updated → $SHARED_IMG"
        fi
    fi
fi

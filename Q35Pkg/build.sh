#!/usr/bin/env bash
# =============================================================================
# build.sh — EDK2 firmware builder
# Usage: ./build.sh [-r|-d] [-c CHIP] [-C] [-s]
#
#   -r          Release build         (default)
#   -d          Debug build
#   -c CHIP     Chip target           (default: q35)
#   -C          Clean before build
#   -s          Sync .efi outputs → shared/apps/ + shared.img after build  (default: on)
#   -S          Skip sync
#   -h          Show this help
#
# Chip configs live in:  chips/<CHIP>/chip.conf
#
# Post-build sync:
#   .efi files named in shared/.sync are copied to Q35Pkg/shared/apps/, then
#   pushed into shared.img *in place* (mcopy -o) so they show up on fs1: on
#   the next run without needing qemu.sh --reset-shared — which wipes and
#   rebuilds the whole image from shared/, destroying anything the VM wrote
#   there (sealed blobs, output files, etc). shared.img is only fully
#   (re)created here if it doesn't exist yet.
#   From the UEFI shell those are visible as:
#     fs1:\apps\TpmProvisionApp.efi   (etc.)
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2_DIR="$(cd "$SCRIPT_DIR/../edk2" && pwd)"

# ---------- defaults ----------------------------------------------------------
BUILD_TYPE="RELEASE"
CHIP="q35"
CLEAN=0
SYNC=1          # on by default; -S turns it off

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
while getopts ":rdc:CsSh" opt; do
    case $opt in
        r) BUILD_TYPE="RELEASE" ;;
        d) BUILD_TYPE="DEBUG"   ;;
        c) CHIP="$OPTARG"       ;;
        C) CLEAN=1              ;;
        s) SYNC=1               ;;
        S) SYNC=0               ;;
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

case "$ARCH" in
    X64)    export GCC_BIN=/usr/bin/x86_64-linux-gnu-             ;;
    AARCH64)export GCC_AARCH64_PREFIX=/usr/bin/aarch64-linux-gnu- ;;
    ARM)    export GCC_ARM_PREFIX=/usr/bin/arm-linux-gnueabihf-   ;;
esac
# NOTE: EDK2's GCC toolchain family only reads GCC_BIN for X64/IA32 — cross
# targets need their own *_PREFIX env var (see BaseTools/Conf/tools_def.template).
# Expected variables from chip.conf:
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
echo "  Sync EFIs : $SYNC"
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
echo "✓ Build complete → $EDK2_DIR/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/FV/"

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
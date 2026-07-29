#!/usr/bin/env bash
# =============================================================================
# make-uefi-shell-img.sh — build Q35Pkg/uefi-shell.img (X64 UEFI Shell, fs0:)
# Usage: ./tools/make-uefi-shell-img.sh [-f]
#
#   -f    Force rebuild even if uefi-shell.img already exists
#   -h    Show this help
#
# uefi-shell.img is gitignored — every dev builds their own. This script
# builds ShellPkg out of the edk2 submodule and packs Shell.efi into a raw
# FAT image at EFI/BOOT/BOOTX64.EFI, so it's the default UEFI boot option
# and auto-launches into the shell on fs0: with a fresh OVMF_VARS.fd.
#
# qemu.sh calls this automatically when uefi-shell.img is missing, so you
# normally don't need to run it by hand — it's here for a manual rebuild
# (e.g. after updating the edk2 submodule).
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EDK2_DIR="$(cd "$REPO_ROOT/edk2" && pwd)"

ARCH="X64"
TOOLCHAIN="GCC"
BUILD_TYPE="RELEASE"
DSC="ShellPkg/ShellPkg.dsc"
SHELL_IMG="$REPO_ROOT/Q35Pkg/uefi-shell.img"
SHELL_IMG_SIZE_MB=64
export GCC_BIN=/usr/bin/x86_64-linux-gnu-

FORCE=0
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
while getopts ":fh" opt; do
    case $opt in
        f) FORCE=1 ;;
        h) usage ;;
       \?) echo "ERROR: Unknown flag -$OPTARG" >&2; exit 1 ;;
    esac
done

if [[ -f "$SHELL_IMG" && "$FORCE" -ne 1 ]]; then
    echo "✓ $SHELL_IMG already exists (use -f to force a rebuild)"
    exit 0
fi

for tool in mformat mmd mcopy; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: '$tool' not found — install mtools" >&2
        exit 1
    fi
done

if [[ ! -f "$EDK2_DIR/edksetup.sh" ]]; then
    echo "ERROR: edksetup.sh not found in $EDK2_DIR"
    exit 1
fi

echo "============================================================"
echo "  Building  : UEFI Shell ($ARCH / $TOOLCHAIN / $BUILD_TYPE)"
echo "  DSC       : $DSC"
echo "  Output    : $SHELL_IMG"
echo "============================================================"

cd "$EDK2_DIR"
export EDK_TOOLS_PATH="$EDK2_DIR/BaseTools"
export PACKAGES_PATH="$EDK2_DIR:$(dirname "$EDK2_DIR")"
export PYTHON_COMMAND=python3

set +u
source "$EDK2_DIR/edksetup.sh" --reconfig
set -u

export WORKSPACE="$(dirname "$EDK2_DIR")"

build \
    -a "$ARCH" \
    -t "$TOOLCHAIN" \
    -b "$BUILD_TYPE" \
    -p "$DSC" \
    -n "$(nproc)"

SHELL_EFI="$WORKSPACE/Build/Shell/${BUILD_TYPE}_${TOOLCHAIN}/$ARCH/Shell.efi"
if [[ ! -f "$SHELL_EFI" ]]; then
    echo "ERROR: build succeeded but Shell.efi not found at $SHELL_EFI" >&2
    exit 1
fi

echo ""
echo "→ Packing Shell.efi into ${SHELL_IMG_SIZE_MB} MB raw FAT image..."
dd if=/dev/zero of="$SHELL_IMG" bs=1M count="$SHELL_IMG_SIZE_MB" status=none
mformat -i "$SHELL_IMG" -F -v UEFISHELL ::
mmd -i "$SHELL_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$SHELL_IMG" "$SHELL_EFI" ::/EFI/BOOT/BOOTX64.EFI

echo "✓ $SHELL_IMG ready"

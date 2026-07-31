#!/usr/bin/env bash
# =============================================================================
# make-uefi-shell-img.sh — build uefi-shell.img (UEFI Shell, fs0:)
# Usage: ./tools/make-uefi-shell-img.sh [-f] [-a]
#
#   -f    Force rebuild even if uefi-shell.img already exists
#   -a    Build AARCH64 shell for ArmVirtOrreryPkg (default: X64 for Q35Pkg)
#   -h    Show this help
#
# uefi-shell.img is gitignored — every dev builds their own. This script
# builds ShellPkg out of the edk2 submodule and packs Shell.efi into a raw
# FAT image at EFI/BOOT/BOOTX64.EFI (or BOOTAA64.EFI for -a), so it's the
# default UEFI boot option and auto-launches into the shell on fs0: with a
# fresh OVMF_VARS.fd.
#
# build.sh calls this automatically when uefi-shell.img is missing, so you
# normally don't need to run it by hand — it's here for a manual rebuild
# (e.g. after updating the edk2 submodule).
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EDK2_DIR="$(cd "$REPO_ROOT/edk2" && pwd)"

# ---------- defaults ------------------------------------------------------
ARCH="X64"
TOOLCHAIN="GCC"
BUILD_TYPE="RELEASE"
DSC="ShellPkg/ShellPkg.dsc"
SHELL_IMG_SIZE_MB=64

# ---------- args --------------------------------------------------------------
FORCE=0
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
while getopts ":fah" opt; do
    case $opt in
        f) FORCE=1 ;;
        a) ARCH="AARCH64" ;;
        h) usage ;;
       \?) echo "ERROR: Unknown flag -$OPTARG" >&2; exit 1 ;;
    esac
done

if [[ "$ARCH" == "X64" ]]; then
    export GCC_BIN=/usr/bin/x86_64-linux-gnu-
    SHELL_IMG="$REPO_ROOT/Q35Pkg/uefi-shell.img"
    BOOT_FILE="BOOTX64.EFI"
else
    export GCC_AARCH64_PREFIX=/usr/bin/aarch64-linux-gnu-
    SHELL_IMG="$REPO_ROOT/ArmVirtOrreryPkg/uefi-shell.img"
    BOOT_FILE="BOOTAA64.EFI"
fi

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

# Shell.efi isn't at a flat <ARCH>/Shell.efi path — it's nested under the
# module's own (per-build) GUID directory, e.g.:
#   Build/Shell/RELEASE_GCC/X64/ShellPkg/Application/Shell/<GUID>/OUTPUT/Shell.efi
BUILD_ARCH_DIR="$WORKSPACE/Build/Shell/${BUILD_TYPE}_${TOOLCHAIN}/$ARCH"
SHELL_EFI="$(find "$BUILD_ARCH_DIR" -path "*/OUTPUT/Shell.efi" 2>/dev/null | head -1)"
if [[ -z "$SHELL_EFI" || ! -f "$SHELL_EFI" ]]; then
    echo "ERROR: build succeeded but Shell.efi not found under $BUILD_ARCH_DIR" >&2
    exit 1
fi

echo ""
echo "→ Packing Shell.efi into ${SHELL_IMG_SIZE_MB} MB raw FAT image..."
dd if=/dev/zero of="$SHELL_IMG" bs=1M count="$SHELL_IMG_SIZE_MB" status=none
mformat -i "$SHELL_IMG" -F -v UEFISHELL ::
mmd -i "$SHELL_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$SHELL_IMG" "$SHELL_EFI" "::/EFI/BOOT/$BOOT_FILE"

echo "✓ $SHELL_IMG ready"

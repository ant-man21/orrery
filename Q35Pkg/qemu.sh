#!/usr/bin/env bash
# =============================================================================
# qemu.sh — Launch QEMU with Q35 firmware
# Usage: ./qemu.sh [-r|-d] [-c CHIP] [-m MEM] [--reset-shared] [-- <extra qemu args>]
#
#   -r              Use RELEASE build firmware  (default)
#   -d              Use DEBUG build firmware
#   -c CHIP         Chip target                 (default: q35)
#   -m MEM          RAM in MB                   (default: 512)
#   --reset-shared  Wipe and recreate shared.img (fresh FAT disk)
#   -h              Show this help
#
# Extra QEMU args can be appended after --
#   e.g.  ./qemu.sh -d -- -cdrom my.iso
#
# Shared folder layout (host side):
#   Q35Pkg/shared/          ← everything here appears on fs1: in UEFI shell
#     apps/                 ← drop .efi files here
#     data/                 ← sealed blobs, output files from UEFI apps, etc.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------- defaults ----------------------------------------------------------
BUILD_TYPE="RELEASE"
CHIP="q35"
MEM_MB=512
RESET_SHARED=0

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -r)             BUILD_TYPE="RELEASE"; shift ;;
        -d)             BUILD_TYPE="DEBUG";   shift ;;
        -c)             CHIP="$2";            shift 2 ;;
        -m)             MEM_MB="$2";          shift 2 ;;
        --reset-shared) RESET_SHARED=1;       shift ;;
        -h)             usage ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
         *)             echo "ERROR: Unknown argument: $1" >&2; exit 1 ;;
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

FV_DIR="$EDK2_DIR/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/FV"
CODE_FD="$FV_DIR/OVMF_CODE.fd"
VARS_SRC="$FV_DIR/OVMF_VARS.fd"

# ---------- sanity checks -----------------------------------------------------
if [[ ! -f "$CODE_FD" ]]; then
    echo "ERROR: Firmware not found: $CODE_FD"
    echo ""
    echo "  You need to build first. Available builds:"
    for bt in DEBUG RELEASE; do
        fd="$EDK2_DIR/$OUTPUT_DIR/${bt}_${TOOLCHAIN}/FV/OVMF_CODE.fd"
        if [[ -f "$fd" ]]; then
            echo "    ✓ $bt  → ./run.sh -${bt:0:1}"
        else
            echo "    ✗ $bt  (not built)"
        fi
    done
    echo ""
    exit 1
fi

# ---------- writable VARS copy ------------------------------------------------
VARS_DIR="$SCRIPT_DIR/chips/$CHIP/vars"
mkdir -p "$VARS_DIR"
VARS_FD="$VARS_DIR/OVMF_VARS_${BUILD_TYPE}.fd"
if [[ ! -f "$VARS_FD" ]]; then
    echo "→ Seeding fresh VARS image from $VARS_SRC"
    cp "$VARS_SRC" "$VARS_FD"
fi

# ---------- shared folder + disk image ----------------------------------------
# Host directory:  Q35Pkg/shared/
#   apps/          → drop .efi binaries here; shell can run them as fs1:\apps\Foo.efi
#   data/          → sealed blobs, output files from UEFI apps
#
# Appears in the UEFI shell as fs1: (fs0: is the UEFI shell image)
#
# The image is a raw FAT32 disk created with mformat.
# Size: 256 MB — big enough for a few OVMF snapshots.
SHARED_DIR="$SCRIPT_DIR/shared"
SHARED_IMG="$SCRIPT_DIR/shared.img"
SHARED_SIZE_MB=256

mkdir -p "$SHARED_DIR/apps"
mkdir -p "$SHARED_DIR/data"

rebuild_shared_img() {
    echo "→ Building shared.img (${SHARED_SIZE_MB} MB FAT32)..."

    # create blank image
    dd if=/dev/zero of="$SHARED_IMG" bs=1M count="$SHARED_SIZE_MB" status=none

    # format as FAT32 (mtools — no loop mount needed, no root required)
    if command -v mformat &>/dev/null; then
        mformat -i "$SHARED_IMG" -F -v SHARED ::

        # copy entire shared tree into image
        if command -v mcopy &>/dev/null; then
            echo "  Copying $SHARED_DIR into shared.img..."
            mcopy -i "$SHARED_IMG" -s "$SHARED_DIR"/* ::
        else
            echo "  mcopy not found — image will be empty"
        fi
    else
        echo "  mtools not found — trying mkfs.fat (may need sudo)"
        mkfs.fat -F 32 -n SHARED "$SHARED_IMG"
        echo "  WARNING: mkfs.fat only formats; it does not copy files"
    fi

    echo "→ shared.img ready"
}

# rebuild shell image

if [[ "$RESET_SHARED" -eq 1 || ! -f "$SHARED_IMG" ]]; then
    rebuild_shared_img
fi

# ---------- swtpm -------------------------------------------------------------
SWTPM_DIR="$SCRIPT_DIR/chips/$CHIP/tpm"
mkdir -p "$SWTPM_DIR"

# kill any leftover swtpm from a previous run
pkill -f "swtpm socket.*$SWTPM_DIR" 2>/dev/null || true
sleep 0.2

swtpm socket \
    --tpmstate dir="$SWTPM_DIR" \
    --ctrl type=unixio,path=/tmp/swtpm-sock \
    --tpm2 \
    --daemon

echo "→ swtpm started, state in $SWTPM_DIR"

# ---------- launch ------------------------------------------------------------
QEMU_CPU="${QEMU_CPU:-max}"
echo "============================================================"
echo "  Chip      : $CHIP  ($QEMU_MACHINE)"
echo "  Build     : $BUILD_TYPE"
echo "  CPU       : $QEMU_CPU"
echo "  RAM       : ${MEM_MB}M"
echo "  CODE fd   : $CODE_FD"
echo "  VARS fd   : $VARS_FD  (persistent)"
echo "  Shared    : $SHARED_IMG  → fs1: in shell"
echo "  Host dir  : $SHARED_DIR"
echo "============================================================"
echo ""

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE,smm=on" \
    -cpu "$QEMU_CPU" \
    -m "${MEM_MB}M" \
    \
    -drive if=pflash,format=raw,readonly=on,file="$CODE_FD" \
    -drive if=pflash,format=raw,file="$VARS_FD" \
    \
    -global driver=cfi.pflash01,property=secure,value=on \
    \
    -serial stdio \
    -display gtk \
    -net none \
    -drive file="$SCRIPT_DIR/uefi-shell.img",format=raw,if=virtio \
    -drive file="$SHARED_IMG",format=raw,if=virtio \
    \
    -debugcon file:debug.log \
    -global isa-debugcon.iobase=0x402 \
    \
    -chardev socket,id=chrtpm,path=/tmp/swtpm-sock \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" &
QEMU_PID=$!
tail -f "$SCRIPT_DIR/debug.log" --pid=$QEMU_PID
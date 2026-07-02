#!/usr/bin/env bash
# =============================================================================
# run.sh — Launch QEMU with Q35 firmware
# Usage: ./run.sh [-r|-d] [-c CHIP] [-- <extra qemu args>]
#
#   -r          Use RELEASE build firmware  (default)
#   -d          Use DEBUG build firmware
#   -c CHIP     Chip target                 (default: q35)
#   -m MEM      RAM in MB                   (default: 512)
#   -h          Show this help
#
# Extra QEMU args can be appended after --
#   e.g.  ./run.sh -d -- -cdrom my.iso
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2_DIR="$(cd "$SCRIPT_DIR/../edk2" && pwd)"

# ---------- defaults ----------------------------------------------------------
BUILD_TYPE="RELEASE"
CHIP="q35"
MEM_MB=512

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}

EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -r) BUILD_TYPE="RELEASE"; shift ;;
        -d) BUILD_TYPE="DEBUG";   shift ;;
        -c) CHIP="$2";            shift 2 ;;
        -m) MEM_MB="$2";          shift 2 ;;
        -h) usage ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
         *) echo "ERROR: Unknown argument: $1" >&2; exit 1 ;;
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
# Expected extra variables from chip.conf:
#   QEMU_MACHINE   e.g. q35
#   QEMU_CPU       e.g. host  (optional, falls back to "max")

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
            echo "    ✓ $bt  → ./qemu.sh -${bt:0:1}"
        else
            echo "    ✗ $bt  (not built)"
        fi
    done
    echo ""
    exit 1
fi

# ---------- writable VARS copy ------------------------------------------------
# Keep a per-chip, per-buildtype VARS image so state persists between runs
# but stays isolated per config.
VARS_DIR="$SCRIPT_DIR/chips/$CHIP/vars"
mkdir -p "$VARS_DIR"
VARS_FD="$VARS_DIR/OVMF_VARS_${BUILD_TYPE}.fd"

if [[ ! -f "$VARS_FD" ]]; then
    echo "→ Seeding fresh VARS image from $VARS_SRC"
    cp "$VARS_SRC" "$VARS_FD"
fi

# ---------- launch ------------------------------------------------------------
QEMU_CPU="${QEMU_CPU:-max}"

echo "============================================================"
echo "  Chip      : $CHIP  ($QEMU_MACHINE)"
echo "  Build     : $BUILD_TYPE"
echo "  CPU       : $QEMU_CPU"
echo "  RAM       : ${MEM_MB}M"
echo "  CODE fd   : $CODE_FD"
echo "  VARS fd   : $VARS_FD  (persistent)"
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
    \
    -debugcon file:debug.log \
    -global isa-debugcon.iobase=0x402 \
    \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" &

QEMU_PID=$!
tail -f "$SCRIPT_DIR/debug.log" --pid=$QEMU_PID

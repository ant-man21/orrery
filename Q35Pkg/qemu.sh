#!/usr/bin/env bash
# =============================================================================
# qemu.sh — Launch QEMU with Q35 firmware
# Usage: ./qemu.sh [-r|-d] [-m MEM] [--reset-shared] [--headless [opts]]
#                  [-- <extra qemu args>]
#
#   -r              Use RELEASE build firmware  (default)
#   -d              Use DEBUG build firmware
#   -m MEM          RAM in MB                   (default: 512)
#   --reset-shared  Wipe and recreate shared.img (fresh FAT disk)
#   -h              Show this help
#
# Headless / automation mode (issue #25):
#   --headless          No GUI, no interactive serial. Implies -display none,
#                       serial redirected to a file, and -no-reboot so a
#                       triple-fault or reset ends the run instead of looping.
#   --serial-log PATH   Where --headless writes the guest's serial console
#                       (default: <script dir>/serial.log). This is the file a
#                       test runner greps — debug.log only ever carries DEBUG()
#                       output from the -debugcon port, which RELEASE builds
#                       leave empty, and never carries what the UEFI shell or
#                       an app's Print() puts on the serial console.
#   --timeout SECS      Host-side wall-clock bound on the whole run
#                       (default: 300). A hung or crashed boot has nothing else
#                       to stop it. Exit code 124 means the timeout fired.
#
# Interactive mode (the default) is unchanged: GUI window, -serial stdio, and
# a tail -f of debug.log that follows the QEMU process.
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

# ---------- platform (fixed — this script only runs Q35Pkg) -------------------
TOOLCHAIN="GCC"
OUTPUT_DIR="Build/OvmfX64"
QEMU_MACHINE="q35"
QEMU_CPU="max"

# Ubuntu 22.04's packaged QEMU (6.2) hits OVMF's "broken CPU hotplug register
# block" assert (fixed in QEMU 8+, see tianocore bug 4250). Prefer a locally
# built QEMU 9.2.4 at ~/.local/qemu-9.2.4 if present, else fall back to PATH.
QEMU_BIN="qemu-system-x86_64"
if [[ -x "$HOME/.local/qemu-9.2.4/bin/qemu-system-x86_64" ]]; then
    QEMU_BIN="$HOME/.local/qemu-9.2.4/bin/qemu-system-x86_64"
fi

# ---------- defaults ------------------------------------------------------
BUILD_TYPE="RELEASE"
MEM_MB=512
RESET_SHARED=0
HEADLESS=0
SERIAL_LOG=""
TIMEOUT_SECS=300

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
        -m)             MEM_MB="$2";          shift 2 ;;
        --reset-shared) RESET_SHARED=1;       shift ;;
        --headless)     HEADLESS=1;           shift ;;
        --serial-log)   SERIAL_LOG="$2";      shift 2 ;;
        --timeout)      TIMEOUT_SECS="$2";    shift 2 ;;
        -h)             usage ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
         *)             echo "ERROR: Unknown argument: $1" >&2; exit 1 ;;
    esac
done

FV_DIR="$EDK2_DIR/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/FV"
CODE_FD="$FV_DIR/OVMF_CODE.fd"
VARS_SRC="$FV_DIR/OVMF_VARS.fd"

# Defaulted here rather than up with the other defaults — it needs SCRIPT_DIR,
# and callers may override it with --serial-log.
: "${SERIAL_LOG:=$SCRIPT_DIR/serial.log}"

if [[ "$HEADLESS" -eq 1 ]]; then
    if ! command -v timeout &>/dev/null; then
        echo "ERROR: --headless needs 'timeout' (coreutils) on PATH" >&2
        exit 1
    fi
    # Start from a clean log: the runner greps this for a pass/fail sentinel,
    # and a stale line from a previous run would be indistinguishable from a
    # real one produced by this boot.
    rm -f "$SERIAL_LOG"
    mkdir -p "$(dirname "$SERIAL_LOG")"
fi

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
VARS_DIR="$SCRIPT_DIR/chips/q35/vars"
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

if [[ "$RESET_SHARED" -eq 1 || ! -f "$SHARED_IMG" ]]; then
    rebuild_shared_img
fi

# ---------- swtpm -------------------------------------------------------------
SWTPM_DIR="$SCRIPT_DIR/chips/q35/tpm"
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
echo "============================================================"
echo "  Platform  : Q35  ($QEMU_MACHINE)"
echo "  Build     : $BUILD_TYPE"
echo "  CPU       : $QEMU_CPU"
echo "  RAM       : ${MEM_MB}M"
echo "  CODE fd   : $CODE_FD"
echo "  VARS fd   : $VARS_FD  (persistent)"
echo "  Shared    : $SHARED_IMG  → fs1: in shell"
echo "  Host dir  : $SHARED_DIR"
if [[ "$HEADLESS" -eq 1 ]]; then
echo "  Mode      : HEADLESS (timeout ${TIMEOUT_SECS}s)"
echo "  Serial    : $SERIAL_LOG"
else
echo "  Mode      : interactive (GUI + serial on stdio)"
fi
echo "============================================================"
echo ""

# Everything except the display/serial wiring is identical in both modes —
# a headless run must exercise the same machine the interactive one does,
# or it isn't testing what a human sees when they boot it by hand.
QEMU_ARGS=(
    -machine "$QEMU_MACHINE,smm=on"
    -cpu "$QEMU_CPU"
    -m "${MEM_MB}M"

    -drive if=pflash,format=raw,readonly=on,file="$CODE_FD"
    -drive if=pflash,format=raw,file="$VARS_FD"

    -global driver=cfi.pflash01,property=secure,value=on

    -net none
    -drive file="$SCRIPT_DIR/uefi-shell.img",format=raw,if=virtio
    -drive file="$SHARED_IMG",format=raw,if=virtio

    -debugcon "file:$SCRIPT_DIR/debug.log"
    -global isa-debugcon.iobase=0x402

    -chardev socket,id=chrtpm,path=/tmp/swtpm-sock
    -tpmdev emulator,id=tpm0,chardev=chrtpm
    -device tpm-tis,tpmdev=tpm0
)

if [[ "$HEADLESS" -eq 1 ]]; then
    # -no-reboot: without it a guest reset silently restarts the boot inside
    # the same QEMU process, so a crash-loop would burn the whole timeout and
    # leave a serial log with N interleaved boots in it.
    QEMU_ARGS+=(
        -display none
        -serial "file:$SERIAL_LOG"
        -no-reboot
    )
    QEMU_ARGS+=("${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}")

    set +e
    timeout "$TIMEOUT_SECS" "$QEMU_BIN" "${QEMU_ARGS[@]}"
    QEMU_RC=$?
    set -e

    if [[ "$QEMU_RC" -eq 124 ]]; then
        echo "→ QEMU hit the ${TIMEOUT_SECS}s timeout and was killed." >&2
        echo "  (Not necessarily a failure — the UEFI shell sits at its prompt" >&2
        echo "   forever after startup.nsh finishes. Judge the run by the" >&2
        echo "   contents of $SERIAL_LOG, not by this exit code.)" >&2
    fi
    exit "$QEMU_RC"
fi

QEMU_ARGS+=(
    -serial stdio
    -display gtk
)
QEMU_ARGS+=("${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}")

"$QEMU_BIN" "${QEMU_ARGS[@]}" &
QEMU_PID=$!
tail -f "$SCRIPT_DIR/debug.log" --pid=$QEMU_PID

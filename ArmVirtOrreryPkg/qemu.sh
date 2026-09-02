#!/usr/bin/env bash
# =============================================================================
# qemu.sh — Launch QEMU with ArmVirt firmware (QEMU 'virt' machine, AArch64)
# Usage: ./qemu.sh [-r|-d] [-m MEM] [-g] [--reset-shared] [-- <extra qemu args>]
#
#   -r              Use RELEASE build firmware  (default)
#   -d              Use DEBUG build firmware
#   -m MEM          RAM in MB                   (default: 512)
#   -g              Start paused with a gdbstub on :1234 (qemu -S -s) —
#                    attach with `gdb-multiarch -ex 'target remote :1234'`
#                    (AArch64 target — plain `gdb` on an x86 host won't
#                    work). Symbols:
#                    Build/ArmVirtQemu-AArch64/<BUILD_TYPE>_GCC/AARCH64/**/DEBUG/*.dll,
#                    or paste the `add-symbol-file <path> 0x<addr>` lines
#                    a DEBUG build prints for each driver as it loads.
#   --reset-shared  Wipe and recreate shared.img (fresh FAT disk)
#   -h              Show this help
#
# Extra QEMU args can be appended after --
#   e.g.  ./qemu.sh -d -- -cdrom my.iso
#
# Shared folder layout (host side):
#   ArmVirtOrreryPkg/shared/  ← everything here appears on fs1: in UEFI shell
#     apps/                   ← drop .efi files here
#     data/                   ← sealed blobs, output files from UEFI apps, etc.
#
# Differences from Q35Pkg/qemu.sh (not just a find/replace):
#   - FV output is QEMU_EFI.fd / QEMU_VARS.fd, not OVMF_CODE.fd / OVMF_VARS.fd.
#   - No `smm=on` / `-global driver=cfi.pflash01,property=secure,value=on` —
#     SMM is an x86 concept, not applicable to the 'virt' machine.
#   - No `-debugcon` / isa-debugcon — 'virt' has no ISA bus, so there's no
#     separate debug I/O port. DEBUG-build prints interleave with the shell
#     on the same -serial console instead of going to their own debug.log.
#   - TPM device is `tpm-tis-device`, not `tpm-tis` — 'virt' has no ISA/LPC
#     bus, so the TIS interface is exposed as a sysbus MMIO device instead
#     of the ISA one Q35 uses. Same swtpm backend either way.
#   - fs0:'s uefi-shell.img must be an AArch64 UEFI Shell image — the X64
#     one from Q35Pkg will not boot here. Provide your own at
#     ArmVirtOrreryPkg/uefi-shell.img (gitignored, same as Q35Pkg's).
#   - Explicit `-device virtio-gpu-pci` / `qemu-xhci` / `usb-kbd` / `usb-tablet`
#     — q35 auto-attaches a default VGA + PS/2 input, 'virt' has no onboard
#     display or input hardware. The firmware (VirtioGpuDxe, XhciDxe, etc.)
#     is built in already; without these -device flags the GTK window stays
#     blank even though the shell is alive and usable over -serial stdio.
#   - Each pflash bank on 'virt' is hardcoded to 64 MiB regardless of the
#     backing file's size, unlike q35 which sizes pflash to match the file.
#     QEMU_EFI.fd/QEMU_VARS.fd are padded up to 64 MiB before use (see below).
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------- platform (fixed — this script only runs ArmVirtOrreryPkg) ---------
TOOLCHAIN="GCC"
OUTPUT_DIR="Build/ArmVirtQemu-AArch64"
QEMU_MACHINE="virt"
QEMU_CPU="max"

# Ubuntu 22.04's packaged QEMU (6.2) hits OVMF's "broken CPU hotplug register
# block" assert (fixed in QEMU 8+, see tianocore bug 4250). Prefer a locally
# built QEMU 9.2.4 at ~/.local/qemu-9.2.4 if present, else fall back to PATH.
QEMU_BIN="qemu-system-aarch64"
if [[ -x "$HOME/.local/qemu-9.2.4/bin/qemu-system-aarch64" ]]; then
    QEMU_BIN="$HOME/.local/qemu-9.2.4/bin/qemu-system-aarch64"
fi

# ---------- defaults ------------------------------------------------------
BUILD_TYPE="RELEASE"
MEM_MB=512
RESET_SHARED=0
GDB=0

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
        -g)             GDB=1;                shift ;;
        --reset-shared) RESET_SHARED=1;       shift ;;
        -h)             usage ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
         *)             echo "ERROR: Unknown argument: $1" >&2; exit 1 ;;
    esac
done

FV_DIR="$EDK2_DIR/$OUTPUT_DIR/${BUILD_TYPE}_${TOOLCHAIN}/FV"
CODE_FD="$FV_DIR/QEMU_EFI.fd"
VARS_SRC="$FV_DIR/QEMU_VARS.fd"

# ---------- sanity checks -----------------------------------------------------
if [[ ! -f "$CODE_FD" ]]; then
    echo "ERROR: Firmware not found: $CODE_FD"
    echo ""
    echo "  You need to build first. Available builds:"
    for bt in DEBUG RELEASE; do
        fd="$EDK2_DIR/$OUTPUT_DIR/${bt}_${TOOLCHAIN}/FV/QEMU_EFI.fd"
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
# The 'virt' machine's pflash banks are hardcoded to 64 MiB each, regardless
# of the backing file's size — unlike Q35, which sizes pflash to match the
# file. edk2's ArmVirtQemu build only allocates as much flash as the
# firmware needs (~3 MB code, ~768 KB vars), so both CODE and VARS files
# must be padded to 64 MiB or QEMU refuses to attach them.
PFLASH_SIZE=$((64 * 1024 * 1024))

VARS_DIR="$SCRIPT_DIR/vars"
mkdir -p "$VARS_DIR"
VARS_FD="$VARS_DIR/QEMU_VARS_${BUILD_TYPE}.fd"
if [[ ! -f "$VARS_FD" ]]; then
    echo "→ Seeding fresh VARS image from $VARS_SRC"
    cp "$VARS_SRC" "$VARS_FD"
fi
if [[ "$(stat -c%s "$VARS_FD")" -lt "$PFLASH_SIZE" ]]; then
    echo "→ Padding VARS image to 64 MiB (pflash bank size)"
    truncate -s "$PFLASH_SIZE" "$VARS_FD"
fi

# CODE is read-only firmware straight from the build — pad a scratch copy
# each run so it always reflects the latest build (unlike VARS, it holds no
# persistent state, so there's nothing to preserve across regenerations).
CODE_FD_PADDED="$VARS_DIR/QEMU_CODE_${BUILD_TYPE}.fd"
cp "$CODE_FD" "$CODE_FD_PADDED"
truncate -s "$PFLASH_SIZE" "$CODE_FD_PADDED"

# ---------- shared folder + disk image ----------------------------------------
# Host directory:  ArmVirtOrreryPkg/shared/
#   apps/          → drop .efi binaries here; shell can run them as fs1:\apps\Foo.efi
#   data/          → sealed blobs, output files from UEFI apps
#
# Appears in the UEFI shell as fs1: (fs0: is the UEFI shell image)
#
# The image is a raw FAT32 disk created with mformat.
# Size: 256 MB — big enough for a few snapshots.
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
SWTPM_DIR="$SCRIPT_DIR/tpm"
mkdir -p "$SWTPM_DIR"

# kill any leftover swtpm from a previous run
pkill -f "swtpm socket.*$SWTPM_DIR" 2>/dev/null || true
sleep 0.2

swtpm socket \
    --tpmstate dir="$SWTPM_DIR" \
    --ctrl type=unixio,path=/tmp/swtpm-sock-armvirt \
    --tpm2 \
    --daemon

echo "→ swtpm started, state in $SWTPM_DIR"

# ---------- launch ------------------------------------------------------------
echo "============================================================"
echo "  Platform  : ArmVirt  ($QEMU_MACHINE)"
echo "  Build     : $BUILD_TYPE"
echo "  CPU       : $QEMU_CPU"
echo "  RAM       : ${MEM_MB}M"
echo "  CODE fd   : $CODE_FD_PADDED"
echo "  VARS fd   : $VARS_FD  (persistent)"
echo "  Shared    : $SHARED_IMG  → fs1: in shell"
echo "  Host dir  : $SHARED_DIR"
GDB_ARGS=()
if [[ "$GDB" -eq 1 ]]; then
    GDB_ARGS=(-s -S)
    echo "  gdbstub   : :1234 (paused at reset — attach before it'll boot)"
fi
echo "============================================================"
echo ""

"$QEMU_BIN" \
    -machine "$QEMU_MACHINE" \
    -cpu "$QEMU_CPU" \
    -m "${MEM_MB}M" \
    \
    -drive if=pflash,format=raw,readonly=on,file="$CODE_FD_PADDED" \
    -drive if=pflash,format=raw,file="$VARS_FD" \
    \
    -serial stdio \
    -display gtk \
    -device virtio-gpu-pci \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -net none \
    -drive file="$SCRIPT_DIR/uefi-shell.img",format=raw,if=virtio \
    -drive file="$SHARED_IMG",format=raw,if=virtio \
    \
    -chardev socket,id=chrtpm,path=/tmp/swtpm-sock-armvirt \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis-device,tpmdev=tpm0 \
    \
    "${GDB_ARGS[@]}" \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

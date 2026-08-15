#!/usr/bin/env bash
# =============================================================================
# qemu.sh — Launch QEMU with SBSA firmware (QEMU 'sbsa-ref' machine, AArch64)
# Usage: ./qemu.sh [-r|-d] [-m MEM] [-g] [--reset-shared] [-- <extra qemu args>]
#
#   -r              Use RELEASE build firmware  (default)
#   -d              Use DEBUG build firmware (BL33 prints DXE dispatch over
#                    -serial; StandaloneMm prints to its own secure UART —
#                    see docs/sbsa_boot_flow.md for how to see that log too)
#   -m MEM          RAM in MB                   (default: 1024)
#   -g              Start paused with a gdbstub on :1234 (qemu -S -s) — see
#                    docs/sbsa_boot_flow.md for attaching gdb-multiarch and
#                    loading symbols for each boot stage (BL1/BL2/BL31/
#                    StandaloneMm/BL33 all run at different times, different
#                    addresses, different ELFs — one `target remote` doesn't
#                    give you all of them at once).
#   --reset-shared  Wipe and recreate shared.img (fresh FAT disk)
#   -h              Show this help
#
# Differences from ArmVirtOrreryPkg/qemu.sh and Q35Pkg/qemu.sh (not just a
# find/replace):
#   - Two *secure* pflash banks (SBSA_FLASH0 = BL1+FIP, SBSA_FLASH1 = BL33 +
#     vars) instead of one CODE/VARS pair — TF-A owns FLASH0 outright, edk2
#     never touches it. See build.sh's header for the BL1..BL33 build chain
#     that produces them.
#   - No separate uefi-shell.img: edk2-platforms' SbsaQemu.fdf links
#     ShellPkg's Shell.efi straight into FVMAIN, so BdsDxe boots directly
#     into the shell with no fs0: boot disk required.
#   - -smp is passed with explicit sockets=/clusters=/cores=/threads=, not
#     just a flat count — TF-A's SIP_SVC_GET_CPU_TOPOLOGY handler reads a
#     "/cpus/topology" DT node that QEMU only emits from this form. (On the
#     QEMU 8.2.2 available when this was written, sbsa-ref never emits that
#     node at all regardless — verified with `-machine dumpdtb=` + `dtc`.
#     SbsaQemuHardwareInfoLibCompat.inf works around the consequence of
#     that gap; keeping the explicit -smp here is still correct so this
#     starts reporting real topology for free on a newer QEMU.)
#   - -global e1000e.romfile= / -global bochs-display.romfile= : sbsa-ref
#     always instantiates an onboard e1000e NIC and a bochs-display VGA
#     card (fixed platform topology, unlike 'virt' or 'q35' — there's no
#     -device/-vga/-net flag that removes them). Their option ROM blobs
#     (efi-e1000e.rom, vgabios-bochs-display.bin) aren't in Ubuntu's
#     qemu-system-data package (only qemu-system-gui ships them), so
#     without this override QEMU refuses to start with "failed to find
#     romfile" the moment those packages are missing. Emptying the romfile
#     property just skips option ROM loading for both — harmless over a
#     serial console with no PXE/legacy-VGA boot path in play.
#   - No swtpm/TPM device wiring yet: OrreryPkg's TPM demo apps build for
#     this platform (Tpm2DeviceLib etc. are wired in SbsaOrreryPkg.dsc) but
#     sbsa-ref's discrete-TPM story (TIS/CRB over which bus) hasn't been
#     verified end-to-end the way ArmVirt's tpm-tis-device / Q35's tpm-tis
#     have — see docs/sbsa_boot_flow.md.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------- platform (fixed — this script only runs SbsaOrreryPkg) ------------
QEMU_MACHINE="sbsa-ref"
QEMU_CPU="max"
SMP_TOPOLOGY="4,sockets=1,clusters=1,cores=4,threads=1"

QEMU_BIN="qemu-system-aarch64"
if [[ -x "$HOME/.local/qemu-9.2.4/bin/qemu-system-aarch64" ]]; then
    QEMU_BIN="$HOME/.local/qemu-9.2.4/bin/qemu-system-aarch64"
fi

# ---------- defaults ------------------------------------------------------
BUILD_TYPE="RELEASE"
MEM_MB=1024
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

VARS_DIR="$SCRIPT_DIR/vars"
FLASH0="$VARS_DIR/SBSA_FLASH0_${BUILD_TYPE}.fd"
FLASH1="$VARS_DIR/SBSA_FLASH1_${BUILD_TYPE}.fd"

# ---------- sanity checks -----------------------------------------------------
if [[ ! -f "$FLASH0" || ! -f "$FLASH1" ]]; then
    echo "ERROR: Flash images not found:"
    echo "    $FLASH0"
    echo "    $FLASH1"
    echo ""
    echo "  You need to build first: ./build.sh -${BUILD_TYPE:0:1,,}"
    exit 1
fi

# ---------- shared folder + disk image ----------------------------------------
# Host directory: SbsaOrreryPkg/shared/  -> fs1: in the shell (fs0: is the
# firmware's own built-in FVMAIN — see build.sh's uefi-shell note above).
SHARED_DIR="$SCRIPT_DIR/shared"
SHARED_IMG="$SCRIPT_DIR/shared.img"
SHARED_SIZE_MB=256

mkdir -p "$SHARED_DIR/apps"
mkdir -p "$SHARED_DIR/data"

rebuild_shared_img() {
    echo "→ Building shared.img (${SHARED_SIZE_MB} MB FAT32)..."
    dd if=/dev/zero of="$SHARED_IMG" bs=1M count="$SHARED_SIZE_MB" status=none
    if command -v mformat &>/dev/null; then
        mformat -i "$SHARED_IMG" -F -v SHARED ::
        if command -v mcopy &>/dev/null; then
            mcopy -i "$SHARED_IMG" -s "$SHARED_DIR"/* ::
        fi
    else
        mkfs.fat -F 32 -n SHARED "$SHARED_IMG"
    fi
    echo "→ shared.img ready"
}

if [[ "$RESET_SHARED" -eq 1 || ! -f "$SHARED_IMG" ]]; then
    rebuild_shared_img
fi

# ---------- launch ------------------------------------------------------------
GDB_ARGS=()
if [[ "$GDB" -eq 1 ]]; then
    GDB_ARGS=(-s -S)
    echo "→ gdbstub on :1234, CPU0 halted at the BL1 reset vector."
    echo "  In another shell: gdb-multiarch -ex 'target remote :1234'"
    echo "  See docs/sbsa_boot_flow.md for per-stage symbol loading."
fi

echo "============================================================"
echo "  Platform  : SBSA  ($QEMU_MACHINE)"
echo "  Build     : $BUILD_TYPE"
echo "  CPU       : $QEMU_CPU  ($SMP_TOPOLOGY)"
echo "  RAM       : ${MEM_MB}M"
echo "  FLASH0    : $FLASH0  (BL1 + FIP: BL2/BL31/BL32)"
echo "  FLASH1    : $FLASH1  (BL33 + vars, persistent)"
echo "  Shared    : $SHARED_IMG  → fs1: in shell"
echo "============================================================"
echo ""

"$QEMU_BIN" \
    -machine "$QEMU_MACHINE" \
    -cpu "$QEMU_CPU" \
    -smp "$SMP_TOPOLOGY" \
    -m "${MEM_MB}M" \
    \
    -pflash "$FLASH0" \
    -pflash "$FLASH1" \
    \
    -serial stdio \
    -display none \
    -global e1000e.romfile= \
    -global bochs-display.romfile= \
    \
    -drive file="$SHARED_IMG",format=raw,if=virtio \
    \
    "${GDB_ARGS[@]}" \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

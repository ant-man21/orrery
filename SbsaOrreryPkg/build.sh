#!/usr/bin/env bash
# =============================================================================
# build.sh — firmware builder for SbsaOrreryPkg (QEMU sbsa-ref, AArch64)
# Usage: ./build.sh [-r|-d] [-C] [-M] [-s|-S]
#
#   -r          Release build         (default; applies to BL33 + StandaloneMm)
#   -d          Debug build           (BL33 prints boot progress over -serial;
#                                       StandaloneMm prints over the secure UART)
#   -C          Clean before build (edk2 Build/ dirs + trusted-firmware-a build/)
#   -M          Build WITHOUT BL32/StandaloneMm — TF-A does a plain
#               BL1->BL2->BL31->BL33 handoff, no SPM_MM at all. This is
#               the fully-verified, reliable path straight to an
#               interactive UEFI shell. Use this if you just want a
#               working SBSA boot; leave it off to build the StandaloneMm
#               integration this package is actually for (see the WARNING
#               below and docs/sbsa_boot_flow.md before expecting that to
#               reach BL33 today).
#   -s          Sync .efi outputs -> shared/apps/ + shared.img after build (default: on)
#   -S          Skip sync
#   -h          Show this help
#
# Unlike ArmVirtOrreryPkg/Q35Pkg, this platform's early boot isn't edk2 at
# all: TF-A (trusted-firmware-a/) does BL1 (ROM) -> BL2 -> BL31 (EL3
# runtime), dispatching into BL32 (StandaloneMm, a Secure-EL0 partition
# built from edk2/StandaloneMmPkg) before finally handing off to BL33
# (edk2-platforms' SbsaQemu UEFI, this dir's own SbsaOrreryPkg.dsc layered
# on top). See docs/sbsa_boot_flow.md for the full picture and why each
# stage exists.
#
# Build order (each stage's output feeds the next):
#   1. StandaloneMm.fd   (edk2 StandaloneMmPkg, our own StMm DSC/FDF)  -> BL32
#   2. trusted-firmware-a, BL32=<1>, SPM_MM=1                          -> bl1.bin, fip.bin
#   3. bl1.bin/fip.bin staged into edk2-non-osi/Platform/Qemu/Sbsa/
#   4. SbsaOrreryPkg.dsc (edk2-platforms SbsaQemu.dsc + OrreryPkg)     -> BL33,
#      combined by GenFds with (3) into SBSA_FLASH0.fd / SBSA_FLASH1.fd
#
# WARNING — current status of the BL32 (StandaloneMm) integration:
#   BL31 genuinely dispatches into BL32 and StandaloneMmCore starts
#   running in S-EL0, but its first runtime memory-hardening call
#   (MM_SP_MEMORY_ATTRIBUTES_SET_AARCH64, marking part of its own image
#   read-only) hits an ASSERT inside TF-A's SPM_MM (spm_mm_main.c:124) —
#   TF-A's xlat tables for the BL32 region get block-coalesced at boot and
#   can't be split to page granularity at runtime for this platform. That
#   ASSERT crashes EL3 entirely, so BL33 never runs when BL32 is included.
#   Root cause, why the obvious fix (PLAT_XLAT_TABLES_DYNAMIC) makes it
#   *worse*, and how to attach gdb and watch it happen yourself: see
#   docs/sbsa_boot_flow.md. Default here is still to build BL32 in, since
#   that's the point of this package — pass -M for a boot that actually
#   reaches BL33.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EDK2_DIR="$REPO_ROOT/edk2"
TFA_DIR="$REPO_ROOT/trusted-firmware-a"
NON_OSI_SBSA_DIR="$REPO_ROOT/edk2-non-osi/Platform/Qemu/Sbsa"

# ---------- platform (fixed — this script only builds SbsaOrreryPkg) ----------
ARCH="AARCH64"
TOOLCHAIN="GCC"
DSC="SbsaOrreryPkg/SbsaOrreryPkg.dsc"
STMM_DSC="SbsaOrreryPkg/StandaloneMm/SbsaOrreryStandaloneMm.dsc"
OUTPUT_DIR="Build/SbsaQemu"          # matches SbsaQemu.dsc's own OUTPUT_DIRECTORY
STMM_OUTPUT_DIR="Build/SbsaOrreryStandaloneMm"
export GCC_AARCH64_PREFIX=/usr/bin/aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-

# ---------- defaults ------------------------------------------------------
BUILD_TYPE="RELEASE"
CLEAN=0
SKIP_BL32=0
SYNC=1

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
while getopts ":rdCMsSh" opt; do
    case $opt in
        r) BUILD_TYPE="RELEASE" ;;
        d) BUILD_TYPE="DEBUG"   ;;
        C) CLEAN=1              ;;
        M) SKIP_BL32=1          ;;
        s) SYNC=1               ;;
        S) SYNC=0               ;;
        h) usage ;;
        :) echo "ERROR: -$OPTARG requires an argument." >&2; exit 1 ;;
       \?) echo "ERROR: Unknown flag -$OPTARG" >&2; exit 1 ;;
    esac
done

# EDK2's DSC_SPECIFICATION build targets are DEBUG/RELEASE; -r/-d select
# that for BL33/StandaloneMm only. trusted-firmware-a is always built
# DEBUG=1 (build/qemu_sbsa/debug/) regardless — see the LTO note below.
EDK2_BUILD_TARGET="$BUILD_TYPE"

# ---------- sanity checks -----------------------------------------------------
for d in "$EDK2_DIR/edksetup.sh" "$REPO_ROOT/edk2-platforms/Platform/Qemu/SbsaQemu/SbsaQemu.dsc" "$TFA_DIR/Makefile"; do
    if [[ ! -e "$d" ]]; then
        echo "ERROR: $d not found — did you 'git submodule update --init edk2 edk2-platforms edk2-non-osi trusted-firmware-a'?"
        exit 1
    fi
done

echo "============================================================"
echo "  Platform  : SBSA (QEMU sbsa-ref / AArch64)"
echo "  Toolchain : $TOOLCHAIN (aarch64-linux-gnu-)"
echo "  Build     : $BUILD_TYPE"
echo "  Clean     : $CLEAN"
echo "  Skip BL32 : $SKIP_BL32"
echo "============================================================"

# ---------- clean -------------------------------------------------------------
if [[ "$CLEAN" -eq 1 ]]; then
    echo "Cleaning edk2 Build/ dirs and trusted-firmware-a build/..."
    rm -rf "$REPO_ROOT/$OUTPUT_DIR" "$REPO_ROOT/$STMM_OUTPUT_DIR" "$TFA_DIR/build"
    echo "Exiting (clean-only mode)"
    exit 0
fi

# ---------- edk2 build environment --------------------------------------------
export EDK_TOOLS_PATH="$EDK2_DIR/BaseTools"
export PACKAGES_PATH="$EDK2_DIR:$REPO_ROOT/edk2-platforms:$REPO_ROOT/edk2-non-osi:$REPO_ROOT"
export PYTHON_COMMAND=python3

if [[ ! -x "$EDK_TOOLS_PATH/Source/C/bin/build" && ! -x "$EDK_TOOLS_PATH/Source/C/bin/GenFv" ]]; then
    echo "→ Building BaseTools (first run)..."
    make -C "$EDK_TOOLS_PATH" -j"$(nproc)"
fi

cd "$EDK2_DIR"
set +u
source "$EDK2_DIR/edksetup.sh" --reconfig
set -u
export WORKSPACE="$REPO_ROOT"
cd "$REPO_ROOT"

# trusted-firmware-a's build system doesn't track flag changes (SPM_MM=1
# vs plain, ENABLE_LTO, etc.) in its .d dependency files the way source
# changes are — switching between a BL32 and a no-BL32 build in the same
# build/qemu_sbsa/ tree silently links stale objects against the wrong
# config (seen firsthand: a plain rebuild after an SPM_MM=1 one failed to
# link with "undefined reference to spm_mm_setup"). Always start clean.
rm -rf "$TFA_DIR/build/qemu_sbsa"

if [[ "$SKIP_BL32" -eq 0 ]]; then
    # ---------- stage 1: StandaloneMm (BL32) -----------------------------------
    echo ""
    echo "→ [1/4] Building StandaloneMm.fd (BL32)..."
    build -a "$ARCH" -t "$TOOLCHAIN" -b "$EDK2_BUILD_TARGET" -p "$STMM_DSC" -n "$(nproc)" \
        -D ENABLE_UEFI_SECURE_VARIABLE=TRUE

    STMM_FD="$REPO_ROOT/$STMM_OUTPUT_DIR/${EDK2_BUILD_TARGET}_${TOOLCHAIN}/FV/BL32_AP_MM.fd"
    if [[ ! -f "$STMM_FD" ]]; then
        echo "ERROR: expected StandaloneMm output not found: $STMM_FD"
        exit 1
    fi

    # ---------- stage 2: trusted-firmware-a (BL1/BL2/BL31, BL32=StandaloneMm) --
    echo ""
    echo "→ [2/4] Building trusted-firmware-a (PLAT=qemu_sbsa, BL32=StandaloneMm, SPM_MM=1)..."
    # DEBUG=1 regardless of $BUILD_TYPE: RELEASE hits a GCC13 LTO
    # type-mismatch bug in TF-A's percpu_data (-Werror=lto-type-mismatch);
    # ENABLE_LTO=0 on the command line does NOT fix it — defaults.mk's
    # unconditional `ENABLE_LTO := 1` for aarch64 release builds wins
    # regardless. See docs/sbsa_boot_flow.md.
    make -C "$TFA_DIR" PLAT=qemu_sbsa DEBUG=1 \
        BL32="$STMM_FD" SPM_MM=1 EL3_EXCEPTION_HANDLING=1 \
        all fip -j"$(nproc)"

    TFA_OUT="$TFA_DIR/build/qemu_sbsa/debug"
    if [[ ! -f "$TFA_OUT/bl1.bin" || ! -f "$TFA_OUT/fip.bin" ]]; then
        echo "ERROR: trusted-firmware-a build did not produce bl1.bin/fip.bin in $TFA_OUT"
        exit 1
    fi

    echo ""
    echo "  ⚠ BL32/StandaloneMm is built in but currently ASSERTs inside TF-A"
    echo "    (spm_mm_main.c:124) before BL33 ever runs — see the WARNING in"
    echo "    this script's header and docs/sbsa_boot_flow.md. Pass -M to build"
    echo "    without BL32 for a boot that actually reaches the UEFI shell."

    # ---------- stage 3: stage TF-A binaries for the edk2 FDF to embed --------
    echo ""
    echo "→ [3/4] Staging bl1.bin/fip.bin into $NON_OSI_SBSA_DIR ..."
    mkdir -p "$NON_OSI_SBSA_DIR"
    cp "$TFA_OUT/bl1.bin" "$NON_OSI_SBSA_DIR/bl1.bin"
    cp "$TFA_OUT/fip.bin" "$NON_OSI_SBSA_DIR/fip.bin"
else
    # ---------- stages 1-3, -M variant: plain TF-A, no BL32 at all -------------
    echo ""
    echo "→ [1-3/4] -M: building trusted-firmware-a WITHOUT BL32/SPM_MM..."
    make -C "$TFA_DIR" PLAT=qemu_sbsa DEBUG=1 all fip -j"$(nproc)"

    TFA_OUT="$TFA_DIR/build/qemu_sbsa/debug"
    if [[ ! -f "$TFA_OUT/bl1.bin" || ! -f "$TFA_OUT/fip.bin" ]]; then
        echo "ERROR: trusted-firmware-a build did not produce bl1.bin/fip.bin in $TFA_OUT"
        exit 1
    fi

    mkdir -p "$NON_OSI_SBSA_DIR"
    cp "$TFA_OUT/bl1.bin" "$NON_OSI_SBSA_DIR/bl1.bin"
    cp "$TFA_OUT/fip.bin" "$NON_OSI_SBSA_DIR/fip.bin"
fi

# ---------- stage 4: BL33 (SbsaOrreryPkg UEFI) + final flash images -----------
echo ""
echo "→ [4/4] Building BL33 (SbsaOrreryPkg.dsc) and composing flash images..."
build -a "$ARCH" -t "$TOOLCHAIN" -b "$EDK2_BUILD_TARGET" -p "$DSC" -n "$(nproc)" \
    -D TPM2_ENABLE=TRUE

FV_DIR="$REPO_ROOT/$OUTPUT_DIR/${EDK2_BUILD_TARGET}_${TOOLCHAIN}/FV"
echo ""
echo "✓ Build complete -> $FV_DIR/SBSA_FLASH0.fd (secure: BL1+FIP), SBSA_FLASH1.fd (BL33+vars)"

# ---------- pad flash images to the 256 MiB pflash bank size ------------------
# QEMU's sbsa-ref machine, like 'virt', hardcodes each pflash bank size
# regardless of the backing file's size — see ArmVirtOrreryPkg/qemu.sh for
# the equivalent story there. edk2 only emits the "used" prefix of each
# flash device (8 MiB / ~3.75 MiB here), so both must be padded.
VARS_DIR="$SCRIPT_DIR/vars"
mkdir -p "$VARS_DIR"
FLASH_SIZE=$((256 * 1024 * 1024))

FLASH0_PADDED="$VARS_DIR/SBSA_FLASH0_${BUILD_TYPE}.fd"
cp "$FV_DIR/SBSA_FLASH0.fd" "$FLASH0_PADDED"
truncate -s "$FLASH_SIZE" "$FLASH0_PADDED"

# FLASH1 holds BL33 code *and* the NV variable store — persistent across
# runs, like Q35's OVMF_VARS.fd / ArmVirt's QEMU_VARS.fd. Only seed it if
# missing so provisioned variables survive a rebuild.
FLASH1_PADDED="$VARS_DIR/SBSA_FLASH1_${BUILD_TYPE}.fd"
if [[ ! -f "$FLASH1_PADDED" ]]; then
    echo "→ Seeding fresh FLASH1 (vars) image"
    cp "$FV_DIR/SBSA_FLASH1.fd" "$FLASH1_PADDED"
    truncate -s "$FLASH_SIZE" "$FLASH1_PADDED"
fi

echo "✓ Padded flash images -> $VARS_DIR/"

# ---------- uefi shell -----------------------------------------------------
# Unlike ArmVirtOrreryPkg/Q35Pkg, SbsaQemu.dsc's own FDF already embeds
# ShellPkg's Shell.efi straight into FVMAIN (BdsDxe boots into it directly)
# — no separate uefi-shell.img boot disk needed here.

# ---------- post-build sync (same shape as the other two platforms) -----------
if [[ "$SYNC" -eq 1 ]]; then
    APPS_DIR="$SCRIPT_DIR/shared/apps"
    mkdir -p "$APPS_DIR"

    BUILD_OUT="$REPO_ROOT/$OUTPUT_DIR/${EDK2_BUILD_TARGET}_${TOOLCHAIN}/$ARCH"
    SYNC_LIST="$SCRIPT_DIR/shared/.sync"

    if [[ ! -f "$SYNC_LIST" ]]; then
        echo "  (no shared/.sync found — skipping EFI sync)"
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

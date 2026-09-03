#!/usr/bin/env bash
# =============================================================================
# qemu.sh — Launch QEMU with the OpenBMC image (QEMU 'romulus-bmc', ARM1176)
# Usage: ./qemu.sh [-m MEM] [--link-sbsa[=PORT]] [-h] [-- <extra qemu args>]
#
#   -m MEM            RAM in MB                                (default: 256
#                       — matches OpenBMC's own docs' romulus example; the
#                       image's u-boot/kernel are sized for this, don't
#                       expect more RAM to matter)
#   --link-sbsa[=PORT] Out-of-band "management LAN" mode: the BMC's onboard
#                       NIC is wired to a private point-to-point QEMU socket
#                       link instead of the default host-facing network, and
#                       LISTENS on 127.0.0.1:PORT (default: 8888) for
#                       SbsaOrreryPkg/qemu.sh's --bmc-mgmt=PORT side to
#                       connect. In this mode none of the default hostfwd
#                       ports below are forwarded — see "Two network modes,
#                       not both at once" in docs/openbmc_boot_flow.md for
#                       why (short version: romulus-bmc only lets QEMU back
#                       ONE of its two hardware NIC ports from the command
#                       line; the second is always present but never
#                       connectable, confirmed empirically, so it's one
#                       backend or the other, not both simultaneously).
#   -h                 Show this help
#
# Default (no --link-sbsa): standalone mode, straight from OpenBMC's own
# "Download and Start QEMU Session" doc — usermode networking with hostfwd
# so IPMI/Redfish/SSH are reachable directly from this host's shell, no
# second VM required:
#   ssh                 -> 127.0.0.1:2222
#   Redfish/HTTPS REST   -> 127.0.0.1:2443
#   IPMI-over-LAN (UDP)  -> 127.0.0.1:2623
#
# Differences from the other three platforms' qemu.sh (not just a
# find/replace):
#   - No -pflash / no edk2 vars story at all: romulus-bmc boots straight
#     from the single downloaded image via -drive ...,if=mtd (u-boot ->
#     Linux -> OpenBMC userspace, no UEFI anywhere in this chain — the BMC
#     is a bare-metal embedded Linux box, not a UEFI platform, in real
#     hardware too).
#   - No -g/gdbstub option (yet): this image's u-boot/kernel are prebuilt
#     (see build.sh), so there's no matching source tree/symbols to attach
#     against the way the other three platforms' -g flags assume. Add one
#     if/when OpenBmcPkg grows a from-source build path.
#   - No shared.img: nothing here parses FAT — the BMC's "shared storage"
#     with the outside world is its network services (SSH/SCP, Redfish),
#     not a mounted disk.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------- platform (fixed — this script only runs romulus-bmc) --------------
QEMU_MACHINE="romulus-bmc"
QEMU_BIN="qemu-system-arm"

# ---------- defaults ------------------------------------------------------
MEM_MB=256
LINK_SBSA=0
LINK_PORT=8888

# ---------- args --------------------------------------------------------------
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -m)             MEM_MB="$2";      shift 2 ;;
        --link-sbsa)    LINK_SBSA=1;      shift ;;
        --link-sbsa=*)  LINK_SBSA=1; LINK_PORT="${1#*=}"; shift ;;
        -h)             usage ;;
        --)             shift; EXTRA_ARGS=("$@"); break ;;
         *)              echo "ERROR: Unknown argument: $1" >&2; exit 1 ;;
    esac
done

IMAGE_FILE="$SCRIPT_DIR/images/obmc-phosphor-image-romulus.static.mtd"

# ---------- sanity checks -----------------------------------------------------
if ! command -v "$QEMU_BIN" &>/dev/null; then
    echo "ERROR: $QEMU_BIN not found."
    echo "  sudo apt install qemu-system-arm    (ships aspeed/romulus-bmc"
    echo "  support upstream since well before this repo's QEMU 8.2 baseline"
    echo "  — no OpenBMC-specific QEMU fork needed, unlike OpenBMC's own docs'"
    echo "  'download our custom qemu-system-arm binary' step, which predates"
    echo "  aspeed support landing in mainline QEMU)."
    exit 1
fi

if [[ ! -f "$IMAGE_FILE" ]]; then
    echo "ERROR: OpenBMC image not found:"
    echo "    $IMAGE_FILE"
    echo ""
    echo "  You need to fetch it first: ./build.sh"
    exit 1
fi

# ---------- networking ----------------------------------------------------
NET_ARGS=()
if [[ "$LINK_SBSA" -eq 1 ]]; then
    echo "→ Management-LAN mode: listening on 127.0.0.1:${LINK_PORT} for the"
    echo "  host side (SbsaOrreryPkg/qemu.sh --bmc-mgmt=${LINK_PORT}) to connect."
    echo "  No hostfwd in this mode — see -h."
    NET_ARGS=(-netdev "socket,id=mgmt0,listen=:${LINK_PORT}" -net "nic,netdev=mgmt0")
else
    echo "→ Standalone mode: ssh->2222, Redfish/HTTPS->2443, IPMI(UDP)->2623"
    NET_ARGS=(-nic "user,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:2443-:443,hostfwd=udp:127.0.0.1:2623-:623,hostname=qemu")
fi

# ---------- launch ------------------------------------------------------------
echo "============================================================"
echo "  Platform  : OpenBMC  ($QEMU_MACHINE)"
echo "  RAM       : ${MEM_MB}M"
echo "  Image     : $IMAGE_FILE"
echo "  Login     : root / 0penBmc   (that's a zero, not a capital O)"
echo "  Exit      : Ctrl-A then X    (this is -nographic serial console, not"
echo "               a normal terminal — Ctrl-C won't reliably stop it)"
echo "============================================================"
echo ""

"$QEMU_BIN" \
    -M "$QEMU_MACHINE" \
    -m "${MEM_MB}M" \
    -nographic \
    -drive file="$IMAGE_FILE",format=raw,if=mtd \
    "${NET_ARGS[@]}" \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

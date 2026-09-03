#!/usr/bin/env bash
# =============================================================================
# power-bridge.sh — make the BMC's power control real: BMC chassis power
# state -> actual SbsaOrreryPkg QEMU process, via QMP.
# Usage: ./power-bridge.sh [-i SECONDS] [-h]
#
#   -i SECONDS   Poll interval (default: 2)
#   -h           Show this help
#
# What this is: on real hardware, "ipmitool power on" (or a Redfish
# ComputerSystem.Reset call) doesn't magically start a computer — it drives
# a GPIO line the BMC has wired to the host's power-button/reset headers,
# and the BMC watches a separate GPIO (power-good) to know if it actually
# worked. Two chips, two firmware stacks, one real wire between them. QEMU's
# romulus-bmc has no such wire to anything (there's no host to attach one
# to) — ipmitool against a bare `OpenBmcPkg/qemu.sh` flips an internal
# dbus state and nothing else happens. This script IS that wire, for the
# sandbox: it polls the BMC's own idea of chassis power over IPMI and
# drives SbsaOrreryPkg's actual QEMU process to match, the same shape as
# the real GPIO relationship (BMC decides, host obeys) even though the
# transport (IPMI-over-LAN + QMP, instead of two GPIO pins) isn't.
#
# Requires:
#   - ipmitool                          (sudo apt install ipmitool)
#   - OpenBmcPkg/qemu.sh already running in its DEFAULT (non --link-sbsa)
#     mode — this needs the IPMI hostfwd port, so it's the one thing that
#     doesn't work in --link-sbsa's isolated management-LAN mode. See
#     "Why the bridge needs standalone mode" in docs/openbmc_boot_flow.md.
#   - SbsaOrreryPkg/qemu.sh already running (any mode) with its QMP socket
#     up at SbsaOrreryPkg/vars/qemu-monitor.sock (on by default, see that
#     script's header).
#
# What it actually does, every poll interval:
#   - Reads chassis power state from the BMC: `ipmitool ... power status`
#     over the hostfwd IPMI-over-LAN port.
#   - Off -> On transition it didn't cause itself: launches
#     SbsaOrreryPkg/qemu.sh in the background (a fresh boot — the closest
#     analogue to a real host actually powering up when told to).
#   - On -> Off transition: sends `system_powerdown` over QMP (ACPI-style
#     graceful shutdown request — SbsaOrreryPkg's UEFI doesn't have an OS
#     to honor it gracefully today, so in practice this behaves like a
#     forced power removal; documented as a known gap, not silently papered
#     over).
#   - Reset (power cycle) reported by the BMC: `system_reset` over QMP.
#
# NOT verified end-to-end (see docs/openbmc_boot_flow.md's "What was and
# wasn't verified here"): this script was written against ipmitool's and
# QEMU QMP's documented command syntax, but this sandbox can't fetch the
# real OpenBMC image to test the IPMI side against a live BMC. The QMP
# side (system_powerdown/system_reset/quit against a running
# SbsaOrreryPkg/qemu.sh) uses the same QMP protocol verified working with
# real edk2/TF-A firmware elsewhere in this repo's debugging — only the
# "poll ipmitool, react" loop here is new and untested against a real BMC.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SBSA_DIR="$(cd "$SCRIPT_DIR/../SbsaOrreryPkg" && pwd)"
QMP_SOCK="$SBSA_DIR/vars/qemu-monitor.sock"

IPMI_HOST=127.0.0.1
IPMI_PORT=2623
IPMI_USER=root
IPMI_PASS=0penBmc
POLL_INTERVAL=2

usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
while getopts ":i:h" opt; do
    case $opt in
        i) POLL_INTERVAL="$OPTARG" ;;
        h) usage ;;
        :) echo "ERROR: -$OPTARG requires an argument." >&2; exit 1 ;;
       \?) echo "ERROR: Unknown flag -$OPTARG" >&2; exit 1 ;;
    esac
done

if ! command -v ipmitool &>/dev/null; then
    echo "ERROR: ipmitool not found. sudo apt install ipmitool" >&2
    exit 1
fi

ipmi() {
    ipmitool -I lanplus -H "$IPMI_HOST" -p "$IPMI_PORT" -U "$IPMI_USER" -P "$IPMI_PASS" "$@"
}

# QMP is JSON-lines over a unix socket: it sends a capabilities greeting on
# connect, expects {"execute":"qmp_capabilities"} before anything else works.
qmp() {
    local cmd="$1"
    if [[ ! -S "$QMP_SOCK" ]]; then
        echo "  (QMP socket not found at $QMP_SOCK — is SbsaOrreryPkg/qemu.sh running?)" >&2
        return 1
    fi
    printf '{"execute":"qmp_capabilities"}\n{"execute":"%s"}\n' "$cmd" \
        | timeout 3 socat - "UNIX-CONNECT:$QMP_SOCK" >/dev/null 2>&1 || true
}

if ! command -v socat &>/dev/null; then
    echo "ERROR: socat not found (used to talk QMP). sudo apt install socat" >&2
    exit 1
fi

echo "============================================================"
echo "  power-bridge: BMC (IPMI $IPMI_HOST:$IPMI_PORT) -> host QEMU (QMP $QMP_SOCK)"
echo "  Poll interval: ${POLL_INTERVAL}s.  Ctrl-C to stop."
echo "============================================================"

LAST_STATE=""
SBSA_PID=""

while true; do
    STATE="$(ipmi power status 2>/dev/null || true)"
    # ipmitool prints e.g. "Chassis Power is on" / "... is off"
    if [[ "$STATE" == *"is on"* ]]; then
        CUR="on"
    elif [[ "$STATE" == *"is off"* ]]; then
        CUR="off"
    else
        CUR="unknown"
    fi

    if [[ "$CUR" != "$LAST_STATE" && "$CUR" != "unknown" ]]; then
        echo "$(date '+%H:%M:%S') BMC reports chassis power: $CUR"
        if [[ "$CUR" == "on" && ( -z "$SBSA_PID" || ! -d "/proc/$SBSA_PID" ) ]]; then
            echo "  -> starting SbsaOrreryPkg/qemu.sh"
            ( cd "$SBSA_DIR" && ./qemu.sh > "$SCRIPT_DIR/sbsa-console.log" 2>&1 ) &
            SBSA_PID=$!
        elif [[ "$CUR" == "off" ]]; then
            echo "  -> system_powerdown over QMP"
            qmp "system_powerdown"
        fi
        LAST_STATE="$CUR"
    fi

    sleep "$POLL_INTERVAL"
done

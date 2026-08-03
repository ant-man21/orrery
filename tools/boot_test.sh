#!/usr/bin/env bash
# =============================================================================
# boot_test.sh — headless end-to-end boot test (issue #25)
#
# Usage: ./tools/boot_test.sh [-d|-r] [--phase all|provision|verify]
#                             [--timeout SECS] [--skip-build] [-h]
#
#   -r                Test the RELEASE build   (default)
#   -d                Test the DEBUG build
#   --phase PHASE     all       — provision, then verify   (default)
#                     provision — first-boot provisioning only
#                     verify    — verify only (assumes an already-provisioned
#                                 TPM; does NOT reset TPM state)
#   --timeout SECS    Per-boot wall-clock bound (default: 300)
#   --skip-build      Don't rebuild; use whatever is already in Build/
#   -h                Show this help
#
# What this actually proves, and why it's worth a separate CI job from
# "does it compile":
#
#   provision  boots TpmProvisionApp — measures the live ROM, extends PCR[16],
#              verifies this build's signed ticket, and writes the secret into
#              a PolicyAuthorize-gated NV index.
#   verify     reboots into TpmVerifyBootApp against the *same* TPM state — it
#              re-measures, re-derives PCR[16], and can only read the secret
#              back out if the firmware is byte-identical to what was
#              provisioned. This is the actual security property the project
#              exists to demonstrate; a compile check can't see it at all.
#
# Both phases share one TPM: `all` resets TPM state once up front, then runs
# the two boots against it in order. Resetting between them would defeat the
# entire point of the verify step.
#
# KNOWN FAILING: --phase verify / --phase all currently FAIL on main, and the
# failure is real, not a harness bug. PR #16 re-gated the NV index's authPolicy
# on TPM2_PolicyAuthorize and rewrote TpmProvisionApp's write path around it,
# but TpmVerifyBootApp was never updated to match — it still builds a
# PolicyPCR-only session digest (no Tpm2PolicyAuthorizeLib in its .inf, no
# ticket read, no PolicyAuthorize call), which no longer satisfies the index's
# authPolicy, so Tpm2NvRead is refused. The app then misreports that refusal as
# "Unseal FAILED — BIOS modified", even though PCR[16] is byte-identical across
# both boots (this harness prints both values; compare them yourself). #16's own
# commit message flags the chain as "shared with what TpmVerifyBootApp's read
# WILL use" — future tense, never landed. Tracked separately; CI runs
# --phase provision until TpmVerifyBootApp is brought onto the same chain.
#
# Exit codes:  0 = pass, 1 = fail (details on stderr + the captured serial log)
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_DIR="$REPO_ROOT/Q35Pkg"

# ---------- defaults ----------------------------------------------------------
BUILD_TYPE="RELEASE"
BUILD_FLAG="-r"
PHASE="all"
TIMEOUT_SECS=300
SKIP_BUILD=0
LOG_DIR="$PKG_DIR/boot-test-logs"

usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -r)           BUILD_TYPE="RELEASE"; BUILD_FLAG="-r"; shift ;;
        -d)           BUILD_TYPE="DEBUG";   BUILD_FLAG="-d"; shift ;;
        --phase)      PHASE="$2";       shift 2 ;;
        --timeout)    TIMEOUT_SECS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1;     shift ;;
        -h)           usage ;;
         *)           echo "ERROR: Unknown argument: $1" >&2; exit 1 ;;
    esac
done

case "$PHASE" in
    all|provision|verify) ;;
    *) echo "ERROR: --phase must be all, provision, or verify (got '$PHASE')" >&2; exit 1 ;;
esac

# ---------- dependency check --------------------------------------------------
# Fail loudly and all at once rather than dying halfway through a build.
MISSING=()
for tool in qemu-system-x86_64 swtpm mformat mcopy timeout python3; do
    command -v "$tool" &>/dev/null || MISSING+=("$tool")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "ERROR: missing required tools: ${MISSING[*]}" >&2
    echo "  Install with: sudo apt-get install qemu-system-x86 swtpm mtools" >&2
    exit 1
fi

mkdir -p "$LOG_DIR"

# ---------- signing key -------------------------------------------------------
# The firmware embeds this key's public half (TrustedUpdateKey.h) and the
# ticket is signed with its private half — they have to be the same keypair or
# Tpm2VerifySignature rejects the ticket and provisioning fails.
KEY_PEM="$REPO_ROOT/tools/keys/update_signing_key.pem"
if [[ ! -f "$KEY_PEM" ]]; then
    echo "→ No signing key found — generating a throwaway one for this test run."
    echo "  NOTE: this rewrites OrreryPkg/Include/TrustedUpdateKey.h (tracked!)."
    echo "        Expect a dirty git tree afterwards; revert it if you didn't"
    echo "        mean to rotate keys:  git checkout -- OrreryPkg/Include/TrustedUpdateKey.h"
    python3 "$SCRIPT_DIR/generate_signing_key.py"
fi

# ---------- build -------------------------------------------------------------
# .sync tells build.sh which .efi files to push onto shared.img. Without it the
# apps never reach fs1:\apps\ and every boot would "fail" for the wrong reason.
SYNC_LIST="$PKG_DIR/shared/.sync"
mkdir -p "$PKG_DIR/shared"
if [[ ! -f "$SYNC_LIST" ]]; then
    echo "→ Creating $SYNC_LIST (was missing)"
    printf 'TpmProvisionApp.efi\nTpmVerifyBootApp.efi\n' > "$SYNC_LIST"
fi

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    echo "→ Building $BUILD_TYPE firmware + signing this build's ticket..."
    "$PKG_DIR/build.sh" "$BUILD_FLAG"
else
    echo "→ --skip-build: using existing Build/ output"
fi

# ---------- state reset -------------------------------------------------------
SWTPM_DIR="$PKG_DIR/chips/q35/tpm"
VARS_FD="$PKG_DIR/chips/q35/vars/OVMF_VARS_${BUILD_TYPE}.fd"

reset_state() {
    # Kill swtpm *before* deleting its state — a live swtpm can flush its own
    # NVRAM back out on exit and silently un-reset what we just cleared.
    pkill -f "swtpm socket.*$SWTPM_DIR" 2>/dev/null || true
    sleep 0.3
    mkdir -p "$SWTPM_DIR"
    bash "$SCRIPT_DIR/tpm_reset.sh" || true
    # Force qemu.sh to reseed a pristine variable store too, so a stale Boot####
    # entry from an earlier run can't change what actually boots.
    rm -f "$VARS_FD"
}

if [[ "$PHASE" != "verify" ]]; then
    echo "→ Resetting TPM + UEFI variable state (clean provisioning run)"
    reset_state
else
    echo "→ --phase verify: keeping existing TPM state (that's the point)"
fi

# ---------- startup.nsh -------------------------------------------------------
# The shell already looks for startup.nsh on its own boot volume and runs it
# after a 5-second ESC window — nothing was ever providing one, which is why
# every boot until now needed a human to type fs1:, cd apps, <app>.efi.
#
# fs0:/fs1: enumeration order is explicitly NOT stable in this project (see
# docs/anti_tamper_roadmap.md), so this probes each volume for the app instead
# of hardcoding fs1:. `reset -s` at the end powers the VM off as soon as the
# app returns — that's the end-of-test signal, so a passing run finishes in
# seconds instead of burning the full timeout at an idle shell prompt.
write_startup_nsh() {
    local app="$1"
    local nsh="$LOG_DIR/startup.nsh"
    cat > "$nsh" <<EOF
@echo -off
echo ORRERY-BOOT-TEST: startup.nsh running, looking for $app
for %f in fs0 fs1 fs2 fs3 fs4
  if exist %f:\\apps\\$app then
    echo ORRERY-BOOT-TEST: found $app on %f:
    %f:
    cd \\apps
    $app
  endif
endfor
echo ORRERY-BOOT-TEST-COMPLETE
reset -s
EOF
    mcopy -o -i "$PKG_DIR/uefi-shell.img" "$nsh" ::
}

# ---------- one boot ----------------------------------------------------------
# Returns 0/1 on the grep result. Deliberately does not trust QEMU's exit code:
# a timeout (124) with a passing sentinel already in the log is still a pass,
# and a clean exit proves nothing about what the app actually did.
run_boot() {
    local app="$1" expect="$2" reject="${3:-}" label="$4"
    local serial_log="$LOG_DIR/serial-${label}.log"

    echo ""
    echo "============================================================"
    echo "  BOOT: $label  ($app)"
    echo "  expecting: $expect"
    echo "============================================================"

    write_startup_nsh "$app"

    set +e
    "$PKG_DIR/qemu.sh" "$BUILD_FLAG" \
        --headless \
        --serial-log "$serial_log" \
        --timeout "$TIMEOUT_SECS"
    local qemu_rc=$?
    set -e

    if [[ ! -s "$serial_log" ]]; then
        echo "FAIL [$label]: serial log is empty — the guest produced no output at all." >&2
        echo "  QEMU exit code was $qemu_rc." >&2
        return 1
    fi

    # Strip the ANSI/cursor escapes the shell paints its UI with, so grepping
    # for a plain string doesn't miss a match that has a colour code in it.
    local clean_log="${serial_log%.log}.clean.log"
    sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' -e 's/\x1b[=>]//g' "$serial_log" > "$clean_log"

    if ! grep -qF "ORRERY-BOOT-TEST: startup.nsh running" "$clean_log"; then
        echo "FAIL [$label]: startup.nsh never ran — the shell didn't pick it up." >&2
        tail -30 "$clean_log" >&2
        return 1
    fi

    if [[ -n "$reject" ]] && grep -qF "$reject" "$clean_log"; then
        echo "FAIL [$label]: found the failure marker '$reject' in the log." >&2
        grep -F "$reject" "$clean_log" >&2
        return 1
    fi

    if ! grep -qF "$expect" "$clean_log"; then
        echo "FAIL [$label]: expected '$expect' but it never appeared." >&2
        echo "  --- last 40 lines of $clean_log ---" >&2
        tail -40 "$clean_log" >&2
        return 1
    fi

    echo "PASS [$label]: found '$expect'"
    return 0
}

# ---------- scenario ----------------------------------------------------------
RC=0

if [[ "$PHASE" == "all" || "$PHASE" == "provision" ]]; then
    run_boot "TpmProvisionApp.efi" \
             "=== TpmProvisionApp done (Success) ===" \
             "" \
             "provision" || RC=1
fi

if [[ "$RC" -eq 0 && ( "$PHASE" == "all" || "$PHASE" == "verify" ) ]]; then
    run_boot "TpmVerifyBootApp.efi" \
             "[VERIFY] Unseal succeeded" \
             "[VERIFY] Unseal FAILED" \
             "verify" || RC=1
fi

# Leave no swtpm behind holding a socket the next run would collide with.
pkill -f "swtpm socket.*$SWTPM_DIR" 2>/dev/null || true

echo ""
echo "============================================================"
if [[ "$RC" -eq 0 ]]; then
    echo "  TEST: PASS  (phase=$PHASE, build=$BUILD_TYPE)"
else
    echo "  TEST: FAIL  (phase=$PHASE, build=$BUILD_TYPE)"
    echo "  Logs: $LOG_DIR/"
fi
echo "============================================================"
exit "$RC"

#!/usr/bin/env bash
# =============================================================================
# build.sh — fetch a bootable OpenBMC image for OpenBmcPkg (QEMU romulus-bmc)
# Usage: ./build.sh [-f] [-h]
#
#   -f   Force re-download even if images/obmc-phosphor-image-romulus.static.mtd
#        already exists.
#   -h   Show this help
#
# Unlike the other three platforms in this repo, OpenBmcPkg doesn't build
# anything from source. It downloads the same prebuilt Romulus image
# OpenBMC's own getting-started docs point newcomers at:
#
#   https://jenkins.openbmc.org/job/latest-master/label=docker-builder,target=romulus/
#     lastSuccessfulBuild/artifact/openbmc/build/tmp/deploy/images/romulus/
#     obmc-phosphor-image-romulus.static.mtd
#
# (see openbmc/docs' development/dev-environment.md, "Download and Start QEMU
# Session" — this is the exact URL and machine that doc uses.)
#
# Why not build it from source like edk2/TF-A/StandaloneMm are built
# elsewhere in this repo: a from-scratch OpenBMC build is a Yocto/bitbake
# build of the whole embedded Linux distro (kernel, u-boot, busybox,
# systemd, phosphor-* dbus services, bmcweb, ...) — 30-120 minutes and
# 60-100+ GB of disk on a normal machine, fetching from a wide scatter of
# hosts (kernel.org, GNU mirrors, sourceforge, individual project sites)
# well beyond just GitHub. That's a legitimate thing to want later (see the
# "Building from source instead" section of docs/openbmc_boot_flow.md for
# the real recipe, upstream-documented), but it's the opposite of the fast
# "see it boot" path this issue asked for, so it isn't what -f/default does
# here.
#
# A note on trust: this pulls a single, official, CI-built artifact from
# the OpenBMC project's own Jenkins — the same one their own docs link to
# for newcomers, not a third-party rebuild. Nothing here fetches from an
# unofficial or unverified source.
#
# IMPORTANT — this download will fail in some sandboxed dev environments:
# jenkins.openbmc.org has to be reachable. It was NOT reachable from the
# sandbox this package was originally written in (network egress there is
# allowlisted to a small set of hosts — GitHub, package registries — and
# jenkins.openbmc.org isn't one of them; confirmed with a plain `curl -I`
# returning a 403 from the sandbox's own egress proxy, not from Jenkins
# itself). If this script fails to download, that's almost certainly what's
# happening — see docs/openbmc_boot_flow.md's "What was and wasn't verified
# here" section before assuming the URL itself is stale. On a normal
# unrestricted machine (your own laptop/desktop) this should just work.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE_URL="https://jenkins.openbmc.org/job/latest-master/label=docker-builder,target=romulus/lastSuccessfulBuild/artifact/openbmc/build/tmp/deploy/images/romulus/obmc-phosphor-image-romulus.static.mtd"
IMAGES_DIR="$SCRIPT_DIR/images"
IMAGE_FILE="$IMAGES_DIR/obmc-phosphor-image-romulus.static.mtd"

FORCE=0
usage() {
    sed -n '/^# Usage/,/^# ====/p' "$0" | grep -v '^# ===='
    exit 0
}
while getopts ":fh" opt; do
    case $opt in
        f) FORCE=1 ;;
        h) usage ;;
        \?) echo "ERROR: Unknown flag -$OPTARG" >&2; exit 1 ;;
    esac
done

mkdir -p "$IMAGES_DIR"

if [[ -f "$IMAGE_FILE" && "$FORCE" -eq 0 ]]; then
    echo "✓ Already have $IMAGE_FILE ($(du -h "$IMAGE_FILE" | cut -f1))"
    echo "  Pass -f to re-download the latest build."
    exit 0
fi

echo "============================================================"
echo "  Platform  : OpenBMC (QEMU romulus-bmc, ARM1176)"
echo "  Source    : OpenBMC's own Jenkins CI (latest-master/romulus)"
echo "  Target    : $IMAGE_FILE"
echo "============================================================"
echo ""
echo "→ Downloading obmc-phosphor-image-romulus.static.mtd ..."

TMP_FILE="$(mktemp "$IMAGES_DIR/.download.XXXXXX")"
cleanup() { rm -f "$TMP_FILE"; }
trap cleanup EXIT

if ! curl -fL --retry 3 --retry-delay 2 -o "$TMP_FILE" "$IMAGE_URL"; then
    echo ""
    echo "ERROR: download failed. Most likely cause in a restricted/sandboxed"
    echo "  dev environment: jenkins.openbmc.org isn't reachable through this"
    echo "  network's egress policy. Try:"
    echo "    curl -I '$IMAGE_URL'"
    echo "  If that also fails with a 403 from a proxy (not from Jenkins itself),"
    echo "  this is a network policy issue, not a broken URL or a code bug —"
    echo "  see docs/openbmc_boot_flow.md. Run this script from an unrestricted"
    echo "  machine instead."
    exit 1
fi

mv "$TMP_FILE" "$IMAGE_FILE"
trap - EXIT

echo "✓ Downloaded -> $IMAGE_FILE ($(du -h "$IMAGE_FILE" | cut -f1))"
echo ""
echo "Next: ./qemu.sh"

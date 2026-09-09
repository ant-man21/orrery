#!/usr/bin/env bash
# Entrypoint for the orrery-build image. Runs as root inside the container
# against /workspace, a bind mount of the repo (see ../docker-compose.yml),
# so every build compiles whatever is on disk on the host right now.
set -euo pipefail

REPO_ROOT="/workspace"
cd "$REPO_ROOT"

# The bind-mounted repo is owned by the host user's UID, not root -- git
# 2.35+'s CVE-2022-24765 fix refuses to operate on a directory it doesn't
# own even when running as root. Trust everything under /workspace.
git config --global --add safe.directory '*'

# Submodules (edk2, edk2-platforms, edk2-non-osi, trusted-firmware-a, plus
# edk2's own nested submodules) are gitignored/not checked out on a fresh
# clone. Initialize them on first run so `git clone && docker compose run`
# is enough to get a working build -- skip on later runs so every build
# doesn't pay for a submodule-status network round trip.
#   If your submodule remotes require auth this container doesn't have,
#   run `git submodule update --init --recursive` on the host first (with
#   your own git credentials) -- this step then sees edksetup.sh already
#   present and skips straight past it.
if [[ ! -f edk2/edksetup.sh ]]; then
    echo "==> Initializing git submodules (first run only, can take a few minutes)..."
    git submodule update --init --recursive
fi

# Mirrors .github/workflows/build.yml's unconditional "Build BaseTools"
# step -- `make` is incremental, so re-running this on a repeat build is
# cheap once BaseTools' C tools are already built.
echo "==> Building BaseTools..."
make -C edk2/BaseTools -j"$(nproc)"

usage() {
    cat <<'USAGE'
Usage: docker compose run --rm build <target> [build.sh flags...]

  target:
    q35        Build Q35Pkg          (QEMU q35/OVMF, X64)
    arm-virt   Build ArmVirtOrreryPkg (QEMU virt, AArch64)
    sbsa       Build SbsaOrreryPkg    (QEMU sbsa-ref, AArch64 + trusted-firmware-a)
    all        Build all three, in that order
    <command>  Anything else runs verbatim inside the build image, e.g.
               `docker compose run --rm build bash` for an interactive shell

  [build.sh flags] are passed straight through to the platform's own
  build.sh, e.g.:
    docker compose run --rm build q35 -d -C
    docker compose run --rm build sbsa -M

  See Q35Pkg/build.sh, ArmVirtOrreryPkg/build.sh, SbsaOrreryPkg/build.sh
  (each has its own -h) and docs/docker_build.md for details.
USAGE
}

case "${1:-}" in
    q35)
        shift
        exec ./Q35Pkg/build.sh "$@"
        ;;
    arm-virt)
        shift
        exec ./ArmVirtOrreryPkg/build.sh "$@"
        ;;
    sbsa)
        shift
        exec ./SbsaOrreryPkg/build.sh "$@"
        ;;
    all)
        shift
        ./Q35Pkg/build.sh "$@"
        ./ArmVirtOrreryPkg/build.sh "$@"
        ./SbsaOrreryPkg/build.sh "$@"
        ;;
    ""|-h|--help|help)
        usage
        ;;
    *)
        exec "$@"
        ;;
esac

# Build image for compiling Orrery's UEFI firmware (Q35, ArmVirtOrreryPkg,
# SbsaOrreryPkg) with Docker instead of setting up the edk2 toolchain by
# hand. This image holds the toolchain only -- it does NOT bundle the
# source. docker-compose.yml bind-mounts the repo at /workspace and runs
# entrypoint.sh inside it, so builds always compile your local checkout,
# including uncommitted changes.
#
# Package list is a direct mirror of .github/workflows/build.yml's "Install
# build dependencies" step -- that workflow is the only build recipe this
# project actually verifies in CI, so the Docker image tracks it exactly
# rather than drifting into its own list. See that file if a platform's
# build starts failing here with a missing-tool error; update both together.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential uuid-dev iasl nasm python3 \
        gcc-x86-64-linux-gnu gcc-aarch64-linux-gnu \
        libssl-dev \
        mtools \
        git make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

WORKDIR /workspace
ENTRYPOINT ["/entrypoint.sh"]

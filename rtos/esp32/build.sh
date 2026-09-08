#!/usr/bin/env bash
# Build the firmware -- natively if you already have ESP-IDF installed
# and sourced (checks for `idf.py` on PATH first), otherwise falls back
# to Espressif's official Docker image so a fresh clone always builds
# somewhere with zero setup. The Docker fallback is pinned to a specific
# image tag (not `latest`) so it always builds against the exact same
# compiler/SDK version, on this machine or CI.
#
# Usage:
#   ./build.sh              # idf.py build (default)
#   ./build.sh fullclean     # any idf.py subcommand works
#
# Flashing/monitoring over USB (and J-Link JTAG debugging) need direct
# device access the Docker path doesn't have by default -- see
# README.md for those; either path here is for compiling only.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

CMD="${*:-build}"

if command -v idf.py >/dev/null 2>&1; then
    echo "Using local ESP-IDF install ($(command -v idf.py))"
    idf.py set-target esp32
    idf.py $CMD
    exit 0
fi

echo "No local idf.py on PATH -- falling back to Docker (espressif/idf:v5.3.1)"
IDF_DOCKER_IMAGE="espressif/idf:v5.3.1"

docker run --rm \
    -v "$PWD":/project \
    -w /project \
    -u "$(id -u)":"$(id -g)" \
    -e HOME=/tmp \
    "$IDF_DOCKER_IMAGE" \
    bash -c ". \"\$IDF_PATH/export.sh\" && idf.py set-target esp32 && idf.py $CMD"
    # Sourcing export.sh explicitly rather than trusting the image's own
    # ENTRYPOINT to do it: that assumption broke CI, where GitHub
    # Actions execs into the container directly and never runs the
    # image's entrypoint at all. Doing it ourselves here works either way.

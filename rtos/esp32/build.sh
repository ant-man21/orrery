#!/usr/bin/env bash
# Reproducible build via Espressif's official ESP-IDF Docker image --
# no local toolchain install required, just Docker. Pinned to a specific
# image tag (not `latest`) so this always builds against the exact same
# compiler/SDK version, on this machine or CI.
#
# Usage:
#   ./build.sh              # idf.py build (default)
#   ./build.sh fullclean     # any idf.py subcommand works
#
# Flashing/monitoring over USB (and J-Link JTAG debugging) need direct
# device access this container doesn't have by default -- see README.md
# for those; this script is for compiling only.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

IDF_DOCKER_IMAGE="espressif/idf:v5.3.1"
CMD="${*:-build}"

docker run --rm \
    -v "$PWD":/project \
    -w /project \
    -u "$(id -u)":"$(id -g)" \
    -e HOME=/tmp \
    "$IDF_DOCKER_IMAGE" \
    bash -c ". \"\$IDF_PATH/export.sh\" && idf.py set-target esp32s3 && idf.py $CMD"
    # Sourcing export.sh explicitly rather than trusting the image's own
    # ENTRYPOINT to do it: that assumption broke CI, where GitHub
    # Actions execs into the container directly and never runs the
    # image's entrypoint at all. Doing it ourselves here works either way.

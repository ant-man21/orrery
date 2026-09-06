#!/usr/bin/env bash
# Flash prebuilt Orrery firmware onto an ESP32-S3 over USB -- the ESP32
# equivalent of clicking "Run" in STM32CubeIDE. Needs only `esptool`
# (a small pip package), NOT the full ESP-IDF toolchain: it just writes
# the .bin files already produced by ./build.sh, a local `idf.py build`,
# or a downloaded CI artifact -- it doesn't rebuild anything.
#
# Usage:
#   ./flash.sh                 # auto-detect port, flash ./build
#   ./flash.sh /dev/ttyACM0    # flash a specific port
#   BUILD_DIR=~/Downloads/firmware-esp32 ./flash.sh   # flash a CI artifact
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

BUILD_DIR="${BUILD_DIR:-build}"
PORT="${1:-}"

if [ ! -f "$BUILD_DIR/flash_args" ]; then
    echo "No build found at '$BUILD_DIR/flash_args'." >&2
    echo "Run ./build.sh first, or set BUILD_DIR to an extracted CI artifact." >&2
    exit 1
fi

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1 || true)
    if [ -z "$PORT" ]; then
        echo "No serial port auto-detected. Plug in the board (either USB-C" >&2
        echo "port) and pass one explicitly: ./flash.sh /dev/ttyACM0" >&2
        exit 1
    fi
    echo "Auto-detected port: $PORT"
fi

if ! python3 -c "import esptool" >/dev/null 2>&1; then
    echo "Installing esptool (one-time, ~1MB, no toolchain needed)..."
    pip install --user esptool
fi

cd "$BUILD_DIR"
python3 -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash @flash_args

cat <<EOF

Flashed. If the board didn't reset into download mode on its own (rare,
but happens over the native USB-OTG port with some boards): hold BOOT,
tap RESET, release BOOT, then re-run this script.

To watch the log:
  pip install --user esp-idf-monitor
  python3 -m esp_idf_monitor --port $PORT
EOF

#!/usr/bin/env bash
# =============================================================================
# post-run.sh — pull files the VM wrote into fs1:\data\ back onto the host.
#
# Usage: ./post-run.sh
#
# shared.img is a real FAT disk image, not a live host-folder mount —
# anything a UEFI app writes to fs1:\data\ (dummy.fd, sealed blobs, ...)
# stays inside shared.img until it's explicitly pulled back out. Run this
# after a qemu session to copy everything under ::data/ down into
# Q35Pkg/shared/data/ on the host (overwrite-only: files already in
# shared/data/ that aren't also on the image are left alone).
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHARED_IMG="$SCRIPT_DIR/shared.img"
DATA_DIR="$SCRIPT_DIR/shared/data"

if [[ ! -f "$SHARED_IMG" ]]; then
    echo "ERROR: $SHARED_IMG not found — nothing to pull from" >&2
    exit 1
fi

mkdir -p "$DATA_DIR"

echo "→ Pulling fs1:\\data\\ from shared.img into $DATA_DIR ..."
if ! mcopy -o -n -i "$SHARED_IMG" -s ::data/* "$DATA_DIR" 2>/dev/null; then
    echo "  (::data/ is empty — nothing to pull)"
    exit 0
fi

echo "✓ Pulled:"
ls -la "$DATA_DIR"

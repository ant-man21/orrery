#!/usr/bin/env bash
# Clears swtpm state (NVRAM, EK/SRK data, etc.) for every chipset's tpm/ directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORRERY_DIR="$SCRIPT_DIR/.."

shopt -s nullglob
tpm_dirs=("$ORRERY_DIR"/Q35Pkg/chips/*/tpm "$ORRERY_DIR"/ArmVirtOrreryPkg/tpm)

if [ ${#tpm_dirs[@]} -eq 0 ]; then
    echo "No tpm directories found"
    exit 0
fi

for dir in "${tpm_dirs[@]}"; do
    # ArmVirtOrreryPkg/tpm is a literal path, not a glob, so nullglob can't
    # drop it when it doesn't exist — on a checkout where that platform has
    # never been run, `find` would error out on a missing directory.
    [ -d "$dir" ] || continue
    chipset="$(basename "$(dirname "$dir")")"
    count=$(find "$dir" -mindepth 1 | wc -l)
    if [ "$count" -eq 0 ]; then
        echo "→ $chipset: already empty"
        continue
    fi
    find "$dir" -mindepth 1 -delete
    echo "→ $chipset: removed $count file(s) from $dir"
done

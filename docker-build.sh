#!/usr/bin/env bash
# Shorthand for `docker compose run --rm build "$@"` -- see docker-compose.yml
# and docs/docker_build.md. Usage: ./docker-build.sh <q35|arm-virt|sbsa|all> [flags]
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec docker compose run --rm build "$@"

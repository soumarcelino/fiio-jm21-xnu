#!/usr/bin/env bash
# Quiet candidate: builds only, using the persistent Data profile. Never formats.
set -euo pipefail
cd "$(dirname "$0")/.."
export JM21_SILENT=1
exec bash tools/build-storage.sh

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export JM21_KERNEL_TREE="$PWD"
export JM21_KERNEL="$PWD/hakios-kernel"
: "${JM21_SYSTEM_TREE:?set JM21_SYSTEM_TREE}"
: "${JM21_FIRMWARE_DIR:?set JM21_FIRMWARE_DIR}"
test -s "$JM21_KERNEL"
for file in "$JM21_SYSTEM_TREE/var.skel" "$JM21_SYSTEM_TREE/fetch-from-ipsw.py"; do
 test -s "$file" || { echo "Missing private input: $file" >&2; exit 1; }
done
exec bash loader/tools/build-storage.sh

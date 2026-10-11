#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${PEPPER_DIR:?Set PEPPER_DIR to the Samsung Pepper 47 SDK directory}"
make -C "$ROOT/nacl" PEPPER_DIR="$PEPPER_DIR" all
python3 "$ROOT/scripts/package.py"

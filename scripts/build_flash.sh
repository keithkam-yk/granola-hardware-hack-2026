#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_ROOT="$PROJECT_ROOT/.tools/esp-idf"
export IDF_TOOLS_PATH="$PROJECT_ROOT/.tools/espressif"
DEVICE="${1:-/dev/cu.usbmodem1101}"

if [[ ! -f "$IDF_ROOT/export.sh" ]]; then
  echo "ESP-IDF is missing. Run scripts/setup_toolchain.sh first." >&2
  exit 1
fi

source "$IDF_ROOT/export.sh"
cd "$PROJECT_ROOT/firmware"
idf.py set-target esp32s3
idf.py build
idf.py -p "$DEVICE" flash

#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_ROOT="$PROJECT_ROOT/.tools"
IDF_ROOT="$TOOLS_ROOT/esp-idf"
export IDF_TOOLS_PATH="$TOOLS_ROOT/espressif"

mkdir -p "$TOOLS_ROOT"
if [[ ! -d "$IDF_ROOT/.git" ]]; then
  git clone --branch v5.5.5 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$IDF_ROOT"
fi

"$IDF_ROOT/install.sh" esp32s3
source "$IDF_ROOT/export.sh"
python -m pip install cmake ninja
echo "ESP-IDF is ready at $IDF_ROOT"

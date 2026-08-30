#!/usr/bin/env bash
# Build and flash the controller firmware.
#   ./flash.sh                    the game controller
#   DOGFIGHT_PROBE=1 ./flash.sh   the button-discovery probe
set -e
. "$HOME/esp/esp-idf/export.sh" >/dev/null
cd "$(dirname "$0")/firmware"
idf.py build
idf.py -p "${1:-/dev/cu.usbmodem101}" flash

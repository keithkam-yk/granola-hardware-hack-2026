#!/usr/bin/env bash
# Start the game host. Controllers on wifi find it by themselves; a board on USB
# needs --serial.
#
#   ./run.sh                                   wifi controllers only
#   ./run.sh --serial /dev/cu.usbmodem101      also take the board on the cable
set -e
PY="$HOME/.espressif/python_env/idf5.4_py3.10_env/bin/python"
[ -x "$PY" ] || PY=python3
exec "$PY" "$(dirname "$0")/host/dogfight.py" "$@"

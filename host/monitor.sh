#!/usr/bin/env bash
# Watch the device's serial output. Ctrl-C to stop.
PORT="${1:-/dev/cu.usbmodem101}"
stty -f "$PORT" 115200 raw
exec cat "$PORT"

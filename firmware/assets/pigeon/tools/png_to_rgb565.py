#!/usr/bin/env python3
"""Convert the controller preview PNG to LVGL-ready little-endian RGB565."""

import argparse
import struct
from pathlib import Path

from PIL import Image


WIDTH = 448
HEIGHT = 368


def rgb565(red: int, green: int, blue: int) -> int:
    red5 = (red * 31 + 127) // 255
    green6 = (green * 63 + 127) // 255
    blue5 = (blue * 31 + 127) // 255
    return (red5 << 11) | (green6 << 5) | blue5


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    image = Image.open(args.source).convert("RGB")
    if image.size != (WIDTH, HEIGHT):
        raise SystemExit(f"expected {WIDTH}x{HEIGHT}, got {image.width}x{image.height}")

    output = bytearray()
    for red, green, blue in image.get_flattened_data():
        output.extend(struct.pack("<H", rgb565(red, green, blue)))

    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(output)
    print(f"wrote {len(output)} bytes to {args.destination}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Predecode a GIF into sequential LVGL ARGB8888 (BGRA byte order) frames."""

import argparse
from pathlib import Path

from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    image = Image.open(args.source)
    output = bytearray()
    frame_count = 0
    durations = []

    while True:
        frame = image.convert("RGBA")
        durations.append(image.info.get("duration", 0))
        for red, green, blue, alpha in frame.get_flattened_data():
            output.extend((blue, green, red, alpha))
        frame_count += 1
        try:
            image.seek(image.tell() + 1)
        except EOFError:
            break

    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(output)
    print(
        f"wrote {frame_count} {image.width}x{image.height} frames "
        f"({durations}, {len(output)} bytes) to {args.destination}"
    )


if __name__ == "__main__":
    main()

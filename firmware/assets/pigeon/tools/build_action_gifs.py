#!/usr/bin/env python3
"""Build compact transparent action GIFs from four-column sprite sheets."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


TRANSPARENT_INDEX = 255


def split_sheet(sheet: Image.Image, frame_count: int) -> list[Image.Image]:
    width, height = sheet.size
    if width % frame_count:
        raise ValueError(f"sheet width {width} is not divisible by {frame_count}")
    frame_width = width // frame_count
    return [
        sheet.crop((i * frame_width, 0, (i + 1) * frame_width, height))
        for i in range(frame_count)
    ]


def resize_frame(frame: Image.Image, size: tuple[int, int], stretch: bool) -> Image.Image:
    if stretch:
        return frame.resize(size, Image.Resampling.NEAREST)

    max_width = size[0] - 4
    max_height = size[1] - 4
    scale = min(max_width / frame.width, max_height / frame.height)
    resized = frame.resize(
        (max(1, round(frame.width * scale)), max(1, round(frame.height * scale))),
        Image.Resampling.NEAREST,
    )
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    x = (size[0] - resized.width) // 2
    y = (size[1] - resized.height) // 2
    canvas.alpha_composite(resized, (x, y))
    return canvas


def shared_palette(frames: list[Image.Image]) -> Image.Image:
    strip = Image.new("RGB", (frames[0].width, frames[0].height * len(frames)))
    for index, frame in enumerate(frames):
        rgb = Image.new("RGB", frame.size)
        rgb.paste(frame.convert("RGB"), mask=frame.getchannel("A"))
        strip.paste(rgb, (0, index * frame.height))
    return strip.quantize(colors=255, method=Image.Quantize.MEDIANCUT)


def quantize_frame(frame: Image.Image, palette: Image.Image) -> Image.Image:
    rgb = Image.new("RGB", frame.size)
    rgb.paste(frame.convert("RGB"), mask=frame.getchannel("A"))
    indexed = rgb.quantize(palette=palette)
    alpha = frame.getchannel("A")
    pixels = bytearray(indexed.tobytes())
    for offset, opacity in enumerate(alpha.tobytes()):
        if opacity < 128:
            pixels[offset] = TRANSPARENT_INDEX
    indexed.frombytes(bytes(pixels))
    indexed.info["transparency"] = TRANSPARENT_INDEX
    return indexed


def build_gif(
    source: Path,
    output: Path,
    size: tuple[int, int],
    durations: list[int],
    stretch: bool,
) -> None:
    sheet = Image.open(source).convert("RGBA")
    frames = [resize_frame(frame, size, stretch) for frame in split_sheet(sheet, len(durations))]
    palette = shared_palette(frames)
    indexed = [quantize_frame(frame, palette) for frame in frames]
    output.parent.mkdir(parents=True, exist_ok=True)
    indexed[0].save(
        output,
        save_all=True,
        append_images=indexed[1:],
        duration=durations,
        loop=0,
        disposal=2,
        transparency=TRANSPARENT_INDEX,
        optimize=False,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--size", required=True, help="WIDTHxHEIGHT")
    parser.add_argument("--durations", required=True, help="comma-separated milliseconds")
    parser.add_argument(
        "--stretch",
        action="store_true",
        help="map each full sprite-sheet cell directly to the output frame",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    width, height = (int(value) for value in args.size.lower().split("x", 1))
    durations = [int(value) for value in args.durations.split(",")]
    build_gif(args.source, args.output, (width, height), durations, args.stretch)


if __name__ == "__main__":
    main()

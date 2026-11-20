#!/usr/bin/env python3
"""Turn raw magenta-background hero renders into editable 512x512 PNGs.

New hero art arrives as large 1920x1920 JPEGs with a flat magenta background.
The sprite pipeline needs transparent PNGs, and the repository should not keep
multi-megabyte originals. This script:

* removes the background with a flood fill from the image border, so
  magenta-like colours inside the character (eyes, aura) survive;
* writes a 512x512 RGBA PNG next to the other hero sources;
* re-encodes only the processed raw JPEGs at 1024x1024, quality 88.

Existing PNG sources are left alone unless --force is given, so hand edits are
never overwritten by accident. Run from the repository root:

    python tools/prepare_hero_sources.py
    python tools/prepare_hero_sources.py --prefix dark royal
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: python -m pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "pixel-heroes"
RAW = SRC / "raw"

PNG_SIZE = 512
RAW_MAX = 1024
RAW_BYTES = 200_000
JPEG_QUALITY = 88
FLOOD_THRESHOLD = 110
SENTINEL = (0, 255, 0)
SOURCE_RE = re.compile(
    r"^ultra-(?P<hero>[a-z]+)-\d{2}-"
    r"(?P<pose>idle|fight|fly|beam|charge|kick|guard|victory)\.jpg$")


def is_magenta(pixel: tuple[int, int, int]) -> bool:
    r, g, b = pixel
    return r > 170 and b > 120 and g < 140 and (r - g) > 60 and (b - g) > 30


def remove_background(image: Image.Image) -> Image.Image:
    image = image.convert("RGB")
    width, height = image.size
    seeds = [
        (0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1),
        (width // 2, 0), (0, height // 2),
        (width - 1, height // 2), (width // 2, height - 1),
    ]
    for seed in seeds:
        if image.getpixel(seed) != SENTINEL:
            ImageDraw.floodfill(image, seed, SENTINEL, thresh=FLOOD_THRESHOLD)

    pixels = image.load()
    alpha = bytearray(width * height)
    for y in range(height):
        row = y * width
        for x in range(width):
            alpha[row + x] = 0 if pixels[x, y] == SENTINEL else 255

    # A resized JPEG leaves a one-pixel magenta fringe around the silhouette.
    # Strip background-like pixels that touch transparency so the small sprite
    # does not keep pink speckles.
    for _ in range(2):
        current = list(alpha)
        remove: list[int] = []
        for y in range(height):
            row = y * width
            for x in range(width):
                index = row + x
                if not current[index] or not is_magenta(pixels[x, y]):
                    continue
                if ((x > 0 and not current[index - 1]) or
                        (x + 1 < width and not current[index + 1]) or
                        (y > 0 and not current[index - width]) or
                        (y + 1 < height and not current[index + width])):
                    remove.append(index)
        for index in remove:
            alpha[index] = 0

    result = Image.new("RGBA", image.size, (0, 0, 0, 0))
    result.putalpha(Image.frombytes("L", image.size, bytes(alpha)))
    result.paste(image, (0, 0), result.getchannel("A"))
    return result


def compress_raw(path: Path) -> int:
    before = path.stat().st_size
    image = Image.open(path).convert("RGB")
    if max(image.size) <= RAW_MAX and before <= RAW_BYTES:
        return before
    image.thumbnail((RAW_MAX, RAW_MAX), Image.LANCZOS)
    image.save(path, "JPEG", quality=JPEG_QUALITY, optimize=True)
    return before


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", nargs="+", default=None,
                        help="only process these hero prefixes, e.g. dark royal")
    parser.add_argument("--force", action="store_true",
                        help="regenerate PNGs even when they already exist")
    args = parser.parse_args()

    allowed = set(args.prefix) if args.prefix else None
    processed = 0
    saved_bytes = 0
    for raw_path in sorted(RAW.glob("ultra-*.jpg")):
        match = SOURCE_RE.match(raw_path.name)
        if not match:
            continue
        hero = match.group("hero")
        if allowed is not None and hero not in allowed:
            continue
        index = raw_path.name.split("-")[2]
        png_path = SRC / f"ultra-{hero}-{index}-{match.group('pose')}.png"

        prepared = False
        if not png_path.exists() or args.force:
            image = Image.open(raw_path).convert("RGB")
            image = image.resize((PNG_SIZE, PNG_SIZE), Image.LANCZOS)
            image = remove_background(image)
            image.save(png_path, optimize=True)
            prepared = True

        before = compress_raw(raw_path)
        if before != raw_path.stat().st_size:
            saved_bytes += before - raw_path.stat().st_size
        if prepared:
            print(f"prepared {png_path.name}")
        if before != raw_path.stat().st_size:
            print(f"compressed {raw_path.name} ({before} -> {raw_path.stat().st_size} bytes)")
        if prepared or before != raw_path.stat().st_size:
            processed += 1

    print(f"Processed {processed} hero poses, saved {saved_bytes} bytes in raw JPEGs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
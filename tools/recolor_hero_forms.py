#!/usr/bin/env python3
"""Recolour two hero pose sets so the art matches the character names.

* ``ultra-emerald-*``: green armour -> gold, for Taiga Photon Earth.
* ``ultra-gold-*``:    red core/energy -> cyan, for Z Delta Rise Claw.

The 512 x 512 PNGs are hand-editable sources, so this script edits them in
place. It is idempotent: the rewritten hue range no longer contains the
source hue. Run from the repository root:

    python tools/recolor_hero_forms.py
"""
from __future__ import annotations

import colorsys
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: python -m pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "pixel-heroes"

SOURCES = [
    ("emerald", "green armour -> gold", lambda h: 70 <= h <= 175, 45.0),
    ("gold", "red core -> cyan", lambda h: h <= 20 or h >= 340, 190.0),
]
POSES = [
    (1, "idle"), (2, "fight"), (3, "fly"), (4, "beam"),
    (5, "charge"), (6, "kick"), (7, "guard"), (8, "victory"),
]
SATURATION_FLOOR = 0.18
VALUE_FLOOR = 0.05


def recolor(path: Path, target_hue: float, in_range) -> int:
    image = Image.open(path).convert("RGBA")
    pixels = image.load()
    changed = 0
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = pixels[x, y]
            if a <= 8:
                continue
            hue, saturation, value = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if saturation < SATURATION_FLOOR or value < VALUE_FLOOR:
                continue
            if not in_range(hue * 360.0):
                continue
            nr, ng, nb = colorsys.hsv_to_rgb(target_hue / 360.0, saturation, value)
            pixels[x, y] = (round(nr * 255), round(ng * 255), round(nb * 255), a)
            changed += 1
    image.save(path, optimize=True)
    return changed


def main() -> int:
    for prefix, description, in_range, target_hue in SOURCES:
        total = 0
        for index, pose in POSES:
            path = SRC / f"ultra-{prefix}-{index:02d}-{pose}.png"
            if not path.is_file():
                sys.exit(f"missing sprite: {path}")
            total += recolor(path, target_hue, in_range)
        print(f"{prefix}: {description}, {total} pixels changed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
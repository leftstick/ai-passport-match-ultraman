#!/usr/bin/env python3
"""Sanity-check generated sprite and sound-effect tables."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPRITES_C = ROOT / "main" / "assets" / "sprites.c"
SPRITES_H = ROOT / "main" / "assets" / "sprites.h"
SFX_C = ROOT / "main" / "audio" / "sfx.c"
SFX_EXTRA_C = ROOT / "main" / "audio" / "sfx_extra.c"
SFX_H = ROOT / "main" / "audio" / "sfx.h"
AUDIO_DOCS = [
    ROOT / "assets" / "audio" / "SOURCES.md",
    ROOT / "assets" / "audio" / "SOURCES.zh_CN.md",
    ROOT / "assets" / "audio" / "CC0-1.0.txt",
]

HERO_SPRITES = 5 * 8
MONSTER_SPRITES = 8 * 5
HERO_PREFIXES = ("blue_", "red_", "emerald_", "gold_", "violet_")
PCM_BUDGET_SAMPLES = 200_000


def check_sprites(errors: list[str]) -> None:
    if not SPRITES_C.is_file() or not SPRITES_H.is_file():
        errors.append("missing main/assets/sprites.c or sprites.h")
        return
    text = SPRITES_C.read_text(encoding="utf-8")
    descriptors = re.findall(
        r"static const lv_image_dsc_t spr_(\w+)_dsc = \{(.*?)\n\};",
        text, re.S,
    )
    if len(descriptors) != HERO_SPRITES + MONSTER_SPRITES:
        errors.append(
            f"sprites.c: expected {HERO_SPRITES + MONSTER_SPRITES} descriptors, "
            f"found {len(descriptors)}")
    for name, body in descriptors:
        width = re.search(r"\.w = (\d+)", body)
        height = re.search(r"\.h = (\d+)", body)
        if not width or not height:
            errors.append(f"sprites.c: {name} is missing header size")
            continue
        w, h = int(width.group(1)), int(height.group(1))
        expected = (72, 88) if name.startswith(HERO_PREFIXES) else (80, 80)
        if (w, h) != expected:
            errors.append(f"sprites.c: {name} is {w}x{h}, expected {expected[0]}x{expected[1]}")
    header = SPRITES_H.read_text(encoding="utf-8")
    for symbol in (
        "HERO_SPRITES[5][8]",
        "MONSTER_SPRITES[8][5]",
        "SPRITE_POSE_VICTORY",
        "SPRITE_MONSTER_HURT",
        "SPRITE_POSE_COUNT",
        "SPRITE_MONSTER_COUNT",
        "sprites_hero",
        "sprites_monster",
    ):
        if symbol not in header and symbol not in text:
            errors.append(f"sprites: missing {symbol}")


def check_sfx(errors: list[str]) -> None:
    if not SFX_C.is_file() or not SFX_H.is_file():
        errors.append("missing main/audio/sfx.c or sfx.h")
        return
    header = SFX_H.read_text(encoding="utf-8")
    ids = re.findall(r"^\s+(SFX_[A-Z_]+)(?:\s*=\s*\d+)?\s*,", header, re.M)
    if "SFX_COUNT" not in ids:
        errors.append("sfx.h: SFX_COUNT is missing")
        return
    ids.remove("SFX_COUNT")
    text = SFX_C.read_text(encoding="utf-8")
    extra = SFX_EXTRA_C.read_text(encoding="utf-8") if SFX_EXTRA_C.is_file() else ""
    total = 0
    for source in (text, extra):
        for match in re.finditer(r"static const int16_t clip_\w+\[\] = \{(.*?)\n\};",
                                 source, re.S):
            values = [v for v in re.findall(r"-?\d+", match.group(1))]
            total += len(values)
    if total == 0:
        errors.append("sfx.c: no PCM samples found")
    elif total > PCM_BUDGET_SAMPLES:
        errors.append(
            f"sfx: {total} samples exceed budget {PCM_BUDGET_SAMPLES}")
    for sfx_id in ids:
        if f"[{sfx_id}]" not in text and sfx_id not in extra:
            errors.append(f"sfx: clip table is missing {sfx_id}")
    for doc in AUDIO_DOCS:
        if not doc.is_file():
            errors.append(f"missing audio documentation: {doc.relative_to(ROOT)}")
    if total:
        duration = total / 16000
        print(f"Assets: {HERO_SPRITES + MONSTER_SPRITES} sprites, "
              f"{len(ids)} sound cues ({duration:.2f}s PCM)")


def main() -> int:
    errors: list[str] = []
    check_sprites(errors)
    check_sfx(errors)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("Asset tables: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Collect every non-ASCII character used by display strings.

Writes the font inventory consumed by tools/generate_app_fonts.sh. Run it after
changing UI, campaign, or question strings:

    python tools/collect_glyphs.py
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INVENTORY = ROOT / "assets" / "fonts" / "math_game_chars.txt"

SOURCE_GLOBS = [
    "main/main.c",
    "main/app/*.c",
    "main/core/*.c",
]


def string_literals(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    text = "\n".join(
        line for line in text.splitlines()
        if "ESP_LOG" not in line and not line.lstrip().startswith("#include")
    )
    literals: list[str] = []
    i = 0
    size = len(text)
    while i < size:
        ch = text[i]
        if ch == "/" and i + 1 < size and text[i + 1] == "/":
            end = text.find("\n", i + 2)
            i = size if end < 0 else end + 1
        elif ch == "/" and i + 1 < size and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = size if end < 0 else end + 2
        elif ch == '"':
            i += 1
            buf: list[str] = []
            while i < size:
                current = text[i]
                if current == "\\" and i + 1 < size:
                    buf.append(text[i + 1])
                    i += 2
                    continue
                if current == '"':
                    i += 1
                    break
                buf.append(current)
                i += 1
            literals.append("".join(buf))
        elif ch == "'":
            i += 1
            while i < size:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == "'":
                    i += 1
                    break
                i += 1
        else:
            i += 1
    return literals


def collect() -> set[str]:
    chars: set[str] = set()
    files = 0
    for pattern in SOURCE_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            if not path.is_file():
                continue
            files += 1
            for literal in string_literals(path):
                for ch in literal:
                    if ord(ch) > 0x7E and not ch.isspace():
                        chars.add(ch)
    if files == 0:
        raise SystemExit("ERROR: no source files matched")
    if not chars:
        raise SystemExit("ERROR: no non-ASCII display characters found")
    return chars


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed inventory is stale")
    args = parser.parse_args()

    chars = collect()
    text = "".join(sorted(chars)) + "\n"
    if args.check:
        current = INVENTORY.read_text(encoding="utf-8") if INVENTORY.is_file() else ""
        if current.strip() != text.strip():
            print("ERROR: assets/fonts/math_game_chars.txt is stale; run "
                  "python tools/collect_glyphs.py", file=sys.stderr)
            return 1
        print(f"Glyph inventory: PASS ({len(chars)} characters)")
        return 0

    INVENTORY.parent.mkdir(parents=True, exist_ok=True)
    INVENTORY.write_text(text, encoding="utf-8")
    print(f"Wrote {INVENTORY} ({len(chars)} characters)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

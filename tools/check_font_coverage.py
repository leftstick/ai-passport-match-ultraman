#!/usr/bin/env python3
"""Check that UI strings are covered by the generated LVGL font subsets."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INVENTORY = ROOT / "assets" / "fonts" / "math_game_chars.txt"
FONT_DIR = ROOT / "main" / "fonts"
ASCII = {chr(code) for code in range(0x20, 0x7F)}
GLYPH_RE = re.compile(r"/\* U\+([0-9A-Fa-f]{4,6}) ")


def read_inventory() -> set[str]:
    text = INVENTORY.read_text(encoding="utf-8")
    chars = {ch for ch in text if not ch.isspace()}
    if not chars:
        raise SystemExit(f"ERROR: {INVENTORY} is empty")
    return chars


def font_codepoints(path: Path) -> set[int]:
    if not path.is_file():
        raise SystemExit(f"ERROR: missing generated font {path}")
    text = path.read_text(encoding="utf-8")
    codes = {int(match, 16) for match in GLYPH_RE.findall(text)}
    if not codes:
        raise SystemExit(f"ERROR: no glyph entries found in {path}")
    return codes


def string_literals(path: Path, drop_logs: bool = False) -> list[str]:
    """Return the contents of C string literals, skipping comments."""
    text = path.read_text(encoding="utf-8")
    if drop_logs:
        # UART log text never reaches the display and does not need glyphs.
        text = "\n".join(
            line for line in text.splitlines() if "ESP_LOG" not in line
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


def check_font(name: str, expected: set[str], errors: list[str]) -> None:
    path = FONT_DIR / f"{name}.c"
    codes = font_codepoints(path)
    missing = sorted(ch for ch in expected if ord(ch) not in codes)
    if missing:
        errors.append(
            f"{path.name}: missing {len(missing)} glyph(s): "
            + " ".join(f"U+{ord(ch):04X}({ch})" for ch in missing)
        )
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    if f"fonts/{name}.c" not in cmake:
        errors.append(f"{path.name}: not listed in main/CMakeLists.txt")


def main() -> int:
    inventory = read_inventory()
    errors: list[str] = []

    fonts = {"app_font_12", "app_font_14", "app_font_20", "app_font_28"}
    for name in sorted(fonts):
        check_font(name, inventory | ASCII, errors)

    # Only compiled application sources paint text. Log text and the
    # uncompiled demo_*.c files are intentionally not checked.
    sources = sorted((ROOT / "main" / "app").glob("*.c")) + \
        sorted((ROOT / "main" / "core").glob("*.c"))
    used: dict[str, set[str]] = {}
    for path in sources:
        for literal in string_literals(path, drop_logs=True):
            for ch in literal:
                if ord(ch) > 0x7E:
                    used.setdefault(ch, set()).add(path.name)

    unknown = sorted(ch for ch in used if ch not in inventory)
    if unknown:
        details = ", ".join(
            f"U+{ord(ch):04X}({ch}) in {', '.join(sorted(used[ch]))}"
            for ch in unknown
        )
        errors.append(
            "UI string characters missing from "
            f"assets/fonts/math_game_chars.txt: {details}. "
            "Add them to the inventory and rerun tools/generate_app_fonts.sh."
        )

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(
        "Font coverage: PASS "
        f"({len(inventory)} non-ASCII characters, 4 generated fonts)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

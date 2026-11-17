#!/usr/bin/env bash
# Regenerate the LVGL font subsets used by the math game.
#
# The Source Han Sans OTF is not committed (it is ~16 MB). Download
# SourceHanSansSC-Regular.otf from the Adobe Fonts release, record its SHA-256
# in assets/fonts/README.md, then run:
#
#   SOURCE_HAN_SANS=/path/to/SourceHanSansSC-Regular.otf tools/generate_app_fonts.sh
#
# Set LV_FONT_CONV to a local lv_font_conv executable if npx is unavailable.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

font="${SOURCE_HAN_SANS:-}"
if [[ -z "${font}" || ! -f "${font}" ]]; then
    echo "ERROR: set SOURCE_HAN_SANS to an existing SourceHanSansSC-Regular.otf." >&2
    exit 1
fi

if [[ -n "${LV_FONT_CONV:-}" ]]; then
    run_conv() { "${LV_FONT_CONV}" "$@"; }
else
    run_conv() { npx -y lv_font_conv@1.5.3 "$@"; }
fi

chars="$(tr -d '\r\n' < assets/fonts/math_game_chars.txt)"
if [[ -z "${chars}" ]]; then
    echo "ERROR: assets/fonts/math_game_chars.txt is empty." >&2
    exit 1
fi

mkdir -p main/fonts
common=(
    --font "${font}"
    --bpp 4
    --no-compress
    --format lvgl
    --lv-include lvgl.h
)

for size in 12 14 20 28; do
    run_conv "${common[@]}" --range 0x20-0x7e --symbols "${chars}" \
        --size "${size}" --lv-font-name "app_font_${size}" \
        --output "main/fonts/app_font_${size}.c"
done

echo "Generated main/fonts/app_font_12.c, app_font_14.c, app_font_20.c, app_font_28.c"

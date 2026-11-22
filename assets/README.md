<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Math game subsets

The math game ships four generated LVGL fonts in `main/fonts/`. They are built
from Source Han Sans SC Regular (Adobe, SIL Open Font License 1.1; the license
text is in [`fonts/SourceHanSansSC-LICENSE.txt`](fonts/SourceHanSansSC-LICENSE.txt)).
The 16 MB source OTF is not committed.

| File | Size | Coverage |
| --- | --- | --- |
| `main/fonts/app_font_12.c` | 12 px, 4 bpp | Printable ASCII + the UI character inventory |
| `main/fonts/app_font_14.c` | 14 px, 4 bpp | Printable ASCII + the UI character inventory |
| `main/fonts/app_font_20.c` | 20 px, 4 bpp | Printable ASCII + the UI character inventory |
| `main/fonts/app_font_28.c` | 28 px, 4 bpp | Printable ASCII + the UI character inventory |

- Inventory: [`fonts/math_game_chars.txt`](fonts/math_game_chars.txt), generated
  by `python tools/collect_glyphs.py` from every display string in
  `main/app/`, `main/core/`, and `main/main.c`. Rerun it before regenerating
  fonts; the static gate fails when the file is stale.
- Converter: `lv_font_conv` 1.5.3, run through `npx`, `--bpp 4 --no-compress`.
- Regenerate with
  `SOURCE_HAN_SANS=/path/to/SourceHanSansSC-Regular.otf tools/generate_app_fonts.sh`.
- Source font SHA-256:
  `f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256`.
- `tools/check_font_coverage.py` runs in the static gate and fails when a UI
  string uses a character that is absent from the inventory or the generated
  fonts.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

### Math game sprites

[`pixel-heroes/`](pixel-heroes/) contains the user-supplied 512 × 512 hero and
monster artwork (plus `raw/` originals). It is the editable source and is not
compiled directly. `tools/build_sprites.py` trims each alpha bounding box,
nearest-neighbour resizes it to 72 × 88 (heroes) or 80 × 80 (monsters), and
writes RGB565A8 arrays to `main/assets/sprites.c`. The current roster has 18
heroes and eleven monsters, grouped in the `heroes/` and `monsters/`
subdirectories. Each hero has eight action poses (idle, fight, fly, beam,
charge, kick, guard, victory) and each monster five (idle, attack, roar,
charge, hurt). The battle screen switches to the pose that matches the current
phase or feedback event, so every state has its own reaction instead of a
single static image. The 199 descriptors total 3,793,152 bytes of payload.
`assets/action_config.json` maps the ten gameplay actions (idle, ready,
answerCorrect, answerWrong, combo, ultimate, victory, hurt, taunt, easterEgg)
onto those poses and is compiled to `main/core/action_config.c` by
`tools/build_action_config.py`. Missing actions fall back to idle.
Those generated tables are what the firmware links;
do not edit them by hand. `tools/prepare_hero_sources.py` rebuilds the
512 × 512 transparent PNGs from the magenta-background originals and compresses
oversized JPEGs under `raw/`.

### Device photos

[`photos/`](photos/) holds cropped real-device photos used by the project README.
They are documentation-only and are not compiled into the firmware.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`photos/device-home.jpg`](photos/device-home.jpg) | 760 × 1368, JPEG | Home screen with the hero and mode selection. User-supplied AI Passport photo, cropped to the device and resized. |
| [`photos/device-battle.jpg`](photos/device-battle.jpg) | 760 × 1394, JPEG | Stage 6 question screen showing King Joe. User-supplied AI Passport photo, rotated upright, cropped to the device, and resized. |

## Music and sound effects

Store reusable music and sound-effect sources in `music/`. Application-specific
cue tables may live in `audio/` when a paired `SOURCES.md` / `SOURCES.zh_CN.md`
records the source and license.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

The math game uses the CC0 Kenney clips documented in
[`audio/SOURCES.md`](audio/SOURCES.md). `tools/build_sfx.py` trims,
peak-normalises, fades, and converts them into `main/audio/sfx.c`.

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).

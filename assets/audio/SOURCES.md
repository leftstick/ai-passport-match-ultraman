<p align="right">
  <a href="SOURCES.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Battle sound effects

The game ships 16 kHz mono signed 16-bit PCM clips produced by two scripts.
Every source is released under CC0 1.0; attribution is not required, but this
file keeps the provenance auditable.

## Kenney packs

`tools/build_sfx.py` trims the regular cues from four Kenney CC0 packs.

| Game cue | Kenney pack | Source clip |
| --- | --- | --- |
| `SFX_MOVE` | Interface Sounds | `click_001.ogg` |
| `SFX_CONFIRM` | Interface Sounds | `confirmation_002.ogg` |
| `SFX_CORRECT` | Music Jingles | `jingles_PIZZI00.ogg` |
| `SFX_WRONG` | Interface Sounds | `error_004.ogg` |
| `SFX_HINT` | Interface Sounds | `question_003.ogg` |
| `SFX_HIT` | Impact Sounds | `impactPunch_medium_002.ogg` |
| `SFX_CRITICAL` | Sci-Fi Sounds | `laserSmall_000.ogg` |
| `SFX_HURT` | Impact Sounds | `impactSoft_heavy_001.ogg` |
| `SFX_CHARGE` | Sci-Fi Sounds | `forceField_000.ogg` |
| `SFX_BEAM` | Sci-Fi Sounds | `laserLarge_000.ogg` |
| `SFX_MONSTER_DOWN` | Sci-Fi Sounds | `explosionCrunch_000.ogg` |
| `SFX_VICTORY` | Music Jingles | `jingles_PIZZI10.ogg` |
| `SFX_DEFEAT` | Music Jingles | `jingles_NES05.ogg` |
| `SFX_PAUSE` | Interface Sounds | `back_002.ogg` |

Packs: <https://kenney.nl/assets/interface-sounds>,
<https://kenney.nl/assets/impact-sounds>,
<https://kenney.nl/assets/sci-fi-sounds>,
<https://kenney.nl/assets/music-jingles>.

## Procedural combat-timing cues

`tools/build_sfx_extra.py` synthesises two cues without external samples.

| Game cue | Origin |
| --- | --- |
| `SFX_SWING` | Procedural, `tools/build_sfx_extra.py` |
| `SFX_GROWL` | Procedural, `tools/build_sfx_extra.py` |

## Monster defeat cries

The four defeat cries use the creature-sound-design approach popularised by
films such as *Jurassic Park*: real large-animal CC0 recordings are layered as
the body, then pitched down, formant-filtered, ring-modulated, driven and sent
through a short room to become a monster. The processing script is
`tools/build_monster_cries.py`.

The exact segments used are committed as trimmed 16 kHz mono WAV files under
`assets/audio/monster-sources/`, so the build works offline and stays
reproducible.

| Game cue | Source recordings (Freesound IDs) |
| --- | --- |
| `SFX_CRY_HEAVY` | 559600 lion roar, 507467 angry elephant, 139052 elephant scream, 536761 growl |
| `SFX_CRY_WINGED` | 571288 tiger roar, 811790 female tiger roar, 139052 elephant scream, 559270 hiss roar |
| `SFX_CRY_MECH` | 350424 rhinoceros trumpet, 535810 Indian rhinoceros, 536761 growl |
| `SFX_CRY_ALIEN` | 139052 elephant scream, 397054 gorilla scream, 631985 snarling alien, 559270 hiss roar |

| Freesound ID | Title | Uploader | License |
| --- | --- | --- | --- |
| 559600 | LION ROAR | Podcapocalipsis | CC0 1.0 |
| 507467 | Angry Elephant | Danjocross | CC0 1.0 |
| 139052 | Elephant scream | Jasher70 | CC0 1.0 |
| 571288 | tiger_roar | Vaporpup | CC0 1.0 |
| 811790 | Tiger roar female | eZZin | CC0 1.0 |
| 350424 | Rhinoceros Trumpeting Angry | bevibeldesign | CC0 1.0 |
| 535810 | Indian rhinoceros | Breviceps | CC0 1.0 |
| 397054 | Gorillaschrei | J0ck0 | CC0 1.0 |
| 536761 | Growl | egomassive | CC0 1.0 |
| 631985 | Snarling Alien | el_boss | CC0 1.0 |
| 559270 | Hiss Roar | KristopherTiberiusHaven | CC0 1.0 |

Each source page is `https://freesound.org/s/<ID>/`.

License: <https://creativecommons.org/publicdomain/zero/1.0/>. The full notice
is in `CC0-1.0.txt`.

## Regenerating

```bash
python tools/build_sfx.py --source-dir /path/to/extracted/kenney
python tools/build_sfx_extra.py
python tools/build_monster_cries.py
python tools/build_monster_cries.py --preview-dir .tmp/monster-cries  # audition WAVs
```

`build_sfx.py` trims each clip and peak-normalises it to -3 dBFS.
`build_monster_cries.py` loudness-matches the four cries by RMS, soft-limits
their transient peaks, and writes `main/audio/monster_cries.c`. Every PCM clip
gets a short fade in/out so playback never clicks.
<p align="right">
  <strong>简体中文</strong> · <a href="SOURCES.md">English</a>
</p>

# 战斗音效

游戏内嵌的 16 kHz 单声道 16 位 PCM 音效由两套脚本生成，全部素材均为
CC0 1.0，不要求署名；本文件用于保留可追溯的来源记录。

## Kenney 音效包

`tools/build_sfx.py` 从 Kenney 的四个 CC0 音效包中裁剪并生成常规音效。

| 游戏音效 | Kenney 音效包 | 源文件 |
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

音效包：<https://kenney.nl/assets/interface-sounds>、
<https://kenney.nl/assets/impact-sounds>、
<https://kenney.nl/assets/sci-fi-sounds>、
<https://kenney.nl/assets/music-jingles>。

## 程序合成的战斗节奏音

`tools/build_sfx_extra.py` 直接合成两个不依赖外部素材的战斗节奏音。

| 游戏音效 | 来源 |
| --- | --- |
| `SFX_SWING` | 程序合成，`tools/build_sfx_extra.py` |
| `SFX_GROWL` | 程序合成，`tools/build_sfx_extra.py` |

## 怪兽被击败惨叫

四种怪兽惨叫采用电影中常见的“真实大型动物叠层”做法：先用狮子、老虎、
大象、犀牛等 CC0 录音做主体，再通过变速降调、共振峰过滤、环形调制、
饱和与短混响塑造成怪兽。处理脚本是 `tools/build_monster_cries.py`。

脚本使用的片段已按固定起点裁剪为 16 kHz 单声道 WAV，存放在
`assets/audio/monster-sources/`，因此离线也能完整重建。

| 游戏音效 | 使用的源素材（Freesound ID） |
| --- | --- |
| `SFX_CRY_HEAVY` | 559600 狮子咆哮、507467 愤怒大象、139052 大象尖叫、536761 低吼 |
| `SFX_CRY_WINGED` | 571288 老虎咆哮、811790 雌虎咆哮、139052 大象尖叫、559270 嘶吼 |
| `SFX_CRY_MECH` | 350424 犀牛号角、535810 印度犀牛、536761 低吼 |
| `SFX_CRY_ALIEN` | 139052 大象尖叫、397054 大猩猩尖叫、631985 外星嘶鸣、559270 嘶吼 |

| Freesound ID | 标题 | 上传者 | 许可 |
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

每条素材的页面地址为 `https://freesound.org/s/<ID>/`。

许可协议：<https://creativecommons.org/publicdomain/zero/1.0/>。完整声明见
`CC0-1.0.txt`。

## 重新生成

```bash
python tools/build_sfx.py --source-dir /path/to/extracted/kenney
python tools/build_sfx_extra.py
python tools/build_monster_cries.py
python tools/build_monster_cries.py --preview-dir .tmp/monster-cries  # 试听 WAV
```

`build_sfx.py` 生成的片段会按上限裁剪并峰值归一化到 -3 dBFS；
`build_monster_cries.py` 会先把四种惨叫匹配到同一 RMS 响度，再用软限幅
控制瞬态峰值，最后写入 `main/audio/monster_cries.c`。所有 PCM 都会加入
短淡入淡出，避免播放时出现爆音。
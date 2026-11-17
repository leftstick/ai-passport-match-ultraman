<p align="right">
  <strong>简体中文</strong> · <a href="SOURCES.md">English</a>
</p>

# 战斗音效

游戏内嵌的 16 kHz 单声道 16 位 PCM 音效由 `tools/build_sfx.py` 生成。全部
源文件来自 Kenney 的 CC0 1.0 音效包，不要求署名；本文件用于保留可追溯的
来源记录。

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
| `SFX_SWING` | 程序合成 | `tools/build_sfx_extra.py` |
| `SFX_GROWL` | 程序合成 | `tools/build_sfx_extra.py` |

音效包：<https://kenney.nl/assets/interface-sounds>、
<https://kenney.nl/assets/impact-sounds>、
<https://kenney.nl/assets/sci-fi-sounds>、
<https://kenney.nl/assets/music-jingles>。

许可协议：<https://creativecommons.org/publicdomain/zero/1.0/>。完整声明见
`CC0-1.0.txt`。

重新生成内嵌音效表：

```bash
python tools/build_sfx.py --source-dir /path/to/extracted/kenney
```

两个战斗节奏音效不使用外部素材,由脚本直接合成:

```bash
python tools/build_sfx_extra.py
```

每个片段都会按上限裁剪、峰值归一化到 -3 dBFS，并加入 5 ms 淡入淡出，
避免播放时出现爆音。

<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐、电池参数等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 算术消消乐字库子集

应用在 `main/fonts/` 中内置四个 LVGL 字库，均由思源黑体（Adobe，SIL Open
Font License 1.1；许可全文见
[`fonts/SourceHanSansSC-LICENSE.txt`](fonts/SourceHanSansSC-LICENSE.txt)）
SourceHanSansSC-Regular 生成。16 MB 的源 OTF 不提交。

| 文件 | 规格 | 覆盖范围 |
| --- | --- | --- |
| `main/fonts/app_font_12.c` | 12 px，4 bpp | 可打印 ASCII + 界面汉字清单 |
| `main/fonts/app_font_14.c` | 14 px，4 bpp | 可打印 ASCII + 界面汉字清单 |
| `main/fonts/app_font_20.c` | 20 px，4 bpp | 可打印 ASCII + 界面汉字清单 |
| `main/fonts/app_font_28.c` | 28 px，4 bpp | 可打印 ASCII + 界面汉字清单 |

- 汉字清单：[`fonts/math_game_chars.txt`](fonts/math_game_chars.txt)，由
  `python tools/collect_glyphs.py` 从 `main/app/`、`main/core/` 与
  `main/main.c` 的全部显示文案自动收集。重新生成字库前先运行该脚本；清单过期
  时静态校验会直接失败。
- 转换工具：`lv_font_conv` 1.5.3，通过 `npx` 调用，参数
  `--bpp 4 --no-compress`。
- 重新生成：
  `SOURCE_HAN_SANS=/path/to/SourceHanSansSC-Regular.otf tools/generate_app_fonts.sh`。
- 源字体 SHA-256：
  `f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256`。
- 静态校验会运行 `tools/check_font_coverage.py`：界面文案出现清单或生成字库
  未覆盖的字符时直接失败。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

### 算术超能战精灵

[`pixel-heroes/`](pixel-heroes/) 保存用户提供的 512 × 512 英雄与怪兽原画
（以及 `raw/` 原始图）。它是可编辑源素材，不直接参与编译。
`tools/build_sprites.py` 会裁剪 alpha 包围盒、按最近邻缩放到 72 × 88（英雄）
或 80 × 80（怪兽），并生成 RGB565A8 数组写入 `main/assets/sprites.c`。当前
阵容包含 16 位英雄与 9 只怪兽，分别放在 `heroes/` 与 `monsters/` 子目录。
每位英雄有 8 种动作（idle、fight、fly、beam、charge、kick、guard、victory），
每只怪兽有 5 种动作（idle、attack、roar、charge、hurt）；战斗界面会根据当前
阶段或反馈事件切换对应姿势，而不是始终显示同一张图。173 个描述符共
3,297,024 字节负载。固件链接的是这些生成表，不要手工修改。
`tools/prepare_hero_sources.py` 负责从洋红背景原图重建 512 × 512 透明 PNG，
并压缩 `raw/` 下体积过大的 JPEG。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。应用专用的音效表可放在 `audio/`，但必须
同时提供 `SOURCES.md` / `SOURCES.zh_CN.md` 记录来源与许可。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

算术超能战使用 CC0 的 Kenney 音效，清单见
[`audio/SOURCES.zh_CN.md`](audio/SOURCES.zh_CN.md)。`tools/build_sfx.py` 负责
裁剪、峰值归一化、淡入淡出并转换为 `main/audio/sfx.c`。

## 电池参数（battery）

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
保存万锂新能源 500 mAh 电芯的 CW2017 参数（profile2、XIL40X、2026-10-20）。
该文件是项目提供的 `喜梨_智能工卡_万锂新能源500mAh_profile2_XIL40X_20261020.txt`
的原样副本，按维护者要求保留用于本次固件更新；原文件没有附带单独的许可说明。

格式为 80 行两位十六进制数字，每行一个字节，按寄存器顺序排列。
BSP 在 `components/bsp/src/bsp_battery.c` 中嵌入这 80 字节，主机检查会验证
源文件及数组内容完全一致。文本文件 SHA-256：
`7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`。
解码后 80 字节 SHA-256：
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`。
这是电量计参数，不是充电配置，也不能通用于所有 500 mAh 电芯；详见
[硬件指南](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md#9-cw2017-电池计)。

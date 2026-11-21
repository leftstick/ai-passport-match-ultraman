# 算术超能战

面向小学二年级以上孩子的算术对战闯关游戏，运行在 **FoloToy AI Passport**（ESP32-C3）上。把口算题变成攻击手段：答对出招、抓准暴击、蓄满必杀，最后和 Boss 对拼光线。

- 目标硬件：ESP32-C3 · 8 MB Flash · 无 PSRAM · 240 × 320 ST7789P3 · 3 个按键
- 开发框架：ESP-IDF 5.5.3 + LVGL 9.5（复用仓库 BSP，界面全部重新设计）
- 界面语言：简体中文（内置 12 / 14 / 20 / 28 px 子集字体）
- 存档：NVS `battle_v1`，保存解锁进度、星级、最高分、英雄、难度和静音状态

<p align="center">
  <img src="assets/pixel-heroes/preview-heroes.png" width="62%" alt="七位英雄的八种动作" />
  <img src="assets/pixel-heroes/preview-monsters.png" width="30%" alt="八只怪兽的五种动作" />
</p>

## 游戏内容

### 八关战役

| 关卡 | 怪兽 | 数学范围 | 题目时间 | 怪兽血量 |
| --- | --- | --- | --- | --- |
| 街区警报 | 巴尔坦星人 | 20 以内加减 · 2~5 乘法 | 8.0 s | 4 |
| 重装对决 | 雷德王 | 20 以内加减 · 2~5 乘法 | 7.8 s | 4 |
| 深海来袭 | 雷伊洛斯 | 100 以内加减 · 6~9 乘法 | 7.0 s | 5 |
| 夜翼危机 | 多拉考 | 100 以内加减 · 6~9 乘法 | 6.8 s | 5 |
| 森林巨兽 | 哥莫拉 | 退位减 · 乘除 · 填空 | 6.2 s | 6 |
| 钢刃追猎 | 加拉特隆 | 退位减 · 乘除 · 填空 | 6.0 s | 6 |
| 赤甲虫王 | 杰顿 | 混合两步 · 100 以内加减 | 5.6 s | 7 |
| 终焉飞龙 | 魔格大蛇 | 混合两步 · 乘除 · 填空 | 5.2 s | 8（Boss） |

### 七位英雄

| 英雄 | 被动 |
| --- | --- |
| 赛罗奥特曼 | 必杀槽获取 +25% |
| 泰罗奥特曼 | 暴击伤害 +1 |
| 高斯奥特曼 | 每题答题时间 +0.8 秒 |
| 奥特之王 | 暴击判定区 +30% |
| 迪迦奥特曼 | 每关首次受伤免疫 |
| 欧布奥特曼·暗耀 | 暴击伤害 +1 |
| 捷德奥特曼·尊皇 | 每关首次受伤免疫 |

### 三档难度

| 难度 | 节奏 | 生命 | 怪兽强度 | 暴击区 |
| --- | --- | --- | --- | --- |
| 星光 | 时间 +31.5 秒 · 数学 -1 档 | 5 颗心 | -1 血 | 50 |
| 烈焰 | 标准节奏 | 4 颗心 | 标准 | 40 |
| 究极 | 时间 -1 秒 | 3 颗心 | +1 血 | 32 |

星光档面向刚上手的孩子：每题最多有 36~40 秒作答，数学题目整体降一档（例如第 3~4 关仍是 20 以内加减 · 2~5 乘法，第 5~6 关才进入 100 以内加减）。表格中的「题目时间」是烈焰档的基础值，星光档在此基础上加 31.5 秒。

### 战斗机制

- **作答**：题目出现后必须先按 ↑ 或 ↓ 选中选项，再按 OK 提交；不选直接按 OK 不会作答。
- **暴击**：答对后出现判定条，看准金色区域按 OK，命中可打出更高伤害。
- **连击与必杀**：连续答对积累连击和必杀槽；槽满后长按 ↑ 释放必杀。
- **动作演出**：普通命中按飞踢、飞拳、蓄力重击、跃身踢的顺序轮换，跨关卡持续；完美暴击只提升伤害与反馈，不会固定成同一招；连击追击和终结技各有独立动作；怪兽攻击也在扑击与蓄力扑击之间交替。
- **Boss 光线对拼**：Boss 战进入光线相持，连续按 OK 把光束推回去。
- **容错与辅助**：答错或超时会掉心；连续失误会自动降难度、延长时间，并给出提示与讲解。
- **结算**：根据剩余生命、用时和连击给 1~3 星，并保存最高分与解锁进度。

## 按键

| 页面 | ↑ / ↓ | OK | 长按 |
| --- | --- | --- | --- |
| 首页 | 切换英雄 | 进入关卡 | 长按 OK 切换难度 |
| 关卡 | 切换关卡 | 出击 | 长按 OK 返回 |
| 战斗 | 选择答案 | 提交 / 暴击 / 光线连按 | 长按 ↑ 必杀；长按 ↓ 提示；长按 OK 暂停 |
| 暂停 / 结算 | 选择选项 | 确认 | 长按 OK 继续或选择默认项 |

## 构建

需要 ESP-IDF 5.5.3：

```bash
source <path-to-esp-idf-v5.5.3>/export.sh
idf.py --version          # 必须报告 v5.5.3

./tools/validate.sh --static     # 仓库检查 + 主机测试
./tools/validate.sh --firmware   # 固件构建 + 合并镜像校验
./tools/validate.sh              # 完整门禁
```

`--firmware` 会生成并校验：

```text
build/FoloToy-AI-Passport-full.bin
```

每次构建都会覆盖这个文件，不再创建新的归档目录。需要同时保留 ELF／MAP 等
崩溃调试产物时，构建前设置 `PASSPORT_FIRMWARE_ARCHIVE=1`。

当前构建结果：

```text
Build:        PASS   app 3,249,632 字节，分区剩余 61%
Host tests:   PASS   math_question / campaign / battle + 仓库主机测试
Device tests: NOT RUN
```

## 刷机

空白设备或整片刷新，把合并镜像写到 **0x0**：

```bash
python -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build/FoloToy-AI-Passport-full.bin
```

也可以使用 `idf.py -p <PORT> flash` 做分段烧录。注意：合并镜像整片写入可能清空 NVS 存档；需要保留进度时使用分段烧录。

## 目录结构

```text
main/
  app/       战斗界面、动画、页面与按键状态机
  core/      纯 C 的题目生成、战役配置、战斗状态机（可主机测试）
  audio/     双声部混音任务与生成的 PCM 音效表
  assets/    生成的 RGB565A8 精灵表
  fonts/     中文字体子集
assets/
  pixel-heroes/  可编辑的英雄/怪兽动作原画
  audio/         音效来源与 CC0 许可记录
  fonts/         字体许可与字形清单
tests/       主机侧单元测试
tools/       精灵、音效、字体生成与资源校验脚本
components/  BSP（显示、按键、音频、电池等）
```

## 素材再生成

- **精灵**：`python tools/build_sprites.py`
  - 读取 `assets/pixel-heroes/`，裁透明边后最近邻缩放到英雄 72 × 88、怪兽 80 × 80
  - 生成 RGB565A8 表到 `main/assets/sprites.c`
  - 共 96 个描述符，负载 1,832,448 字节
  - 每位英雄 8 个动作：idle / fight / fly / beam / charge / kick / guard / victory
  - 每只怪兽 5 个动作：idle / attack / roar / charge / hurt
- **音效**：`python tools/build_sfx.py --source-dir <kenney-pack-root>`
  - 20 个音效，16 kHz 单声道 PCM，共 12.09 秒
  - 来源与许可见 [`assets/audio/SOURCES.md`](assets/audio/SOURCES.md)
- **怪兽惨叫**：`python tools/build_monster_cries.py`
  - 用狮子、老虎、大象、犀牛等 CC0 录音分轨降调、调制、叠加合成
  - 试听：`python tools/build_monster_cries.py --preview-dir .tmp/monster-cries`
- **字体**：`SOURCE_HAN_SANS=<font.otf> tools/generate_app_fonts.sh`
  - 从 UI 字符串自动采集字形，生成 12 / 14 / 20 / 28 px 4bpp 子集
  - 字体为思源黑体（Source Han Sans SC），SIL Open Font License 1.1

## 测试与未验证项

主机测试覆盖题目生成、战役数值和战斗状态机，包括“未选答案时连按 OK 无效”的回归测试。

尚未在真机上验证：

- 240 × 320 屏幕上的动作辨识度、动画节奏和中文清晰度
- 三个 ADC 按键的长按 / 连按手感，尤其是 Boss 光线对拼
- ES8311 实际音量、混音效果与是否爆音
- 低电量场景下的表现

## 许可与致谢

- 代码：MIT License，基于 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) 的 BSP 与工程结构。
- 英雄与怪兽原画：本仓库作者提供的原创像素素材；角色名称引用特摄作品设定，未使用官方剧照或音频。
- 音效：Kenney 素材包与 Freesound 动物录音，CC0 1.0，明细见 [`assets/audio/SOURCES.md`](assets/audio/SOURCES.md)。
- 字体：思源黑体（Source Han Sans SC），SIL Open Font License 1.1，见 [`assets/fonts/SourceHanSansSC-LICENSE.txt`](assets/fonts/SourceHanSansSC-LICENSE.txt)。

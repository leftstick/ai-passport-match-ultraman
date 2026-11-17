// main/app/battle_app.c —— 算术超能战:英雄/关卡选择、对战界面、暂停与结算。
//
// 线程模型:
//   * 按键回调在 esp_timer 任务里只入队;输入任务调用 battle_app_handle_key()。
//   * app 任务每 50ms 推进战斗计时、动画与电量刷新。
//   * 两个任务都只在 bsp_lvgl_lock() 内访问 battle 状态和 LVGL 对象。
//   * NVS 写入由 app 任务在锁外完成,避免在 LVGL 锁内做慢操作。
#include "battle_app.h"

#include "audio_player.h"
#include "battle.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "campaign.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "sprites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(app_font_12);
LV_FONT_DECLARE(app_font_14);
LV_FONT_DECLARE(app_font_20);
LV_FONT_DECLARE(app_font_28);

static const char *TAG = "battle_app";

// ---------------------------------------------------------------------------
// 配色与尺寸
// ---------------------------------------------------------------------------
#define COL_BG        0x060B18
#define COL_PANEL     0x111B33
#define COL_TILE      0x18264A
#define COL_TILE_SEL  0x223866
#define COL_EDGE      0x2C4477
#define COL_ACCENT    0x36E0FF
#define COL_GOLD      0xFFC53D
#define COL_TEXT      0xEAF4FF
#define COL_DIM       0x7E93B8
#define COL_DANGER    0xFF5A6E
#define COL_OK        0x45E39B
#define COL_INK       0x04101F

#define SCREEN_W      240
#define SCREEN_H      320

#define ARENA_X       6
#define ARENA_Y       22
#define ARENA_W       228
#define ARENA_H       110

#define HERO_X        10
#define HERO_Y        24
#define MONSTER_X     150
#define MONSTER_Y     26

#define ACTION_X      6
#define ACTION_Y      208
#define ACTION_W      228
#define ACTION_H      86

#define TICK_MS       50
#define BATTERY_TICKS 30

#define NVS_NAMESPACE "battle_v1"

typedef enum {
    PAGE_HOME = 0,
    PAGE_STAGES,
    PAGE_BATTLE,
} page_t;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_PAUSE,
    OVERLAY_CLEAR,
    OVERLAY_OVER,
} overlay_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *hero_img;
    lv_obj_t *hero_name;
    lv_obj_t *hero_perk;
    lv_obj_t *hero_index;
    lv_obj_t *pill[CAMPAIGN_DIFFICULTY_COUNT];
    lv_obj_t *pill_text[CAMPAIGN_DIFFICULTY_COUNT];
    lv_obj_t *diff_detail;
    lv_obj_t *start_label;
} home_ui_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *row[CAMPAIGN_STAGE_COUNT];
    lv_obj_t *stars[CAMPAIGN_STAGE_COUNT];
    lv_obj_t *status[CAMPAIGN_STAGE_COUNT];
    lv_obj_t *detail_1;
    lv_obj_t *detail_2;
} stages_ui_t;

typedef struct {
    lv_obj_t *stage_label;
    lv_obj_t *monster_name;
    lv_obj_t *battery;
    lv_obj_t *arena;
    lv_obj_t *hero_img;
    lv_obj_t *monster_img;
    lv_obj_t *monster_hp;
    lv_obj_t *hearts;
    lv_obj_t *gauge;
    lv_obj_t *gauge_text;
    lv_obj_t *question;
    lv_obj_t *hint;
    lv_obj_t *timer;
    lv_obj_t *score;
    lv_obj_t *combo;
    lv_obj_t *footer;
    lv_obj_t *row[MATH_QUESTION_OPTION_COUNT];
    lv_obj_t *row_text[MATH_QUESTION_OPTION_COUNT];
    lv_obj_t *row_mark[MATH_QUESTION_OPTION_COUNT];
    lv_obj_t *action;
    lv_obj_t *action_title;
    lv_obj_t *action_sub;
    lv_obj_t *crit_track;
    lv_obj_t *crit_zone;
    lv_obj_t *crit_marker;
    lv_obj_t *beam_track;
    lv_obj_t *beam_fill;
    lv_obj_t *beam_glow;
    lv_obj_t *beam_core;
    lv_obj_t *flash;
} battle_ui_t;

static struct {
    battle_t battle;
    page_t page;
    overlay_t overlay;

    uint8_t hero;
    uint8_t difficulty;
    uint8_t stage;
    uint8_t unlocked;
    uint32_t stars;
    uint32_t best[CAMPAIGN_STAGE_COUNT];
    bool muted;

    lv_obj_t *root;
    lv_obj_t *overlay_root;
    home_ui_t home;
    stages_ui_t stages;
    battle_ui_t battle_ui;

    uint8_t overlay_items;
    uint8_t overlay_index;

    int battery_soc;
    int64_t notice_until_us;
    char notice[48];

    int64_t last_tick_us;
    int64_t monster_flash_until_us;
    int64_t hero_flash_until_us;
    int64_t arena_flash_until_us;
    int64_t hit_stop_until_us;

    int8_t previous_pose;
    int8_t previous_monster_pose;

    uint8_t feedback_correct;
    uint8_t feedback_wrong;
} s_app;

static volatile bool s_save_pending;

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------
static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w,
                            const char *text, const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    if (w > 0) {
        lv_obj_set_width(label, w);
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *make_center(lv_obj_t *parent, int x, int y, int w,
                             const char *text, const lv_font_t *font,
                             uint32_t color)
{
    lv_obj_t *label = make_label(parent, x, y, w, text, font, color);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void label_set(lv_obj_t *label, const char *text)
{
    if (label == NULL) return;
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void label_color(lv_obj_t *label, uint32_t color)
{
    if (label == NULL) return;
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void page_switch(lv_obj_t *scr)
{
    lv_obj_t *old = s_app.root;
    s_app.root = scr;
    lv_screen_load(scr);
    if (old != NULL) {
        lv_obj_delete(old);
    }
    s_app.overlay_root = NULL;
    s_app.overlay = OVERLAY_NONE;
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
static void nvs_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "暂无存档,使用默认进度");
        return;
    }
    uint8_t u8 = 0;
    if (nvs_get_u8(handle, "unlocked", &u8) == ESP_OK && u8 < CAMPAIGN_STAGE_COUNT) {
        s_app.unlocked = u8;
    }
    if (nvs_get_u8(handle, "hero", &u8) == ESP_OK && u8 < CAMPAIGN_HERO_COUNT) {
        s_app.hero = u8;
    }
    if (nvs_get_u8(handle, "difficulty", &u8) == ESP_OK &&
        u8 < CAMPAIGN_DIFFICULTY_COUNT) {
        s_app.difficulty = u8;
    }
    if (nvs_get_u8(handle, "muted", &u8) == ESP_OK) {
        s_app.muted = u8 != 0;
    }
    (void)nvs_get_u32(handle, "stars", &s_app.stars);
    size_t size = sizeof(s_app.best);
    (void)nvs_get_blob(handle, "best", s_app.best, &size);
    nvs_close(handle);
}

static void nvs_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败,进度未保存");
        return;
    }
    (void)nvs_set_u8(handle, "unlocked", s_app.unlocked);
    (void)nvs_set_u8(handle, "hero", s_app.hero);
    (void)nvs_set_u8(handle, "difficulty", s_app.difficulty);
    (void)nvs_set_u8(handle, "muted", s_app.muted ? 1 : 0);
    (void)nvs_set_u32(handle, "stars", s_app.stars);
    (void)nvs_set_blob(handle, "best", s_app.best, sizeof(s_app.best));
    const esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 提交失败: %s", esp_err_to_name(err));
    }
}

static void request_save(void)
{
    s_save_pending = true;
}

static uint8_t stage_star_count(uint8_t stage)
{
    return (uint8_t)((s_app.stars >> (stage * 2u)) & 3u);
}

static void stage_record_result(void)
{
    const uint8_t stage = s_app.battle.stage;
    const uint8_t stars = s_app.battle.stars;
    const uint32_t shift = (uint32_t)stage * 2u;
    const uint32_t old = (s_app.stars >> shift) & 3u;
    if (stars > old) {
        s_app.stars = (s_app.stars & ~(3u << shift)) | ((uint32_t)stars << shift);
    }
    if (s_app.battle.score > 0 &&
        (uint32_t)s_app.battle.score > s_app.best[stage]) {
        s_app.best[stage] = (uint32_t)s_app.battle.score;
    }
    if (stage + 1u < CAMPAIGN_STAGE_COUNT && s_app.unlocked < stage + 1u) {
        s_app.unlocked = (uint8_t)(stage + 1u);
    }
    request_save();
}

// ---------------------------------------------------------------------------
// 文本片段
// ---------------------------------------------------------------------------
static void battery_string(char *buf, size_t size)
{
    if (s_app.battery_soc < 0) {
        snprintf(buf, size, "--");
    } else {
        snprintf(buf, size, "%d%%", s_app.battery_soc);
    }
}

static void hearts_string(char *buf, size_t size)
{
    const uint16_t hp = s_app.battle.hero_hp;
    const uint16_t max = s_app.battle.hero_hp_max;
    size_t pos = 0;
    for (uint16_t i = 0; i < max && pos + 4 < size; i++) {
        const char *glyph = i < hp ? "♥" : "♡";
        const size_t len = strlen(glyph);
        memcpy(buf + pos, glyph, len);
        pos += len;
    }
    buf[pos] = '\0';
}

static void stars_string(char *buf, size_t size, uint8_t stars)
{
    size_t pos = 0;
    for (uint8_t i = 0; i < 3 && pos + 4 < size; i++) {
        const char *glyph = i < stars ? "★" : "☆";
        const size_t len = strlen(glyph);
        memcpy(buf + pos, glyph, len);
        pos += len;
    }
    buf[pos] = '\0';
}

static void set_battery_labels(void)
{
    char buf[16];
    battery_string(buf, sizeof(buf));
    switch (s_app.page) {
    case PAGE_HOME:
        label_set(s_app.home.battery, buf);
        break;
    case PAGE_STAGES:
        label_set(s_app.stages.battery, buf);
        break;
    case PAGE_BATTLE:
        label_set(s_app.battle_ui.battery, buf);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 首页
// ---------------------------------------------------------------------------
static void refresh_home(void)
{
    if (s_app.page != PAGE_HOME || s_app.root == NULL) return;
    const hero_def_t *hero = campaign_hero(s_app.hero);
    const difficulty_def_t *diff = campaign_difficulty(s_app.difficulty);

    lv_image_set_src(s_app.home.hero_img, sprites_hero(s_app.hero, SPRITE_POSE_IDLE));
    label_set(s_app.home.hero_name, hero->name);
    label_color(s_app.home.hero_name, hero->accent_color);
    label_set(s_app.home.hero_perk, hero->perk_text);

    char buf[48];
    snprintf(buf, sizeof(buf), "< %d / %d >", (int)s_app.hero + 1,
             (int)CAMPAIGN_HERO_COUNT);
    label_set(s_app.home.hero_index, buf);
    snprintf(buf, sizeof(buf), "%s · %s", diff->name, diff->detail);
    label_set(s_app.home.diff_detail, buf);

    for (int i = 0; i < CAMPAIGN_DIFFICULTY_COUNT; i++) {
        const bool selected = i == s_app.difficulty;
        lv_obj_set_style_bg_color(s_app.home.pill[i],
                                  lv_color_hex(selected ? hero->accent_color : COL_TILE), 0);
        lv_obj_set_style_border_width(s_app.home.pill[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_app.home.pill[i], lv_color_hex(COL_TEXT), 0);
        label_color(s_app.home.pill_text[i], selected ? COL_INK : COL_DIM);
    }
}

static void build_home(void)
{
    lv_obj_t *scr = make_screen();
    const hero_def_t *hero = campaign_hero(s_app.hero);

    // 背景点缀:几颗像素星,让深色屏幕不空。
    static const int star_pos[][2] = {
        {18, 42}, {206, 38}, {28, 128}, {198, 116}, {16, 250}, {214, 268},
    };
    for (size_t i = 0; i < sizeof(star_pos) / sizeof(star_pos[0]); i++) {
        lv_obj_t *dot = make_panel(scr, star_pos[i][0], star_pos[i][1], 2, 2,
                                   i % 2 ? COL_EDGE : COL_GOLD, 1);
        (void)dot;
    }

    s_app.home.battery = make_label(scr, 194, 8, 40, " --", &app_font_12, COL_DIM);
    lv_obj_set_style_text_align(s_app.home.battery, LV_TEXT_ALIGN_RIGHT, 0);

    make_center(scr, 0, 8, SCREEN_W, "算术超能战", &app_font_28, COL_GOLD);
    make_center(scr, 0, 42, SCREEN_W, "口算就是超能力", &app_font_12, COL_DIM);

    s_app.home.hero_img = lv_image_create(scr);
    lv_image_set_src(s_app.home.hero_img, sprites_hero(s_app.hero, SPRITE_POSE_IDLE));
    lv_obj_set_pos(s_app.home.hero_img, 84, 56);
    lv_obj_set_size(s_app.home.hero_img, 72, 88);

    s_app.home.hero_name = make_center(scr, 0, 148, SCREEN_W, "", &app_font_20,
                                       hero->accent_color);
    s_app.home.hero_perk = make_center(scr, 0, 172, SCREEN_W, "", &app_font_12, COL_DIM);
    s_app.home.hero_index = make_center(scr, 0, 190, SCREEN_W, "", &app_font_12, COL_DIM);

    make_label(scr, 10, 212, 30, "难度", &app_font_12, COL_DIM);
    for (int i = 0; i < CAMPAIGN_DIFFICULTY_COUNT; i++) {
        const int x = 40 + i * 66;
        s_app.home.pill[i] = make_panel(scr, x, 208, 62, 22, COL_TILE, 7);
        s_app.home.pill_text[i] = make_center(scr, x, 211, 62,
                                              campaign_difficulty(i)->name,
                                              &app_font_12, COL_DIM);
    }
    s_app.home.diff_detail = make_center(scr, 0, 234, SCREEN_W, "", &app_font_12,
                                         COL_DIM);
    make_panel(scr, 70, 256, 100, 30, hero->accent_color, 9);
    s_app.home.start_label = make_center(scr, 70, 258, 100, "出击", &app_font_20,
                                         COL_INK);

    make_center(scr, 0, 296, SCREEN_W, "上下选英雄 · 长按OK换难度", &app_font_12,
                COL_DIM);

    page_switch(scr);
    refresh_home();
    set_battery_labels();
}

// ---------------------------------------------------------------------------
// 关卡选择
// ---------------------------------------------------------------------------
static void refresh_stages(void)
{
    if (s_app.page != PAGE_STAGES || s_app.root == NULL) return;

    for (int i = 0; i < CAMPAIGN_STAGE_COUNT; i++) {
        const bool selected = i == s_app.stage;
        const bool locked = i > s_app.unlocked;
        lv_obj_set_style_bg_color(s_app.stages.row[i],
                                  lv_color_hex(selected ? COL_TILE_SEL : COL_TILE), 0);
        lv_obj_set_style_border_width(s_app.stages.row[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_app.stages.row[i], lv_color_hex(COL_ACCENT), 0);

        if (locked) {
            label_set(s_app.stages.stars[i], "?");
            label_color(s_app.stages.stars[i], COL_DIM);
            label_set(s_app.stages.status[i], "未");
            label_color(s_app.stages.status[i], COL_DIM);
        } else {
            char buf[16];
            stars_string(buf, sizeof(buf), stage_star_count((uint8_t)i));
            label_set(s_app.stages.stars[i], buf);
            label_color(s_app.stages.stars[i], COL_GOLD);
            label_set(s_app.stages.status[i], "");
        }
    }

    const stage_def_t *stage = campaign_stage(s_app.stage);
    char line[64];
    snprintf(line, sizeof(line), "对手:%s", stage->monster_name);
    label_set(s_app.stages.detail_1, line);
    label_set(s_app.stages.detail_2, stage->domain);
}

static void build_stages(void)
{
    lv_obj_t *scr = make_screen();
    const difficulty_def_t *diff = campaign_difficulty(s_app.difficulty);

    s_app.stages.battery = make_label(scr, 194, 6, 40, " --", &app_font_12, COL_DIM);
    lv_obj_set_style_text_align(s_app.stages.battery, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(scr, 8, 5, 150, "选择关卡", &app_font_20, COL_TEXT);
    char diff_text[32];
    snprintf(diff_text, sizeof(diff_text), "难度 %s", diff->name);
    make_label(scr, 120, 9, 74, diff_text, &app_font_12, COL_GOLD);

    for (int i = 0; i < CAMPAIGN_STAGE_COUNT; i++) {
        const int y = 32 + i * 28;
        const stage_def_t *stage = campaign_stage((uint8_t)i);
        s_app.stages.row[i] = make_panel(scr, 4, y, 232, 26, COL_TILE, 6);

        char num[8];
        snprintf(num, sizeof(num), "%d", i + 1);
        make_label(s_app.stages.row[i], 6, 5, 14, num, &app_font_12, COL_DIM);
        make_label(s_app.stages.row[i], 22, 5, 120, stage->name, &app_font_12,
                   COL_TEXT);
        s_app.stages.stars[i] = make_center(s_app.stages.row[i], 148, 5, 52, "",
                                            &app_font_12, COL_GOLD);
        s_app.stages.status[i] = make_label(s_app.stages.row[i], 202, 5, 24, "",
                                            &app_font_12, COL_DIM);
    }

    s_app.stages.detail_1 = make_label(scr, 8, 258, 224, "", &app_font_12, COL_TEXT);
    s_app.stages.detail_2 = make_label(scr, 8, 274, 224, "", &app_font_12, COL_DIM);
    make_center(scr, 0, 298, SCREEN_W, "上下选关 · 确定出击 · 长按返回", &app_font_12,
                COL_DIM);

    page_switch(scr);
    refresh_stages();
    set_battery_labels();
}

// ---------------------------------------------------------------------------
// 战斗界面
// ---------------------------------------------------------------------------
static void refresh_battle_texts(void)
{
    const battle_t *battle = &s_app.battle;
    const stage_def_t *stage = battle_stage_def(battle);
    const hero_def_t *hero = battle_hero_def(battle);

    char buf[80];
    snprintf(buf, sizeof(buf), "第%d关", (int)battle->stage + 1);
    label_set(s_app.battle_ui.stage_label, buf);
    label_set(s_app.battle_ui.monster_name, stage->monster_name);
    (void)stage->name;

    hearts_string(buf, sizeof(buf));
    label_set(s_app.battle_ui.hearts, buf);

    lv_bar_set_range(s_app.battle_ui.monster_hp, 0,
                     battle->monster_hp_max ? battle->monster_hp_max : 1);
    lv_bar_set_value(s_app.battle_ui.monster_hp, battle->monster_hp, LV_ANIM_OFF);
    lv_bar_set_range(s_app.battle_ui.gauge, 0, BATTLE_GAUGE_MAX);
    lv_bar_set_value(s_app.battle_ui.gauge, battle->gauge, LV_ANIM_OFF);

    if (battle_can_ultimate(battle)) {
        label_set(s_app.battle_ui.gauge_text, "必杀就绪!");
        label_color(s_app.battle_ui.gauge_text, COL_GOLD);
    } else {
        snprintf(buf, sizeof(buf), "必杀 %d/%d", (int)battle->gauge,
                 (int)BATTLE_GAUGE_MAX);
        label_set(s_app.battle_ui.gauge_text, buf);
        label_color(s_app.battle_ui.gauge_text, COL_DIM);
    }

    label_set(s_app.battle_ui.question, battle->question.text);

    const char *hint = "";
    switch (battle->phase) {
    case BATTLE_PHASE_QUESTION:
        if (s_app.notice_until_us > esp_timer_get_time()) {
            hint = s_app.notice;
        } else if (battle->hint_used) {
            hint = battle->question.hint;
        } else if (battle->last_stand_bonus) {
            hint = "背水一战!时间增加";
        } else if (battle->cursor == BATTLE_CURSOR_NONE) {
            hint = "先按↑或↓选答案";
        } else if (battle->assist_active) {
            hint = "别急,这题简单一点";
        } else {
            hint = "答对可蓄力 · 长按↓提示";
        }
        break;
    case BATTLE_PHASE_CRITICAL:
        hint = "在金色区域按下确定";
        break;
    case BATTLE_PHASE_ATTACK:
        hint = "漂亮的一击!";
        break;
    case BATTLE_PHASE_ENEMY:
        hint = "被怪兽反击了!";
        break;
    case BATTLE_PHASE_ULTIMATE:
        hint = "必杀技发动!";
        break;
    case BATTLE_PHASE_BEAM:
        hint = "连按确定,把光线推回去!";
        break;
    case BATTLE_PHASE_CLEAR:
        hint = "怪兽被击败!";
        break;
    case BATTLE_PHASE_OVER:
        hint = "战斗结束";
        break;
    case BATTLE_PHASE_PAUSED:
        hint = "暂停中";
        break;
    default:
        break;
    }
    label_set(s_app.battle_ui.hint, hint);
    if (battle->phase == BATTLE_PHASE_QUESTION &&
        s_app.notice_until_us > esp_timer_get_time()) {
        label_color(s_app.battle_ui.hint, COL_GOLD);
    } else if (battle->hint_used) {
        label_color(s_app.battle_ui.hint, COL_ACCENT);
    } else {
        label_color(s_app.battle_ui.hint, COL_DIM);
    }

    const int32_t total = battle->question_ms > 0 ? battle->question_ms : 1;
    const int32_t remain = battle->question_remaining_ms;
    lv_bar_set_range(s_app.battle_ui.timer, 0, total);
    lv_bar_set_value(s_app.battle_ui.timer, remain > 0 ? remain : 0, LV_ANIM_OFF);
    uint32_t timer_color = COL_OK;
    if (remain * 3 < total) {
        timer_color = COL_DANGER;
    } else if (remain * 2 < total) {
        timer_color = COL_GOLD;
    }
    lv_obj_set_style_bg_color(s_app.battle_ui.timer, lv_color_hex(timer_color),
                              LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "得分 %d", (int)battle->score);
    label_set(s_app.battle_ui.score, buf);
    if (battle->combo >= 2) {
        snprintf(buf, sizeof(buf), "连击 x%d", (int)battle->combo);
        label_set(s_app.battle_ui.combo, buf);
        label_color(s_app.battle_ui.combo, COL_GOLD);
    } else {
        label_set(s_app.battle_ui.combo, "");
    }

    if (battle_can_ultimate(battle)) {
        label_set(s_app.battle_ui.footer, "必杀就绪!长按↑发动");
        label_color(s_app.battle_ui.footer, COL_GOLD);
    } else if (battle->phase == BATTLE_PHASE_BEAM) {
        label_set(s_app.battle_ui.footer, "连按OK!把光线推回去");
        label_color(s_app.battle_ui.footer, COL_DANGER);
    } else if (battle->phase == BATTLE_PHASE_CRITICAL) {
        label_set(s_app.battle_ui.footer, "看准金色区域,按下OK");
        label_color(s_app.battle_ui.footer, COL_GOLD);
    } else {
        label_set(s_app.battle_ui.footer, "↑↓选 · OK答 · 长按OK暂停");
        label_color(s_app.battle_ui.footer, COL_DIM);
    }

    (void)hero;
}

static void refresh_battle_rows(void)
{
    const battle_t *battle = &s_app.battle;
    const bool action = battle->phase == BATTLE_PHASE_CRITICAL ||
                        battle->phase == BATTLE_PHASE_ULTIMATE ||
                        battle->phase == BATTLE_PHASE_BEAM;
    // Correct/wrong feedback belongs to the attack exchange only. Showing it
    // during QUESTION used to leave a green "correct" row on screen and made
    // it look like the cursor was parked on the right answer.
    const bool show_feedback = battle->phase == BATTLE_PHASE_ATTACK ||
                               battle->phase == BATTLE_PHASE_ENEMY;

    for (int i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        set_hidden(s_app.battle_ui.row[i], action);
        set_hidden(s_app.battle_ui.row_mark[i], true);
        if (action) continue;

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)battle->question.options[i]);
        label_set(s_app.battle_ui.row_text[i], buf);

        const bool selected = battle->phase == BATTLE_PHASE_QUESTION &&
                              battle->cursor == i;
        uint32_t bg = COL_TILE;
        uint32_t fg = COL_TEXT;
        int border = 0;
        uint32_t border_color = COL_ACCENT;
        if (show_feedback && s_app.feedback_correct == i) {
            bg = 0x123B2A;
            fg = COL_OK;
            border = 2;
            border_color = COL_OK;
        } else if (show_feedback && s_app.feedback_wrong == i) {
            bg = 0x3A1520;
            fg = COL_DANGER;
            border = 2;
            border_color = COL_DANGER;
        } else if (selected) {
            bg = COL_TILE_SEL;
            fg = COL_GOLD;
            border = 2;
            border_color = COL_GOLD;
            set_hidden(s_app.battle_ui.row_mark[i], false);
        }
        lv_obj_set_style_bg_color(s_app.battle_ui.row[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_width(s_app.battle_ui.row[i], border, 0);
        lv_obj_set_style_border_color(s_app.battle_ui.row[i],
                                      lv_color_hex(border_color), 0);
        label_color(s_app.battle_ui.row_text[i], fg);
    }
}

static void refresh_battle_action(void)
{
    const battle_t *battle = &s_app.battle;

    if (battle->phase == BATTLE_PHASE_CRITICAL) {
        set_hidden(s_app.battle_ui.action, false);
        label_set(s_app.battle_ui.action_title, "暴击时刻!");
        label_color(s_app.battle_ui.action_title, COL_GOLD);
        label_set(s_app.battle_ui.action_sub, "金色区域按下确定");
        set_hidden(s_app.battle_ui.crit_track, false);
        set_hidden(s_app.battle_ui.crit_marker, false);
        set_hidden(s_app.battle_ui.beam_track, true);

        const int half = battle->crit_window / 2;
        int left = (int)battle->crit_center - half;
        if (left < 0) left = 0;
        int width = battle->crit_window;
        if (left + width > 100) width = 100 - left;
        lv_obj_set_pos(s_app.battle_ui.crit_zone, left * 200 / 100, 2);
        lv_obj_set_size(s_app.battle_ui.crit_zone, width * 200 / 100, 10);
        const int marker_x = ACTION_X + 14 + (int)battle->crit_marker * 200 / 100 - 2;
        lv_obj_set_pos(s_app.battle_ui.crit_marker, marker_x - ACTION_X, 32);
        return;
    }

    if (battle->phase == BATTLE_PHASE_ULTIMATE) {
        set_hidden(s_app.battle_ui.action, false);
        label_set(s_app.battle_ui.action_title, "必杀技!");
        label_color(s_app.battle_ui.action_title, COL_ACCENT);
        label_set(s_app.battle_ui.action_sub, "超能光线全功率释放");
        set_hidden(s_app.battle_ui.crit_track, true);
        set_hidden(s_app.battle_ui.crit_marker, true);
        set_hidden(s_app.battle_ui.beam_track, true);
        return;
    }

    if (battle->phase == BATTLE_PHASE_BEAM) {
        set_hidden(s_app.battle_ui.action, false);
        label_set(s_app.battle_ui.action_title, "光线对决!");
        label_color(s_app.battle_ui.action_title, COL_DANGER);
        char sub[48];
        snprintf(sub, sizeof(sub), "连按确定!已按 %d 次", (int)battle->beam_presses);
        label_set(s_app.battle_ui.action_sub, sub);
        set_hidden(s_app.battle_ui.crit_track, true);
        set_hidden(s_app.battle_ui.crit_marker, true);
        set_hidden(s_app.battle_ui.beam_track, false);
        lv_obj_set_size(s_app.battle_ui.beam_fill, battle->beam_force * 160 / 100, 6);
        return;
    }

    set_hidden(s_app.battle_ui.action, true);
}

static void refresh_battle_sprites(void)
{
    const battle_t *battle = &s_app.battle;

    sprite_pose_t hero_pose = SPRITE_POSE_IDLE;
    sprite_monster_pose_t monster_pose = SPRITE_MONSTER_IDLE;
    switch (battle->phase) {
    case BATTLE_PHASE_CRITICAL:
        hero_pose = SPRITE_POSE_FIGHT;
        break;
    case BATTLE_PHASE_ATTACK:
        // Alternate a flying punch and a kick so repeated hits do not look
        // like the same frame every time.
        hero_pose = (battle->correct_count & 1u) ? SPRITE_POSE_KICK
                                                 : SPRITE_POSE_FLY;
        monster_pose = SPRITE_MONSTER_HURT;
        break;
    case BATTLE_PHASE_ENEMY:
        hero_pose = SPRITE_POSE_GUARD;
        monster_pose = SPRITE_MONSTER_ATTACK;
        break;
    case BATTLE_PHASE_ULTIMATE:
        hero_pose = SPRITE_POSE_CHARGE;
        monster_pose = SPRITE_MONSTER_ROAR;
        break;
    case BATTLE_PHASE_BEAM:
        hero_pose = SPRITE_POSE_BEAM;
        monster_pose = SPRITE_MONSTER_CHARGE;
        break;
    case BATTLE_PHASE_CLEAR:
        hero_pose = SPRITE_POSE_VICTORY;
        monster_pose = SPRITE_MONSTER_HURT;
        break;
    case BATTLE_PHASE_OVER:
        hero_pose = SPRITE_POSE_GUARD;
        monster_pose = SPRITE_MONSTER_ROAR;
        break;
    default:
        break;
    }

    if (s_app.previous_pose != (int8_t)hero_pose) {
        s_app.previous_pose = (int8_t)hero_pose;
        lv_image_set_src(s_app.battle_ui.hero_img,
                         sprites_hero(battle->hero, hero_pose));
    }
    if (s_app.previous_monster_pose != (int8_t)monster_pose) {
        s_app.previous_monster_pose = (int8_t)monster_pose;
        lv_image_set_src(s_app.battle_ui.monster_img,
                         sprites_monster(battle->stage, monster_pose));
    }
}

static void refresh_battle_animation(void)
{
    const battle_t *battle = &s_app.battle;
    const int64_t now = esp_timer_get_time();
    const uint32_t anim = (uint32_t)(now / 1000);

    int hx = HERO_X;
    int hy = HERO_Y;
    int mx = MONSTER_X;
    int my = MONSTER_Y;
    const int bob = ((anim / 420) % 2) ? 1 : 0;

    switch (battle->phase) {
    case BATTLE_PHASE_QUESTION:
    case BATTLE_PHASE_CRITICAL:
        hy += bob;
        break;
    case BATTLE_PHASE_ATTACK: {
        const int p = (BATTLE_ATTACK_MS - (int)battle->phase_ms) * 100 / BATTLE_ATTACK_MS;
        const int tri = 100 - abs(p * 2 - 100);
        hx += tri * 30 / 100;
        hy -= tri * 4 / 100;
        break;
    }
    case BATTLE_PHASE_ENEMY: {
        const int p = (BATTLE_ENEMY_MS - (int)battle->phase_ms) * 100 / BATTLE_ENEMY_MS;
        const int tri = 100 - abs(p * 2 - 100);
        mx -= tri * 26 / 100;
        my -= tri * 6 / 100;
        break;
    }
    case BATTLE_PHASE_ULTIMATE: {
        const int p = (BATTLE_ULTIMATE_MS - (int)battle->phase_ms) * 100 /
                      BATTLE_ULTIMATE_MS;
        const int tri = 100 - abs(p * 2 - 100);
        hx += tri * 8 / 100;
        break;
    }
    case BATTLE_PHASE_CLEAR:
        hy -= bob * 3;
        break;
    default:
        break;
    }

    lv_obj_set_pos(s_app.battle_ui.hero_img, hx - ARENA_X, hy - ARENA_Y);
    lv_obj_set_pos(s_app.battle_ui.monster_img, mx - ARENA_X, my - ARENA_Y);

    const bool monster_flash = now < s_app.monster_flash_until_us;
    lv_obj_set_style_image_recolor(s_app.battle_ui.monster_img,
                                   lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(s_app.battle_ui.monster_img,
                                       monster_flash ? LV_OPA_70 : LV_OPA_TRANSP, 0);
    const bool hero_flash = now < s_app.hero_flash_until_us;
    lv_obj_set_style_image_recolor(s_app.battle_ui.hero_img,
                                   lv_color_hex(COL_DANGER), 0);
    lv_obj_set_style_image_recolor_opa(s_app.battle_ui.hero_img,
                                       hero_flash ? LV_OPA_70 : LV_OPA_TRANSP, 0);

    lv_obj_set_style_bg_opa(s_app.battle_ui.flash,
                            now < s_app.arena_flash_until_us ? LV_OPA_40
                                                             : LV_OPA_TRANSP,
                            0);

    const bool beam = battle->phase == BATTLE_PHASE_ULTIMATE ||
                      battle->phase == BATTLE_PHASE_BEAM;
    set_hidden(s_app.battle_ui.beam_glow, !beam);
    set_hidden(s_app.battle_ui.beam_core, !beam);
    if (beam) {
        const int force = battle->phase == BATTLE_PHASE_BEAM ? battle->beam_force : 65;
        const int glow_h = 10 + force * 12 / 100;
        const int core_h = 4 + force * 8 / 100;
        const int x0 = hx + 54;
        int width = mx - x0;
        if (width < 4) width = 4;
        const int cy = 66;
        lv_obj_set_pos(s_app.battle_ui.beam_glow, x0 - ARENA_X,
                       cy - glow_h / 2 - ARENA_Y);
        lv_obj_set_size(s_app.battle_ui.beam_glow, width, glow_h);
        lv_obj_set_pos(s_app.battle_ui.beam_core, x0 - ARENA_X,
                       cy - core_h / 2 - ARENA_Y);
        lv_obj_set_size(s_app.battle_ui.beam_core, width, core_h);
    }
}

static void refresh_battle(void)
{
    if (s_app.page != PAGE_BATTLE || s_app.root == NULL) return;
    refresh_battle_texts();
    refresh_battle_rows();
    refresh_battle_action();
    refresh_battle_sprites();
    refresh_battle_animation();
    set_battery_labels();
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t bg, uint32_t fill)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(fill), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar, 0, 0);
    return bar;
}

static void build_battle(void)
{
    lv_obj_t *scr = make_screen();
    battle_ui_t *ui = &s_app.battle_ui;
    memset(ui, 0, sizeof(*ui));

    ui->stage_label = make_label(scr, 6, 4, 48, "", &app_font_12, COL_DIM);
    ui->monster_name = make_center(scr, 54, 4, 112, "", &app_font_12, COL_TEXT);
    ui->battery = make_label(scr, 194, 4, 40, " --", &app_font_12, COL_DIM);
    lv_obj_set_style_text_align(ui->battery, LV_TEXT_ALIGN_RIGHT, 0);

    ui->arena = make_panel(scr, ARENA_X, ARENA_Y, ARENA_W, ARENA_H, COL_PANEL, 10);
    lv_obj_set_style_border_width(ui->arena, 1, 0);
    lv_obj_set_style_border_color(ui->arena, lv_color_hex(COL_EDGE), 0);

    ui->hero_img = lv_image_create(ui->arena);
    lv_obj_set_pos(ui->hero_img, HERO_X - ARENA_X, HERO_Y - ARENA_Y);
    lv_obj_set_size(ui->hero_img, 72, 88);

    ui->monster_img = lv_image_create(ui->arena);
    lv_obj_set_pos(ui->monster_img, MONSTER_X - ARENA_X, MONSTER_Y - ARENA_Y);
    lv_obj_set_size(ui->monster_img, 80, 80);

    ui->beam_glow = make_panel(ui->arena, 80, 60, 50, 16, COL_ACCENT, 6);
    lv_obj_set_style_bg_opa(ui->beam_glow, LV_OPA_50, 0);
    ui->beam_core = make_panel(ui->arena, 80, 63, 50, 8, COL_TEXT, 4);

    ui->monster_hp = make_bar(ui->arena, MONSTER_X - ARENA_X, 84, 88, 6,
                              COL_EDGE, COL_DANGER);
    lv_obj_set_style_border_width(ui->monster_hp, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->monster_hp, lv_color_hex(COL_BG), LV_PART_MAIN);

    ui->hearts = make_label(ui->arena, 4, 90, 84, "", &app_font_12, COL_DANGER);
    ui->gauge = make_bar(scr, 6, 134, 166, 8, COL_EDGE, COL_ACCENT);
    ui->gauge_text = make_label(scr, 178, 131, 56, "", &app_font_12, COL_DIM);

    ui->question = make_center(scr, 0, 146, SCREEN_W, "", &app_font_20, COL_TEXT);
    ui->hint = make_center(scr, 0, 168, SCREEN_W, "", &app_font_12, COL_DIM);
    ui->timer = make_bar(scr, 6, 186, 228, 5, COL_EDGE, COL_OK);
    ui->score = make_label(scr, 6, 193, 90, "", &app_font_12, COL_DIM);
    ui->combo = make_label(scr, 150, 193, 84, "", &app_font_12, COL_GOLD);
    lv_obj_set_style_text_align(ui->combo, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        const int y = 208 + i * 30;
        ui->row[i] = make_panel(scr, 6, y, 228, 26, COL_TILE, 6);
        ui->row_text[i] = make_center(ui->row[i], 0, 3, 228, "", &app_font_20,
                                      COL_TEXT);
        // A filled dot marks the option the player is about to confirm, so the
        // cursor is never confused with the green "correct answer" feedback.
        ui->row_mark[i] = make_panel(ui->row[i], 7, 10, 6, 6, COL_GOLD, 3);
        set_hidden(ui->row_mark[i], true);
    }

    ui->footer = make_center(scr, 0, 296, SCREEN_W, "", &app_font_12, COL_DIM);

    // 暴击/必杀提示区,盖住答题行。
    ui->action = make_panel(scr, ACTION_X, ACTION_Y, ACTION_W, ACTION_H,
                            COL_PANEL, 10);
    lv_obj_set_style_border_width(ui->action, 1, 0);
    lv_obj_set_style_border_color(ui->action, lv_color_hex(COL_EDGE), 0);
    ui->action_title = make_center(ui->action, 0, 6, ACTION_W, "", &app_font_20,
                                   COL_GOLD);
    ui->action_sub = make_center(ui->action, 0, 64, ACTION_W, "", &app_font_12,
                                 COL_DIM);

    ui->crit_track = make_panel(ui->action, 14, 38, 200, 14, COL_TILE, 7);
    lv_obj_add_flag(ui->crit_track, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    ui->crit_zone = make_panel(ui->crit_track, 90, 2, 20, 10, COL_OK, 5);
    ui->crit_marker = make_panel(ui->action, 112, 32, 4, 22, COL_TEXT, 2);

    ui->beam_track = make_panel(ui->action, 34, 40, 160, 10, COL_TILE, 5);
    ui->beam_fill = make_panel(ui->beam_track, 0, 2, 0, 6, COL_DANGER, 3);

    ui->flash = make_panel(ui->arena, 0, 0, ARENA_W, ARENA_H, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(ui->flash, LV_OPA_TRANSP, 0);

    page_switch(scr);
    s_app.previous_pose = -1;
    s_app.previous_monster_pose = -1;
    s_app.feedback_correct = 0xFF;
    s_app.feedback_wrong = 0xFF;
    s_app.notice[0] = '\0';
    s_app.notice_until_us = 0;
    s_app.last_tick_us = esp_timer_get_time();
    refresh_battle_sprites();
    refresh_battle();
}

// ---------------------------------------------------------------------------
// 浮层
// ---------------------------------------------------------------------------
static void destroy_overlay(void)
{
    if (s_app.overlay_root != NULL) {
        lv_obj_delete(s_app.overlay_root);
        s_app.overlay_root = NULL;
    }
    s_app.overlay = OVERLAY_NONE;
}

static const char *overlay_item_text(uint8_t index)
{
    if (s_app.overlay == OVERLAY_PAUSE) {
        static const char *const items[] = { "继续", "重来", "回主页" };
        return index < 3 ? items[index] : "";
    }
    if (s_app.overlay == OVERLAY_CLEAR) {
        const bool last = s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT;
        if (last) {
            static const char *const items[] = { "重玩本关", "回主页" };
            return index < 2 ? items[index] : "";
        }
        static const char *const items[] = { "下一关", "重来", "回主页" };
        return index < 3 ? items[index] : "";
    }
    if (s_app.overlay == OVERLAY_OVER) {
        static const char *const items[] = { "重来", "回主页" };
        return index < 2 ? items[index] : "";
    }
    return "";
}

static uint8_t overlay_item_count(void)
{
    if (s_app.overlay == OVERLAY_CLEAR &&
        s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT) {
        return 2;
    }
    if (s_app.overlay == OVERLAY_OVER) {
        return 2;
    }
    return 3;
}

static void rebuild_overlay(void)
{
    if (s_app.overlay_root != NULL) {
        lv_obj_delete(s_app.overlay_root);
        s_app.overlay_root = NULL;
    }
    s_app.overlay_root = make_panel(s_app.root, 0, 0, SCREEN_W, SCREEN_H,
                                   0x000000, 0);
    lv_obj_set_style_bg_opa(s_app.overlay_root, LV_OPA_70, 0);

    int card_h = 180;
    if (s_app.overlay == OVERLAY_CLEAR) {
        card_h = 220;
    }
    const int card_y = (SCREEN_H - card_h) / 2;
    lv_obj_t *card = make_panel(s_app.overlay_root, 14, card_y, 212, card_h,
                                COL_PANEL, 12);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_EDGE), 0);

    const char *title = "";
    const char *sub = "";
    if (s_app.overlay == OVERLAY_PAUSE) {
        title = "暂停";
        sub = "休息一下,继续战斗";
    } else if (s_app.overlay == OVERLAY_CLEAR) {
        title = s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT ? "全员通关!"
                                                              : "关卡完成";
        sub = "怪兽被击败了!";
    } else {
        title = "战败";
        sub = "怪兽太强了,再来一次!";
    }
    make_center(card, 0, 10, 212, title, &app_font_20,
                s_app.overlay == OVERLAY_OVER ? COL_DANGER : COL_GOLD);

    int item_y = 52;
    if (s_app.overlay == OVERLAY_CLEAR) {
        char stars[16];
        stars_string(stars, sizeof(stars), s_app.battle.stars);
        make_center(card, 0, 40, 212, stars, &app_font_20, COL_GOLD);
        char line[48];
        snprintf(line, sizeof(line), "得分 %d", (int)s_app.battle.score);
        make_center(card, 0, 70, 212, line, &app_font_12, COL_TEXT);
        snprintf(line, sizeof(line), "本关最高 %u",
                 (unsigned)s_app.best[s_app.battle.stage]);
        make_center(card, 0, 86, 212, line, &app_font_12, COL_DIM);
        item_y = 112;
    } else {
        make_center(card, 0, 40, 212, sub, &app_font_12, COL_DIM);
        item_y = 60;
    }

    s_app.overlay_items = overlay_item_count();
    if (s_app.overlay_index >= s_app.overlay_items) {
        s_app.overlay_index = 0;
    }
    for (uint8_t i = 0; i < s_app.overlay_items; i++) {
        const bool selected = i == s_app.overlay_index;
        lv_obj_t *item = make_panel(card, 26, item_y + i * 32, 160, 26,
                                    selected ? COL_TILE_SEL : COL_TILE, 7);
        if (selected) {
            lv_obj_set_style_border_width(item, 2, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(COL_ACCENT), 0);
        }
        make_center(item, 0, 3, 160, overlay_item_text(i), &app_font_20,
                    selected ? COL_TEXT : COL_DIM);
    }
}

static void show_overlay(overlay_t overlay)
{
    s_app.overlay = overlay;
    s_app.overlay_index = 0;
    rebuild_overlay();
}

// ---------------------------------------------------------------------------
// 战斗流程
// ---------------------------------------------------------------------------
static void start_battle(uint8_t stage)
{
    if (stage >= CAMPAIGN_STAGE_COUNT) {
        stage = 0;
    }
    s_app.stage = stage;
    battle_start(&s_app.battle, stage, s_app.difficulty, s_app.hero,
                 (uint32_t)esp_timer_get_time() ^ 0x5EED1234u);
    s_app.page = PAGE_BATTLE;
    build_battle();
    audio_player_play(SFX_CONFIRM);
}

static void end_battle_to_page(page_t page)
{
    s_app.page = page;
    if (page == PAGE_HOME) {
        build_home();
    } else {
        build_stages();
    }
}

static void set_notice(const char *text)
{
    snprintf(s_app.notice, sizeof(s_app.notice), "%s", text);
    s_app.notice_until_us = esp_timer_get_time() + 1600000LL;
}

static void handle_battle_event(battle_event_t event)
{
    battle_t *battle = &s_app.battle;
    const int64_t now = esp_timer_get_time();

    switch (event) {
    case BATTLE_EV_MOVED:
        audio_player_play(SFX_MOVE);
        break;
    case BATTLE_EV_CORRECT:
        audio_player_play(SFX_CORRECT);
        s_app.feedback_correct = battle->question.correct_index;
        s_app.feedback_wrong = 0xFF;
        break;
    case BATTLE_EV_WRONG:
    case BATTLE_EV_TIMEOUT:
        audio_player_play(SFX_WRONG);
        audio_player_play(SFX_HURT);
        s_app.feedback_correct = battle->question.correct_index;
        s_app.feedback_wrong = event == BATTLE_EV_WRONG ? battle->cursor : 0xFF;
        s_app.hero_flash_until_us = now + 260000;
        s_app.arena_flash_until_us = now + 100000;
        break;
    case BATTLE_EV_CRITICAL_HIT:
        audio_player_play(SFX_CRITICAL);
        audio_player_play(SFX_HIT);
        s_app.monster_flash_until_us = now + 320000;
        s_app.arena_flash_until_us = now + 90000;
        break;
    case BATTLE_EV_CRITICAL_MISS:
        audio_player_play(SFX_HIT);
        s_app.monster_flash_until_us = now + 140000;
        break;
    case BATTLE_EV_ATTACK_DONE:
    case BATTLE_EV_ENEMY_DONE:
        s_app.feedback_correct = 0xFF;
        s_app.feedback_wrong = 0xFF;
        break;
    case BATTLE_EV_ULTIMATE_START:
        audio_player_play(SFX_CHARGE);
        set_notice("必杀技发动!");
        break;
    case BATTLE_EV_ULTIMATE_DONE:
        break;
    case BATTLE_EV_MONSTER_DEFEATED:
        audio_player_play(SFX_MONSTER_DOWN);
        s_app.monster_flash_until_us = now + 700000;
        s_app.arena_flash_until_us = now + 180000;
        set_notice("怪兽被击败!");
        break;
    case BATTLE_EV_HERO_DEFEATED:
        audio_player_play(SFX_DEFEAT);
        show_overlay(OVERLAY_OVER);
        break;
    case BATTLE_EV_STAGE_CLEAR:
        stage_record_result();
        audio_player_play(SFX_VICTORY);
        show_overlay(OVERLAY_CLEAR);
        break;
    case BATTLE_EV_BEAM_START:
        audio_player_play(SFX_BEAM);
        set_notice("连按确定,把光线推回去!");
        break;
    case BATTLE_EV_BEAM_PRESS:
        audio_player_play(SFX_HIT);
        break;
    case BATTLE_EV_BEAM_WIN:
        audio_player_play(SFX_MONSTER_DOWN);
        s_app.monster_flash_until_us = now + 800000;
        s_app.arena_flash_until_us = now + 220000;
        set_notice("光线压制成功!");
        break;
    case BATTLE_EV_BEAM_LOSE:
        audio_player_play(SFX_HURT);
        s_app.hero_flash_until_us = now + 300000;
        set_notice("光线被压制!怪兽还剩2点血");
        break;
    case BATTLE_EV_HINT:
        audio_player_play(SFX_HINT);
        break;
    case BATTLE_EV_NEED_SELECT:
        audio_player_play(SFX_MOVE);
        set_notice("先按↑或↓选答案");
        break;
    default:
        break;
    }
}

static void tick_battle(void)
{
    const int64_t now = esp_timer_get_time();
    uint32_t elapsed = (uint32_t)((now - s_app.last_tick_us) / 1000);
    s_app.last_tick_us = now;
    if (elapsed > 250) {
        elapsed = 250;
    }
    const battle_event_t event = battle_tick(&s_app.battle, elapsed);
    if (event != BATTLE_EV_NONE) {
        handle_battle_event(event);
    }
    refresh_battle();
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------
static void menu_move(int delta, uint8_t count)
{
    if (count == 0) return;
    int next = (int)s_app.overlay_index + delta;
    while (next < 0) next += count;
    s_app.overlay_index = (uint8_t)(next % count);
    audio_player_play(SFX_MOVE);
    rebuild_overlay();
}

static void run_overlay_action(void)
{
    if (s_app.overlay == OVERLAY_PAUSE) {
        if (s_app.overlay_index == 0) {
            battle_resume(&s_app.battle);
            destroy_overlay();
            s_app.last_tick_us = esp_timer_get_time();
            audio_player_play(SFX_CONFIRM);
        } else if (s_app.overlay_index == 1) {
            const uint8_t stage = s_app.battle.stage;
            destroy_overlay();
            start_battle(stage);
        } else {
            destroy_overlay();
            end_battle_to_page(PAGE_HOME);
            audio_player_play(SFX_CONFIRM);
        }
        return;
    }

    if (s_app.overlay == OVERLAY_CLEAR) {
        const bool last = s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT;
        if (s_app.overlay_index == 0) {
            if (last) {
                const uint8_t stage = s_app.battle.stage;
                destroy_overlay();
                start_battle(stage);
            } else {
                const uint8_t next = (uint8_t)(s_app.battle.stage + 1);
                destroy_overlay();
                start_battle(next);
            }
        } else if (!last && s_app.overlay_index == 1) {
            const uint8_t stage = s_app.battle.stage;
            destroy_overlay();
            start_battle(stage);
        } else {
            destroy_overlay();
            end_battle_to_page(PAGE_HOME);
            audio_player_play(SFX_CONFIRM);
        }
        return;
    }

    if (s_app.overlay == OVERLAY_OVER) {
        if (s_app.overlay_index == 0) {
            const uint8_t stage = s_app.battle.stage;
            destroy_overlay();
            start_battle(stage);
        } else {
            destroy_overlay();
            end_battle_to_page(PAGE_HOME);
            audio_player_play(SFX_CONFIRM);
        }
    }
}

static void handle_home_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
        s_app.hero = (uint8_t)((s_app.hero + CAMPAIGN_HERO_COUNT - 1) %
                               CAMPAIGN_HERO_COUNT);
        audio_player_play(SFX_MOVE);
        refresh_home();
        request_save();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
        s_app.hero = (uint8_t)((s_app.hero + 1) % CAMPAIGN_HERO_COUNT);
        audio_player_play(SFX_MOVE);
        refresh_home();
        request_save();
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_app.difficulty = (uint8_t)((s_app.difficulty + 1) %
                                     CAMPAIGN_DIFFICULTY_COUNT);
        audio_player_play(SFX_CONFIRM);
        refresh_home();
        request_save();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        s_app.stage = s_app.unlocked;
        s_app.page = PAGE_STAGES;
        audio_player_play(SFX_CONFIRM);
        build_stages();
    }
}

static void handle_stage_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_UP && s_app.stage > 0) {
        s_app.stage--;
        audio_player_play(SFX_MOVE);
        refresh_stages();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN &&
        s_app.stage + 1 < CAMPAIGN_STAGE_COUNT) {
        s_app.stage++;
        audio_player_play(SFX_MOVE);
        refresh_stages();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        if (s_app.stage > s_app.unlocked) {
            audio_player_play(SFX_WRONG);
            return;
        }
        start_battle(s_app.stage);
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        audio_player_play(SFX_PAUSE);
        end_battle_to_page(PAGE_HOME);
    }
}

static void handle_battle_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (s_app.overlay != OVERLAY_NONE) {
        const uint8_t count = overlay_item_count();
        if (event == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            menu_move(-1, count);
        } else if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            menu_move(1, count);
        } else if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            run_overlay_action();
        } else if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            if (s_app.overlay == OVERLAY_PAUSE) {
                battle_resume(&s_app.battle);
                destroy_overlay();
                s_app.last_tick_us = esp_timer_get_time();
            } else {
                run_overlay_action();
            }
            audio_player_play(SFX_CONFIRM);
        }
        return;
    }

    if (s_app.battle.phase == BATTLE_PHASE_CRITICAL && event == BSP_BTN_PRESS &&
        btn == BSP_BTN_OK) {
        handle_battle_event(battle_confirm(&s_app.battle));
        refresh_battle();
        return;
    }
    if (s_app.battle.phase == BATTLE_PHASE_BEAM && event == BSP_BTN_PRESS &&
        btn == BSP_BTN_OK) {
        handle_battle_event(battle_confirm(&s_app.battle));
        refresh_battle();
        return;
    }

    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (s_app.battle.phase == BATTLE_PHASE_QUESTION) {
            battle_pause(&s_app.battle);
            audio_player_play(SFX_PAUSE);
            show_overlay(OVERLAY_PAUSE);
            refresh_battle();
        }
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        if (s_app.battle.phase == BATTLE_PHASE_QUESTION) {
            handle_battle_event(battle_use_hint(&s_app.battle));
            refresh_battle();
        }
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (s_app.battle.phase == BATTLE_PHASE_QUESTION) {
            const battle_event_t ev = battle_use_ultimate(&s_app.battle);
            if (ev != BATTLE_EV_NONE) {
                handle_battle_event(ev);
            } else {
                set_notice("必杀槽还没满,先答对几题");
            }
            refresh_battle();
        }
        return;
    }

    if (event == BSP_BTN_CLICK && s_app.battle.phase == BATTLE_PHASE_QUESTION) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            handle_battle_event(battle_move_cursor(&s_app.battle,
                                                   btn == BSP_BTN_UP ? -1 : 1));
            refresh_battle();
        } else if (btn == BSP_BTN_OK) {
            handle_battle_event(battle_confirm(&s_app.battle));
            refresh_battle();
        }
    }
}

void battle_app_handle_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (!bsp_lvgl_lock(100)) {
        return;
    }
    switch (s_app.page) {
    case PAGE_HOME:
        handle_home_key(btn, event);
        break;
    case PAGE_STAGES:
        handle_stage_key(btn, event);
        break;
    case PAGE_BATTLE:
        handle_battle_key(btn, event);
        break;
    default:
        break;
    }
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------------------
// 应用任务
// ---------------------------------------------------------------------------
static void app_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    int battery_ticks = 0;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));

        int soc = -1;
        if (++battery_ticks >= BATTERY_TICKS) {
            battery_ticks = 0;
            soc = bsp_battery_soc();
        }

        if (s_save_pending) {
            s_save_pending = false;
            nvs_save();
        }

        if (!bsp_lvgl_lock(100)) {
            continue;
        }
        if (s_app.page == PAGE_BATTLE) {
            tick_battle();
        }
        if (soc >= 0) {
            s_app.battery_soc = soc;
            set_battery_labels();
        }
        bsp_lvgl_unlock();
    }
}

esp_err_t battle_app_start(void)
{
    nvs_load();
    s_app.battery_soc = -1;
    s_app.page = PAGE_HOME;
    s_app.stage = s_app.unlocked;
    s_app.last_tick_us = esp_timer_get_time();

    if (audio_player_start() != ESP_OK) {
        ESP_LOGW(TAG, "音效任务启动失败,游戏将静音运行");
    }
    audio_player_set_muted(s_app.muted);

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败");
        return ESP_ERR_TIMEOUT;
    }
    build_home();
    bsp_lvgl_unlock();

    if (xTaskCreate(app_task, "battle_app", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "算术超能战就绪:英雄=%d 难度=%d 已解锁=%d",
             (int)s_app.hero, (int)s_app.difficulty, (int)s_app.unlocked + 1);
    return ESP_OK;
}

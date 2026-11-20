// main/app/battle_app.c —— 算术超能战:英雄/关卡选择、对战界面、暂停与结算。
//
// 线程模型:
//   * 按键回调在 esp_timer 任务里只入队;输入任务调用 battle_app_handle_key()。
//   * app 任务每 50ms 推进战斗计时、动画与电量刷新。
//   * 两个任务都只在 bsp_lvgl_lock() 内访问 battle 状态和 LVGL 对象。
//   * NVS 写入由 app 任务在锁外完成,避免在 LVGL 锁内做慢操作。
#include "battle_app.h"

#include "anim.h"
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
#include "review.h"
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

#define TICK_MS       33
#define BATTERY_TICKS 45

#define ACTION_ANIM_MS 140
#define INPUT_BUFFER_MS 150
#define READY_MS 1000
#define POPUP_COUNT 4
#define POPUP_LIFE_US 620000LL
#define HP_TWEEN_X10_STEP 3
#define GAUGE_TWEEN_X10_STEP 10

// The core anim enums are intentionally ordered like the sprite enums. Keep
// them pinned together so a future pose insertion fails at compile time.
_Static_assert((int)ANIM_POSE_IDLE == (int)SPRITE_POSE_IDLE, "hero pose order");
_Static_assert((int)ANIM_POSE_FIGHT == (int)SPRITE_POSE_FIGHT, "hero pose order");
_Static_assert((int)ANIM_POSE_FLY == (int)SPRITE_POSE_FLY, "hero pose order");
_Static_assert((int)ANIM_POSE_BEAM == (int)SPRITE_POSE_BEAM, "hero pose order");
_Static_assert((int)ANIM_POSE_CHARGE == (int)SPRITE_POSE_CHARGE, "hero pose order");
_Static_assert((int)ANIM_POSE_KICK == (int)SPRITE_POSE_KICK, "hero pose order");
_Static_assert((int)ANIM_POSE_GUARD == (int)SPRITE_POSE_GUARD, "hero pose order");
_Static_assert((int)ANIM_POSE_VICTORY == (int)SPRITE_POSE_VICTORY, "hero pose order");
_Static_assert((int)ANIM_MONSTER_IDLE == (int)SPRITE_MONSTER_IDLE, "monster pose order");
_Static_assert((int)ANIM_MONSTER_ATTACK == (int)SPRITE_MONSTER_ATTACK, "monster pose order");
_Static_assert((int)ANIM_MONSTER_ROAR == (int)SPRITE_MONSTER_ROAR, "monster pose order");
_Static_assert((int)ANIM_MONSTER_CHARGE == (int)SPRITE_MONSTER_CHARGE, "monster pose order");
_Static_assert((int)ANIM_MONSTER_HURT == (int)SPRITE_MONSTER_HURT, "monster pose order");

#define NVS_NAMESPACE "battle_v1"
#define REVIEW_PAGE_SIZE 2
#define REVIEW_VIEW_MAX 6
#define GAUNTLET_WAVES 3

typedef enum {
    PAGE_HOME = 0,
    PAGE_STAGES,
    PAGE_BATTLE,
    PAGE_SETTINGS,
} page_t;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_PAUSE,
    OVERLAY_CLEAR,
    OVERLAY_OVER,
    OVERLAY_REVIEW,
    OVERLAY_WAVE,
} overlay_t;

typedef enum {
    MODE_CAMPAIGN = 0,
    MODE_ENDLESS,
    MODE_GAUNTLET,
    MODE_COUNT,
} game_mode_t;

typedef enum {
    IMPACT_NONE = 0,
    IMPACT_HERO_HIT,
    IMPACT_HERO_PERFECT,
    IMPACT_HERO_PURSUIT,
    IMPACT_COMBO_FINISH,
    IMPACT_HERO_MISS,
    IMPACT_ENEMY,
    IMPACT_ULT_STEP,
    IMPACT_GUARD_BLOCK,
} impact_kind_t;

typedef struct {
    bool active;
    int64_t start_us;
    int16_t x;
    int16_t y;
    int16_t y_last;
} dmg_popup_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *hero_img;
    lv_obj_t *hero_name;
    lv_obj_t *hero_perk;
    lv_obj_t *hero_index;
    lv_obj_t *pill[CAMPAIGN_DIFFICULTY_COUNT];
    lv_obj_t *pill_text[CAMPAIGN_DIFFICULTY_COUNT];
    lv_obj_t *diff_detail;
    lv_obj_t *mode_pill[MODE_COUNT];
    lv_obj_t *mode_name[MODE_COUNT];
    lv_obj_t *mode_info[MODE_COUNT];
    lv_obj_t *start_panel;
    lv_obj_t *start_label;
    lv_obj_t *footer;
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
    lv_obj_t *battery;
    lv_obj_t *row[4];
    lv_obj_t *name[4];
    lv_obj_t *value[4];
} settings_ui_t;

typedef struct {
    lv_obj_t *stage_label;
    lv_obj_t *monster_name;
    lv_obj_t *battery;
    lv_obj_t *arena;
    lv_obj_t *stage;
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
    lv_obj_t *row_icon[MATH_QUESTION_OPTION_COUNT];
    lv_obj_t *action;
    lv_obj_t *action_title;
    lv_obj_t *action_sub;
    lv_obj_t *crit_track;
    lv_obj_t *crit_zone;
    lv_obj_t *crit_perfect;
    lv_obj_t *crit_marker;
    lv_obj_t *beam_track;
    lv_obj_t *beam_fill;
    lv_obj_t *beam_glow;
    lv_obj_t *beam_core;
    lv_obj_t *flash;
    lv_obj_t *popup[POPUP_COUNT];
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

    game_mode_t mode;
    bool low_age;
    bool reduce_motion;
    uint8_t wave;
    uint8_t pending_stage;
    uint8_t run_bonus_pct;
    uint16_t run_hp;
    int32_t run_score;
    uint32_t endless_best;
    uint32_t gauntlet_best;
    uint8_t settings_index;

    review_store_t review;
    review_record_t review_view[REVIEW_VIEW_MAX];
    uint8_t review_view_count;
    uint8_t review_page;
    overlay_t review_return_overlay;
    uint8_t review_return_index;

    lv_obj_t *root;
    lv_obj_t *overlay_root;
    home_ui_t home;
    stages_ui_t stages;
    settings_ui_t settings;
    battle_ui_t battle_ui;

    uint8_t overlay_items;
    uint8_t overlay_index;

    int battery_soc;
    int64_t notice_until_us;
    char notice[48];

    int64_t last_tick_us;
    int64_t arena_flash_until_us;
    int64_t ready_until_us;

    int8_t previous_pose;
    int8_t previous_monster_pose;

    uint8_t feedback_correct;
    uint8_t feedback_wrong;

    // Damage is applied by the core immediately but revealed on the impact
    // frame so the hit, the flash, the HP loss and the number line up.
    int32_t monster_hp_disp_x10;
    int32_t hero_hp_disp_x10;
    int32_t gauge_disp_x10;
    uint16_t monster_hp_goal;
    uint16_t hero_hp_goal;
    uint16_t monster_hp_seen;
    uint16_t hero_hp_seen;

    uint8_t impact_kind;
    int64_t impact_at_us;
    int32_t impact_damage;
    bool hero_attack_missed;

    int8_t buffered_move;

    int16_t action_offset;
    bool action_target_hidden;
    uint8_t prev_phase;

    dmg_popup_t popup[POPUP_COUNT];
    uint8_t popup_next;

    // Style/size caches. LVGL invalidates an object on every style write even
    // when the value is unchanged, so only push changed values.
    int16_t ui_monster_hp;
    int16_t ui_gauge;
    int16_t ui_timer_value;
    int16_t ui_timer_total;
    uint32_t ui_timer_color;
    uint8_t row_sig[MATH_QUESTION_OPTION_COUNT];
    int16_t row_value[MATH_QUESTION_OPTION_COUNT];
    int16_t ui_hero_x;
    int16_t ui_hero_y;
    int16_t ui_monster_x;
    int16_t ui_monster_y;
    uint8_t ui_hero_flash;
    uint8_t ui_monster_flash;
    int8_t ui_shake_x;
    int8_t ui_shake_y;
    bool ui_beam_visible;
    bool ui_flash_visible;
    int16_t ui_crit_left;
    int16_t ui_crit_width;
    int16_t ui_beam_fill;
} s_app;

static volatile bool s_save_pending;

static void set_notice(const char *text);
static void set_notice_ms(const char *text, int64_t duration_us);

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
    const lv_color_t next = lv_color_hex(color);
    if (lv_color_eq(lv_obj_get_style_text_color(label, 0), next)) return;
    lv_obj_set_style_text_color(label, next, 0);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
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
    review_store_init(&s_app.review);
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
    if (nvs_get_u8(handle, "mode", &u8) == ESP_OK && u8 < MODE_COUNT) {
        s_app.mode = (game_mode_t)u8;
    }
    if (nvs_get_u8(handle, "low_age", &u8) == ESP_OK) {
        s_app.low_age = u8 != 0;
    }
    if (nvs_get_u8(handle, "reduce_motion", &u8) == ESP_OK) {
        s_app.reduce_motion = u8 != 0;
    }
    (void)nvs_get_u32(handle, "stars", &s_app.stars);
    (void)nvs_get_u32(handle, "endless_best", &s_app.endless_best);
    (void)nvs_get_u32(handle, "gauntlet_best", &s_app.gauntlet_best);
    size_t size = sizeof(s_app.best);
    (void)nvs_get_blob(handle, "best", s_app.best, &size);
    size = sizeof(s_app.review);
    if (nvs_get_blob(handle, "review", &s_app.review, &size) == ESP_OK) {
        if (size != sizeof(s_app.review) || s_app.review.count > REVIEW_CAPACITY) {
            review_store_init(&s_app.review);
        }
    }
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
    (void)nvs_set_u8(handle, "mode", (uint8_t)s_app.mode);
    (void)nvs_set_u8(handle, "low_age", s_app.low_age ? 1 : 0);
    (void)nvs_set_u8(handle, "reduce_motion", s_app.reduce_motion ? 1 : 0);
    (void)nvs_set_u32(handle, "stars", s_app.stars);
    (void)nvs_set_u32(handle, "endless_best", s_app.endless_best);
    (void)nvs_set_u32(handle, "gauntlet_best", s_app.gauntlet_best);
    (void)nvs_set_blob(handle, "best", s_app.best, sizeof(s_app.best));
    (void)nvs_set_blob(handle, "review", &s_app.review, sizeof(s_app.review));
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

static void record_mistake(void)
{
    if (s_app.battle.question.text[0] == '\0') {
        return;
    }
    // The pool lives in RAM during the battle; results and page exits flush it
    // so a wrong answer does not trigger an NVS write on every turn.
    (void)review_store_record(&s_app.review, &s_app.battle.question,
                              s_app.battle.stage);
}

// Snapshot the newest mistakes for the result overlay. The played stage comes
// first, then anything else the pool holds.
static void prepare_review_view(void)
{
    const uint8_t tier = campaign_stage(s_app.battle.stage)->math_stage;
    s_app.review_view_count =
        review_store_collect(&s_app.review, s_app.battle.stage, tier,
                             s_app.review_view, REVIEW_VIEW_MAX);
    s_app.review_page = 0;
    request_save();
}

static uint8_t gauntlet_stage(uint8_t wave)
{
    // The three hardest fights double as the elite gauntlet.
    static const uint8_t stages[GAUNTLET_WAVES] = { 5, 6, 7 };
    if (wave == 0 || wave > GAUNTLET_WAVES) {
        wave = 1;
    }
    return stages[wave - 1];
}

static const char *mode_title(game_mode_t mode)
{
    switch (mode) {
    case MODE_ENDLESS:
        return "无尽";
    case MODE_GAUNTLET:
        return "三连战";
    default:
        return "闯关";
    }
}

static bool mode_unlocked(game_mode_t mode)
{
    switch (mode) {
    case MODE_ENDLESS:
        // Clearing stage 1 unlocks the second stage, which gates endless.
        return s_app.unlocked >= 1;
    case MODE_GAUNTLET:
        // Clearing stage 5 unlocks stage 6; the gauntlet starts there.
        return s_app.unlocked >= 5;
    default:
        return true;
    }
}

static const char *mode_lock_hint(game_mode_t mode)
{
    switch (mode) {
    case MODE_ENDLESS:
        return "通关第1关解锁 · 长按↑换模式";
    case MODE_GAUNTLET:
        return "通关第5关解锁 · 长按↑换模式";
    default:
        return "长按OK难度 · ↑模式 · ↓设置";
    }
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

static void hearts_string(char *buf, size_t size, uint16_t hp)
{
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

static const char *stage_rule_text(uint8_t stage)
{
    if (campaign_stage_has_rule(stage, STAGE_RULE_HINT_FREE)) {
        return "提示免费";
    }
    if (campaign_stage_has_rule(stage, STAGE_RULE_HALF_NONCRIT)) {
        return "非完美伤害减半";
    }
    if (campaign_stage_has_rule(stage, STAGE_RULE_FAST_GAUGE)) {
        return "快节奏 · 蓄力+1";
    }
    if (campaign_stage_has_rule(stage, STAGE_RULE_GUARD_QTE)) {
        return "偷袭格挡";
    }
    if (campaign_stage_has_rule(stage, STAGE_RULE_ARMOR_BREAK)) {
        return "完美破甲";
    }
    if (campaign_stage_has_rule(stage, STAGE_RULE_ENRAGE)) {
        return "半血狂暴";
    }
    return "";
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
    case PAGE_SETTINGS:
        label_set(s_app.settings.battery, buf);
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

    for (int i = 0; i < MODE_COUNT; i++) {
        const game_mode_t mode = (game_mode_t)i;
        const bool selected = mode == s_app.mode;
        const bool locked = !mode_unlocked(mode);
        lv_obj_set_style_bg_color(s_app.home.mode_pill[i],
                                  lv_color_hex(selected ? COL_TILE_SEL : COL_TILE), 0);
        lv_obj_set_style_border_width(s_app.home.mode_pill[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_app.home.mode_pill[i],
                                      lv_color_hex(COL_ACCENT), 0);
        label_color(s_app.home.mode_name[i], selected ? COL_TEXT : COL_DIM);
        if (locked) {
            label_set(s_app.home.mode_info[i], "锁");
            label_color(s_app.home.mode_info[i], COL_DANGER);
        } else if (mode == MODE_CAMPAIGN) {
            snprintf(buf, sizeof(buf), "%d关", (int)CAMPAIGN_STAGE_COUNT);
            label_set(s_app.home.mode_info[i], buf);
            label_color(s_app.home.mode_info[i], COL_DIM);
        } else {
            const uint32_t best = mode == MODE_ENDLESS ? s_app.endless_best
                                                       : s_app.gauntlet_best;
            snprintf(buf, sizeof(buf), "高%u", (unsigned)best);
            label_set(s_app.home.mode_info[i], buf);
            label_color(s_app.home.mode_info[i], COL_ACCENT);
        }
    }

    const bool mode_locked = !mode_unlocked(s_app.mode);
    label_set(s_app.home.start_label, mode_locked ? "未解锁" : "出击");
    label_color(s_app.home.start_label, mode_locked ? COL_DIM : COL_INK);
    lv_obj_set_style_bg_color(s_app.home.start_panel,
                              lv_color_hex(mode_locked ? COL_TILE : hero->accent_color), 0);
    label_set(s_app.home.footer,
              mode_locked ? mode_lock_hint(s_app.mode)
                          : "长按OK难度 · ↑模式 · ↓设置");
    label_color(s_app.home.footer, mode_locked ? COL_DANGER : COL_DIM);

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
    lv_obj_set_pos(s_app.home.hero_img, 84, 52);
    lv_obj_set_size(s_app.home.hero_img, 72, 88);

    s_app.home.hero_name = make_center(scr, 0, 144, SCREEN_W, "", &app_font_20,
                                       hero->accent_color);
    s_app.home.hero_perk = make_center(scr, 0, 168, SCREEN_W, "", &app_font_12, COL_DIM);
    s_app.home.hero_index = make_center(scr, 0, 184, SCREEN_W, "", &app_font_12, COL_DIM);

    make_label(scr, 10, 202, 30, "难度", &app_font_12, COL_DIM);
    for (int i = 0; i < CAMPAIGN_DIFFICULTY_COUNT; i++) {
        const int x = 40 + i * 66;
        s_app.home.pill[i] = make_panel(scr, x, 198, 62, 22, COL_TILE, 7);
        s_app.home.pill_text[i] = make_center(scr, x, 201, 62,
                                              campaign_difficulty(i)->name,
                                              &app_font_12, COL_DIM);
    }
    s_app.home.diff_detail = make_center(scr, 0, 224, SCREEN_W, "", &app_font_12,
                                         COL_DIM);

    make_label(scr, 10, 246, 30, "模式", &app_font_12, COL_DIM);
    for (int i = 0; i < MODE_COUNT; i++) {
        const int x = 40 + i * 66;
        s_app.home.mode_pill[i] = make_panel(scr, x, 240, 62, 32, COL_TILE, 7);
        s_app.home.mode_name[i] = make_center(scr, x, 242, 62,
                                              mode_title((game_mode_t)i),
                                              &app_font_12, COL_DIM);
        s_app.home.mode_info[i] = make_center(scr, x, 256, 62, "",
                                              &app_font_12, COL_DIM);
    }

    s_app.home.start_panel = make_panel(scr, 70, 276, 100, 26,
                                        hero->accent_color, 9);
    s_app.home.start_label = make_center(scr, 70, 278, 100, "出击", &app_font_20,
                                         COL_INK);

    s_app.home.footer = make_center(scr, 0, 304, SCREEN_W,
                                    "长按OK难度 · ↑模式 · ↓设置",
                                    &app_font_12, COL_DIM);

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
    const char *rule = stage_rule_text(s_app.stage);
    char line[64];
    if (rule[0] != '\0') {
        snprintf(line, sizeof(line), "对手:%s · %s", stage->monster_name, rule);
    } else {
        snprintf(line, sizeof(line), "对手:%s", stage->monster_name);
    }
    label_set(s_app.stages.detail_1, line);
    label_set(s_app.stages.detail_2,
              "★3 0错满血 · ★2 ≤1错 · ★1 通关");
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

    s_app.stages.detail_1 = make_label(scr, 8, 256, 224, "", &app_font_12, COL_TEXT);
    s_app.stages.detail_2 = make_center(scr, 0, 272, SCREEN_W, "", &app_font_12,
                                        COL_GOLD);
    make_center(scr, 0, 298, SCREEN_W, "上下选关 · 确定出击 · 长按返回", &app_font_12,
                COL_DIM);

    page_switch(scr);
    refresh_stages();
    set_battery_labels();
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------
static const char *settings_name(uint8_t index)
{
    static const char *const names[] = { "低龄模式", "减弱动态", "音效", "返回" };
    return index < 4 ? names[index] : "";
}

static const char *settings_detail(uint8_t index)
{
    static const char *const details[] = {
        "隐藏格挡QTE · 暴击区更宽",
        "关闭震屏和强闪光",
        "战斗音效开关",
        "回到主界面",
    };
    return index < 4 ? details[index] : "";
}

static bool settings_value(uint8_t index)
{
    switch (index) {
    case 0:
        return s_app.low_age;
    case 1:
        return s_app.reduce_motion;
    case 2:
        return !s_app.muted;
    default:
        return false;
    }
}

static void refresh_settings(void)
{
    if (s_app.page != PAGE_SETTINGS || s_app.root == NULL) return;
    for (int i = 0; i < 4; i++) {
        const bool selected = i == s_app.settings_index;
        lv_obj_set_style_bg_color(s_app.settings.row[i],
                                  lv_color_hex(selected ? COL_TILE_SEL : COL_TILE), 0);
        lv_obj_set_style_border_width(s_app.settings.row[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_app.settings.row[i],
                                      lv_color_hex(COL_ACCENT), 0);
        label_color(s_app.settings.name[i], selected ? COL_TEXT : COL_DIM);
        if (i == 3) {
            label_set(s_app.settings.value[i], ">");
            label_color(s_app.settings.value[i], COL_DIM);
        } else {
            const bool on = settings_value((uint8_t)i);
            label_set(s_app.settings.value[i], on ? "开" : "关");
            label_color(s_app.settings.value[i], on ? COL_OK : COL_DIM);
        }
    }
}

static void build_settings(void)
{
    lv_obj_t *scr = make_screen();
    s_app.settings.battery = make_label(scr, 194, 6, 40, " --", &app_font_12, COL_DIM);
    lv_obj_set_style_text_align(s_app.settings.battery, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(scr, 8, 5, 150, "设置", &app_font_20, COL_TEXT);
    make_label(scr, 120, 9, 74, "长按OK返回", &app_font_12, COL_DIM);

    for (int i = 0; i < 4; i++) {
        const int y = 40 + i * 54;
        s_app.settings.row[i] = make_panel(scr, 6, y, 228, 46, COL_TILE, 8);
        s_app.settings.name[i] = make_label(s_app.settings.row[i], 10, 5, 150,
                                            settings_name((uint8_t)i),
                                            &app_font_20, COL_TEXT);
        s_app.settings.value[i] = make_label(s_app.settings.row[i], 184, 8, 34, "",
                                             &app_font_20, COL_DIM);
        lv_obj_set_style_text_align(s_app.settings.value[i], LV_TEXT_ALIGN_RIGHT, 0);
        make_label(s_app.settings.row[i], 10, 27, 208,
                   settings_detail((uint8_t)i), &app_font_12, COL_DIM);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "错题本已记录 %d 题", (int)s_app.review.count);
    make_center(scr, 0, 266, SCREEN_W, buf, &app_font_12, COL_DIM);
    make_center(scr, 0, 296, SCREEN_W, "↑↓选择 · OK切换", &app_font_12, COL_DIM);

    page_switch(scr);
    refresh_settings();
    set_battery_labels();
}

static void leave_settings(void)
{
    s_app.page = PAGE_HOME;
    build_home();
    audio_player_play(SFX_CONFIRM);
}

static void handle_settings_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
        s_app.settings_index = (uint8_t)((s_app.settings_index + 3) % 4);
        audio_player_play(SFX_MOVE);
        refresh_settings();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
        s_app.settings_index = (uint8_t)((s_app.settings_index + 1) % 4);
        audio_player_play(SFX_MOVE);
        refresh_settings();
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        leave_settings();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        if (s_app.settings_index == 0) {
            s_app.low_age = !s_app.low_age;
        } else if (s_app.settings_index == 1) {
            s_app.reduce_motion = !s_app.reduce_motion;
        } else if (s_app.settings_index == 2) {
            s_app.muted = !s_app.muted;
            audio_player_set_muted(s_app.muted);
        } else {
            leave_settings();
            return;
        }
        request_save();
        audio_player_play(SFX_CONFIRM);
        refresh_settings();
    }
}

// ---------------------------------------------------------------------------
// 战斗界面
// ---------------------------------------------------------------------------
static void refresh_battle_texts(void)
{
    battle_t *battle = &s_app.battle;
    const stage_def_t *stage = battle_stage_def(battle);
    const int64_t now = esp_timer_get_time();

    char buf[80];
    snprintf(buf, sizeof(buf), "第%d关", (int)battle->stage + 1);
    label_set(s_app.battle_ui.stage_label, buf);
    label_set(s_app.battle_ui.monster_name, stage->monster_name);

    const uint16_t hero_hp = (uint16_t)((s_app.hero_hp_disp_x10 + 5) / 10);
    hearts_string(buf, sizeof(buf), hero_hp);
    label_set(s_app.battle_ui.hearts, buf);

    const int16_t monster_hp = (int16_t)((s_app.monster_hp_disp_x10 + 5) / 10);
    if (monster_hp != s_app.ui_monster_hp) {
        s_app.ui_monster_hp = monster_hp;
        lv_bar_set_value(s_app.battle_ui.monster_hp, monster_hp, LV_ANIM_OFF);
    }
    const int16_t gauge = (int16_t)((s_app.gauge_disp_x10 + 5) / 10);
    if (gauge != s_app.ui_gauge) {
        s_app.ui_gauge = gauge;
        lv_bar_set_value(s_app.battle_ui.gauge, gauge, LV_ANIM_OFF);
    }

    if (battle_can_ultimate(battle)) {
        label_set(s_app.battle_ui.gauge_text, "必杀就绪!");
        label_color(s_app.battle_ui.gauge_text, COL_GOLD);
    } else {
        snprintf(buf, sizeof(buf), "必杀 %d/%d", (int)gauge,
                 (int)BATTLE_GAUGE_MAX);
        label_set(s_app.battle_ui.gauge_text, buf);
        label_color(s_app.battle_ui.gauge_text, COL_DIM);
    }

    label_set(s_app.battle_ui.question, battle->question.text);
    if (battle_question_is_review(battle) &&
        battle->phase == BATTLE_PHASE_QUESTION) {
        label_color(s_app.battle_ui.question, COL_GOLD);
    } else {
        label_color(s_app.battle_ui.question, COL_TEXT);
    }

    const char *hint = "";
    switch (battle->phase) {
    case BATTLE_PHASE_QUESTION:
        if (s_app.notice_until_us > now) {
            hint = s_app.notice;
        } else if (battle->hint_used) {
            hint = battle->question.hint;
        } else if (battle_question_is_review(battle)) {
            hint = "复习题 · 答对额外加分";
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
        hint = battle->crit_bonus ? "速攻加成!金色区域更宽"
                                  : "看准金色区域,按下OK";
        break;
    case BATTLE_PHASE_ATTACK:
        if (now < s_app.impact_at_us && s_app.impact_kind != IMPACT_NONE) {
            hint = "出招!";
        } else {
            hint = s_app.hero_attack_missed ? "出手落空了…" : "漂亮的一击!";
        }
        break;
    case BATTLE_PHASE_ENEMY:
        hint = (now < s_app.impact_at_us && s_app.impact_kind == IMPACT_ENEMY)
                   ? "怪兽扑过来了!"
                   : "被怪兽反击了!";
        break;
    case BATTLE_PHASE_ULTIMATE: {
        const int step = battle_ultimate_step(battle);
        const int steps = battle_ultimate_steps(battle);
        const uint8_t input = battle_ultimate_input(battle);
        if (step >= steps) {
            hint = "必杀释放!";
        } else if (input == ULT_INPUT_UP) {
            hint = "看准金色区域,按 ↑ 接拍!";
        } else if (input == ULT_INPUT_DOWN) {
            hint = "看准金色区域,按 ↓ 接拍!";
        } else {
            hint = "看准金色区域,按OK接拍";
        }
        break;
    }
    case BATTLE_PHASE_GUARD:
        hint = battle_guard_dir(battle) == 0 ? "怪兽偷袭!按 ↑ 格挡"
                                             : "怪兽偷袭!按 ↓ 格挡";
        break;
    case BATTLE_PHASE_BEAM: {
        const int beats = (int)battle_beam_beats_left(battle);
        if (beats > 0) {
            snprintf(buf, sizeof(buf), "跟着节拍按OK · 还剩%d拍", beats);
            hint = buf;
        } else {
            hint = "光线对决结算!";
        }
        break;
    }
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
    const bool ready = s_app.ready_until_us > now;
    if (ready) {
        hint = "准备…按OK开始";
    }
    label_set(s_app.battle_ui.hint, hint);
    if (ready) {
        label_color(s_app.battle_ui.hint, COL_GOLD);
    } else if ((battle->phase == BATTLE_PHASE_QUESTION &&
                s_app.notice_until_us > now) ||
               battle->phase == BATTLE_PHASE_CRITICAL ||
               battle->phase == BATTLE_PHASE_ULTIMATE) {
        label_color(s_app.battle_ui.hint, COL_GOLD);
    } else if (battle->phase == BATTLE_PHASE_GUARD) {
        label_color(s_app.battle_ui.hint, COL_DANGER);
    } else if (battle->hint_used ||
               (battle->phase == BATTLE_PHASE_QUESTION &&
                battle_question_is_review(battle) &&
                s_app.notice_until_us <= now)) {
        label_color(s_app.battle_ui.hint, COL_ACCENT);
    } else {
        label_color(s_app.battle_ui.hint, COL_DIM);
    }

    const int16_t total = (int16_t)(battle->question_ms > 0
                                        ? battle->question_ms
                                        : 1);
    const int32_t remain = battle->question_remaining_ms;
    if (total != s_app.ui_timer_total) {
        s_app.ui_timer_total = total;
        lv_bar_set_range(s_app.battle_ui.timer, 0, total);
    }
    const int16_t timer_value = (int16_t)(remain > 0 ? remain : 0);
    if (timer_value != s_app.ui_timer_value) {
        s_app.ui_timer_value = timer_value;
        lv_bar_set_value(s_app.battle_ui.timer, timer_value, LV_ANIM_OFF);
    }
    uint32_t timer_color = COL_OK;
    if (remain * 3 < total) {
        timer_color = COL_DANGER;
    } else if (remain * 2 < total) {
        timer_color = COL_GOLD;
    }
    if (timer_color != s_app.ui_timer_color) {
        s_app.ui_timer_color = timer_color;
        lv_obj_set_style_bg_color(s_app.battle_ui.timer,
                                  lv_color_hex(timer_color),
                                  LV_PART_INDICATOR);
    }

    snprintf(buf, sizeof(buf), "得分 %d", (int)battle->score);
    label_set(s_app.battle_ui.score, buf);
    if (battle->combo >= 2) {
        snprintf(buf, sizeof(buf), "连击 x%d", (int)battle->combo);
        label_set(s_app.battle_ui.combo, buf);
        label_color(s_app.battle_ui.combo, COL_GOLD);
    } else {
        label_set(s_app.battle_ui.combo, "");
    }

    if (ready) {
        label_set(s_app.battle_ui.footer, "准备…按OK开始");
        label_color(s_app.battle_ui.footer, COL_GOLD);
    } else if (battle_can_ultimate(battle)) {
        label_set(s_app.battle_ui.footer, "必杀就绪!长按↑发动");
        label_color(s_app.battle_ui.footer, COL_GOLD);
    } else if (battle->phase == BATTLE_PHASE_GUARD) {
        label_set(s_app.battle_ui.footer, "按 ↑ / ↓ 格挡怪兽偷袭");
        label_color(s_app.battle_ui.footer, COL_DANGER);
    } else if (battle->phase == BATTLE_PHASE_ULTIMATE) {
        const uint8_t input = battle_ultimate_input(battle);
        if (battle_ultimate_step(battle) >= battle_ultimate_steps(battle)) {
            label_set(s_app.battle_ui.footer, "必杀释放!");
        } else if (input == ULT_INPUT_UP) {
            label_set(s_app.battle_ui.footer, "看准金色区域,按 ↑ 接拍");
        } else if (input == ULT_INPUT_DOWN) {
            label_set(s_app.battle_ui.footer, "看准金色区域,按 ↓ 接拍");
        } else {
            label_set(s_app.battle_ui.footer, "看准金色区域,按OK接拍");
        }
        label_color(s_app.battle_ui.footer, COL_GOLD);
    } else if (battle->phase == BATTLE_PHASE_BEAM) {
        label_set(s_app.battle_ui.footer, "跟着5拍节奏 · 按OK");
        label_color(s_app.battle_ui.footer, COL_DANGER);
    } else if (battle->phase == BATTLE_PHASE_CRITICAL) {
        label_set(s_app.battle_ui.footer, "看准金色区域,按下OK");
        label_color(s_app.battle_ui.footer, COL_GOLD);
    } else if (battle->phase == BATTLE_PHASE_ATTACK ||
               battle->phase == BATTLE_PHASE_ENEMY) {
        label_set(s_app.battle_ui.footer, "↑↓可预选下一题");
        label_color(s_app.battle_ui.footer, COL_ACCENT);
    } else {
        label_set(s_app.battle_ui.footer, "↑↓选 · OK答 · 长按OK暂停");
        label_color(s_app.battle_ui.footer, COL_DIM);
    }
}

static void refresh_battle_rows(void)
{
    const battle_t *battle = &s_app.battle;
    // Correct/wrong feedback belongs to the attack exchange only. Showing it
    // during QUESTION used to leave a green "correct" row on screen and made
    // it look like the cursor was parked on the right answer.
    const bool show_feedback = battle->phase == BATTLE_PHASE_ATTACK ||
                               battle->phase == BATTLE_PHASE_ENEMY;

    for (int i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        const bool selected = battle->phase == BATTLE_PHASE_QUESTION &&
                              battle->cursor == i;
        const bool correct = show_feedback && s_app.feedback_correct == i;
        const bool wrong = show_feedback && s_app.feedback_wrong == i;
        const uint8_t sig = (uint8_t)((selected ? 1u : 0u) |
                                      (correct ? 2u : 0u) |
                                      (wrong ? 4u : 0u));
        const int16_t value = (int16_t)battle->question.options[i];
        if (sig == s_app.row_sig[i] && value == s_app.row_value[i]) {
            continue;
        }
        s_app.row_sig[i] = sig;
        s_app.row_value[i] = value;

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)value);
        label_set(s_app.battle_ui.row_text[i], buf);

        uint32_t bg = COL_TILE;
        uint32_t fg = COL_TEXT;
        int border = 0;
        uint32_t border_color = COL_ACCENT;
        if (correct) {
            bg = 0x123B2A;
            fg = COL_OK;
            border = 2;
            border_color = COL_OK;
        } else if (wrong) {
            bg = 0x3A1520;
            fg = COL_DANGER;
            border = 2;
            border_color = COL_DANGER;
        } else if (selected) {
            bg = COL_TILE_SEL;
            fg = COL_GOLD;
            border = 2;
            border_color = COL_GOLD;
        }
        lv_obj_set_style_bg_color(s_app.battle_ui.row[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_width(s_app.battle_ui.row[i], border, 0);
        lv_obj_set_style_border_color(s_app.battle_ui.row[i],
                                      lv_color_hex(border_color), 0);
        label_color(s_app.battle_ui.row_text[i], fg);
        label_set(s_app.battle_ui.row_icon[i],
                  correct ? "对" : (wrong ? "错" : ""));
        label_color(s_app.battle_ui.row_icon[i],
                    correct ? COL_OK : (wrong ? COL_DANGER : COL_DIM));
        set_hidden(s_app.battle_ui.row_mark[i], !selected);
    }
}

static const char *ultimate_title(uint8_t hero)
{
    switch (hero) {
    case 0:
        return "赛罗 · 赛罗双射线";
    case 1:
        return "泰罗 · 斯特利姆光线";
    case 2:
        return "高斯 · 满月光波";
    case 3:
        return "奥特之王 · 王者闪光";
    case 4:
        return "迪迦 · 泽佩利敖光线";
    default:
        return "必杀技!";
    }
}

static void set_action_zone(int left_pct, int width_pct)
{
    int left = left_pct;
    if (left < 0) left = 0;
    int width = width_pct;
    if (left + width > 100) width = 100 - left;
    const int left_px = left * 200 / 100;
    const int width_px = width * 200 / 100;
    if (left_px != s_app.ui_crit_left || width_px != s_app.ui_crit_width) {
        s_app.ui_crit_left = (int16_t)left_px;
        s_app.ui_crit_width = (int16_t)width_px;
        lv_obj_set_pos(s_app.battle_ui.crit_zone, left_px, 2);
        lv_obj_set_size(s_app.battle_ui.crit_zone, width_px, 10);
        const int perfect_w = width_px / 2;
        lv_obj_set_pos(s_app.battle_ui.crit_perfect,
                       left_px + (width_px - perfect_w) / 2, 3);
        lv_obj_set_size(s_app.battle_ui.crit_perfect, perfect_w, 8);
    }
}

static void refresh_battle_action(void)
{
    battle_t *battle = &s_app.battle;

    if (battle->phase == BATTLE_PHASE_CRITICAL) {
        label_set(s_app.battle_ui.action_title, "暴击时刻!");
        label_color(s_app.battle_ui.action_title, COL_GOLD);
        label_set(s_app.battle_ui.action_sub,
                  battle->crit_bonus ? "速攻加成!金色区域更宽"
                                     : "金色区域按下确定");
        set_hidden(s_app.battle_ui.crit_track, false);
        set_hidden(s_app.battle_ui.crit_marker, false);
        set_hidden(s_app.battle_ui.beam_track, true);

        const int eff = battle->crit_window_effective > 0
                            ? battle->crit_window_effective
                            : battle->crit_window;
        const int half = eff / 2;
        set_action_zone((int)battle->crit_center - half, eff);
        lv_obj_set_pos(s_app.battle_ui.crit_marker,
                       14 + (int)battle->crit_marker * 200 / 100 - 2, 32);
        return;
    }

    if (battle->phase == BATTLE_PHASE_ULTIMATE) {
        const int steps = battle_ultimate_steps(battle);
        const int step = battle_ultimate_step(battle);
        label_set(s_app.battle_ui.action_title, ultimate_title(battle->hero));
        label_color(s_app.battle_ui.action_title, COL_ACCENT);
        if (step >= steps) {
            label_set(s_app.battle_ui.action_sub, "必杀释放!");
            set_hidden(s_app.battle_ui.crit_track, true);
            set_hidden(s_app.battle_ui.crit_marker, true);
            set_hidden(s_app.battle_ui.beam_track, true);
            return;
        }

        const uint8_t input = battle_ultimate_input(battle);
        const char *key = input == ULT_INPUT_UP ? "↑"
                          : input == ULT_INPUT_DOWN ? "↓"
                                                    : "OK";
        char sub[48];
        snprintf(sub, sizeof(sub), "第 %d/%d 拍 · 按%s", step + 1, steps, key);
        label_set(s_app.battle_ui.action_sub, sub);
        set_hidden(s_app.battle_ui.crit_track, false);
        set_hidden(s_app.battle_ui.crit_marker, false);
        set_hidden(s_app.battle_ui.beam_track, true);

        // The beat centre sits at 55% of the step; the gold band is the
        // forgiving hit window, the bright core is the perfect zone.
        set_action_zone(40, 30);
        const uint16_t step_ms = battle->ult.step_ms;
        int marker = 0;
        if (step_ms > 0) {
            marker = (int)((uint32_t)battle->ult.step_elapsed * 100u / step_ms);
        }
        if (marker > 100) marker = 100;
        lv_obj_set_pos(s_app.battle_ui.crit_marker,
                       14 + marker * 200 / 100 - 2, 32);
        return;
    }

    if (battle->phase == BATTLE_PHASE_GUARD) {
        label_set(s_app.battle_ui.action_title, "格挡!");
        label_color(s_app.battle_ui.action_title, COL_DANGER);
        label_set(s_app.battle_ui.action_sub,
                  battle_guard_dir(battle) == 0 ? "按 ↑ 挡住偷袭!"
                                               : "按 ↓ 挡住偷袭!");
        set_hidden(s_app.battle_ui.crit_track, true);
        set_hidden(s_app.battle_ui.crit_marker, true);
        set_hidden(s_app.battle_ui.beam_track, false);
        const uint16_t remaining = battle_guard_remaining_ms(battle);
        const int16_t fill =
            (int16_t)((uint32_t)remaining * 160u / BATTLE_GUARD_MS);
        if (fill != s_app.ui_beam_fill) {
            s_app.ui_beam_fill = fill;
            lv_obj_set_size(s_app.battle_ui.beam_fill, fill, 6);
        }
        return;
    }

    if (battle->phase == BATTLE_PHASE_BEAM) {
        label_set(s_app.battle_ui.action_title, "光线对决!");
        label_color(s_app.battle_ui.action_title, COL_DANGER);
        const int beats = (int)battle_beam_beats_left(battle);
        char sub[48];
        if (beats > 0) {
            snprintf(sub, sizeof(sub), "还剩 %d 拍 · 力度 %d", beats,
                     (int)battle_beam_force(battle));
        } else {
            snprintf(sub, sizeof(sub), "力度 %d",
                     (int)battle_beam_force(battle));
        }
        label_set(s_app.battle_ui.action_sub, sub);
        set_hidden(s_app.battle_ui.crit_track, true);
        set_hidden(s_app.battle_ui.crit_marker, true);
        set_hidden(s_app.battle_ui.beam_track, false);
        const int16_t fill = (int16_t)(battle_beam_force(battle) * 160 / 100);
        if (fill != s_app.ui_beam_fill) {
            s_app.ui_beam_fill = fill;
            lv_obj_set_size(s_app.battle_ui.beam_fill, fill, 6);
        }
        return;
    }
}

static void apply_poses(uint8_t hero_pose, uint8_t monster_pose)
{
    const battle_t *battle = &s_app.battle;
    if (hero_pose >= SPRITE_POSE_COUNT) {
        hero_pose = SPRITE_POSE_IDLE;
    }
    if (monster_pose >= SPRITE_MONSTER_COUNT) {
        monster_pose = SPRITE_MONSTER_IDLE;
    }
    if (s_app.previous_pose != (int8_t)hero_pose) {
        s_app.previous_pose = (int8_t)hero_pose;
        lv_image_set_src(s_app.battle_ui.hero_img,
                         sprites_hero(battle->hero, (sprite_pose_t)hero_pose));
    }
    if (s_app.previous_monster_pose != (int8_t)monster_pose) {
        s_app.previous_monster_pose = (int8_t)monster_pose;
        lv_image_set_src(s_app.battle_ui.monster_img,
                         sprites_monster(battle->stage,
                                         (sprite_monster_pose_t)monster_pose));
    }
}

static void spawn_popup(const char *text, uint32_t color, int x, int y)
{
    const uint8_t slot = s_app.popup_next;
    s_app.popup_next = (uint8_t)((s_app.popup_next + 1u) % POPUP_COUNT);
    lv_obj_t *label = s_app.battle_ui.popup[slot];
    if (label == NULL) {
        return;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_pos(label, x, y);
    set_hidden(label, false);
    dmg_popup_t *popup = &s_app.popup[slot];
    popup->active = true;
    popup->start_us = esp_timer_get_time();
    popup->x = (int16_t)x;
    popup->y = (int16_t)y;
    popup->y_last = (int16_t)y;
}

static void refresh_popups(int64_t now)
{
    for (int i = 0; i < POPUP_COUNT; i++) {
        dmg_popup_t *popup = &s_app.popup[i];
        if (!popup->active) {
            continue;
        }
        const int64_t age = now - popup->start_us;
        lv_obj_t *label = s_app.battle_ui.popup[i];
        if (age >= POPUP_LIFE_US) {
            popup->active = false;
            set_hidden(label, true);
            continue;
        }
        const int rise = (int)(age * 26 / 1000);
        const int16_t y = (int16_t)(popup->y - rise);
        if (y != popup->y_last) {
            popup->y_last = y;
            lv_obj_set_pos(label, popup->x, y);
        }
        const int opa = 255 - (int)(age * 255 / POPUP_LIFE_US);
        lv_obj_set_style_text_opa(label, (lv_opa_t)opa, 0);
    }
}

static int32_t approach(int32_t current, int32_t goal, int32_t step)
{
    if (current < goal) {
        return (current + step > goal) ? goal : current + step;
    }
    if (current > goal) {
        return (current - step < goal) ? goal : current - step;
    }
    return current;
}

static void tween_bars(uint32_t dt_ms)
{
    if (dt_ms == 0) {
        dt_ms = TICK_MS;
    }
    int32_t hp_step = (int32_t)(HP_TWEEN_X10_STEP * dt_ms / TICK_MS);
    if (hp_step < 1) {
        hp_step = 1;
    }
    s_app.monster_hp_disp_x10 =
        approach(s_app.monster_hp_disp_x10,
                 (int32_t)s_app.monster_hp_goal * 10, hp_step);
    s_app.hero_hp_disp_x10 =
        approach(s_app.hero_hp_disp_x10, (int32_t)s_app.hero_hp_goal * 10,
                 hp_step);

    int32_t gauge_step = (int32_t)(GAUGE_TWEEN_X10_STEP * dt_ms / TICK_MS);
    if (gauge_step < 1) {
        gauge_step = 1;
    }
    s_app.gauge_disp_x10 =
        approach(s_app.gauge_disp_x10, (int32_t)s_app.battle.gauge * 10,
                 gauge_step);
}

static void update_panel(uint32_t dt_ms)
{
    const uint8_t phase = s_app.battle.phase;
    const bool want_hidden = !(phase == BATTLE_PHASE_CRITICAL ||
                               phase == BATTLE_PHASE_GUARD ||
                               phase == BATTLE_PHASE_ULTIMATE ||
                               phase == BATTLE_PHASE_BEAM);
    s_app.action_target_hidden = want_hidden;
    const int16_t target = want_hidden ? ACTION_H : 0;
    if (s_app.action_offset == target) {
        set_hidden(s_app.battle_ui.action, want_hidden);
        return;
    }
    set_hidden(s_app.battle_ui.action, false);
    if (dt_ms == 0) {
        return;
    }
    const int step =
        ((ACTION_H * (int)dt_ms) + ACTION_ANIM_MS - 1) / ACTION_ANIM_MS;
    if (s_app.action_offset < target) {
        s_app.action_offset = (int16_t)(s_app.action_offset + step > target
                                            ? target
                                            : s_app.action_offset + step);
    } else {
        s_app.action_offset = (int16_t)(s_app.action_offset - step < target
                                            ? target
                                            : s_app.action_offset - step);
    }
    lv_obj_set_pos(s_app.battle_ui.action, ACTION_X,
                   ACTION_Y + s_app.action_offset);
}

// Pick the strike that matches the current combo state. Every ordinary hit
// alternates between a flying kick and a flying punch, and the starting move
// differs per hero, so repeated answers do not replay one animation. Perfect
// timing upgrades the strike to a charged heavy kick; combo rewards keep their
// dedicated pursuit and finisher moves.
static anim_id_t hero_attack_anim(const battle_t *battle)
{
    if (battle->combo_finisher) {
        return ANIM_COMBO_FINISH;
    }
    if (battle->crit_pursuit) {
        return ANIM_HERO_PURSUIT;
    }
    if (battle->crit_perfect) {
        return ANIM_HERO_ATTACK_HEAVY;
    }
    return ((battle->correct_count + battle->hero) & 1u) != 0
               ? ANIM_HERO_ATTACK_PUNCH
               : ANIM_HERO_ATTACK;
}

// Enemy attacks also alternate: a direct roar-pounce or a charge-up pounce.
static anim_id_t enemy_attack_anim(const battle_t *battle)
{
    return ((battle->wrong_count + battle->stage) & 1u) != 0
               ? ANIM_ENEMY_ATTACK_CHARGE
               : ANIM_ENEMY_ATTACK;
}

static void schedule_impact(impact_kind_t kind, int damage)
{
    anim_id_t id = hero_attack_anim(&s_app.battle);
    uint32_t duration = BATTLE_ATTACK_MS;
    uint32_t delay_ms = 0;

    if (kind == IMPACT_ENEMY) {
        id = enemy_attack_anim(&s_app.battle);
        duration = BATTLE_ENEMY_MS;
    } else if (kind == IMPACT_ULT_STEP) {
        // Ultimate hits are judged on the input, so the visual reaction only
        // needs a short cause-and-effect beat.
        delay_ms = 180;
    } else if (kind == IMPACT_GUARD_BLOCK) {
        delay_ms = 120;
    }

    if (delay_ms == 0) {
        delay_ms = anim_impact_ms(id, duration);
    }
    s_app.impact_kind = (uint8_t)kind;
    s_app.impact_damage = damage;
    s_app.impact_at_us = esp_timer_get_time() + (int64_t)delay_ms * 1000;
}

static void update_impact(int64_t now)
{
    if (s_app.impact_kind == IMPACT_NONE || now < s_app.impact_at_us) {
        return;
    }
    battle_t *battle = &s_app.battle;
    const impact_kind_t kind = (impact_kind_t)s_app.impact_kind;
    s_app.impact_kind = IMPACT_NONE;
    int damage = (int)s_app.impact_damage;
    if (damage < 0) {
        damage = 0;
    }

    char text[16];
    if (kind == IMPACT_ENEMY) {
        s_app.hero_hp_goal = battle->hero_hp;
        if (damage > 0) {
            snprintf(text, sizeof(text), "-%d", damage);
            spawn_popup(text, COL_DANGER, HERO_X - ARENA_X + 13, 34);
            audio_player_play(SFX_HURT);
        } else {
            spawn_popup("护盾", COL_ACCENT, HERO_X - ARENA_X + 10, 34);
        }
        s_app.arena_flash_until_us = now + 110000;
        return;
    }

    if (kind == IMPACT_HERO_MISS) {
        spawn_popup("MISS", COL_DIM, MONSTER_X - ARENA_X + 10, 30);
        return;
    }

    if (kind == IMPACT_GUARD_BLOCK) {
        s_app.monster_hp_goal = battle->monster_hp;
        spawn_popup("格挡!", COL_ACCENT, HERO_X - ARENA_X + 8, 30);
        audio_player_play(SFX_CRITICAL);
        s_app.arena_flash_until_us = now + 90000;
        return;
    }

    s_app.monster_hp_goal = battle->monster_hp;
    snprintf(text, sizeof(text), "-%d", damage > 0 ? damage : 1);
    if (kind == IMPACT_ULT_STEP) {
        spawn_popup(text, COL_ACCENT, MONSTER_X - ARENA_X + 16, 26);
        audio_player_play(SFX_HIT);
        s_app.arena_flash_until_us = now + 120000;
    } else {
        const bool perfect = kind == IMPACT_HERO_PERFECT ||
                             kind == IMPACT_COMBO_FINISH;
        spawn_popup(text, perfect ? COL_GOLD : COL_TEXT,
                    MONSTER_X - ARENA_X + 16, 26);
        audio_player_play(SFX_HIT);
        if (kind == IMPACT_COMBO_FINISH) {
            audio_player_play(SFX_CRITICAL);
        }
        if (perfect) {
            // Keep the praise brief so the next question's own hint returns
            // before the player needs it.
            set_notice_ms("完美暴击!", 700000LL);
        }
        s_app.arena_flash_until_us =
            now + (kind == IMPACT_COMBO_FINISH ? 140000 : 90000);
    }
}

static void refresh_battle_animation(void)
{
    battle_t *battle = &s_app.battle;
    const int64_t now = esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now / 1000);

    anim_id_t id = ANIM_IDLE;
    uint32_t duration = 1000;
    uint32_t elapsed = 0;
    bool idle_breath = false;
    bool guard_pose = false;

    switch (battle->phase) {
    case BATTLE_PHASE_ATTACK:
        id = hero_attack_anim(battle);
        duration = BATTLE_ATTACK_MS;
        elapsed = (uint32_t)(BATTLE_ATTACK_MS - battle->phase_ms);
        break;
    case BATTLE_PHASE_ENEMY:
        id = enemy_attack_anim(battle);
        duration = BATTLE_ENEMY_MS;
        elapsed = (uint32_t)(BATTLE_ENEMY_MS - battle->phase_ms);
        break;
    case BATTLE_PHASE_GUARD:
        guard_pose = true;
        break;
    case BATTLE_PHASE_ULTIMATE: {
        const uint32_t total = battle_ultimate_total_ms(&s_app.battle);
        const uint32_t spent = total > (uint32_t)battle->phase_ms
                                   ? total - (uint32_t)battle->phase_ms
                                   : 0;
        if (battle_ultimate_step(battle) >= battle_ultimate_steps(battle)) {
            id = ANIM_ULTIMATE_FIRE;
            duration = 500;
            elapsed = spent > duration ? duration : spent;
        } else {
            id = ANIM_ULTIMATE_CHARGE;
            duration = total;
            elapsed = spent;
        }
        break;
    }
    case BATTLE_PHASE_BEAM:
        id = ANIM_BEAM;
        duration = BATTLE_BEAM_MS;
        elapsed = (uint32_t)(BATTLE_BEAM_MS - battle->phase_ms);
        break;
    case BATTLE_PHASE_CLEAR:
        id = ANIM_CLEAR;
        duration = BATTLE_CLEAR_MS;
        elapsed = (uint32_t)(BATTLE_CLEAR_MS - battle->phase_ms);
        break;
    case BATTLE_PHASE_OVER:
        id = ANIM_OVER;
        duration = 1000;
        elapsed = 500;
        break;
    default:
        idle_breath = true;
        break;
    }

    anim_frame_t frame = anim_sample(id, elapsed, duration);
    if (s_app.reduce_motion) {
        // Keep the fighters readable: no breathing, flashes or screen shake.
        idle_breath = false;
        frame.hero_flash = 0;
        frame.monster_flash = 0;
        frame.shake = 0;
    }
    if (guard_pose) {
        frame.hero_pose = ANIM_POSE_GUARD;
        frame.monster_pose = ANIM_MONSTER_CHARGE;
        frame.monster_dy = 0;
    }
    if (battle->phase == BATTLE_PHASE_ATTACK && s_app.hero_attack_missed) {
        // A whiff must not knock the monster back or make it flash.
        frame.monster_pose = ANIM_MONSTER_IDLE;
        frame.monster_dx = 0;
        frame.monster_dy = 0;
        frame.monster_flash = 0;
    }

    int hx = HERO_X + frame.hero_dx;
    int hy = HERO_Y + frame.hero_dy;
    int mx = MONSTER_X + frame.monster_dx;
    int my = MONSTER_Y + frame.monster_dy;
    if (idle_breath) {
        // Desynchronised two-step breathing instead of a 1 px square wave.
        hy += anim_breathe(now_ms, 0, 2400);
        my += anim_breathe(now_ms, 1200, 2600);
    }

    apply_poses(frame.hero_pose, frame.monster_pose);

    if (hx != s_app.ui_hero_x || hy != s_app.ui_hero_y) {
        s_app.ui_hero_x = (int16_t)hx;
        s_app.ui_hero_y = (int16_t)hy;
        lv_obj_set_pos(s_app.battle_ui.hero_img, hx - ARENA_X, hy - ARENA_Y);
    }
    if (mx != s_app.ui_monster_x || my != s_app.ui_monster_y) {
        s_app.ui_monster_x = (int16_t)mx;
        s_app.ui_monster_y = (int16_t)my;
        lv_obj_set_pos(s_app.battle_ui.monster_img, mx - ARENA_X, my - ARENA_Y);
    }

    if (frame.hero_flash != s_app.ui_hero_flash) {
        s_app.ui_hero_flash = frame.hero_flash;
        lv_obj_set_style_image_recolor_opa(
            s_app.battle_ui.hero_img,
            (lv_opa_t)((uint32_t)frame.hero_flash * 255u / 100u), 0);
    }
    if (frame.monster_flash != s_app.ui_monster_flash) {
        s_app.ui_monster_flash = frame.monster_flash;
        lv_obj_set_style_image_recolor_opa(
            s_app.battle_ui.monster_img,
            (lv_opa_t)((uint32_t)frame.monster_flash * 255u / 100u), 0);
    }

    int8_t shake_x = 0;
    int8_t shake_y = 0;
    if (frame.shake > 0) {
        const uint32_t frame_index = now_ms / TICK_MS;
        const uint32_t hash = frame_index * 2654435761u;
        const uint32_t span = (uint32_t)frame.shake * 2u + 1u;
        shake_x = (int8_t)((int)((hash >> 16) % span) - frame.shake);
        shake_y = (int8_t)((int)((hash >> 8) % span) - frame.shake);
    }
    if (shake_x != s_app.ui_shake_x || shake_y != s_app.ui_shake_y) {
        s_app.ui_shake_x = shake_x;
        s_app.ui_shake_y = shake_y;
        lv_obj_set_pos(s_app.battle_ui.stage, shake_x, shake_y);
    }

    const bool flash_on = !s_app.reduce_motion && now < s_app.arena_flash_until_us;
    if (flash_on != s_app.ui_flash_visible) {
        s_app.ui_flash_visible = flash_on;
        lv_obj_set_style_bg_opa(s_app.battle_ui.flash,
                                flash_on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
    }

    const bool beam = frame.beam > 0;
    if (beam != s_app.ui_beam_visible) {
        s_app.ui_beam_visible = beam;
        set_hidden(s_app.battle_ui.beam_glow, !beam);
        set_hidden(s_app.battle_ui.beam_core, !beam);
    }
    if (beam) {
        const int force = battle->phase == BATTLE_PHASE_BEAM
                              ? battle_beam_force(battle)
                              : frame.beam;
        const int glow_h = 8 + force * 12 / 100;
        const int core_h = 3 + force * 8 / 100;
        const int x0 = hx + 54;
        int width = mx - x0;
        if (width < 4) {
            width = 4;
        }
        const int cy = 66;
        lv_obj_set_pos(s_app.battle_ui.beam_glow, x0 - ARENA_X,
                       cy - glow_h / 2 - ARENA_Y);
        lv_obj_set_size(s_app.battle_ui.beam_glow, width, glow_h);
        lv_obj_set_pos(s_app.battle_ui.beam_core, x0 - ARENA_X,
                       cy - core_h / 2 - ARENA_Y);
        lv_obj_set_size(s_app.battle_ui.beam_core, width, core_h);
    }
}

static void refresh_battle_with_dt(uint32_t dt_ms)
{
    if (s_app.page != PAGE_BATTLE || s_app.root == NULL) return;
    const int64_t now = esp_timer_get_time();
    update_impact(now);
    tween_bars(dt_ms);
    update_panel(dt_ms);
    refresh_battle_texts();
    refresh_battle_rows();
    refresh_battle_action();
    refresh_battle_animation();
    refresh_popups(now);
    set_battery_labels();
}

static void refresh_battle(void)
{
    refresh_battle_with_dt(TICK_MS);
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
    lv_obj_add_flag(ui->arena, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Inner layer so the screen shake moves the fighters but not the HP bar,
    // hearts or flash overlay.
    ui->stage = lv_obj_create(ui->arena);
    lv_obj_remove_flag(ui->stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ui->stage, 0, 0);
    lv_obj_set_size(ui->stage, ARENA_W, ARENA_H);
    lv_obj_set_style_bg_opa(ui->stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->stage, 0, 0);
    lv_obj_set_style_pad_all(ui->stage, 0, 0);
    lv_obj_add_flag(ui->stage, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    ui->hero_img = lv_image_create(ui->stage);
    lv_obj_set_pos(ui->hero_img, HERO_X - ARENA_X, HERO_Y - ARENA_Y);
    lv_obj_set_size(ui->hero_img, 72, 88);
    lv_obj_set_style_image_recolor(ui->hero_img, lv_color_hex(COL_DANGER), 0);
    lv_obj_set_style_image_recolor_opa(ui->hero_img, LV_OPA_TRANSP, 0);

    ui->monster_img = lv_image_create(ui->stage);
    lv_obj_set_pos(ui->monster_img, MONSTER_X - ARENA_X, MONSTER_Y - ARENA_Y);
    lv_obj_set_size(ui->monster_img, 80, 80);
    lv_obj_set_style_image_recolor(ui->monster_img, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(ui->monster_img, LV_OPA_TRANSP, 0);

    ui->beam_glow = make_panel(ui->stage, 80, 60, 50, 16, COL_ACCENT, 6);
    lv_obj_set_style_bg_opa(ui->beam_glow, LV_OPA_50, 0);
    ui->beam_core = make_panel(ui->stage, 80, 63, 50, 8, COL_TEXT, 4);
    set_hidden(ui->beam_glow, true);
    set_hidden(ui->beam_core, true);

    // Impact flash sits above the fighters but below the HP bar and hearts, so
    // the arena flashes without hiding the information the player needs.
    ui->flash = make_panel(ui->arena, 0, 0, ARENA_W, ARENA_H, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(ui->flash, LV_OPA_TRANSP, 0);

    ui->monster_hp = make_bar(ui->arena, MONSTER_X - ARENA_X, 84, 88, 6,
                              COL_EDGE, COL_DANGER);
    lv_obj_set_style_border_width(ui->monster_hp, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->monster_hp, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_bar_set_range(ui->monster_hp, 0,
                     s_app.battle.monster_hp_max ? s_app.battle.monster_hp_max : 1);

    ui->hearts = make_label(ui->arena, 4, 90, 84, "", &app_font_12, COL_DANGER);
    ui->gauge = make_bar(scr, 6, 134, 166, 8, COL_EDGE, COL_ACCENT);
    lv_bar_set_range(ui->gauge, 0, BATTLE_GAUGE_MAX);
    ui->gauge_text = make_label(scr, 178, 131, 56, "", &app_font_12, COL_DIM);

    ui->question = make_center(scr, 0, 146, SCREEN_W, "", &app_font_20, COL_TEXT);
    ui->hint = make_center(scr, 0, 168, SCREEN_W, "", &app_font_14, COL_DIM);
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
        // A word, not just a color, so feedback still reads without color.
        ui->row_icon[i] = make_label(ui->row[i], 192, 5, 30, "", &app_font_12,
                                     COL_OK);
        lv_obj_set_style_text_align(ui->row_icon[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    ui->footer = make_center(scr, 0, 296, SCREEN_W, "", &app_font_12, COL_DIM);

    // 暴击/必杀提示区,盖住答题行。
    ui->action = make_panel(scr, ACTION_X, ACTION_Y + ACTION_H, ACTION_W,
                            ACTION_H, COL_PANEL, 10);
    lv_obj_add_flag(ui->action, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_border_width(ui->action, 1, 0);
    lv_obj_set_style_border_color(ui->action, lv_color_hex(COL_EDGE), 0);
    set_hidden(ui->action, true);
    ui->action_title = make_center(ui->action, 0, 6, ACTION_W, "", &app_font_20,
                                   COL_GOLD);
    ui->action_sub = make_center(ui->action, 0, 64, ACTION_W, "", &app_font_12,
                                 COL_DIM);

    ui->crit_track = make_panel(ui->action, 14, 38, 200, 14, COL_TILE, 7);
    lv_obj_add_flag(ui->crit_track, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    ui->crit_zone = make_panel(ui->crit_track, 90, 2, 20, 10, COL_GOLD, 5);
    ui->crit_perfect = make_panel(ui->crit_track, 94, 3, 10, 8, 0xFFF3B0, 4);
    ui->crit_marker = make_panel(ui->action, 112, 32, 4, 22, COL_TEXT, 2);

    ui->beam_track = make_panel(ui->action, 34, 40, 160, 10, COL_TILE, 5);
    ui->beam_fill = make_panel(ui->beam_track, 0, 2, 0, 6, COL_DANGER, 3);

    for (int i = 0; i < POPUP_COUNT; i++) {
        ui->popup[i] = make_center(ui->arena, 0, 0, 54, "", &app_font_20,
                                   COL_TEXT);
        lv_obj_set_style_text_opa(ui->popup[i], LV_OPA_COVER, 0);
        set_hidden(ui->popup[i], true);
    }

    page_switch(scr);
    s_app.previous_pose = -1;
    s_app.previous_monster_pose = -1;
    s_app.feedback_correct = 0xFF;
    s_app.feedback_wrong = 0xFF;
    s_app.notice[0] = '\0';
    s_app.notice_until_us = 0;
    s_app.last_tick_us = esp_timer_get_time();

    s_app.monster_hp_seen = s_app.battle.monster_hp;
    s_app.hero_hp_seen = s_app.battle.hero_hp;
    s_app.monster_hp_goal = s_app.battle.monster_hp;
    s_app.hero_hp_goal = s_app.battle.hero_hp;
    s_app.monster_hp_disp_x10 = (int32_t)s_app.battle.monster_hp * 10;
    s_app.hero_hp_disp_x10 = (int32_t)s_app.battle.hero_hp * 10;
    s_app.gauge_disp_x10 = (int32_t)s_app.battle.gauge * 10;
    s_app.impact_kind = IMPACT_NONE;
    s_app.impact_at_us = 0;
    s_app.impact_damage = 0;
    s_app.hero_attack_missed = false;
    s_app.buffered_move = 0;
    s_app.prev_phase = s_app.battle.phase;
    s_app.action_offset = ACTION_H;
    s_app.action_target_hidden = true;
    lv_obj_set_pos(s_app.battle_ui.action, ACTION_X, ACTION_Y + ACTION_H);
    memset(s_app.popup, 0, sizeof(s_app.popup));
    s_app.popup_next = 0;

    s_app.ui_monster_hp = -1;
    s_app.ui_gauge = -1;
    s_app.ui_timer_value = -1;
    s_app.ui_timer_total = -1;
    s_app.ui_timer_color = 0xFFFFFFFFu;
    memset(s_app.row_sig, 0xFF, sizeof(s_app.row_sig));
    memset(s_app.row_value, 0xFF, sizeof(s_app.row_value));
    s_app.ui_hero_x = -32768;
    s_app.ui_hero_y = -32768;
    s_app.ui_monster_x = -32768;
    s_app.ui_monster_y = -32768;
    s_app.ui_hero_flash = 0xFF;
    s_app.ui_monster_flash = 0xFF;
    s_app.ui_shake_x = 0;
    s_app.ui_shake_y = 0;
    s_app.ui_beam_visible = false;
    s_app.ui_flash_visible = false;
    s_app.ui_crit_left = -1;
    s_app.ui_crit_width = -1;
    s_app.ui_beam_fill = -1;

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

// Result overlays append "错题回顾" after their normal actions, so the item
// list stays stable while the review pool is empty.
static uint8_t overlay_base_items(void)
{
    switch (s_app.overlay) {
    case OVERLAY_PAUSE:
        return 3;
    case OVERLAY_CLEAR:
        if (s_app.mode != MODE_CAMPAIGN) {
            return 2;
        }
        return s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT ? 2 : 3;
    case OVERLAY_OVER:
    case OVERLAY_WAVE:
        return 2;
    default:
        return 0;
    }
}

static bool overlay_can_review(void)
{
    return s_app.overlay != OVERLAY_PAUSE &&
           s_app.overlay != OVERLAY_WAVE &&
           s_app.review_view_count > 0;
}

static const char *overlay_item_text(uint8_t index)
{
    if (overlay_can_review() && index == overlay_base_items()) {
        return "错题回顾";
    }
    if (s_app.overlay == OVERLAY_PAUSE) {
        static const char *const items[] = { "继续", "重来", "回主页" };
        return index < 3 ? items[index] : "";
    }
    if (s_app.overlay == OVERLAY_CLEAR) {
        if (s_app.mode != MODE_CAMPAIGN) {
            static const char *const items[] = { "再来一局", "回主页" };
            return index < 2 ? items[index] : "";
        }
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
    if (s_app.overlay == OVERLAY_WAVE) {
        static const char *const items[] = { "继续", "收手" };
        return index < 2 ? items[index] : "";
    }
    return "";
}

static uint8_t overlay_item_count(void)
{
    if (s_app.overlay == OVERLAY_REVIEW) {
        return 1;
    }
    uint8_t count = overlay_base_items();
    if (overlay_can_review()) {
        count++;
    }
    return count;
}

static void build_review_overlay(void)
{
    s_app.overlay_root = make_panel(s_app.root, 0, 0, SCREEN_W, SCREEN_H,
                                   0x000000, 0);
    lv_obj_set_style_bg_opa(s_app.overlay_root, LV_OPA_70, 0);

    const uint8_t pages =
        (uint8_t)((s_app.review_view_count + REVIEW_PAGE_SIZE - 1) /
                  REVIEW_PAGE_SIZE);
    if (pages == 0) {
        s_app.review_page = 0;
    } else if (s_app.review_page >= pages) {
        s_app.review_page = (uint8_t)(pages - 1);
    }

    lv_obj_t *card = make_panel(s_app.overlay_root, 14, 30, 212, 260,
                                COL_PANEL, 12);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_EDGE), 0);
    make_center(card, 0, 8, 212, "错题回顾", &app_font_20, COL_GOLD);

    char buf[64];
    snprintf(buf, sizeof(buf), "第 %d / %d 页",
             (int)s_app.review_page + 1, (int)pages);
    make_center(card, 0, 34, 212, buf, &app_font_12, COL_DIM);

    const uint8_t first = (uint8_t)(s_app.review_page * REVIEW_PAGE_SIZE);
    for (uint8_t i = 0; i < REVIEW_PAGE_SIZE; i++) {
        const uint8_t slot = (uint8_t)(first + i);
        const int y = 56 + i * 90;
        lv_obj_t *row = make_panel(card, 8, y, 196, 84, COL_TILE, 8);
        if (slot >= s_app.review_view_count) {
            make_center(row, 0, 30, 196, "没有更多错题", &app_font_12, COL_DIM);
            continue;
        }
        const review_record_t *record = &s_app.review_view[slot];
        const int16_t answer = record->question.options[record->question.correct_index];
        snprintf(buf, sizeof(buf), "%s 答:%d", record->question.text,
                 (int)answer);
        make_label(row, 8, 6, 180, buf, &app_font_12, COL_TEXT);
        make_label(row, 8, 28, 180,
                   record->question.hint[0] ? record->question.hint : "别急,再做一遍",
                   &app_font_12, COL_DIM);
        snprintf(buf, sizeof(buf), "错 %d 次", (int)record->mistakes);
        lv_obj_t *count = make_label(row, 8, 60, 180, buf, &app_font_12,
                                     COL_ACCENT);
        lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);
    }

    make_center(card, 0, 238, 212, "↑↓翻页 · OK返回", &app_font_12, COL_DIM);
}

static void rebuild_overlay(void)
{
    if (s_app.overlay_root != NULL) {
        lv_obj_delete(s_app.overlay_root);
        s_app.overlay_root = NULL;
    }
    if (s_app.overlay == OVERLAY_REVIEW) {
        build_review_overlay();
        return;
    }

    const uint8_t items = overlay_item_count();
    int item_y = 60;
    int card_h = item_y + (int)items * 32 + 6;
    if (s_app.overlay == OVERLAY_CLEAR) {
        item_y = 112;
        card_h = item_y + (int)items * 32 + 6;
    } else if (s_app.overlay == OVERLAY_WAVE) {
        item_y = 96;
        card_h = item_y + (int)items * 32 + 6;
    } else if (s_app.overlay == OVERLAY_OVER &&
               s_app.mode != MODE_CAMPAIGN) {
        item_y = 76;
        card_h = item_y + (int)items * 32 + 6;
    }

    s_app.overlay_root = make_panel(s_app.root, 0, 0, SCREEN_W, SCREEN_H,
                                   0x000000, 0);
    lv_obj_set_style_bg_opa(s_app.overlay_root, LV_OPA_70, 0);

    const int card_y = (SCREEN_H - card_h) / 2;
    lv_obj_t *card = make_panel(s_app.overlay_root, 14, card_y, 212, card_h,
                                COL_PANEL, 12);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_EDGE), 0);

    char wave_title[24];
    const char *title = "";
    const char *sub = "";
    if (s_app.overlay == OVERLAY_PAUSE) {
        title = "暂停";
        sub = "休息一下,继续战斗";
    } else if (s_app.overlay == OVERLAY_WAVE) {
        snprintf(wave_title, sizeof(wave_title), "第 %u 战胜利",
                 (unsigned)s_app.wave);
        title = wave_title;
        sub = "继续冒险,还是见好就收?";
    } else if (s_app.overlay == OVERLAY_CLEAR) {
        if (s_app.mode == MODE_ENDLESS) {
            title = "挑战结束";
            sub = "见好就收,分数已结算";
        } else if (s_app.mode == MODE_GAUNTLET) {
            title = "三连战达成!";
            sub = "三名强敌全部击破";
        } else if (s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT) {
            title = "全员通关!";
            sub = "所有关卡都拿下了";
        } else {
            title = "关卡完成";
            sub = "怪兽被击败了!";
        }
    } else {
        title = "战败";
        if (s_app.mode == MODE_ENDLESS) {
            sub = "无尽挑战结束了";
        } else if (s_app.mode == MODE_GAUNTLET) {
            sub = "三连战中断了";
        } else {
            sub = "怪兽太强了,再来一次!";
        }
    }
    make_center(card, 0, 10, 212, title, &app_font_20,
                s_app.overlay == OVERLAY_OVER ? COL_DANGER : COL_GOLD);

    char line[64];
    if (s_app.overlay == OVERLAY_WAVE) {
        snprintf(line, sizeof(line), "得分 %d · 生命 %d/%d",
                 (int)s_app.run_score, (int)s_app.run_hp,
                 (int)s_app.battle.hero_hp_max);
        make_center(card, 0, 56, 212, line, &app_font_12, COL_TEXT);
        const unsigned next_bonus =
            s_app.run_bonus_pct < 100 ? s_app.run_bonus_pct + 10u : 100u;
        snprintf(line, sizeof(line), "继续:回1心 · 得分 +%u%%", next_bonus);
        make_center(card, 0, 74, 212, line, &app_font_12, COL_ACCENT);
    } else if (s_app.overlay == OVERLAY_CLEAR) {
        if (s_app.mode == MODE_ENDLESS) {
            snprintf(line, sizeof(line), "本次得分 %d", (int)s_app.battle.score);
            make_center(card, 0, 40, 212, line, &app_font_20, COL_GOLD);
            snprintf(line, sizeof(line), "打到第 %u 战 · 最高 %u",
                     (unsigned)s_app.wave, (unsigned)s_app.endless_best);
            make_center(card, 0, 70, 212, line, &app_font_12, COL_TEXT);
            make_center(card, 0, 86, 212, "继续挑战更高分", &app_font_12, COL_DIM);
        } else {
            if (s_app.mode == MODE_GAUNTLET) {
                make_center(card, 0, 40, 212, "★ ★ ★ 三连胜", &app_font_20,
                            COL_GOLD);
            } else {
                char stars[16];
                stars_string(stars, sizeof(stars), s_app.battle.stars);
                make_center(card, 0, 40, 212, stars, &app_font_20, COL_GOLD);
            }
            snprintf(line, sizeof(line), "本次得分 %d",
                     (int)s_app.battle.score);
            make_center(card, 0, 70, 212, line, &app_font_12, COL_TEXT);
            if (s_app.mode == MODE_GAUNTLET) {
                snprintf(line, sizeof(line), "最高记录 %u",
                         (unsigned)s_app.gauntlet_best);
            } else {
                snprintf(line, sizeof(line), "本关最高 %u",
                         (unsigned)s_app.best[s_app.battle.stage]);
            }
            make_center(card, 0, 86, 212, line, &app_font_12, COL_DIM);
        }
    } else {
        make_center(card, 0, 40, 212, sub, &app_font_12, COL_DIM);
        if (s_app.overlay == OVERLAY_OVER && s_app.mode != MODE_CAMPAIGN) {
            if (s_app.mode == MODE_ENDLESS) {
                snprintf(line, sizeof(line), "打到第 %u 战 · 得分 %d",
                         (unsigned)s_app.wave, (int)s_app.battle.score);
            } else {
                snprintf(line, sizeof(line), "第 %u 战 · 得分 %d",
                         (unsigned)s_app.wave, (int)s_app.battle.score);
            }
            make_center(card, 0, 56, 212, line, &app_font_12, COL_TEXT);
        }
    }

    s_app.overlay_items = items;
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

static void open_review(void)
{
    if (s_app.review_view_count == 0) {
        return;
    }
    s_app.review_return_overlay = s_app.overlay;
    s_app.review_return_index = s_app.overlay_index;
    s_app.review_page = 0;
    s_app.overlay = OVERLAY_REVIEW;
    rebuild_overlay();
}

static void close_review(void)
{
    s_app.overlay = s_app.review_return_overlay;
    s_app.overlay_index = s_app.review_return_index;
    rebuild_overlay();
}

// ---------------------------------------------------------------------------
// 战斗流程
// ---------------------------------------------------------------------------
static void begin_battle(uint8_t stage, uint16_t carry_hp, int32_t carry_score)
{
    if (stage >= CAMPAIGN_STAGE_COUNT) {
        stage = 0;
    }
    s_app.stage = stage;
    battle_start(&s_app.battle, stage, s_app.difficulty, s_app.hero,
                 (uint32_t)esp_timer_get_time() ^ 0x5EED1234u);
    battle_set_low_age(&s_app.battle, s_app.low_age);
    battle_set_score_bonus(&s_app.battle,
                           s_app.mode == MODE_ENDLESS ? s_app.run_bonus_pct : 0);
    if (carry_hp > 0 && carry_hp < s_app.battle.hero_hp) {
        s_app.battle.hero_hp = carry_hp;
    }
    s_app.battle.score = carry_score;

    // Wrong answers come back as the 2nd and 5th question of the round. Only
    // questions at or below this stage's math tier are eligible.
    review_record_t picks[BATTLE_REVIEW_SLOTS];
    const uint8_t tier = campaign_stage(stage)->math_stage;
    const uint8_t count =
        review_store_collect(&s_app.review, stage, tier, picks,
                             BATTLE_REVIEW_SLOTS);
    math_question_t questions[BATTLE_REVIEW_SLOTS];
    for (uint8_t i = 0; i < count; i++) {
        questions[i] = picks[i].question;
    }
    battle_queue_review(&s_app.battle, questions, count);

    // Hold the round on a short READY beat so the player can see the board
    // before the question timer starts. OK skips it.
    battle_pause(&s_app.battle);
    s_app.ready_until_us = esp_timer_get_time() + (int64_t)READY_MS * 1000;

    s_app.page = PAGE_BATTLE;
    build_battle();
    audio_player_play(SFX_CONFIRM);

    char intro[32];
    if (s_app.mode == MODE_ENDLESS) {
        snprintf(intro, sizeof(intro), "无尽挑战 · 第%d关", (int)stage + 1);
        set_notice_ms(intro, 2200000LL);
    } else if (s_app.mode == MODE_GAUNTLET) {
        snprintf(intro, sizeof(intro), "三连战 · 第%d战", (int)s_app.wave);
        set_notice_ms(intro, 2200000LL);
    }
}

static void start_battle(uint8_t stage)
{
    begin_battle(stage, 0, 0);
}

static void start_run(game_mode_t mode)
{
    s_app.mode = mode;
    s_app.wave = 1;
    s_app.run_hp = 0;
    s_app.run_score = 0;
    s_app.run_bonus_pct = 0;
    s_app.pending_stage = 0;
    if (mode == MODE_GAUNTLET) {
        start_battle(gauntlet_stage(1));
    } else {
        start_battle(0);
    }
}

static void restart_current(void)
{
    if (s_app.mode == MODE_CAMPAIGN) {
        start_battle(s_app.battle.stage);
    } else {
        start_run(s_app.mode);
    }
}

static void record_run_best(void)
{
    if (s_app.mode == MODE_ENDLESS) {
        if ((uint32_t)s_app.battle.score > s_app.endless_best) {
            s_app.endless_best = (uint32_t)s_app.battle.score;
        }
    } else if (s_app.mode == MODE_GAUNTLET) {
        if ((uint32_t)s_app.battle.score > s_app.gauntlet_best) {
            s_app.gauntlet_best = (uint32_t)s_app.battle.score;
        }
    }
    request_save();
}

static void handle_run_clear(void)
{
    battle_t *battle = &s_app.battle;
    if (s_app.mode == MODE_ENDLESS) {
        s_app.run_hp = battle->hero_hp;
        s_app.run_score = battle->score;
        s_app.pending_stage =
            (uint8_t)((battle->stage + 1u) % CAMPAIGN_STAGE_COUNT);
        // Endless hands the breather to the player: bank the score, or spend
        // one more heart and keep the multiplier climbing.
        show_overlay(OVERLAY_WAVE);
        return;
    }

    s_app.run_score = battle->score;
    if (s_app.wave >= GAUNTLET_WAVES) {
        record_run_best();
        prepare_review_view();
        show_overlay(OVERLAY_CLEAR);
        return;
    }
    s_app.wave++;
    uint16_t hp = (uint16_t)(battle->hero_hp + 1u);
    if (hp > battle->hero_hp_max) {
        hp = battle->hero_hp_max;
    }
    s_app.run_hp = hp;
    begin_battle(gauntlet_stage(s_app.wave), s_app.run_hp, s_app.run_score);
}

static void end_battle_to_page(page_t page)
{
    request_save();
    s_app.ready_until_us = 0;
    s_app.page = page;
    if (page == PAGE_HOME) {
        build_home();
    } else {
        build_stages();
    }
}

static void set_notice_ms(const char *text, int64_t duration_us)
{
    snprintf(s_app.notice, sizeof(s_app.notice), "%s", text);
    s_app.notice_until_us = esp_timer_get_time() + duration_us;
}

static void set_notice(const char *text)
{
    set_notice_ms(text, 1600000LL);
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
        if (battle->current_review) {
            const bool removed =
                review_store_mastered(&s_app.review, &battle->question);
            spawn_popup(removed ? "掌握!" : "记住", COL_OK,
                        HERO_X - ARENA_X + 8, 22);
            request_save();
        }
        s_app.feedback_correct = battle->question.correct_index;
        s_app.feedback_wrong = 0xFF;
        break;
    case BATTLE_EV_WRONG:
    case BATTLE_EV_TIMEOUT: {
        audio_player_play(SFX_WRONG);
        audio_player_play(SFX_GROWL);
        record_mistake();
        s_app.feedback_correct = battle->question.correct_index;
        s_app.feedback_wrong = event == BATTLE_EV_WRONG ? battle->cursor : 0xFF;
        const int damage = (int)s_app.hero_hp_seen - (int)battle->hero_hp;
        schedule_impact(IMPACT_ENEMY, damage);
        break;
    }
    case BATTLE_EV_CRITICAL_HIT:
    case BATTLE_EV_CRITICAL_PERFECT: {
        audio_player_play(SFX_SWING);
        audio_player_play(SFX_CRITICAL);
        s_app.hero_attack_missed = false;
        const int damage = (int)s_app.monster_hp_seen - (int)battle->monster_hp;
        impact_kind_t kind = IMPACT_HERO_HIT;
        if (battle->combo_finisher) {
            kind = IMPACT_COMBO_FINISH;
        } else if (battle->crit_pursuit) {
            kind = IMPACT_HERO_PURSUIT;
        } else if (event == BATTLE_EV_CRITICAL_PERFECT) {
            kind = IMPACT_HERO_PERFECT;
        }
        schedule_impact(kind, damage);
        break;
    }
    case BATTLE_EV_CRITICAL_MISS: {
        audio_player_play(SFX_SWING);
        s_app.hero_attack_missed = true;
        // The marker and the window are still intact here, so tell the player
        // which side of the target they pressed on.
        const bool early = battle->crit_marker < battle->crit_center;
        spawn_popup(early ? "快了!" : "慢了!", COL_GOLD,
                    HERO_X - ARENA_X + 8, 22);
        const int damage = (int)s_app.monster_hp_seen - (int)battle->monster_hp;
        schedule_impact(IMPACT_HERO_MISS, damage);
        break;
    }
    case BATTLE_EV_ATTACK_DONE:
    case BATTLE_EV_ENEMY_DONE:
        s_app.feedback_correct = 0xFF;
        s_app.feedback_wrong = 0xFF;
        s_app.hero_attack_missed = false;
        break;
    case BATTLE_EV_ULTIMATE_START: {
        audio_player_play(SFX_CHARGE);
        set_notice(ultimate_title(battle->hero));
        break;
    }
    case BATTLE_EV_ULTIMATE_PERFECT:
    case BATTLE_EV_ULTIMATE_HIT: {
        audio_player_play(SFX_HIT);
        const int damage = (int)s_app.monster_hp_seen - (int)battle->monster_hp;
        schedule_impact(IMPACT_ULT_STEP, damage);
        if (event == BATTLE_EV_ULTIMATE_PERFECT) {
            set_notice_ms("完美!", 600000LL);
        }
        break;
    }
    case BATTLE_EV_ULTIMATE_MISS:
        audio_player_play(SFX_WRONG);
        set_notice_ms("没接上", 700000LL);
        break;
    case BATTLE_EV_ULTIMATE_DONE:
        break;
    case BATTLE_EV_GUARD_START:
        audio_player_play(SFX_WRONG);
        audio_player_play(SFX_GROWL);
        record_mistake();
        s_app.feedback_correct = battle->question.correct_index;
        s_app.feedback_wrong = battle->cursor;
        set_notice("偷袭!按提示方向格挡");
        break;
    case BATTLE_EV_GUARD_BLOCK: {
        audio_player_play(SFX_SWING);
        const int damage = (int)s_app.monster_hp_seen - (int)battle->monster_hp;
        schedule_impact(IMPACT_GUARD_BLOCK, damage);
        break;
    }
    case BATTLE_EV_GUARD_FAIL: {
        audio_player_play(SFX_WRONG);
        const int damage = (int)s_app.hero_hp_seen - (int)battle->hero_hp;
        schedule_impact(IMPACT_ENEMY, damage);
        break;
    }
    case BATTLE_EV_MONSTER_DEFEATED:
        audio_player_play(SFX_MONSTER_DOWN);
        s_app.monster_hp_goal = battle->monster_hp;
        s_app.arena_flash_until_us = now + 180000;
        set_notice("怪兽被击败!");
        break;
    case BATTLE_EV_HERO_DEFEATED:
        audio_player_play(SFX_DEFEAT);
        s_app.hero_hp_goal = battle->hero_hp;
        if (s_app.mode != MODE_CAMPAIGN) {
            record_run_best();
        }
        prepare_review_view();
        show_overlay(OVERLAY_OVER);
        break;
    case BATTLE_EV_STAGE_CLEAR:
        audio_player_play(SFX_VICTORY);
        if (s_app.mode == MODE_CAMPAIGN) {
            stage_record_result();
            prepare_review_view();
            show_overlay(OVERLAY_CLEAR);
        } else {
            handle_run_clear();
        }
        break;
    case BATTLE_EV_BEAM_START:
        audio_player_play(SFX_BEAM);
        set_notice("踩准节拍点确定,推回光线!");
        break;
    case BATTLE_EV_BEAM_PERFECT:
        audio_player_play(SFX_CRITICAL);
        set_notice_ms("完美接拍!", 600000LL);
        break;
    case BATTLE_EV_BEAM_HIT:
        audio_player_play(SFX_HIT);
        break;
    case BATTLE_EV_BEAM_MISS:
        audio_player_play(SFX_WRONG);
        break;
    case BATTLE_EV_BEAM_WIN:
        audio_player_play(SFX_MONSTER_DOWN);
        s_app.monster_hp_goal = battle->monster_hp;
        s_app.arena_flash_until_us = now + 220000;
        set_notice("光线压制成功!");
        break;
    case BATTLE_EV_BEAM_LOSE:
        audio_player_play(SFX_HURT);
        s_app.hero_hp_goal = battle->hero_hp;
        s_app.monster_hp_goal = battle->monster_hp;
        s_app.arena_flash_until_us = now + 140000;
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

    s_app.hero_hp_seen = battle->hero_hp;
    s_app.monster_hp_seen = battle->monster_hp;
}

static void tick_battle(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_app.ready_until_us > 0) {
        if (now < s_app.ready_until_us) {
            // The round is paused, so the question clock stays frozen while
            // the player reads the board.
            s_app.last_tick_us = now;
            refresh_battle_with_dt(0);
            return;
        }
        s_app.ready_until_us = 0;
        battle_resume(&s_app.battle);
        s_app.last_tick_us = now;
        refresh_battle();
        return;
    }

    uint32_t elapsed = (uint32_t)((now - s_app.last_tick_us) / 1000);
    s_app.last_tick_us = now;
    if (elapsed > 250) {
        elapsed = 250;
    }
    if (elapsed == 0) {
        elapsed = 1;
    }

    const uint8_t before = s_app.battle.phase;
    const battle_event_t event = battle_tick(&s_app.battle, elapsed);
    if (event != BATTLE_EV_NONE) {
        handle_battle_event(event);
    }

    // A direction pressed during the last 150 ms of an exchange selects an
    // answer as soon as the next question appears.
    if (s_app.battle.phase == BATTLE_PHASE_QUESTION &&
        before != BATTLE_PHASE_QUESTION && s_app.buffered_move != 0) {
        battle_event_t move = battle_move_cursor(&s_app.battle, 1);
        if (s_app.buffered_move > 0) {
            move = battle_move_cursor(&s_app.battle, s_app.buffered_move);
        }
        handle_battle_event(move);
        s_app.buffered_move = 0;
    }
    // The buffer only makes sense for the exchange it was captured in. Drop it
    // when the round diverts into a crit, ultimate, beam, result or pause.
    if (s_app.battle.phase != BATTLE_PHASE_QUESTION &&
        s_app.battle.phase != BATTLE_PHASE_ATTACK &&
        s_app.battle.phase != BATTLE_PHASE_ENEMY) {
        s_app.buffered_move = 0;
    }

    s_app.prev_phase = s_app.battle.phase;
    refresh_battle_with_dt(elapsed);
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
    if (s_app.overlay == OVERLAY_REVIEW) {
        close_review();
        audio_player_play(SFX_CONFIRM);
        return;
    }

    if (s_app.overlay != OVERLAY_PAUSE && s_app.review_view_count > 0 &&
        s_app.overlay_index == overlay_base_items()) {
        open_review();
        audio_player_play(SFX_CONFIRM);
        return;
    }

    if (s_app.overlay == OVERLAY_WAVE) {
        const uint8_t index = s_app.overlay_index;
        destroy_overlay();
        if (index == 0) {
            s_app.wave++;
            uint16_t hp = (uint16_t)(s_app.run_hp + 1u);
            if (hp > s_app.battle.hero_hp_max) {
                hp = s_app.battle.hero_hp_max;
            }
            s_app.run_hp = hp;
            if (s_app.run_bonus_pct < 100) {
                s_app.run_bonus_pct = (uint8_t)(s_app.run_bonus_pct + 10u);
            }
            begin_battle(s_app.pending_stage, s_app.run_hp, s_app.run_score);
        } else {
            record_run_best();
            prepare_review_view();
            show_overlay(OVERLAY_CLEAR);
            audio_player_play(SFX_CONFIRM);
        }
        return;
    }

    if (s_app.overlay == OVERLAY_PAUSE) {
        if (s_app.overlay_index == 0) {
            battle_resume(&s_app.battle);
            destroy_overlay();
            s_app.last_tick_us = esp_timer_get_time();
            audio_player_play(SFX_CONFIRM);
        } else if (s_app.overlay_index == 1) {
            destroy_overlay();
            restart_current();
        } else {
            destroy_overlay();
            end_battle_to_page(PAGE_HOME);
            audio_player_play(SFX_CONFIRM);
        }
        return;
    }

    if (s_app.overlay == OVERLAY_CLEAR) {
        if (s_app.mode == MODE_ENDLESS) {
            destroy_overlay();
            if (s_app.overlay_index == 0) {
                start_run(MODE_ENDLESS);
            } else {
                end_battle_to_page(PAGE_HOME);
                audio_player_play(SFX_CONFIRM);
            }
            return;
        }
        if (s_app.mode == MODE_GAUNTLET) {
            destroy_overlay();
            if (s_app.overlay_index == 0) {
                start_run(MODE_GAUNTLET);
            } else {
                end_battle_to_page(PAGE_HOME);
                audio_player_play(SFX_CONFIRM);
            }
            return;
        }
        const bool last = s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT;
        if (s_app.overlay_index == 0) {
            const uint8_t target =
                last ? s_app.battle.stage : (uint8_t)(s_app.battle.stage + 1);
            destroy_overlay();
            start_battle(target);
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
        destroy_overlay();
        if (s_app.overlay_index == 0) {
            restart_current();
        } else {
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
    if (event == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        s_app.mode = (game_mode_t)((s_app.mode + 1) % MODE_COUNT);
        audio_player_play(SFX_CONFIRM);
        refresh_home();
        request_save();
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        s_app.page = PAGE_SETTINGS;
        s_app.settings_index = 0;
        audio_player_play(SFX_CONFIRM);
        build_settings();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        if (s_app.mode != MODE_CAMPAIGN && !mode_unlocked(s_app.mode)) {
            audio_player_play(SFX_WRONG);
            refresh_home();
            return;
        }
        audio_player_play(SFX_CONFIRM);
        if (s_app.mode == MODE_CAMPAIGN) {
            s_app.stage = s_app.unlocked;
            s_app.page = PAGE_STAGES;
            build_stages();
        } else {
            start_run(s_app.mode);
        }
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
        s_app.mode = MODE_CAMPAIGN;
        request_save();
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
    if (s_app.ready_until_us > 0) {
        if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            s_app.ready_until_us = 0;
            battle_resume(&s_app.battle);
            s_app.last_tick_us = esp_timer_get_time();
            audio_player_play(SFX_CONFIRM);
            refresh_battle();
        }
        return;
    }

    if (s_app.overlay != OVERLAY_NONE) {
        if (s_app.overlay == OVERLAY_REVIEW) {
            const uint8_t pages =
                (uint8_t)((s_app.review_view_count + REVIEW_PAGE_SIZE - 1) /
                          REVIEW_PAGE_SIZE);
            if (event == BSP_BTN_CLICK && pages > 0 &&
                (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
                if (btn == BSP_BTN_UP) {
                    s_app.review_page =
                        (uint8_t)((s_app.review_page + pages - 1) % pages);
                } else {
                    s_app.review_page = (uint8_t)((s_app.review_page + 1) % pages);
                }
                audio_player_play(SFX_MOVE);
                rebuild_overlay();
            } else if ((event == BSP_BTN_CLICK || event == BSP_BTN_LONG) &&
                       btn == BSP_BTN_OK) {
                run_overlay_action();
            }
            return;
        }
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

    // Buffer a direction during the last 150 ms of the exchange. The move is
    // applied when the next question becomes active.
    if ((s_app.battle.phase == BATTLE_PHASE_ATTACK ||
         s_app.battle.phase == BATTLE_PHASE_ENEMY) &&
        event == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) &&
        s_app.battle.phase_ms <= INPUT_BUFFER_MS) {
        s_app.buffered_move = btn == BSP_BTN_UP ? -1 : 1;
        audio_player_play(SFX_MOVE);
        return;
    }

    if (s_app.battle.phase == BATTLE_PHASE_CRITICAL && event == BSP_BTN_PRESS &&
        btn == BSP_BTN_OK) {
        handle_battle_event(battle_confirm(&s_app.battle));
        refresh_battle();
        return;
    }
    if (s_app.battle.phase == BATTLE_PHASE_ULTIMATE && event == BSP_BTN_PRESS) {
        uint8_t input = 0xFF;
        if (btn == BSP_BTN_OK) {
            input = ULT_INPUT_OK;
        } else if (btn == BSP_BTN_UP) {
            input = ULT_INPUT_UP;
        } else if (btn == BSP_BTN_DOWN) {
            input = ULT_INPUT_DOWN;
        }
        if (input != 0xFF) {
            const battle_event_t ev = battle_ultimate_press(&s_app.battle, input);
            if (ev != BATTLE_EV_NONE) {
                handle_battle_event(ev);
            }
            refresh_battle();
        }
        return;
    }
    if (s_app.battle.phase == BATTLE_PHASE_GUARD && event == BSP_BTN_PRESS) {
        uint8_t input = 0xFF;
        if (btn == BSP_BTN_UP) {
            input = ULT_INPUT_UP;
        } else if (btn == BSP_BTN_DOWN) {
            input = ULT_INPUT_DOWN;
        }
        if (input != 0xFF) {
            const battle_event_t ev = battle_guard_press(&s_app.battle, input);
            if (ev != BATTLE_EV_NONE) {
                handle_battle_event(ev);
            }
            refresh_battle();
        }
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
    case PAGE_SETTINGS:
        handle_settings_key(btn, event);
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

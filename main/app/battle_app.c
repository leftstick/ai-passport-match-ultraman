// main/app/battle_app.c —— 算术超能战:英雄/关卡选择、对战界面、暂停与结算。
//
// 线程模型:
//   * 按键回调在 esp_timer 任务里只入队;输入任务调用 battle_app_handle_key()。
//   * app 任务每 50ms 推进战斗计时、动画与电量刷新。
//   * 两个任务都只在 bsp_lvgl_lock() 内访问 battle 状态和 LVGL 对象。
//   * NVS 写入由 app 任务在锁外完成,避免在 LVGL 锁内做慢操作。
#include "battle_app.h"

#include "action_config.h"
#include "anim.h"
#include "audio_player.h"
#include "battle.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "campaign.h"
#include "cycle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "progress.h"
#include "review.h"
#include "sprites.h"
#include "time_attack.h"

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

// The beam marker sweeps across the gold band so the beat is visible instead
// of only being audible. The hit window straddles the band centre.
#define BEAM_SWEEP_MS 450
#define BEAM_ZONE_LEFT 21
#define BEAM_ZONE_WIDTH 58
#define BEAM_CUE_COUNT (BEAM_BEAT_COUNT + 2)

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
#define SETTINGS_COUNT 8
#define SHOP_ITEM_COUNT 3
#define SHOP_GOLD_BEAM 0x01u
#define SHOP_GALLERY_LORE 0x02u
#define SHOP_SKIN_PACK 0x04u
#define GALLERY_COMPLETE_REWARD 0x08u
#define COLLECTION_PAGES 3
#define COLLECTION_HERO_CELLS CAMPAIGN_HERO_COUNT
#define COLLECTION_MONSTER_CELLS CAMPAIGN_MONSTER_COUNT
#define COLLECTION_BADGE_CELLS 6
#define COLLECTION_CELLS COLLECTION_HERO_CELLS
#define TIME_RESULT_ITEMS 2

typedef enum {
    PAGE_HOME = 0,
    PAGE_STAGES,
    PAGE_BATTLE,
    PAGE_SETTINGS,
    PAGE_TIME,
    PAGE_COLLECTION,
    PAGE_SHOP,
    PAGE_PREVIEW,
} page_t;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_PAUSE,
    OVERLAY_CLEAR,
    OVERLAY_OVER,
    OVERLAY_REVIEW,
    OVERLAY_WAVE,
    OVERLAY_CYCLE,
} overlay_t;

// First-stage coaching cards. They freeze the battle until OK is pressed so
// the text cannot scroll past while a scene transition runs.
typedef enum {
    TUTORIAL_NONE = 0,
    TUTORIAL_SELECT,
    TUTORIAL_CRITICAL,
} tutorial_card_t;

typedef enum {
    MODE_CAMPAIGN = 0,
    MODE_ENDLESS,
    MODE_GAUNTLET,
    MODE_TIME,
    MODE_HUNDRED,
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
    lv_obj_t *footer;
} stages_ui_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *row[SETTINGS_COUNT];
    lv_obj_t *name[SETTINGS_COUNT];
    lv_obj_t *value[SETTINGS_COUNT];
} settings_ui_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *timer_text;
    lv_obj_t *timer_fill;
    lv_obj_t *score;
    lv_obj_t *combo;
    lv_obj_t *hazard;
    lv_obj_t *question;
    lv_obj_t *hint;
    lv_obj_t *row[MATH_QUESTION_OPTION_COUNT];
    lv_obj_t *row_text[MATH_QUESTION_OPTION_COUNT];
    lv_obj_t *row_mark[MATH_QUESTION_OPTION_COUNT];
    lv_obj_t *result;
    lv_obj_t *result_title;
    lv_obj_t *result_detail;
    lv_obj_t *result_note;
    lv_obj_t *result_opt[TIME_RESULT_ITEMS];
    lv_obj_t *result_opt_text[TIME_RESULT_ITEMS];
} time_ui_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *tab[COLLECTION_PAGES];
    lv_obj_t *tab_text[COLLECTION_PAGES];
    lv_obj_t *cell[COLLECTION_CELLS];
    lv_obj_t *cell_img[COLLECTION_CELLS];
    lv_obj_t *cell_title[COLLECTION_CELLS];
    lv_obj_t *cell_detail[COLLECTION_CELLS];
} collection_ui_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *row[SHOP_ITEM_COUNT];
    lv_obj_t *name[SHOP_ITEM_COUNT];
    lv_obj_t *detail[SHOP_ITEM_COUNT];
    lv_obj_t *price[SHOP_ITEM_COUNT];
} shop_ui_t;

// 事件驱动动作层的单侧播放状态。一次只播一个动作, 播完回到常规动画。
typedef struct {
    bool active;
    uint8_t action;     // action_id_t
    int64_t start_us;
} action_play_t;

typedef struct {
    lv_obj_t *battery;
    lv_obj_t *title;
    lv_obj_t *image;
    lv_obj_t *action;
    lv_obj_t *detail;
    lv_obj_t *footer;
    uint8_t kind;       // action_character_t
    uint8_t index;
    uint8_t action_id;
    uint8_t frame;
    int64_t frame_at_us;
    int64_t action_at_us;
} preview_ui_t;

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
    lv_obj_t *beam_pip[BEAM_BEAT_COUNT];
    lv_obj_t *beam_glow;
    lv_obj_t *beam_core;
    lv_obj_t *flash;
    lv_obj_t *tutorial;
    lv_obj_t *tutorial_title;
    lv_obj_t *tutorial_body;
    lv_obj_t *tutorial_footer;
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
    uint8_t strike_seq;
    anim_id_t strike_anim;
    uint16_t run_hp;
    int32_t run_score;
    uint32_t endless_best;
    uint32_t gauntlet_best;
    uint32_t hundred_best;
    uint8_t settings_index;
    uint8_t shop_index;

    // 周目: 每通一次 9 关推进一轮, 怪兽/题型/挑战词缀随种子轮换。
    cycle_plan_t cycle_plan;
    uint16_t cycle;
    uint32_t cycle_seed;
    uint32_t coins;
    uint8_t fragments;
    uint32_t shop_flags;
    bool cycle_variation;
    // 当前这一轮冒险的累计统计, 失败或重来不会清零。
    uint32_t run_total_score;
    uint32_t run_coins;
    uint32_t run_correct;
    uint32_t run_wrong;
    uint32_t run_answer_total;
    uint16_t run_fastest;
    uint16_t run_paid_mask;
    uint32_t stage_coins;
    bool stage_paid;
    uint8_t cycle_result_fragments;
    bool cycle_result_awarded;
    char stage_encourage[40];
    bool cycle_secret_pending;
    bool cycle_secret_boss;
    bool cycle_secret_done;
    bool cycle_gallery_bonus;

    progress_t progress;
    time_attack_t time;
    bool time_result;
    uint8_t time_result_index;
    int64_t time_notice_until_us;
    char time_notice[48];
    uint8_t collection_page;
    uint8_t collection_index;

    // 动作层: 只叠加姿势, 不参与答题/判题/计分。
    bool action_easter_egg;
    action_play_t hero_action;
    action_play_t monster_action;
    math_rng_t action_rng;
    int64_t easter_egg_at_us;
    uint8_t easter_egg_rolls;
    uint8_t action_question_index;
    preview_ui_t preview;

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
    time_ui_t time_ui;
    collection_ui_t collection;
    shop_ui_t shop;
    battle_ui_t battle_ui;

    uint8_t overlay_items;
    uint8_t overlay_index;

    int battery_soc;
    int64_t notice_until_us;
    char notice[48];

    int64_t last_tick_us;
    int64_t arena_flash_until_us;
    int64_t ready_until_us;
    // 怪兽惨叫延后到 KO 最后一帧, 与淡出同步。
    int64_t ko_cry_at_us;
    sfx_id_t ko_cry_sfx;
    bool ko_cry_pending;
    uint8_t ko_fx_step;

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
    int32_t ui_timer_value;
    int32_t ui_timer_total;
    uint32_t ui_timer_color;
    uint8_t row_sig[MATH_QUESTION_OPTION_COUNT];
    int16_t row_value[MATH_QUESTION_OPTION_COUNT];
    int16_t ui_hero_x;
    int16_t ui_hero_y;
    int16_t ui_monster_x;
    int16_t ui_monster_y;
    uint8_t ui_hero_flash;
    uint8_t ui_monster_flash;
    uint8_t ui_monster_fade;
    int8_t ui_shake_x;
    int8_t ui_shake_y;
    bool ui_beam_visible;
    bool ui_flash_visible;
    int16_t ui_crit_left;
    int16_t ui_crit_width;
    int16_t ui_time_fill;
    uint8_t time_row_sig[MATH_QUESTION_OPTION_COUNT];
    int16_t time_row_value[MATH_QUESTION_OPTION_COUNT];
    int16_t ui_beam_fill;
    int16_t ui_beam_pips;
    bool ui_pips_visible;
    uint8_t beam_cue_next;

    uint8_t tutorial;
    bool tutorial_enabled;
    bool tutorial_crit_shown;
} s_app;

static volatile bool s_save_pending;

static void set_notice(const char *text);
static void set_notice_ms(const char *text, int64_t duration_us);
static void show_tutorial(uint8_t card);
static void build_collection(void);
static void build_preview(void);
static void build_shop(void);
static bool cycle_stage_secret(uint8_t stage);
static void tick_time_attack(void);
static void start_time_attack(void);
static bool award_badge(uint8_t badge);
static void finish_time_attack(void);

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
static void cycle_rebuild_plan(void)
{
    cycle_plan_build(&s_app.cycle_plan, s_app.cycle, s_app.cycle_seed,
                     s_app.cycle_variation);
}

static void nvs_load(void)
{
    review_store_init(&s_app.review);
    progress_init(&s_app.progress);
    s_app.cycle = 1;
    s_app.cycle_seed = 0xC0FFEEu;
    s_app.cycle_variation = true;
    s_app.action_easter_egg = true;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "暂无存档,使用默认进度");
        cycle_rebuild_plan();
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
    (void)nvs_get_u32(handle, "hundred_best", &s_app.hundred_best);
    (void)nvs_get_u16(handle, "cycle", &s_app.cycle);
    (void)nvs_get_u32(handle, "cycle_seed", &s_app.cycle_seed);
    (void)nvs_get_u32(handle, "coins", &s_app.coins);
    (void)nvs_get_u32(handle, "shop", &s_app.shop_flags);
    if (nvs_get_u8(handle, "fragments", &u8) == ESP_OK) {
        s_app.fragments = u8;
    }
    if (nvs_get_u8(handle, "cycle_var", &u8) == ESP_OK) {
        s_app.cycle_variation = u8 != 0;
    }
    if (nvs_get_u8(handle, "egg_fx", &u8) == ESP_OK) {
        s_app.action_easter_egg = u8 != 0;
    }
    if (s_app.cycle == 0) {
        s_app.cycle = 1;
    }
    cycle_rebuild_plan();
    size_t size = sizeof(s_app.best);
    (void)nvs_get_blob(handle, "best", s_app.best, &size);
    size = sizeof(s_app.review);
    if (nvs_get_blob(handle, "review", &s_app.review, &size) == ESP_OK) {
        if (size != sizeof(s_app.review) || s_app.review.count > REVIEW_CAPACITY) {
            review_store_init(&s_app.review);
        }
    }
    // 新存档为 32 位英雄掩码; 旧的 16 位布局交给 progress_load 迁移。
    uint8_t progress_blob[sizeof(progress_t) + 8];
    size = sizeof(progress_blob);
    if (nvs_get_blob(handle, "progress", progress_blob, &size) != ESP_OK) {
        progress_init(&s_app.progress);
    } else {
        (void)progress_load(&s_app.progress, progress_blob, size);
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
    (void)nvs_set_u32(handle, "hundred_best", s_app.hundred_best);
    (void)nvs_set_u16(handle, "cycle", s_app.cycle);
    (void)nvs_set_u32(handle, "cycle_seed", s_app.cycle_seed);
    (void)nvs_set_u32(handle, "coins", s_app.coins);
    (void)nvs_set_u32(handle, "shop", s_app.shop_flags);
    (void)nvs_set_u8(handle, "fragments", s_app.fragments);
    (void)nvs_set_u8(handle, "cycle_var", s_app.cycle_variation ? 1 : 0);
    (void)nvs_set_u8(handle, "egg_fx", s_app.action_easter_egg ? 1 : 0);
    (void)nvs_set_blob(handle, "best", s_app.best, sizeof(s_app.best));
    (void)nvs_set_blob(handle, "review", &s_app.review, sizeof(s_app.review));
    (void)nvs_set_blob(handle, "progress", &s_app.progress,
                       sizeof(s_app.progress));
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
    // 周目替换怪兽后, 图鉴按真实出现的怪兽登记。
    (void)progress_mark_monster(&s_app.progress, s_app.battle.monster);
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
    // 徽章按「达成即永久保留」处理,不因之后的重玩而撤销。
    if (s_app.battle.hero_hp == s_app.battle.hero_hp_max) {
        (void)award_badge(PROGRESS_BADGE_FLAWLESS);
    }
    if (s_app.battle.best_combo >= 8) {
        (void)award_badge(PROGRESS_BADGE_COMBO);
    }
    if (stage + 1u == CAMPAIGN_STAGE_COUNT) {
        (void)award_badge(PROGRESS_BADGE_ALL_CLEAR);
    }
    if (progress_three_star_count(s_app.stars) == CAMPAIGN_STAGE_COUNT) {
        (void)award_badge(PROGRESS_BADGE_THREE_STAR);
    }
    request_save();
}

static void stage_award_reward(void)
{
    battle_t *battle = &s_app.battle;
    const bool secret_stage = cycle_stage_secret(battle->stage);
    const bool secret_boss = s_app.cycle_secret_boss;
    const uint16_t bit = (uint16_t)(1u << (battle->stage & 15u));
    const bool first_count = (s_app.run_paid_mask & bit) == 0;

    uint32_t coins = cycle_stage_coin_reward(
        battle->stars, battle->boss, secret_stage || secret_boss,
        s_app.cycle_plan.mods);
    // 同一周目内重复刷同一关不再产出金币和统计, 避免反复重打刷分。
    if (!first_count && !secret_boss) {
        coins = 0;
    }
    if (secret_boss) {
        coins = coins * 2u + 20u;
        s_app.cycle_secret_done = true;
    }
    if (first_count || secret_boss) {
        s_app.run_paid_mask |= bit;
        s_app.run_correct += battle->correct_count;
        s_app.run_wrong += battle->wrong_count;
        s_app.run_answer_total += battle->answer_total_ms;
        if (battle->fastest_answer_ms != 0 &&
            (s_app.run_fastest == 0 ||
             battle->fastest_answer_ms < s_app.run_fastest)) {
            s_app.run_fastest = battle->fastest_answer_ms;
        }
        if (battle->score > 0) {
            s_app.run_total_score += (uint32_t)battle->score;
        }
    }

    s_app.coins += coins;
    s_app.run_coins += coins;
    s_app.stage_coins = coins;
    s_app.stage_paid = first_count || secret_boss;

    static const char *const cheers[] = {
        "光之战士,继续前进!",
        "怪兽的弱点被你抓住了!",
        "这一击漂亮,保持节奏!",
        "稳住呼吸,下一关也不怕!",
        "能量充足,继续冒险!",
    };
    const uint32_t pick =
        ((uint32_t)s_app.cycle * 7u + (uint32_t)battle->stage * 3u +
         (uint32_t)battle->stars + (uint32_t)(esp_timer_get_time() / 1000000)) %
        (uint32_t)(sizeof(cheers) / sizeof(cheers[0]));
    snprintf(s_app.stage_encourage, sizeof(s_app.stage_encourage), "%s",
             cheers[pick]);
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
    const uint8_t tier =
        campaign_math_stage(s_app.battle.stage, s_app.battle.difficulty);
    s_app.review_view_count =
        review_store_collect(&s_app.review, s_app.battle.stage, tier,
                             s_app.review_view, REVIEW_VIEW_MAX);
    s_app.review_page = 0;
    request_save();
}

static uint8_t gauntlet_stage(uint8_t wave)
{
    // The three hardest fights double as the elite gauntlet.
    static const uint8_t stages[GAUNTLET_WAVES] = { 6, 7, 8 };
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
    case MODE_TIME:
        return "限时";
    case MODE_HUNDRED:
        return "百减";
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
        // Clearing stage 6 unlocks stage 7; the gauntlet starts there.
        return s_app.unlocked >= 6;
    case MODE_TIME:
        // Clearing stage 2 unlocks stage 3; time attack opens there.
        return s_app.unlocked >= 2;
    case MODE_HUNDRED:
        // 100 减法专项不依赖战役进度, 随时可练。
        return true;
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
        return "通关第6关解锁 · 长按↑换模式";
    case MODE_TIME:
        return "通关第2关解锁 · 长按↑换模式";
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
    case PAGE_TIME:
        label_set(s_app.time_ui.battery, buf);
        break;
    case PAGE_COLLECTION:
        label_set(s_app.collection.battery, buf);
        break;
    case PAGE_SHOP:
        label_set(s_app.shop.battery, buf);
        break;
    case PAGE_PREVIEW:
        label_set(s_app.preview.battery, buf);
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
            uint32_t best = s_app.progress.time_best;
            if (mode == MODE_ENDLESS) {
                best = s_app.endless_best;
            } else if (mode == MODE_GAUNTLET) {
                best = s_app.gauntlet_best;
            } else if (mode == MODE_HUNDRED) {
                best = s_app.hundred_best;
            }
            // The pill is only 40 px wide; drop the prefix once the number
            // would wrap onto a second line.
            if (best < 1000) {
                snprintf(buf, sizeof(buf), "高%u", (unsigned)best);
            } else {
                snprintf(buf, sizeof(buf), "%u", (unsigned)best);
            }
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

    make_label(scr, 6, 246, 28, "模式", &app_font_12, COL_DIM);
    // 五个模式共用一行: 40 px 卡片 + 1 px 间距, 仍然落在 240 px 内。
    for (int i = 0; i < MODE_COUNT; i++) {
        const int x = 32 + i * 41;
        s_app.home.mode_pill[i] = make_panel(scr, x, 240, 40, 32, COL_TILE, 7);
        s_app.home.mode_name[i] = make_center(scr, x, 242, 40,
                                              mode_title((game_mode_t)i),
                                              &app_font_12, COL_DIM);
        s_app.home.mode_info[i] = make_center(scr, x, 256, 40, "",
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
static void cycle_mods_text(char *out, size_t size)
{
    out[0] = '\0';
    size_t used = 0;
    const uint8_t bits[] = { CYCLE_MOD_SPEED, CYCLE_MOD_GUARD,
                             CYCLE_MOD_MIXED, CYCLE_MOD_BOSS };
    const size_t bit_count = sizeof(bits) / sizeof(bits[0]);
    for (size_t i = 0; i < bit_count; i++) {
        if ((s_app.cycle_plan.mods & bits[i]) == 0) {
            continue;
        }
        const char *name = cycle_mod_name(bits[i]);
        const int n = snprintf(out + used, size - used, "%s%s",
                               used > 0 ? " · " : "", name);
        if (n < 0 || (size_t)n >= size - used) {
            break;
        }
        used += (size_t)n;
    }
    if (out[0] == '\0') {
        snprintf(out, size, "无词缀");
    }
}

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

    const char *rule = stage_rule_text(s_app.stage);
    const uint8_t monster = s_app.cycle_plan.monster[s_app.stage] <
                                    CAMPAIGN_MONSTER_COUNT
                                ? s_app.cycle_plan.monster[s_app.stage]
                                : s_app.stage;
    const uint8_t style = cycle_style_for_stage(&s_app.cycle_plan, s_app.stage);
    const char *style_name = cycle_style_name(style);
    char line[80];
    if (s_app.cycle_variation && s_app.cycle >= 2) {
        snprintf(line, sizeof(line), "对手:%s · %s",
                 campaign_monster_name(monster), style_name);
    } else if (rule[0] != '\0') {
        snprintf(line, sizeof(line), "对手:%s · %s",
                 campaign_monster_name(monster), rule);
    } else {
        snprintf(line, sizeof(line), "对手:%s", campaign_monster_name(monster));
    }
    label_set(s_app.stages.detail_1, line);

    const bool tutorial_stage =
        s_app.stage <= 1 && s_app.unlocked <= s_app.stage;
    if (s_app.cycle_variation && s_app.cycle_plan.mods != 0) {
        char mods[64];
        cycle_mods_text(mods, sizeof(mods));
        snprintf(line, sizeof(line), "词缀:%s", mods);
        label_set(s_app.stages.detail_2, line);
    } else if (s_app.cycle_variation && cycle_stage_secret(s_app.stage)) {
        label_set(s_app.stages.detail_2, "隐藏怪兽 · 金币加成");
    } else {
        label_set(s_app.stages.detail_2,
                  tutorial_stage ? "教程关 · 会停下来教你操作"
                                 : "★3 0错满血 · ★2 ≤1错 · ★1 通关");
    }
    if (s_app.stages.footer != NULL) {
        if (cycle_has_rare_pool(s_app.cycle, s_app.cycle_variation)) {
            snprintf(line, sizeof(line), "金币 %u · 稀有怪兽池开启",
                     (unsigned)s_app.coins);
        } else {
            snprintf(line, sizeof(line), "金币 %u · 长按下重掷词缀",
                     (unsigned)s_app.coins);
        }
        label_set(s_app.stages.footer, line);
    }
}

static void build_stages(void)
{
    lv_obj_t *scr = make_screen();
    const difficulty_def_t *diff = campaign_difficulty(s_app.difficulty);

    s_app.stages.battery = make_label(scr, 194, 6, 40, " --", &app_font_12, COL_DIM);
    lv_obj_set_style_text_align(s_app.stages.battery, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(scr, 8, 5, 110, "选择关卡", &app_font_20, COL_TEXT);
    char cycle_text[24];
    snprintf(cycle_text, sizeof(cycle_text), "第%u周目",
             (unsigned)s_app.cycle);
    make_label(scr, 88, 9, 60, cycle_text, &app_font_12, COL_ACCENT);
    char diff_text[32];
    snprintf(diff_text, sizeof(diff_text), "难度 %s", diff->name);
    make_label(scr, 152, 9, 80, diff_text, &app_font_12, COL_GOLD);

    for (int i = 0; i < CAMPAIGN_STAGE_COUNT; i++) {
        const int y = 30 + i * 25;
        const stage_def_t *stage = campaign_stage((uint8_t)i);
        s_app.stages.row[i] = make_panel(scr, 4, y, 232, 24, COL_TILE, 5);

        char num[8];
        snprintf(num, sizeof(num), "%d", i + 1);
        make_label(s_app.stages.row[i], 6, 4, 14, num, &app_font_12, COL_DIM);
        make_label(s_app.stages.row[i], 22, 4, 120, stage->name, &app_font_12,
                   COL_TEXT);
        s_app.stages.stars[i] = make_center(s_app.stages.row[i], 148, 4, 52, "",
                                            &app_font_12, COL_GOLD);
        s_app.stages.status[i] = make_label(s_app.stages.row[i], 202, 4, 24, "",
                                            &app_font_12, COL_DIM);
    }

    s_app.stages.detail_1 = make_label(scr, 8, 258, 224, "", &app_font_12, COL_TEXT);
    s_app.stages.detail_2 = make_center(scr, 0, 274, SCREEN_W, "", &app_font_12,
                                        COL_GOLD);
    s_app.stages.footer = make_center(scr, 0, 298, SCREEN_W, "",
                                      &app_font_12, COL_DIM);

    page_switch(scr);
    refresh_stages();
    set_battery_labels();
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------
static const char *settings_name(uint8_t index)
{
    static const char *const names[] = { "低龄模式", "减弱动态", "音效",
                                         "周目变化", "随机彩蛋", "收藏图鉴",
                                         "硬币商店", "返回" };
    return index < SETTINGS_COUNT ? names[index] : "";
}

static const char *settings_detail(uint8_t index)
{
    static const char *const details[] = {
        "隐藏格挡QTE · 暴击区更宽 · 关闭干扰",
        "关闭震屏和强闪光",
        "战斗音效开关",
        "每周目轮换怪兽/题型/词缀 (关闭=固定练习)",
        "待机时播放小动作 (关闭=安静刷题)",
        "英雄、怪兽与徽章进度",
        "用金币兑换光线颜色、图鉴解说和皮肤碎片",
        "回到主界面",
    };
    return index < SETTINGS_COUNT ? details[index] : "";
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
    case 3:
        return s_app.cycle_variation;
    case 4:
        return s_app.action_easter_egg;
    default:
        return false;
    }
}

static void refresh_settings(void)
{
    if (s_app.page != PAGE_SETTINGS || s_app.root == NULL) return;
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        const bool selected = i == s_app.settings_index;
        lv_obj_set_style_bg_color(s_app.settings.row[i],
                                  lv_color_hex(selected ? COL_TILE_SEL : COL_TILE), 0);
        lv_obj_set_style_border_width(s_app.settings.row[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_app.settings.row[i],
                                      lv_color_hex(COL_ACCENT), 0);
        label_color(s_app.settings.name[i], selected ? COL_TEXT : COL_DIM);
        if (i >= 5) {
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

    for (int i = 0; i < SETTINGS_COUNT; i++) {
        const int y = 30 + i * 28;
        s_app.settings.row[i] = make_panel(scr, 6, y, 228, 26, COL_TILE, 8);
        s_app.settings.name[i] = make_label(s_app.settings.row[i], 10, 1, 150,
                                            settings_name((uint8_t)i),
                                            &app_font_14, COL_TEXT);
        s_app.settings.value[i] = make_label(s_app.settings.row[i], 184, 3, 34, "",
                                             &app_font_14, COL_DIM);
        lv_obj_set_style_text_align(s_app.settings.value[i], LV_TEXT_ALIGN_RIGHT, 0);
        make_label(s_app.settings.row[i], 10, 14, 208,
                   settings_detail((uint8_t)i), &app_font_12, COL_DIM);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "金币 %u · 碎片 %u · 第%u周目",
             (unsigned)s_app.coins, (unsigned)s_app.fragments,
             (unsigned)s_app.cycle);
    make_center(scr, 0, 266, SCREEN_W, buf, &app_font_12, COL_GOLD);
    make_center(scr, 0, 292, SCREEN_W, "↑↓选择 · OK切换 · 长按OK返回",
                &app_font_12, COL_DIM);

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
        s_app.settings_index =
            (uint8_t)((s_app.settings_index + SETTINGS_COUNT - 1) % SETTINGS_COUNT);
        audio_player_play(SFX_MOVE);
        refresh_settings();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
        s_app.settings_index =
            (uint8_t)((s_app.settings_index + 1) % SETTINGS_COUNT);
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
        } else if (s_app.settings_index == 3) {
            s_app.cycle_variation = !s_app.cycle_variation;
            cycle_rebuild_plan();
        } else if (s_app.settings_index == 4) {
            s_app.action_easter_egg = !s_app.action_easter_egg;
            if (!s_app.action_easter_egg) {
                s_app.easter_egg_at_us = 0;
            }
        } else if (s_app.settings_index == 5) {
            s_app.collection_page = 0;
            s_app.collection_index = 0;
            s_app.page = PAGE_COLLECTION;
            audio_player_play(SFX_CONFIRM);
            build_collection();
            return;
        } else if (s_app.settings_index == 6) {
            s_app.shop_index = 0;
            s_app.page = PAGE_SHOP;
            audio_player_play(SFX_CONFIRM);
            build_shop();
            return;
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
// 硬币商店
// ---------------------------------------------------------------------------
static const char *shop_item_name(uint8_t index)
{
    static const char *const names[] = { "黄金光线", "怪兽图鉴解说", "皮肤碎片包" };
    return index < SHOP_ITEM_COUNT ? names[index] : "";
}

static const char *shop_item_detail(uint8_t index)
{
    static const char *const details[] = {
        "把必杀与对拼光线换成金色",
        "收藏图鉴怪兽页显示擅长题型",
        "立刻获得 3 枚奥特曼皮肤碎片",
    };
    return index < SHOP_ITEM_COUNT ? details[index] : "";
}

static uint32_t shop_item_price(uint8_t index)
{
    static const uint32_t prices[] = { 80, 120, 60 };
    return index < SHOP_ITEM_COUNT ? prices[index] : 0;
}

static uint32_t shop_item_flag(uint8_t index)
{
    static const uint32_t flags[] = { SHOP_GOLD_BEAM, SHOP_GALLERY_LORE,
                                      SHOP_SKIN_PACK };
    return index < SHOP_ITEM_COUNT ? flags[index] : 0;
}

static void refresh_shop(void)
{
    if (s_app.page != PAGE_SHOP || s_app.root == NULL) return;
    for (int i = 0; i < SHOP_ITEM_COUNT; i++) {
        const bool selected = i == s_app.shop_index;
        const uint32_t flag = shop_item_flag((uint8_t)i);
        const bool owned = flag != 0 && (s_app.shop_flags & flag) != 0;
        lv_obj_set_style_bg_color(s_app.shop.row[i],
                                  lv_color_hex(selected ? COL_TILE_SEL : COL_TILE), 0);
        lv_obj_set_style_border_width(s_app.shop.row[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_app.shop.row[i],
                                      lv_color_hex(COL_GOLD), 0);
        label_color(s_app.shop.name[i], selected ? COL_TEXT : COL_DIM);
        char buf[24];
        if (owned) {
            snprintf(buf, sizeof(buf), "已拥有");
            label_color(s_app.shop.price[i], COL_OK);
        } else if (s_app.coins < shop_item_price((uint8_t)i)) {
            snprintf(buf, sizeof(buf), "%u 金币", (unsigned)shop_item_price((uint8_t)i));
            label_color(s_app.shop.price[i], COL_DANGER);
        } else {
            snprintf(buf, sizeof(buf), "%u 金币", (unsigned)shop_item_price((uint8_t)i));
            label_color(s_app.shop.price[i], COL_GOLD);
        }
        label_set(s_app.shop.price[i], buf);
    }
}

static void build_shop(void)
{
    lv_obj_t *scr = make_screen();
    memset(&s_app.shop, 0, sizeof(s_app.shop));
    s_app.shop.battery = make_label(scr, 194, 6, 40, " --", &app_font_12, COL_DIM);
    lv_obj_set_style_text_align(s_app.shop.battery, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(scr, 8, 5, 110, "硬币商店", &app_font_20, COL_TEXT);

    char buf[64];
    snprintf(buf, sizeof(buf), "金币 %u", (unsigned)s_app.coins);
    lv_obj_t *coin = make_label(scr, 130, 9, 102, buf, &app_font_12, COL_GOLD);
    lv_obj_set_style_text_align(coin, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < SHOP_ITEM_COUNT; i++) {
        const int y = 40 + i * 74;
        s_app.shop.row[i] = make_panel(scr, 6, y, 228, 66, COL_TILE, 8);
        s_app.shop.name[i] = make_label(s_app.shop.row[i], 10, 4, 150,
                                        shop_item_name((uint8_t)i),
                                        &app_font_14, COL_TEXT);
        s_app.shop.price[i] = make_label(s_app.shop.row[i], 152, 5, 70, "",
                                         &app_font_12, COL_GOLD);
        lv_obj_set_style_text_align(s_app.shop.price[i], LV_TEXT_ALIGN_RIGHT, 0);
        make_label(s_app.shop.row[i], 10, 26, 208,
                   shop_item_detail((uint8_t)i), &app_font_12, COL_DIM);
        make_label(s_app.shop.row[i], 10, 46, 208,
                   (s_app.shop_flags & shop_item_flag((uint8_t)i)) != 0
                       ? "已解锁"
                       : "OK 兑换",
                   &app_font_12, COL_ACCENT);
    }

    snprintf(buf, sizeof(buf), "皮肤碎片 %u · 第%u周目",
             (unsigned)s_app.fragments, (unsigned)s_app.cycle);
    make_center(scr, 0, 272, SCREEN_W, buf, &app_font_12, COL_DIM);
    make_center(scr, 0, 296, SCREEN_W, "↑↓选择 · OK兑换 · 长按OK返回",
                &app_font_12, COL_DIM);

    page_switch(scr);
    refresh_shop();
    set_battery_labels();
}

static void handle_shop_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
        s_app.shop_index = (uint8_t)((s_app.shop_index + SHOP_ITEM_COUNT - 1) %
                                     SHOP_ITEM_COUNT);
        audio_player_play(SFX_MOVE);
        refresh_shop();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
        s_app.shop_index =
            (uint8_t)((s_app.shop_index + 1) % SHOP_ITEM_COUNT);
        audio_player_play(SFX_MOVE);
        refresh_shop();
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_app.page = PAGE_SETTINGS;
        build_settings();
        audio_player_play(SFX_CONFIRM);
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        const uint32_t flag = shop_item_flag(s_app.shop_index);
        const uint32_t price = shop_item_price(s_app.shop_index);
        if (flag == 0 || (s_app.shop_flags & flag) != 0 || s_app.coins < price) {
            audio_player_play(SFX_WRONG);
            return;
        }
        s_app.coins -= price;
        s_app.shop_flags |= flag;
        if (flag == SHOP_SKIN_PACK) {
            s_app.fragments = (uint8_t)(s_app.fragments + 3);
        }
        request_save();
        audio_player_play(SFX_CONFIRM);
        build_shop();
    }
}

// ---------------------------------------------------------------------------
// 限时猎手
// ---------------------------------------------------------------------------
static bool award_badge(uint8_t badge)
{
    if (!progress_award(&s_app.progress, badge)) {
        return false;
    }
    request_save();
    return true;
}

// 限时页的短提示(换位、解冻、未选答案)只在几百毫秒内压过常规干扰文案。
static void time_notice(const char *text, int64_t duration_us)
{
    snprintf(s_app.time_notice, sizeof(s_app.time_notice), "%s", text);
    s_app.time_notice_until_us = esp_timer_get_time() + duration_us;
}

static void refresh_time_result(void)
{
    time_ui_t *ui = &s_app.time_ui;
    if (ui->result == NULL) return;
    set_hidden(ui->result, !s_app.time_result);
    if (!s_app.time_result) return;

    char buf[64];
    label_set(ui->result_title, "时间到!");
    snprintf(buf, sizeof(buf), "得分 %ld · 最佳 %u\n命中 %d · 最高连击 %d",
             (long)s_app.time.score,
             (unsigned)s_app.progress.time_best,
             (int)s_app.time.correct_count, (int)s_app.time.best_combo);
    label_set(ui->result_detail, buf);
    for (int i = 0; i < TIME_RESULT_ITEMS; i++) {
        const bool selected = i == s_app.time_result_index;
        lv_obj_set_style_bg_color(ui->result_opt[i],
                                  lv_color_hex(selected ? COL_TILE_SEL : COL_TILE), 0);
        lv_obj_set_style_border_width(ui->result_opt[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(ui->result_opt[i], lv_color_hex(COL_GOLD), 0);
        label_color(ui->result_opt_text[i], selected ? COL_GOLD : COL_DIM);
    }
}

static void refresh_time(void)
{
    if (s_app.page != PAGE_TIME || s_app.root == NULL) return;
    time_ui_t *ui = &s_app.time_ui;
    const time_attack_t *run = &s_app.time;

    if (run->finished && !s_app.time_result) {
        // The tick normally reports this first; keep the result path single.
        finish_time_attack();
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u", (unsigned)(run->remaining_ms / 1000u),
             (unsigned)((run->remaining_ms % 1000u) / 100u));
    label_set(ui->timer_text, buf);
    label_color(ui->timer_text,
                run->remaining_ms <= 10000u ? COL_DANGER : COL_ACCENT);

    const int16_t fill =
        (int16_t)(226u * run->remaining_ms / TIME_ATTACK_MS);
    if (fill != s_app.ui_time_fill) {
        s_app.ui_time_fill = fill;
        lv_obj_set_width(ui->timer_fill, fill);
        lv_obj_set_style_bg_color(ui->timer_fill,
                                  lv_color_hex(run->remaining_ms <= 10000u
                                                   ? COL_DANGER
                                                   : COL_GOLD), 0);
    }

    snprintf(buf, sizeof(buf), "得分 %ld", (long)run->score);
    label_set(ui->score, buf);
    snprintf(buf, sizeof(buf), "连击 x%u", (unsigned)run->combo);
    label_set(ui->combo, buf);
    label_color(ui->combo, run->combo >= TIME_ATTACK_MASK_COMBO ? COL_GOLD : COL_DIM);

    const char *hazard = time_attack_hazard_text(run);
    const bool notice_live = s_app.time_notice_until_us > esp_timer_get_time();
    label_set(ui->hazard,
              notice_live ? s_app.time_notice
                          : (hazard[0] != '\0' ? hazard
                                               : "连击越高, 怪兽干扰越强"));
    label_color(ui->hazard,
                (notice_live || hazard[0] != '\0') ? COL_GOLD : COL_DIM);

    label_set(ui->question, run->question.text);
    label_set(ui->hint, run->question.hint);

    const bool feedback = run->phase == TIME_PHASE_FEEDBACK;
    for (int i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        const bool masked = time_attack_option_hidden(run, (uint8_t)i);
        const bool locked = time_attack_option_locked(run, (uint8_t)i);
        const bool selected = run->phase == TIME_PHASE_QUESTION &&
                              run->cursor == i;
        const bool correct = feedback && run->last_correct == i;
        const bool wrong = feedback && run->last_wrong == i;

        const uint8_t sig = (uint8_t)((selected ? 1u : 0u) |
                                      (correct ? 2u : 0u) |
                                      (wrong ? 4u : 0u) |
                                      (masked ? 8u : 0u) |
                                      (locked ? 16u : 0u));
        const int16_t value = run->question.options[i];
        if (sig == s_app.time_row_sig[i] && value == s_app.time_row_value[i]) {
            continue;
        }
        s_app.time_row_sig[i] = sig;
        s_app.time_row_value[i] = value;

        if (masked) {
            label_set(ui->row_text[i], "??");
        } else {
            snprintf(buf, sizeof(buf), "%d", (int)run->question.options[i]);
            label_set(ui->row_text[i], buf);
        }

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
        } else if (locked) {
            fg = COL_ACCENT;
        } else if (masked) {
            fg = COL_DIM;
        }
        lv_obj_set_style_bg_color(ui->row[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_width(ui->row[i], border, 0);
        lv_obj_set_style_border_color(ui->row[i], lv_color_hex(border_color), 0);
        label_color(ui->row_text[i], fg);
        set_hidden(ui->row_mark[i], !selected);
    }

    refresh_time_result();
}

static void finish_time_attack(void)
{
    if (s_app.time_result) return;
    s_app.time_result = true;
    s_app.time_result_index = 0;

    const uint32_t score = (uint32_t)(s_app.time.score < 0 ? 0 : s_app.time.score);
    const bool new_best = progress_record_time(&s_app.progress, score);
    const bool new_badge =
        score >= TIME_ATTACK_BADGE_SCORE &&
        progress_award(&s_app.progress, PROGRESS_BADGE_TIME);
    request_save();
    audio_player_play(new_best ? SFX_VICTORY : SFX_DEFEAT);

    char note[64];
    note[0] = '\0';
    if (new_best) {
        snprintf(note, sizeof(note), "新纪录! %s",
                 new_badge ? "获得徽章:限时猎手" : "");
    } else if (new_badge) {
        snprintf(note, sizeof(note), "获得徽章:限时猎手");
    }
    label_set(s_app.time_ui.result_note, note);
    label_color(s_app.time_ui.result_note, COL_GOLD);
}

static void handle_time_event(time_event_t event)
{
    switch (event) {
    case TIME_EV_MOVED:
        audio_player_play(SFX_MOVE);
        break;
    case TIME_EV_NEED_SELECT:
        audio_player_play(SFX_MOVE);
        time_notice("先按↑或↓选答案", 900000LL);
        break;
    case TIME_EV_UNLOCKED:
        audio_player_play(SFX_CRITICAL);
        time_notice("解冻!再按一次OK作答", 900000LL);
        break;
    case TIME_EV_SWAP:
        audio_player_play(SFX_GROWL);
        time_notice("怪兽换位!", 1000000LL);
        break;
    case TIME_EV_CORRECT:
        audio_player_play(SFX_CORRECT);
        break;
    case TIME_EV_WRONG:
        audio_player_play(SFX_WRONG);
        audio_player_play(SFX_GROWL);
        break;
    case TIME_EV_FINISHED:
        finish_time_attack();
        break;
    default:
        break;
    }
}

static void build_time(void)
{
    lv_obj_t *scr = make_screen();
    time_ui_t *ui = &s_app.time_ui;
    memset(ui, 0, sizeof(*ui));
    s_app.ui_time_fill = -1;
    for (int i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        s_app.time_row_sig[i] = 0xFF;
        s_app.time_row_value[i] = -1;
    }

    ui->battery = make_label(scr, 194, 6, 40, " --", &app_font_12, COL_DIM);
    lv_obj_set_style_text_align(ui->battery, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(scr, 8, 4, 120, "限时猎手", &app_font_20, COL_GOLD);
    ui->timer_text = make_label(scr, 128, 4, 66, "60.0", &app_font_20, COL_ACCENT);
    lv_obj_set_style_text_align(ui->timer_text, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *track = make_panel(scr, 6, 30, 228, 8, COL_EDGE, 4);
    ui->timer_fill = make_panel(track, 1, 1, 226, 6, COL_GOLD, 3);

    ui->score = make_label(scr, 6, 44, 120, "得分 0", &app_font_14, COL_TEXT);
    ui->combo = make_label(scr, 116, 44, 118, "连击 x0", &app_font_14, COL_DIM);
    lv_obj_set_style_text_align(ui->combo, LV_TEXT_ALIGN_RIGHT, 0);
    ui->hazard = make_center(scr, 0, 64, SCREEN_W, "", &app_font_12, COL_DIM);

    lv_obj_t *board = make_panel(scr, 6, 82, 228, 52, COL_PANEL, 8);
    lv_obj_set_style_border_width(board, 1, 0);
    lv_obj_set_style_border_color(board, lv_color_hex(COL_EDGE), 0);
    ui->question = make_center(board, 0, 4, 228, "", &app_font_20, COL_TEXT);
    ui->hint = make_center(board, 0, 32, 228, "", &app_font_12, COL_DIM);

    for (int i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        const int y = 140 + i * 34;
        ui->row[i] = make_panel(scr, 6, y, 228, 30, COL_TILE, 6);
        ui->row_text[i] = make_center(ui->row[i], 0, 4, 228, "", &app_font_20,
                                      COL_TEXT);
        ui->row_mark[i] = make_panel(ui->row[i], 7, 12, 6, 6, COL_GOLD, 3);
        set_hidden(ui->row_mark[i], true);
    }

    make_center(scr, 0, 288, SCREEN_W, "↑↓选择 · OK确定 · 长按OK结束",
                &app_font_12, COL_DIM);

    ui->result = make_panel(scr, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(ui->result, LV_OPA_70, 0);
    lv_obj_set_style_border_width(ui->result, 0, 0);
    lv_obj_t *card = make_panel(ui->result, 20, 64, 200, 192, COL_PANEL, 12);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_GOLD), 0);
    ui->result_title = make_center(card, 0, 12, 200, "", &app_font_20, COL_GOLD);
    ui->result_detail = make_center(card, 0, 42, 200, "", &app_font_14, COL_TEXT);
    ui->result_note = make_center(card, 0, 90, 200, "", &app_font_12, COL_GOLD);
    for (int i = 0; i < TIME_RESULT_ITEMS; i++) {
        const int y = 112 + i * 34;
        ui->result_opt[i] = make_panel(card, 16, y, 168, 28, COL_TILE, 6);
        ui->result_opt_text[i] = make_center(card, 16, y + 4, 168,
                                             i == 0 ? "再来一次" : "返回主页",
                                             &app_font_14, COL_DIM);
    }
    set_hidden(ui->result, true);

    page_switch(scr);
    refresh_time();
    set_battery_labels();
}

static void start_time_attack(void)
{
    s_app.mode = MODE_TIME;
    s_app.time_result = false;
    s_app.time_result_index = 0;
    s_app.time_notice_until_us = 0;
    s_app.page = PAGE_TIME;
    s_app.last_tick_us = esp_timer_get_time();
    // 低龄模式既降一档题目,也关掉烟雾/冰冻/换位。
    time_attack_start(&s_app.time, (uint32_t)esp_timer_get_time(),
                      s_app.low_age, !s_app.low_age);
    build_time();
    audio_player_play(SFX_CONFIRM);
}

static void leave_time(void)
{
    request_save();
    s_app.page = PAGE_HOME;
    build_home();
    audio_player_play(SFX_CONFIRM);
}

static void handle_time_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (s_app.time_result) {
        if (event == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            s_app.time_result_index =
                (uint8_t)((s_app.time_result_index + 1) % TIME_RESULT_ITEMS);
            audio_player_play(SFX_MOVE);
            refresh_time_result();
            return;
        }
        if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            if (s_app.time_result_index == 0) {
                start_time_attack();
            } else {
                leave_time();
            }
            return;
        }
        if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            leave_time();
        }
        return;
    }

    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        leave_time();
        return;
    }
    if (event == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        handle_time_event(time_attack_move(&s_app.time,
                                           btn == BSP_BTN_UP ? -1 : 1));
        refresh_time();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        handle_time_event(time_attack_confirm(&s_app.time));
        refresh_time();
    }
}

static void tick_time_attack(void)
{
    const int64_t now = esp_timer_get_time();
    uint32_t elapsed = (uint32_t)((now - s_app.last_tick_us) / 1000);
    s_app.last_tick_us = now;
    if (elapsed > 250) {
        elapsed = 250;
    }
    if (elapsed == 0) {
        elapsed = 1;
    }
    const time_event_t event = time_attack_tick(&s_app.time, elapsed);
    if (event != TIME_EV_NONE) {
        handle_time_event(event);
    }
    if (s_app.page == PAGE_TIME) {
        refresh_time();
    }
}

// ---------------------------------------------------------------------------
// 收藏图鉴
// ---------------------------------------------------------------------------
static void collection_tabs(lv_obj_t *scr)
{
    static const char *const tabs[COLLECTION_PAGES] = { "英雄", "怪兽", "徽章" };
    for (int i = 0; i < COLLECTION_PAGES; i++) {
        const bool selected = i == s_app.collection_page;
        const int x = 8 + i * 76;
        s_app.collection.tab[i] = make_panel(scr, x, 30, 72, 20,
                                             selected ? COL_TILE_SEL : COL_TILE, 6);
        lv_obj_set_style_border_width(s_app.collection.tab[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_app.collection.tab[i],
                                      lv_color_hex(COL_ACCENT), 0);
        s_app.collection.tab_text[i] = make_center(scr, x, 33, 72, tabs[i],
                                                   &app_font_12,
                                                   selected ? COL_TEXT : COL_DIM);
    }
}

static void style_collection_cell(lv_obj_t *cell, bool selected)
{
    lv_obj_set_style_bg_color(cell, lv_color_hex(selected ? COL_TILE_SEL : COL_TILE), 0);
    lv_obj_set_style_border_width(cell, selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(COL_ACCENT), 0);
}

static uint8_t collection_page_count(void)
{
    if (s_app.collection_page == 0) {
        return COLLECTION_HERO_CELLS;
    }
    if (s_app.collection_page == 1) {
        return COLLECTION_MONSTER_CELLS;
    }
    return COLLECTION_BADGE_CELLS;
}

static bool collection_cell_found(uint8_t index)
{
    if (s_app.collection_page == 0) {
        return (s_app.progress.heroes & (1u << index)) != 0;
    }
    if (s_app.collection_page == 1) {
        return (s_app.progress.monsters & (uint16_t)(1u << index)) != 0;
    }
    return progress_has_badge(&s_app.progress, index);
}

static void build_collection_heroes(lv_obj_t *scr)
{
    collection_ui_t *ui = &s_app.collection;
    // 18 heroes in two columns of nine; the step shrinks so no scrolling is
    // needed on the 240x320 panel.
    for (uint8_t i = 0; i < COLLECTION_HERO_CELLS; i++) {
        const int col = i % 2;
        const int row = i / 2;
        const int x = col == 0 ? 6 : 120;
        const int y = 52 + row * 26;
        const bool found = collection_cell_found(i);
        ui->cell[i] = make_panel(scr, x, y, 114, 24, COL_TILE, 5);
        style_collection_cell(ui->cell[i], i == s_app.collection_index);
        ui->cell_img[i] = lv_image_create(ui->cell[i]);
        lv_image_set_src(ui->cell_img[i], sprites_hero(i, SPRITE_POSE_IDLE));
        lv_obj_set_pos(ui->cell_img[i], 3, 1);
        lv_obj_set_size(ui->cell_img[i], 20, 22);
        if (!found) {
            lv_obj_set_style_image_opa(ui->cell_img[i], LV_OPA_30, 0);
        }
        ui->cell_title[i] = make_label(ui->cell[i], 25, 5, 87,
                                       found ? campaign_hero(i)->short_name : "???",
                                       &app_font_12, found ? COL_TEXT : COL_DIM);
    }
}

static void build_collection_monsters(lv_obj_t *scr)
{
    collection_ui_t *ui = &s_app.collection;
    // 11 monsters in two columns; the second line keeps the domain readable.
    for (uint8_t i = 0; i < COLLECTION_MONSTER_CELLS; i++) {
        const int col = i % 2;
        const int row = i / 2;
        const int x = col == 0 ? 6 : 120;
        const int y = 52 + row * 38;
        const bool found = collection_cell_found(i);
        ui->cell[i] = make_panel(scr, x, y, 114, 36, COL_TILE, 5);
        style_collection_cell(ui->cell[i], i == s_app.collection_index);
        ui->cell_img[i] = lv_image_create(ui->cell[i]);
        lv_image_set_src(ui->cell_img[i], sprites_monster(i, SPRITE_MONSTER_IDLE));
        lv_obj_set_pos(ui->cell_img[i], 3, 2);
        lv_obj_set_size(ui->cell_img[i], 26, 26);
        if (!found) {
            lv_obj_set_style_image_opa(ui->cell_img[i], LV_OPA_30, 0);
        }
        char name[32];
        if (found) {
            snprintf(name, sizeof(name), "%d %s", (int)i + 1,
                     campaign_monster(i)->name);
        } else {
            snprintf(name, sizeof(name), "%d ???", (int)i + 1);
        }
        ui->cell_title[i] = make_label(ui->cell[i], 32, 1, 80, name,
                                       &app_font_12, found ? COL_TEXT : COL_DIM);
        const char *detail = "未遭遇";
        if (found) {
            detail = (s_app.shop_flags & SHOP_GALLERY_LORE) != 0
                         ? campaign_monster(i)->domain
                         : "已遭遇";
        }
        ui->cell_detail[i] = make_label(ui->cell[i], 32, 17, 80, detail,
                                        &app_font_12,
                                        found ? COL_OK : COL_DIM);
    }
}

static void build_collection_badges(lv_obj_t *scr)
{
    collection_ui_t *ui = &s_app.collection;
    for (uint8_t i = 0; i < COLLECTION_BADGE_CELLS; i++) {
        const int y = 52 + i * 38;
        const bool earned = progress_has_badge(&s_app.progress, i);
        ui->cell[i] = make_panel(scr, 6, y, 228, 36, COL_TILE, 6);
        style_collection_cell(ui->cell[i], i == s_app.collection_index);
        ui->cell_title[i] = make_label(ui->cell[i], 10, 3, 150,
                                       progress_badge_name(i), &app_font_14,
                                       earned ? COL_GOLD : COL_DIM);
        ui->cell_detail[i] = make_label(ui->cell[i], 10, 21, 170,
                                        progress_badge_detail(i), &app_font_12,
                                        COL_DIM);
        lv_obj_t *mark = make_label(ui->cell[i], 194, 7, 26, earned ? "★" : "☆",
                                    &app_font_20, earned ? COL_GOLD : COL_DIM);
        (void)mark;
    }
}

static void build_collection(void)
{
    lv_obj_t *scr = make_screen();
    memset(&s_app.collection, 0, sizeof(s_app.collection));
    const uint8_t count = collection_page_count();
    if (s_app.collection_index >= count) {
        s_app.collection_index = 0;
    }

    s_app.collection.battery = make_label(scr, 194, 6, 40, " --", &app_font_12,
                                          COL_DIM);
    lv_obj_set_style_text_align(s_app.collection.battery, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(scr, 8, 5, 150, "收藏图鉴", &app_font_20, COL_TEXT);
    collection_tabs(scr);

    if (s_app.collection_page == 0) {
        build_collection_heroes(scr);
    } else if (s_app.collection_page == 1) {
        build_collection_monsters(scr);
    } else {
        build_collection_badges(scr);
    }

    const char *footer = s_app.collection_page == 2
                             ? "↑↓选择 · 长按OK返回"
                             : "↑↓选择 · OK预览 · 长按OK返回";
    make_center(scr, 0, 300, SCREEN_W, footer, &app_font_12, COL_DIM);

    page_switch(scr);
    set_battery_labels();
}

// ---------------------------------------------------------------------------
// 图鉴动作预览
// ---------------------------------------------------------------------------
static uint8_t preview_order(uint8_t *order)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < ACTION_COUNT; i++) {
        if (action_supported((action_character_t)s_app.preview.kind,
                             s_app.preview.index, (action_id_t)i)) {
            order[count++] = i;
        }
    }
    return count;
}

static uint8_t preview_action_count(void)
{
    uint8_t order[ACTION_COUNT];
    return preview_order(order);
}

static uint8_t preview_action_ordinal(void)
{
    uint8_t ordinal = 0;
    for (uint8_t i = 0; i < ACTION_COUNT; i++) {
        if (!action_supported((action_character_t)s_app.preview.kind,
                              s_app.preview.index, (action_id_t)i)) {
            continue;
        }
        if (i == s_app.preview.action_id) {
            return ordinal;
        }
        ordinal++;
    }
    return 0;
}

static void preview_show_frame(const action_seq_t *seq, uint8_t frame)
{
    if (frame >= seq->count) {
        frame = (uint8_t)(seq->count - 1u);
    }
    if (s_app.preview.image == NULL) {
        return;
    }
    if (s_app.preview.kind == ACTION_CHARACTER_HERO) {
        lv_image_set_src(s_app.preview.image,
                         sprites_hero(s_app.preview.index,
                                      (sprite_pose_t)seq->frames[frame].pose));
    } else {
        lv_image_set_src(s_app.preview.image,
                         sprites_monster(s_app.preview.index,
                                         (sprite_monster_pose_t)seq->frames[frame].pose));
    }
}

static void preview_restart(action_id_t action)
{
    s_app.preview.action_id = (uint8_t)action;
    s_app.preview.frame = 0xFF;
    s_app.preview.action_at_us = esp_timer_get_time();
    s_app.preview.frame_at_us = s_app.preview.action_at_us;
}

static void refresh_preview(void)
{
    if (s_app.page != PAGE_PREVIEW || s_app.preview.image == NULL) {
        return;
    }
    const action_character_t kind = (action_character_t)s_app.preview.kind;
    const action_seq_t *seq =
        action_sequence(kind, s_app.preview.index,
                        (action_id_t)s_app.preview.action_id);
    const int64_t now = esp_timer_get_time();
    const uint32_t elapsed =
        now > s_app.preview.action_at_us
            ? (uint32_t)((now - s_app.preview.action_at_us) / 1000)
            : 0;
    uint32_t cursor = 0;
    uint8_t frame = 0;
    for (uint8_t i = 0; i < seq->count; i++) {
        if (elapsed < cursor + seq->frames[i].duration_ms) {
            frame = i;
            break;
        }
        cursor += seq->frames[i].duration_ms;
        frame = i;
    }
    if (frame != s_app.preview.frame) {
        s_app.preview.frame = frame;
        preview_show_frame(seq, frame);
    }
    if (s_app.preview.action != NULL) {
        label_set(s_app.preview.action,
                  action_label((action_id_t)s_app.preview.action_id));
    }
    if (s_app.preview.detail != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "动作 %u/%u · ↑↓切换 · OK重播",
                 (unsigned)preview_action_ordinal() + 1u,
                 (unsigned)preview_action_count());
        label_set(s_app.preview.detail, buf);
    }
    if (elapsed >= seq->total_ms + 260u) {
        // 自动播放下一个动作; 手动切换时会被 preview_step 重置。
        uint8_t order[ACTION_COUNT];
        const uint8_t count = preview_order(order);
        if (count > 0) {
            uint8_t pos = 0;
            for (uint8_t i = 0; i < count; i++) {
                if (order[i] == s_app.preview.action_id) {
                    pos = i;
                    break;
                }
            }
            pos = (uint8_t)((pos + 1u) % count);
            preview_restart((action_id_t)order[pos]);
            s_app.preview.frame = 0xFF;
            refresh_preview();
        }
    }
}

static void preview_step(int delta)
{
    uint8_t order[ACTION_COUNT];
    const uint8_t count = preview_order(order);
    if (count == 0) {
        return;
    }
    uint8_t pos = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (order[i] == s_app.preview.action_id) {
            pos = i;
            break;
        }
    }
    pos = (uint8_t)((pos + (uint8_t)(delta < 0 ? count - 1 : 1)) % count);
    preview_restart((action_id_t)order[pos]);
    audio_player_play(SFX_MOVE);
    refresh_preview();
}

static void build_preview(void)
{
    lv_obj_t *scr = make_screen();
    s_app.preview.battery = make_label(scr, 194, 6, 40, " --", &app_font_12,
                                       COL_DIM);
    lv_obj_set_style_text_align(s_app.preview.battery, LV_TEXT_ALIGN_RIGHT, 0);

    const bool hero = s_app.preview.kind == ACTION_CHARACTER_HERO;
    const char *name = hero ? campaign_hero(s_app.preview.index)->short_name
                            : campaign_monster(s_app.preview.index)->name;
    s_app.preview.title = make_label(scr, 8, 5, 180, name, &app_font_20, COL_TEXT);

    const int w = hero ? 72 : 80;
    const int h = hero ? 88 : 80;
    lv_obj_t *frame = make_panel(scr, (SCREEN_W - w - 12) / 2, 44, w + 12, h + 12,
                                 COL_PANEL, 10);
    s_app.preview.image = lv_image_create(frame);
    lv_obj_set_pos(s_app.preview.image, 6, 6);
    lv_obj_set_size(s_app.preview.image, w, h);
    s_app.preview.action = make_center(scr, 0, 158, SCREEN_W, "", &app_font_20,
                                       COL_GOLD);
    s_app.preview.detail = make_center(scr, 0, 194, SCREEN_W, "", &app_font_12,
                                       COL_DIM);
    make_center(scr, 0, 226, SCREEN_W,
                hero ? "英雄动作预览" : "怪兽动作预览", &app_font_14, COL_ACCENT);
    s_app.preview.footer = make_center(scr, 0, 300, SCREEN_W,
                                       "↑↓切换动作 · OK重播 · 长按OK返回",
                                       &app_font_12, COL_DIM);

    page_switch(scr);
    set_battery_labels();
    refresh_preview();
}

static void tick_preview(void)
{
    refresh_preview();
}

// 图鉴预览使用同一套动作表; 选中角色后循环播放它支持的全部动作。

static void open_preview(void)
{
    if (s_app.collection_page == 2) {
        return;
    }
    const uint8_t index = s_app.collection_index;
    const bool found = collection_cell_found(index);
    if (!found) {
        audio_player_play(SFX_WRONG);
        return;
    }
    s_app.preview.kind = s_app.collection_page == 0 ? ACTION_CHARACTER_HERO
                                                    : ACTION_CHARACTER_MONSTER;
    s_app.preview.index = index;
    preview_restart(ACTION_IDLE);
    s_app.preview.frame = 0xFF;
    s_app.page = PAGE_PREVIEW;
    audio_player_play(SFX_CONFIRM);
    build_preview();
}

static void handle_preview_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        preview_step(btn == BSP_BTN_UP ? -1 : 1);
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        preview_restart((action_id_t)s_app.preview.action_id);
        s_app.preview.frame = 0xFF;
        audio_player_play(SFX_CONFIRM);
        refresh_preview();
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_app.page = PAGE_COLLECTION;
        audio_player_play(SFX_CONFIRM);
        build_collection();
    }
}

static void handle_collection_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        const int delta = btn == BSP_BTN_UP ? -1 : 1;
        const int next = (int)s_app.collection_index + delta;
        if (next < 0) {
            s_app.collection_page =
                (uint8_t)((s_app.collection_page + COLLECTION_PAGES - 1) %
                          COLLECTION_PAGES);
            s_app.collection_index = (uint8_t)(collection_page_count() - 1u);
        } else if (next >= (int)collection_page_count()) {
            s_app.collection_page =
                (uint8_t)((s_app.collection_page + 1) % COLLECTION_PAGES);
            s_app.collection_index = 0;
        } else {
            s_app.collection_index = (uint8_t)next;
        }
        audio_player_play(SFX_MOVE);
        build_collection();
        return;
    }
    if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        open_preview();
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_app.page = PAGE_SETTINGS;
        build_settings();
        audio_player_play(SFX_CONFIRM);
    }
}

// ---------------------------------------------------------------------------
// 战斗界面
// ---------------------------------------------------------------------------
static void refresh_battle_texts(void)
{
    battle_t *battle = &s_app.battle;
    const int64_t now = esp_timer_get_time();

    char buf[80];
    if (s_app.mode == MODE_HUNDRED) {
        // 百减模式顶栏直接说明练习范围, 不再显示关卡序号。
        label_set(s_app.battle_ui.stage_label, "百减模式｜100减法专项");
        label_color(s_app.battle_ui.stage_label, COL_ACCENT);
        label_set(s_app.battle_ui.monster_name, "");
    } else {
        snprintf(buf, sizeof(buf), "第%d关", (int)battle->stage + 1);
        label_set(s_app.battle_ui.stage_label, buf);
        label_color(s_app.battle_ui.stage_label, COL_DIM);
        label_set(s_app.battle_ui.monster_name,
                  campaign_monster_name(battle->monster));
    }

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
        if (battle_beam_elapsed_ms(battle) + BEAM_SWEEP_MS <
            battle_beam_beat_ms(0)) {
            hint = "准备!听到滴声就按OK";
        } else if (beats > 0) {
            snprintf(buf, sizeof(buf), "还剩%d拍 · 金色区按OK", beats);
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

    const int32_t total = battle->question_ms > 0 ? battle->question_ms : 1;
    const int32_t remain = battle->question_remaining_ms;
    if (total != s_app.ui_timer_total) {
        s_app.ui_timer_total = total;
        lv_bar_set_range(s_app.battle_ui.timer, 0, total);
    }
    const int32_t timer_value = remain > 0 ? remain : 0;
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
        label_set(s_app.battle_ui.footer, "听到滴声按一下OK");
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
        return "梦比优斯 · 梦比姆射线";
    case 3:
        return "泽塔 · 泽斯蒂姆光线";
    case 4:
        return "迪迦 · 泽佩利敖光线";
    case 5:
        return "贝利亚 · 帝斯修姆光线";
    case 6:
        return "捷德 · 皇家终结";
    case 7:
        return "赛文 · 艾梅利姆光线";
    case 8:
        return "银河 · 银河交叉射线";
    case 9:
        return "欧布 · 斯派修姆泽佩利敖";
    case 10:
        return "欧布 · 斯特比姆爆炸";
    case 11:
        return "捷德 · 银河冲击";
    case 12:
        return "特利迦 · 泽佩利敖光线";
    case 13:
        return "德凯 · 塞尔杰特光线";
    case 14:
        return "布莱泽 · 螺旋光矛";
    case 15:
        return "究极赛罗 · 赛罗终极剑";
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

static void show_beam_pips(bool show)
{
    if (s_app.ui_pips_visible == show) {
        return;
    }
    s_app.ui_pips_visible = show;
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        set_hidden(s_app.battle_ui.beam_pip[i], !show);
    }
}

static void refresh_beam_pips(const battle_t *battle)
{
    const int next = BEAM_BEAT_COUNT - (int)battle_beam_beats_left(battle);
    uint16_t sig = 0;
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        const uint8_t judge = (uint8_t)battle_beam_judge(battle, (uint8_t)i);
        uint8_t state = 0;  // pending
        if (judge == BEAM_JUDGE_PERFECT) {
            state = 1;
        } else if (judge == BEAM_JUDGE_HIT) {
            state = 2;
        } else if (judge == BEAM_JUDGE_MISS) {
            state = 3;
        } else if (i == next) {
            state = 4;
        }
        sig |= (uint16_t)(state << (i * 3));
    }
    if ((int16_t)sig == s_app.ui_beam_pips) {
        return;
    }
    s_app.ui_beam_pips = (int16_t)sig;
    static const uint32_t PIP_COLORS[5] = {COL_EDGE, 0xFFF3B0, COL_OK,
                                           COL_DANGER, COL_GOLD};
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        const uint8_t state = (uint8_t)((sig >> (i * 3)) & 0x7u);
        lv_obj_set_style_bg_color(s_app.battle_ui.beam_pip[i],
                                  lv_color_hex(PIP_COLORS[state]), 0);
    }
}

static void refresh_battle_action(void)
{
    battle_t *battle = &s_app.battle;

    if (battle->phase != BATTLE_PHASE_BEAM) {
        show_beam_pips(false);
    }

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

        const uint32_t elapsed = battle_beam_elapsed_ms(battle);
        const uint32_t first_beat = battle_beam_beat_ms(0);
        const int beats = (int)battle_beam_beats_left(battle);
        char sub[48];
        if (elapsed + BEAM_SWEEP_MS < first_beat) {
            // Count the player in before the marker reaches the track.
            const uint32_t until = first_beat - elapsed - BEAM_SWEEP_MS;
            snprintf(sub, sizeof(sub), "准备 %u",
                     (unsigned)((until + 999u) / 1000u));
        } else if (beats > 0) {
            snprintf(sub, sizeof(sub), "已接 %d/%d 拍 · 还剩 %d",
                     (int)battle_beam_clean(battle),
                     (int)battle_beam_goal(battle), beats);
        } else {
            snprintf(sub, sizeof(sub), "已接 %d/%d 拍",
                     (int)battle_beam_clean(battle),
                     (int)battle_beam_goal(battle));
        }
        label_set(s_app.battle_ui.action_sub, sub);

        // Reuse the critical bar: one gold band, one marker per beat, so the
        // beam uses the same "press when the marker crosses the gold" rule the
        // player already learned.
        set_hidden(s_app.battle_ui.crit_track, false);
        set_hidden(s_app.battle_ui.crit_marker, false);
        set_hidden(s_app.battle_ui.beam_track, true);
        set_action_zone(BEAM_ZONE_LEFT, BEAM_ZONE_WIDTH);

        const uint32_t next = battle_beam_next_beat_ms(battle);
        int marker = 50;
        if (next != UINT32_MAX) {
            const int32_t start = (int32_t)next - BEAM_SWEEP_MS;
            const int32_t span = BEAM_SWEEP_MS * 2;
            int32_t offset = (int32_t)elapsed - start;
            if (offset < 0) {
                offset = 0;
            }
            if (offset > span) {
                offset = span;
            }
            marker = (int)(offset * 100 / span);
        }
        lv_obj_set_pos(s_app.battle_ui.crit_marker,
                       14 + marker * 200 / 100 - 2, 32);

        show_beam_pips(true);
        refresh_beam_pips(battle);
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
                         sprites_monster(battle->monster,
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

// Advance the presentation-only strike rotation. Every answered strike takes
// the next silhouette, independent of critical quality, so hitting the perfect
// zone on every question still produces different actions. Combo rewards keep
// their dedicated moves.
static anim_id_t take_hero_strike(const battle_t *battle)
{
    if (battle->combo_finisher || battle->crit_pursuit) {
        // Special moves do not consume a rotation slot.
        return anim_hero_strike(0, battle->combo_finisher, battle->crit_pursuit);
    }
    const anim_id_t id = anim_hero_strike(s_app.strike_seq, false, false);
    s_app.strike_seq++;
    return id;
}

// Enemy attacks also alternate: a direct roar-pounce or a charge-up pounce.
static anim_id_t enemy_attack_anim(const battle_t *battle)
{
    return anim_enemy_strike(
        (uint8_t)(battle->wrong_count + battle->stage));
}

static void schedule_impact(impact_kind_t kind, int damage)
{
    anim_id_t id = s_app.strike_anim;
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

// ---------------------------------------------------------------------------
// 事件驱动动作层
//
// 这一层只改写最终交给 apply_poses() 的姿势, 不推进战斗状态、不拦截按键,
// 因此动画播放期间玩家仍然可以正常答题。动作按周目解锁; 未解锁或素材缺失
// 时直接跳过, 由原有的 phase 动画兜底。
// ---------------------------------------------------------------------------
static bool action_play_start(action_play_t *play, action_character_t kind,
                              uint8_t index, action_id_t action)
{
    if (!action_is_unlocked(action, s_app.cycle)) {
        return false;
    }
    if (!action_supported(kind, index, action)) {
        return false;
    }
    play->active = true;
    play->action = (uint8_t)action;
    play->start_us = esp_timer_get_time();
    return true;
}

static bool action_pose_at(const action_play_t *play, action_character_t kind,
                           uint8_t index, uint32_t now_ms, uint8_t *pose)
{
    if (!play->active) {
        return false;
    }
    const action_seq_t *seq =
        action_sequence(kind, index, (action_id_t)play->action);
    if (seq == NULL || seq->frames == NULL || seq->count == 0) {
        return false;
    }
    const uint32_t start_ms = (uint32_t)(play->start_us / 1000);
    const uint32_t elapsed = now_ms >= start_ms ? now_ms - start_ms : 0;
    if (elapsed >= seq->total_ms) {
        return false;
    }
    uint32_t cursor = 0;
    for (uint8_t i = 0; i < seq->count; i++) {
        if (elapsed < cursor + seq->frames[i].duration_ms) {
            *pose = seq->frames[i].pose;
            return true;
        }
        cursor += seq->frames[i].duration_ms;
    }
    *pose = seq->frames[seq->count - 1].pose;
    return true;
}

static void action_on_question_ready(void)
{
    battle_t *battle = &s_app.battle;
    (void)action_play_start(&s_app.hero_action, ACTION_CHARACTER_HERO,
                            battle->hero, ACTION_READY);
    (void)action_play_start(&s_app.monster_action, ACTION_CHARACTER_MONSTER,
                            battle->monster, ACTION_READY);
}

static void action_on_correct(void)
{
    battle_t *battle = &s_app.battle;
    const action_id_t hero_action =
        battle->combo >= 2 ? ACTION_COMBO : ACTION_ANSWER_CORRECT;
    (void)action_play_start(&s_app.hero_action, ACTION_CHARACTER_HERO,
                            battle->hero, hero_action);
    // 每次答对都让怪兽进入受击硬直, 连对就连续播放。
    (void)action_play_start(&s_app.monster_action, ACTION_CHARACTER_MONSTER,
                            battle->monster, ACTION_HURT);
}

static void action_on_wrong(void)
{
    battle_t *battle = &s_app.battle;
    (void)action_play_start(&s_app.hero_action, ACTION_CHARACTER_HERO,
                            battle->hero, ACTION_ANSWER_WRONG);
    (void)action_play_start(&s_app.monster_action, ACTION_CHARACTER_MONSTER,
                            battle->monster, ACTION_TAUNT);
}

static void action_on_monster_defeated(void)
{
    battle_t *battle = &s_app.battle;
    // 终结一击: 英雄完整必杀, 怪兽交给 KO 淡出, 不再叠加受击动作。
    (void)action_play_start(&s_app.hero_action, ACTION_CHARACTER_HERO,
                            battle->hero, ACTION_ULTIMATE);
    s_app.monster_action.active = false;
}

static void action_on_stage_clear(void)
{
    battle_t *battle = &s_app.battle;
    (void)action_play_start(&s_app.hero_action, ACTION_CHARACTER_HERO,
                            battle->hero, ACTION_VICTORY);
}

static void schedule_easter_egg(void)
{
    if (!s_app.action_easter_egg) {
        s_app.easter_egg_at_us = 0;
        return;
    }
    const uint32_t low = action_easter_egg_min_ms();
    const uint32_t high = action_easter_egg_max_ms();
    const uint32_t delay =
        low + math_rng_below(&s_app.action_rng, high > low ? high - low + 1u : 1u);
    s_app.easter_egg_at_us = esp_timer_get_time() + (int64_t)delay * 1000;
}

static void tick_easter_egg(int64_t now)
{
    battle_t *battle = &s_app.battle;
    if (!s_app.action_easter_egg || battle->phase != BATTLE_PHASE_QUESTION ||
        s_app.ready_until_us > 0 || s_app.tutorial != TUTORIAL_NONE ||
        s_app.overlay != OVERLAY_NONE) {
        return;
    }
    if (s_app.easter_egg_at_us == 0 || now < s_app.easter_egg_at_us) {
        return;
    }
    s_app.easter_egg_rolls++;
    const uint8_t percent = action_easter_egg_percent(s_app.cycle);
    if (math_rng_below(&s_app.action_rng, 100) < percent) {
        if (math_rng_below(&s_app.action_rng, 2) == 0) {
            (void)action_play_start(&s_app.hero_action, ACTION_CHARACTER_HERO,
                                    battle->hero, ACTION_EASTER_EGG);
        } else {
            (void)action_play_start(&s_app.monster_action,
                                    ACTION_CHARACTER_MONSTER, battle->monster,
                                    ACTION_EASTER_EGG);
        }
    }
    schedule_easter_egg();
}

static void maybe_action_question_ready(void)
{
    battle_t *battle = &s_app.battle;
    if (battle->phase != BATTLE_PHASE_QUESTION || s_app.ready_until_us > 0 ||
        s_app.tutorial != TUTORIAL_NONE || s_app.overlay != OVERLAY_NONE) {
        return;
    }
    if (s_app.action_question_index == battle->question_index) {
        return;
    }
    s_app.action_question_index = battle->question_index;
    action_on_question_ready();
    // 彩蛋计时跨题保留: 题目通常短于 8 秒, 每题重置会让彩蛋永远不触发。
    if (s_app.easter_egg_at_us == 0) {
        schedule_easter_egg();
    }
}

static void apply_action_overrides(anim_frame_t *frame)
{
    battle_t *battle = &s_app.battle;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    uint8_t pose = 0;
    if (s_app.hero_action.active) {
        if (action_pose_at(&s_app.hero_action, ACTION_CHARACTER_HERO,
                           battle->hero, now_ms, &pose)) {
            frame->hero_pose = pose;
        } else {
            s_app.hero_action.active = false;
        }
    }
    if (s_app.monster_action.active) {
        // 怪兽出手和 KO 淡出时必须让位给原有演出。
        if (battle->phase == BATTLE_PHASE_ENEMY ||
            battle->phase == BATTLE_PHASE_CLEAR ||
            battle->phase == BATTLE_PHASE_OVER) {
            s_app.monster_action.active = false;
        } else if (action_pose_at(&s_app.monster_action, ACTION_CHARACTER_MONSTER,
                                  battle->monster, now_ms, &pose)) {
            frame->monster_pose = pose;
        } else {
            s_app.monster_action.active = false;
        }
    }
}

static void refresh_battle_animation(void)
{
    battle_t *battle = &s_app.battle;
    maybe_action_question_ready();
    tick_easter_egg(esp_timer_get_time());
    const int64_t now = esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(now / 1000);

    anim_id_t id = ANIM_IDLE;
    uint32_t duration = 1000;
    uint32_t elapsed = 0;
    bool idle_breath = false;
    bool guard_pose = false;

    switch (battle->phase) {
    case BATTLE_PHASE_ATTACK:
        id = s_app.strike_anim;
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
        // 击败后直接进入 KO 演出: 光线扫射、闪白抖动、碎裂淡出。
        id = battle->boss ? ANIM_BOSS_KO : ANIM_KO;
        duration = battle_clear_ms(battle);
        elapsed = duration > (uint32_t)battle->phase_ms
                      ? duration - (uint32_t)battle->phase_ms
                      : duration;
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
    // 只覆盖姿势, 保留 hit-stop、闪白、震屏和 KO 淡出等原有演出。
    apply_action_overrides(&frame);

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
    if (frame.monster_fade != s_app.ui_monster_fade) {
        s_app.ui_monster_fade = frame.monster_fade;
        lv_obj_set_style_opa(
            s_app.battle_ui.monster_img,
            (lv_opa_t)(255u - (uint32_t)frame.monster_fade * 255u / 100u), 0);
        // BOSS 碎裂时补两批星星粒子, 普通怪只在最后一帧闪一下。
        if (battle->boss && frame.monster_fade >= 60 && s_app.ko_fx_step < 1) {
            s_app.ko_fx_step = 1;
            spawn_popup("★", COL_GOLD, MONSTER_X - ARENA_X + 8, 28);
            spawn_popup("★", COL_GOLD, MONSTER_X - ARENA_X + 30, 44);
            spawn_popup("★", COL_ACCENT, MONSTER_X - ARENA_X + 20, 12);
        }
        if (battle->boss && frame.monster_fade >= 90 && s_app.ko_fx_step < 2) {
            s_app.ko_fx_step = 2;
            spawn_popup("★", COL_GOLD, MONSTER_X - ARENA_X + 38, 30);
            spawn_popup("★", COL_GOLD, MONSTER_X - ARENA_X + 12, 50);
        }
        if (!battle->boss && frame.monster_fade >= 85 && s_app.ko_fx_step < 1) {
            s_app.ko_fx_step = 1;
            spawn_popup("★", COL_GOLD, MONSTER_X - ARENA_X + 22, 30);
        }
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

    // 百减模式的顶栏标题更长, 占用怪兽名原本的位置; 其他模式保持 48 px。
    const int title_w = s_app.mode == MODE_HUNDRED ? 188 : 48;
    ui->stage_label = make_label(scr, 6, 4, title_w, "", &app_font_12, COL_DIM);
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

    ui->beam_glow = make_panel(ui->stage, 80, 60, 50, 16,
                               (s_app.shop_flags & SHOP_GOLD_BEAM) != 0
                                   ? COL_GOLD
                                   : COL_ACCENT,
                               6);
    lv_obj_set_style_bg_opa(ui->beam_glow, LV_OPA_50, 0);
    ui->beam_core = make_panel(ui->stage, 80, 63, 50, 8,
                               (s_app.shop_flags & SHOP_GOLD_BEAM) != 0
                                   ? COL_GOLD
                                   : COL_TEXT,
                               4);
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
    ui->action_sub = make_center(ui->action, 0, 68, ACTION_W, "", &app_font_12,
                                 COL_DIM);

    ui->crit_track = make_panel(ui->action, 14, 38, 200, 14, COL_TILE, 7);
    lv_obj_add_flag(ui->crit_track, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    ui->crit_zone = make_panel(ui->crit_track, 90, 2, 20, 10, COL_GOLD, 5);
    ui->crit_perfect = make_panel(ui->crit_track, 94, 3, 10, 8, 0xFFF3B0, 4);
    ui->crit_marker = make_panel(ui->action, 112, 32, 4, 22, COL_TEXT, 2);

    ui->beam_track = make_panel(ui->action, 34, 40, 160, 10, COL_TILE, 5);
    ui->beam_fill = make_panel(ui->beam_track, 0, 2, 0, 6, COL_DANGER, 3);

    // Five dots mirror the five beats: gold marks the beat being played,
    // green a clean hit, red a miss.
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        ui->beam_pip[i] = make_panel(ui->action, 70 + i * 20, 55, 8, 8,
                                     COL_EDGE, 4);
        set_hidden(ui->beam_pip[i], true);
    }

    // Coaching card for the first two campaign stages. The dim layer freezes
    // the scene visually while the battle clock is held by tick_battle().
    ui->tutorial = make_panel(scr, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(ui->tutorial, LV_OPA_70, 0);
    lv_obj_set_style_border_width(ui->tutorial, 0, 0);
    set_hidden(ui->tutorial, true);
    lv_obj_t *card = make_panel(ui->tutorial, 20, 65, 200, 190, COL_PANEL, 12);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_GOLD), 0);
    ui->tutorial_title = make_center(card, 0, 12, 200, "", &app_font_20,
                                     COL_GOLD);
    ui->tutorial_body = make_label(card, 12, 46, 176, "", &app_font_14,
                                   COL_TEXT);
    lv_obj_set_style_text_align(ui->tutorial_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui->tutorial_body, LV_LABEL_LONG_WRAP);
    ui->tutorial_footer = make_center(card, 0, 164, 200, "", &app_font_12,
                                      COL_DIM);

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
    s_app.ui_beam_pips = -1;
    s_app.ui_pips_visible = false;
    s_app.beam_cue_next = 0;
    s_app.tutorial = TUTORIAL_NONE;

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
        return s_app.cycle_secret_pending ? 3 : 2;
    case OVERLAY_OVER:
    case OVERLAY_WAVE:
        return 2;
    case OVERLAY_CYCLE:
        return 1;
    default:
        return 0;
    }
}

static bool overlay_can_review(void)
{
    return s_app.overlay != OVERLAY_PAUSE &&
           s_app.overlay != OVERLAY_WAVE &&
           s_app.overlay != OVERLAY_CYCLE &&
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
        static const char *const next_items[] = { "继续前进", "回主页" };
        static const char *const last_items[] = { "查看周目结算", "回主页" };
        static const char *const secret_items[] = {
            "迎战隐藏BOSS", "跳过并结算", "回主页"
        };
        const bool last = s_app.battle.stage + 1 >= CAMPAIGN_STAGE_COUNT;
        const char *const *items = last ? last_items : next_items;
        uint8_t count = 2;
        if (s_app.cycle_secret_pending) {
            items = secret_items;
            count = 3;
        }
        return index < count ? items[index] : "";
    }
    if (s_app.overlay == OVERLAY_CYCLE) {
        return index == 0 ? "开启下一轮冒险" : "";
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

static cycle_grade_t run_cycle_grade(void)
{
    return cycle_grade(s_app.run_correct, s_app.run_wrong, s_app.run_fastest);
}

static void build_cycle_overlay(void)
{
    s_app.overlay_root = make_panel(s_app.root, 0, 0, SCREEN_W, SCREEN_H,
                                   0x000000, 0);
    lv_obj_set_style_bg_opa(s_app.overlay_root, LV_OPA_70, 0);
    lv_obj_t *card = make_panel(s_app.overlay_root, 12, 12, 216, 296,
                                COL_PANEL, 12);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_GOLD), 0);

    char buf[64];
    snprintf(buf, sizeof(buf), "第 %u 周目完成", (unsigned)s_app.cycle);
    make_center(card, 0, 8, 216, buf, &app_font_20, COL_GOLD);

    const cycle_grade_t grade = run_cycle_grade();
    make_center(card, 0, 36, 216, "周目评价", &app_font_12, COL_DIM);
    make_center(card, 0, 48, 216, cycle_grade_name(grade), &app_font_28,
                grade >= CYCLE_GRADE_S ? COL_GOLD : COL_TEXT);

    const uint32_t total = s_app.run_correct + s_app.run_wrong;
    const uint32_t accuracy =
        total > 0 ? s_app.run_correct * 100u / total : 0;
    snprintf(buf, sizeof(buf), "总分 %u", (unsigned)s_app.run_total_score);
    make_center(card, 0, 92, 216, buf, &app_font_12, COL_TEXT);
    snprintf(buf, sizeof(buf), "金币 +%u · 共 %u", (unsigned)s_app.run_coins,
             (unsigned)s_app.coins);
    make_center(card, 0, 112, 216, buf, &app_font_12, COL_GOLD);
    snprintf(buf, sizeof(buf), "题目 %u · 正确率 %u%%", (unsigned)total,
             (unsigned)accuracy);
    make_center(card, 0, 132, 216, buf, &app_font_12, COL_TEXT);
    if (s_app.run_fastest > 0) {
        snprintf(buf, sizeof(buf), "最快一题 %u.%us",
                 (unsigned)(s_app.run_fastest / 1000u),
                 (unsigned)((s_app.run_fastest % 1000u) / 100u));
    } else {
        snprintf(buf, sizeof(buf), "最快一题 --");
    }
    make_center(card, 0, 152, 216, buf, &app_font_12, COL_TEXT);

    make_center(card, 0, 180, 216, "怪兽图鉴已永久收录", &app_font_12, COL_OK);
    snprintf(buf, sizeof(buf), "奥特曼皮肤碎片 +%u (共%u)",
             (unsigned)s_app.cycle_result_fragments, (unsigned)s_app.fragments);
    make_center(card, 0, 198, 216, buf, &app_font_12, COL_ACCENT);
    if (s_app.cycle_gallery_bonus) {
        make_center(card, 0, 216, 216, "图鉴完成 · 稀有奖励 +5碎片", &app_font_12,
                    COL_GOLD);
    } else if (s_app.cycle_secret_done) {
        make_center(card, 0, 216, 216, "隐藏BOSS 击破 · 额外金币", &app_font_12,
                    COL_GOLD);
    } else if (cycle_has_secret_boss(s_app.cycle, s_app.cycle_variation)) {
        make_center(card, 0, 216, 216, "隐藏BOSS 尚未出现", &app_font_12,
                    COL_DIM);
    } else {
        make_center(card, 0, 216, 216, "下一周目怪兽池将轮换", &app_font_12,
                    COL_DIM);
    }

    lv_obj_t *item = make_panel(card, 28, 244, 160, 28,
                                COL_TILE_SEL, 8);
    lv_obj_set_style_border_width(item, 2, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(COL_ACCENT), 0);
    make_center(item, 0, 4, 160, overlay_item_text(0), &app_font_14, COL_TEXT);
    make_center(card, 0, 276, 216, "OK 开始新一轮冒险", &app_font_12,
                COL_DIM);
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
    if (s_app.overlay == OVERLAY_CYCLE) {
        build_cycle_overlay();
        return;
    }

    const uint8_t items = overlay_item_count();
    int item_y = 60;
    int card_h = item_y + (int)items * 32 + 6;
    if (s_app.overlay == OVERLAY_CLEAR) {
        if (s_app.mode == MODE_CAMPAIGN) {
            item_y = 172;
        } else {
            item_y = 112;
        }
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
        } else if (s_app.mode == MODE_HUNDRED) {
            title = "百减结束";
            sub = "见好就收,分数已结算";
        } else if (s_app.mode == MODE_GAUNTLET) {
            title = "三连战达成!";
            sub = "三名强敌全部击破";
        } else if (s_app.cycle_secret_boss) {
            title = "隐藏BOSS击破!";
            sub = "额外金币已到手";
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
        } else if (s_app.mode == MODE_HUNDRED) {
            sub = "百减挑战结束了";
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
        if (s_app.mode == MODE_ENDLESS || s_app.mode == MODE_HUNDRED) {
            snprintf(line, sizeof(line), "本次得分 %d", (int)s_app.battle.score);
            make_center(card, 0, 40, 212, line, &app_font_20, COL_GOLD);
            const uint32_t best = s_app.mode == MODE_HUNDRED
                                      ? s_app.hundred_best
                                      : s_app.endless_best;
            snprintf(line, sizeof(line), "打到第 %u 战 · 最高 %u",
                     (unsigned)s_app.wave, (unsigned)best);
            make_center(card, 0, 70, 212, line, &app_font_12, COL_TEXT);
            make_center(card, 0, 86, 212, "继续挑战更高分", &app_font_12, COL_DIM);
        } else if (s_app.mode == MODE_CAMPAIGN) {
            // 单关奖励面板: 星级、答对/答错、平均用时、金币与一句鼓励。
            char stars[16];
            stars_string(stars, sizeof(stars), s_app.battle.stars);
            make_center(card, 0, 36, 212, stars, &app_font_20, COL_GOLD);
            snprintf(line, sizeof(line), "击败 %s!",
                     campaign_monster_name(s_app.battle.monster));
            make_center(card, 0, 60, 212, line, &app_font_12, COL_TEXT);
            const uint32_t avg = battle_average_answer_ms(&s_app.battle);
            snprintf(line, sizeof(line), "答对 %u · 答错 %u · 平均 %u.%us",
                     (unsigned)s_app.battle.correct_count,
                     (unsigned)s_app.battle.wrong_count,
                     (unsigned)(avg / 1000u),
                     (unsigned)((avg % 1000u) / 100u));
            make_center(card, 0, 80, 212, line, &app_font_12, COL_TEXT);
            if (s_app.stage_coins > 0) {
                snprintf(line, sizeof(line), "+%u 金币 · 共 %u",
                         (unsigned)s_app.stage_coins, (unsigned)s_app.coins);
                make_center(card, 0, 100, 212, line, &app_font_12, COL_GOLD);
            } else {
                make_center(card, 0, 100, 212, "本关奖励已结算", &app_font_12,
                            COL_DIM);
            }
            make_center(card, 0, 122, 212, s_app.stage_encourage,
                        &app_font_12, COL_ACCENT);
            if (s_app.cycle_secret_pending) {
                make_center(card, 0, 144, 212, "隐藏BOSS 出现了!",
                            &app_font_12, COL_DANGER);
            } else if (s_app.battle.stage + 1u >= CAMPAIGN_STAGE_COUNT ||
                       s_app.cycle_secret_boss) {
                make_center(card, 0, 144, 212, "准备周目结算", &app_font_12,
                            COL_GOLD);
            } else {
                snprintf(line, sizeof(line), "第 %u / %d 关",
                         (unsigned)s_app.battle.stage + 1u,
                         CAMPAIGN_STAGE_COUNT);
                make_center(card, 0, 144, 212, line, &app_font_12, COL_DIM);
            }
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
        } else if (s_app.overlay == OVERLAY_OVER &&
                   s_app.mode == MODE_CAMPAIGN) {
            // 失败不清空周目进度: 金币、图鉴和已通过关卡全部保留。
            snprintf(line, sizeof(line), "金币 %u 已保留", (unsigned)s_app.coins);
            make_center(card, 0, 54, 212, line, &app_font_12, COL_GOLD);
            snprintf(line, sizeof(line), "重来只重打第 %u 关",
                     (unsigned)s_app.battle.stage + 1u);
            make_center(card, 0, 72, 212, line, &app_font_12, COL_TEXT);
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

static void prepare_cycle_result(void)
{
    if (!s_app.cycle_result_awarded) {
        const cycle_grade_t grade = run_cycle_grade();
        uint8_t fragments = 1;
        if (grade == CYCLE_GRADE_SS || grade == CYCLE_GRADE_S) {
            fragments = 3;
        } else if (grade == CYCLE_GRADE_A) {
            fragments = 2;
        }
        s_app.fragments = (uint8_t)(s_app.fragments + fragments);
        s_app.cycle_result_fragments = fragments;
        // 第 10 周目起, 集齐全部怪兽图鉴可领取一次稀有完成奖励。
        const uint16_t all_monsters =
            (uint16_t)((1u << PROGRESS_MONSTER_COUNT) - 1u);
        if (s_app.cycle >= 10 &&
            (s_app.progress.monsters & all_monsters) == all_monsters &&
            (s_app.shop_flags & GALLERY_COMPLETE_REWARD) == 0) {
            s_app.shop_flags |= GALLERY_COMPLETE_REWARD;
            s_app.coins += 50;
            s_app.run_coins += 50;
            s_app.fragments = (uint8_t)(s_app.fragments + 5);
            s_app.cycle_result_fragments =
                (uint8_t)(s_app.cycle_result_fragments + 5);
            s_app.cycle_gallery_bonus = true;
        }
        s_app.cycle_result_awarded = true;
        request_save();
    }
    show_overlay(OVERLAY_CYCLE);
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
static bool cycle_stage_secret(uint8_t stage)
{
    return s_app.mode == MODE_CAMPAIGN && s_app.cycle_variation &&
           stage == 4 && cycle_has_secret_stage(s_app.cycle, true);
}

// 只在整轮冒险开始时清零统计; 失败重试和继续下一关都保留。
static void reset_cycle_run(void)
{
    s_app.run_total_score = 0;
    s_app.run_coins = 0;
    s_app.run_correct = 0;
    s_app.run_wrong = 0;
    s_app.run_answer_total = 0;
    s_app.run_fastest = 0;
    s_app.stage_coins = 0;
    s_app.cycle_result_fragments = 0;
    s_app.cycle_result_awarded = false;
    s_app.cycle_secret_pending = false;
    s_app.cycle_secret_boss = false;
    s_app.cycle_secret_done = false;
    s_app.cycle_gallery_bonus = false;
}

static void begin_battle(uint8_t stage, uint16_t carry_hp, int32_t carry_score)
{
    if (stage >= CAMPAIGN_STAGE_COUNT) {
        stage = 0;
    }
    s_app.stage = stage;
    s_app.strike_anim = ANIM_HERO_ATTACK;
    const bool new_hero = progress_mark_hero(&s_app.progress, s_app.hero);

    battle_variant_t variant;
    const battle_variant_t *variant_ptr = NULL;
    if (s_app.mode == MODE_CAMPAIGN) {
        variant.monster = s_app.cycle_plan.monster[stage];
        if (variant.monster >= CAMPAIGN_MONSTER_COUNT) {
            variant.monster = stage;
        }
        variant.question_style = s_app.cycle_plan.style[stage];
        variant.max_wrong =
            cycle_max_wrong(s_app.cycle_plan.mods);
        variant.time_pct =
            cycle_question_time_pct(s_app.cycle_plan.mods);
        variant.boss_bonus_hp =
            cycle_boss_bonus_hp(s_app.cycle_plan.mods);
        if (s_app.cycle_secret_boss) {
            variant.boss_bonus_hp = (uint8_t)(variant.boss_bonus_hp + 4u);
        }
        variant_ptr = &variant;
    }

    battle_start_variant(&s_app.battle, stage, s_app.difficulty, s_app.hero,
                         (uint32_t)esp_timer_get_time() ^ 0x5EED1234u,
                         s_app.mode == MODE_HUNDRED, variant_ptr);
    const bool new_monster =
        progress_mark_monster(&s_app.progress, s_app.battle.monster);
    if (new_hero || new_monster) {
        request_save();
    }
    s_app.ko_cry_pending = false;
    s_app.hero_action.active = false;
    s_app.monster_action.active = false;
    s_app.action_question_index = 0xFF;
    s_app.easter_egg_at_us = 0;
    battle_set_low_age(&s_app.battle, s_app.low_age);
    battle_set_score_bonus(&s_app.battle,
                           s_app.mode == MODE_ENDLESS || s_app.mode == MODE_HUNDRED
                               ? s_app.run_bonus_pct
                               : 0);
    if (carry_hp > 0 && carry_hp < s_app.battle.hero_hp) {
        s_app.battle.hero_hp = carry_hp;
    }
    s_app.battle.score = carry_score;

    // Wrong answers come back as the 2nd and 5th question of the round. Only
    // questions at or below this stage's math tier are eligible.
    review_record_t picks[BATTLE_REVIEW_SLOTS];
    const uint8_t tier = campaign_math_stage(stage, s_app.difficulty);
    // 百减模式要求每题都是 100 减法, 因此不插入其他题型的错题。
    const uint8_t count =
        s_app.mode == MODE_HUNDRED
            ? 0
            : review_store_collect(&s_app.review, stage, tier, picks,
                                   BATTLE_REVIEW_SLOTS);
    math_question_t questions[BATTLE_REVIEW_SLOTS];
    for (uint8_t i = 0; i < count; i++) {
        questions[i] = picks[i].question;
    }
    battle_queue_review(&s_app.battle, questions, count);

    // The first two campaign stages coach the player. Cards appear only while
    // the stage is still uncleared, so replays do not interrupt the flow.
    s_app.tutorial_enabled = s_app.mode == MODE_CAMPAIGN &&
                             s_app.battle.stage <= 1 &&
                             s_app.unlocked <= s_app.battle.stage;
    s_app.tutorial_crit_shown = false;
    const bool start_tutorial =
        s_app.tutorial_enabled && s_app.battle.stage == 0;

    // Hold the round on a short READY beat so the player can see the board
    // before the question timer starts. OK skips it. Stage 1 keeps the READY
    // beat; stage 0 opens on a coaching card instead.
    battle_pause(&s_app.battle);
    if (start_tutorial) {
        s_app.ready_until_us = 0;
    } else {
        s_app.ready_until_us = esp_timer_get_time() + (int64_t)READY_MS * 1000;
    }

    s_app.page = PAGE_BATTLE;
    build_battle();
    if (start_tutorial) {
        show_tutorial(TUTORIAL_SELECT);
    }
    audio_player_play(SFX_CONFIRM);

    char intro[64];
    if (s_app.mode == MODE_ENDLESS) {
        snprintf(intro, sizeof(intro), "无尽挑战 · 第%d关", (int)stage + 1);
        set_notice_ms(intro, 2200000LL);
    } else if (s_app.mode == MODE_HUNDRED) {
        snprintf(intro, sizeof(intro), "百减模式 · 第%d战", (int)stage + 1);
        set_notice_ms(intro, 2200000LL);
    } else if (s_app.mode == MODE_GAUNTLET) {
        snprintf(intro, sizeof(intro), "三连战 · 第%d战", (int)s_app.wave);
        set_notice_ms(intro, 2200000LL);
    } else if (s_app.mode == MODE_CAMPAIGN) {
        if (s_app.cycle_secret_boss) {
            set_notice_ms("隐藏BOSS · 杰顿强化体", 2400000LL);
        } else if (cycle_stage_secret(stage)) {
            set_notice_ms("隐藏怪兽出现 · 金币加成", 2400000LL);
        } else if (s_app.cycle_variation && s_app.cycle_plan.mods != 0) {
            snprintf(intro, sizeof(intro), "第%u周目 · 挑战词缀生效",
                     (unsigned)s_app.cycle);
            set_notice_ms(intro, 2200000LL);
        }
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
    s_app.strike_seq = 0;
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
    } else if (s_app.mode == MODE_HUNDRED) {
        if ((uint32_t)s_app.battle.score > s_app.hundred_best) {
            s_app.hundred_best = (uint32_t)s_app.battle.score;
        }
    }
    request_save();
}

static void handle_run_clear(void)
{
    battle_t *battle = &s_app.battle;
    if (s_app.mode == MODE_ENDLESS || s_app.mode == MODE_HUNDRED) {
        s_app.run_hp = battle->hero_hp;
        s_app.run_score = battle->score;
        s_app.pending_stage =
            (uint8_t)((battle->stage + 1u) % CAMPAIGN_STAGE_COUNT);
        // Endless and 百减 hand the breather to the player: bank the score, or
        // spend one more heart and keep the multiplier climbing.
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

// Defeat cries follow the monster's build: armoured beasts bellow, the chimera
// shrieks, mechanical shells grind, and the alien/fish types warble.
static sfx_id_t monster_cry_id(uint8_t stage)
{
    switch (stage) {
    case 1:  // 贝姆拉
    case 3:  // 哥尔赞
    case 4:  // 哥莫拉
    case 6:  // 雷德王
        return SFX_CRY_HEAVY;
    case 7:  // 泰兰特
    case 10: // 双尾怪
        return SFX_CRY_WINGED;
    case 5:  // 金古桥
    case 8:  // 杰顿
        return SFX_CRY_MECH;
    case 9:  // 布莱克王
        return SFX_CRY_HEAVY;
    default:  // 巴尔坦星人 / 艾雷王
        return SFX_CRY_ALIEN;
    }
}

// 惨叫不在命中瞬间播放, 而是等 KO 淡出接近最后一帧再响。
static void schedule_ko_cry(const battle_t *battle)
{
    const anim_id_t id = battle->boss ? ANIM_BOSS_KO : ANIM_KO;
    const uint32_t duration = battle_clear_ms(battle);
    s_app.ko_cry_at_us = esp_timer_get_time() +
                         (int64_t)anim_ko_cry_ms(id, duration) * 1000;
    s_app.ko_cry_sfx = monster_cry_id(battle->monster);
    s_app.ko_cry_pending = true;
    s_app.ko_fx_step = 0;
}

// ---------------------------------------------------------------------------
// 教程卡
// ---------------------------------------------------------------------------
static void show_tutorial(uint8_t card)
{
    battle_ui_t *ui = &s_app.battle_ui;
    if (ui->tutorial == NULL) {
        return;
    }
    s_app.tutorial = card;
    if (card == TUTORIAL_SELECT) {
        label_set(ui->tutorial_title, "教程 · 答题");
        label_set(ui->tutorial_body,
                  "用 ↑ ↓ 选择答案\n\n选中后按 OK 确认\n\n答对才能攻击怪兽");
        label_set(ui->tutorial_footer, "按 OK 开始");
    } else {
        label_set(ui->tutorial_title, "教程 · 追加攻击");
        label_set(ui->tutorial_body,
                  "答对后会出现进度条\n\n白条扫到金色区域时\n再按一次 OK\n\n就能打出更强的一击");
        label_set(ui->tutorial_footer, "按 OK 继续");
    }
    set_hidden(ui->tutorial, false);
}

static void dismiss_tutorial(void)
{
    s_app.tutorial = TUTORIAL_NONE;
    if (s_app.battle_ui.tutorial != NULL) {
        set_hidden(s_app.battle_ui.tutorial, true);
    }
    if (s_app.battle.phase == BATTLE_PHASE_PAUSED) {
        battle_resume(&s_app.battle);
    }
    s_app.ready_until_us = 0;
    s_app.last_tick_us = esp_timer_get_time();
    audio_player_play(SFX_CONFIRM);
    refresh_battle();
}

static void maybe_show_critical_tutorial(void)
{
    if (!s_app.tutorial_enabled || s_app.tutorial_crit_shown) {
        return;
    }
    s_app.tutorial_crit_shown = true;
    show_tutorial(TUTORIAL_CRITICAL);
    refresh_battle();
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
        action_on_correct();
        if (battle->best_combo >= 8) {
            (void)award_badge(PROGRESS_BADGE_COMBO);
        }
        if (battle->current_review) {
            const bool removed =
                review_store_mastered(&s_app.review, &battle->question);
            spawn_popup(removed ? "掌握!" : "记住", COL_OK,
                        HERO_X - ARENA_X + 8, 22);
            request_save();
        }
        s_app.feedback_correct = battle->question.correct_index;
        s_app.feedback_wrong = 0xFF;
        maybe_show_critical_tutorial();
        break;
    case BATTLE_EV_WRONG:
    case BATTLE_EV_TIMEOUT: {
        audio_player_play(SFX_WRONG);
        audio_player_play(SFX_GROWL);
        action_on_wrong();
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
        s_app.strike_anim = take_hero_strike(battle);
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
        s_app.strike_anim = take_hero_strike(battle);
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
        action_on_monster_defeated();
        schedule_ko_cry(battle);
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
        if (s_app.mode == MODE_ENDLESS && s_app.wave >= 5) {
            (void)award_badge(PROGRESS_BADGE_ENDLESS);
        }
        prepare_review_view();
        show_overlay(OVERLAY_OVER);
        break;
    case BATTLE_EV_STAGE_CLEAR:
        audio_player_play(SFX_VICTORY);
        action_on_stage_clear();
        if (s_app.mode == MODE_CAMPAIGN) {
            stage_record_result();
            stage_award_reward();
            prepare_review_view();
            const bool last =
                s_app.battle.stage + 1u >= CAMPAIGN_STAGE_COUNT;
            // 第 9 关后若本轮周目已解锁隐藏BOSS, 先给玩家一次迎战选择。
            s_app.cycle_secret_pending =
                last && !s_app.cycle_secret_boss && !s_app.cycle_secret_done &&
                s_app.cycle_variation &&
                cycle_has_secret_boss(s_app.cycle, true);
            show_overlay(OVERLAY_CLEAR);
        } else {
            handle_run_clear();
        }
        break;
    case BATTLE_EV_BEAM_START:
        audio_player_play(SFX_BEAM);
        s_app.beam_cue_next = 0;
        s_app.ui_beam_pips = -1;
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
        action_on_monster_defeated();
        schedule_ko_cry(battle);
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

// Metronome for the beam duel: two count-in ticks, then one tick on every
// beat, so the rhythm is audible while the player watches the marker.
static uint32_t beam_cue_ms(uint8_t index)
{
    if (index < 2) {
        return (uint32_t)((int)BEAM_LEAD_IN_MS -
                          2 * BEAM_BEAT_INTERVAL_MS +
                          index * BEAM_BEAT_INTERVAL_MS);
    }
    return battle_beam_beat_ms((uint8_t)(index - 2));
}

static void tick_beam_cues(void)
{
    if (s_app.battle.phase != BATTLE_PHASE_BEAM) {
        return;
    }
    const uint32_t elapsed = battle_beam_elapsed_ms(&s_app.battle);
    while (s_app.beam_cue_next < BEAM_CUE_COUNT) {
        const uint32_t cue = beam_cue_ms(s_app.beam_cue_next);
        if (cue == UINT32_MAX || elapsed < cue) {
            break;
        }
        audio_player_play(SFX_MOVE);
        s_app.beam_cue_next++;
    }
}

static void tick_battle(void)
{
    const int64_t now = esp_timer_get_time();

    if (s_app.ko_cry_pending && now >= s_app.ko_cry_at_us) {
        audio_player_play(s_app.ko_cry_sfx);
        s_app.ko_cry_pending = false;
    }

    // A coaching card freezes the round: the question clock, the critical
    // marker and the beam timer all stop until the player presses OK.
    if (s_app.tutorial != TUTORIAL_NONE) {
        s_app.last_tick_us = now;
        refresh_battle_with_dt(0);
        return;
    }

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
    tick_beam_cues();
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
            if (s_app.mode == MODE_ENDLESS && s_app.wave >= 5) {
                (void)award_badge(PROGRESS_BADGE_ENDLESS);
            }
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

    if (s_app.overlay == OVERLAY_CYCLE) {
        destroy_overlay();
        // 进入下一周目: 换种子 -> 轮换怪兽/题型/词缀 -> 从第 1 关重开。
        s_app.cycle = (uint16_t)(s_app.cycle + 1u);
        s_app.cycle_seed = s_app.cycle_seed * 1664525u + 1013904223u;
        if (s_app.cycle_seed == 0) {
            s_app.cycle_seed = 0x5EEDu;
        }
        cycle_rebuild_plan();
        reset_cycle_run();
        // 新一轮才允许重新结算各关金币; 同一周目内反复选关不刷新。
        s_app.run_paid_mask = 0;
        s_app.stage = 0;
        request_save();
        audio_player_play(SFX_CONFIRM);
        start_battle(0);
        return;
    }

    if (s_app.overlay == OVERLAY_CLEAR) {
        if (s_app.mode == MODE_ENDLESS || s_app.mode == MODE_HUNDRED) {
            destroy_overlay();
            if (s_app.overlay_index == 0) {
                start_run(s_app.mode);
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
        // 闯关模式: 每关先看奖励面板。最后一关的通关按钮进入周目结算,
        // 若隐藏BOSS已解锁则先让玩家选择是否迎战。
        const uint8_t stage = s_app.battle.stage;
        const bool last = stage + 1u >= CAMPAIGN_STAGE_COUNT;
        const bool pending = s_app.cycle_secret_pending;
        const bool secret_clear = s_app.cycle_secret_boss;
        const uint8_t choice = s_app.overlay_index;
        destroy_overlay();
        if (pending) {
            if (choice == 0) {
                s_app.cycle_secret_pending = false;
                s_app.cycle_secret_boss = true;
                start_battle(CAMPAIGN_STAGE_COUNT - 1);
                return;
            }
            if (choice == 1) {
                // 跳过隐藏BOSS也要正常结算本轮周目, 不能卡在第 9 关。
                s_app.cycle_secret_pending = false;
                prepare_cycle_result();
                audio_player_play(SFX_CONFIRM);
                return;
            }
            end_battle_to_page(PAGE_HOME);
            audio_player_play(SFX_CONFIRM);
            return;
        }
        if (choice != 0) {
            end_battle_to_page(PAGE_HOME);
            audio_player_play(SFX_CONFIRM);
            return;
        }
        if (last || secret_clear) {
            prepare_cycle_result();
            audio_player_play(SFX_CONFIRM);
            return;
        }
        start_battle((uint8_t)(stage + 1u));
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
        } else if (s_app.mode == MODE_TIME) {
            start_time_attack();
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
        reset_cycle_run();
        start_battle(s_app.stage);
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        // 重掷周目词缀: 同一周目内重新抽怪兽/题型/挑战, 方便选一个想打的组合。
        if (s_app.cycle_variation) {
            s_app.cycle_seed = s_app.cycle_seed * 1103515245u + 12345u;
            if (s_app.cycle_seed == 0) {
                s_app.cycle_seed = 0x1234u;
            }
            cycle_rebuild_plan();
            request_save();
            audio_player_play(SFX_CONFIRM);
            refresh_stages();
        } else {
            audio_player_play(SFX_WRONG);
        }
        return;
    }
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        audio_player_play(SFX_PAUSE);
        end_battle_to_page(PAGE_HOME);
    }
}

static void handle_battle_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (s_app.tutorial != TUTORIAL_NONE) {
        if ((event == BSP_BTN_CLICK || event == BSP_BTN_LONG) &&
            btn == BSP_BTN_OK) {
            dismiss_tutorial();
        }
        return;
    }

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
    case PAGE_TIME:
        handle_time_key(btn, event);
        break;
    case PAGE_COLLECTION:
        handle_collection_key(btn, event);
        break;
    case PAGE_SHOP:
        handle_shop_key(btn, event);
        break;
    case PAGE_PREVIEW:
        handle_preview_key(btn, event);
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
        } else if (s_app.page == PAGE_TIME) {
            tick_time_attack();
        } else if (s_app.page == PAGE_PREVIEW) {
            tick_preview();
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
    s_app.action_question_index = 0xFF;
    math_rng_seed(&s_app.action_rng,
                  (uint32_t)s_app.last_tick_us ^ 0xA17E5u);

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

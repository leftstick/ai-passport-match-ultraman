// Pure battle state machine. No LVGL/ESP-IDF dependencies.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "beam_rhythm.h"
#include "campaign.h"
#include "math_question.h"
#include "ultimate.h"

#define BATTLE_GAUGE_MAX 20
#define BATTLE_REVIEW_SLOTS 2
// The critical sweep lasts 5.8 s (was 0.8 s) so a first-time player can read
// the prompt, find the gold band, and still press OK.
#define BATTLE_CRITICAL_MS 5800
#define BATTLE_ATTACK_MS 520
#define BATTLE_ENEMY_MS 760
#define BATTLE_GUARD_MS 1100
// Three-second count-in plus five 1.2-second beats, with a short tail after the
// final beat so the last marker can leave the track.
#define BATTLE_BEAM_MS 8400
// KO 演出: 普通怪 0.9 秒, BOSS 关 1.2 秒, 都在 0.7~1.2 秒预算内。
#define BATTLE_CLEAR_MS 900
#define BATTLE_BOSS_CLEAR_MS 1200

// 周目挑战会替换关卡题型。取值与 cycle_style_t 对齐。
#define BATTLE_STYLE_NORMAL 0
#define BATTLE_STYLE_HUNDRED 1
#define BATTLE_STYLE_MIXED 2

typedef enum {
    BATTLE_PHASE_QUESTION = 0,
    BATTLE_PHASE_CRITICAL,
    BATTLE_PHASE_ATTACK,
    BATTLE_PHASE_ENEMY,
    BATTLE_PHASE_GUARD,
    BATTLE_PHASE_ULTIMATE,
    BATTLE_PHASE_BEAM,
    BATTLE_PHASE_CLEAR,
    BATTLE_PHASE_OVER,
    BATTLE_PHASE_PAUSED,
} battle_phase_t;

typedef enum {
    BATTLE_EV_NONE = 0,
    BATTLE_EV_MOVED,
    BATTLE_EV_CORRECT,
    BATTLE_EV_WRONG,
    BATTLE_EV_TIMEOUT,
    BATTLE_EV_CRITICAL_HIT,
    BATTLE_EV_CRITICAL_MISS,
    BATTLE_EV_CRITICAL_PERFECT,
    BATTLE_EV_ATTACK_DONE,
    BATTLE_EV_ENEMY_DONE,
    BATTLE_EV_ULTIMATE_START,
    BATTLE_EV_ULTIMATE_HIT,
    BATTLE_EV_ULTIMATE_PERFECT,
    BATTLE_EV_ULTIMATE_MISS,
    BATTLE_EV_ULTIMATE_DONE,
    BATTLE_EV_GUARD_START,
    BATTLE_EV_GUARD_BLOCK,
    BATTLE_EV_GUARD_FAIL,
    BATTLE_EV_MONSTER_DEFEATED,
    BATTLE_EV_HERO_DEFEATED,
    BATTLE_EV_STAGE_CLEAR,
    BATTLE_EV_BEAM_START,
    BATTLE_EV_BEAM_PERFECT,
    BATTLE_EV_BEAM_HIT,
    BATTLE_EV_BEAM_MISS,
    BATTLE_EV_BEAM_WIN,
    BATTLE_EV_BEAM_LOSE,
    BATTLE_EV_HINT,
    BATTLE_EV_NEED_SELECT,
} battle_event_t;

// No option is selected until the player presses up/down. This keeps OK from
// answering by accident and stops blind OK-mashing from grinding a stage.
#define BATTLE_CURSOR_NONE 0xFF

typedef struct {
    uint8_t stage;
    uint8_t difficulty;
    uint8_t hero;
    uint8_t phase;
    uint8_t stars;

    uint16_t hero_hp;
    uint16_t hero_hp_max;
    uint16_t monster_hp;
    uint16_t monster_hp_max;
    int32_t score;
    uint8_t combo;
    uint8_t best_combo;
    uint8_t gauge;
    uint8_t correct_count;
    uint8_t wrong_count;
    uint8_t miss_streak;

    bool boss;
    bool assist_active;
    bool last_stand_used;
    bool last_stand_bonus;
    bool shield_used;
    bool hint_used;
    bool clear_reported;
    bool armor_broken;
    bool enraged;
    // Presentation and accessibility flags.
    bool crit_pursuit;
    bool combo_finisher;
    bool low_age;
    bool current_review;
    // 百减模式下所有新题都走 generateHundredSubQuestion, 且不注入错题。
    bool hundred_sub;
    // 视觉怪兽编号, 默认等于 stage; 周目轮换只影响表现与图鉴, 不改关卡规则。
    uint8_t monster;
    uint8_t question_style;
    // 0 = 不限制; 2 = 累计 2 次错误直接失败.
    uint8_t max_wrong;
    // 100 = 原始答题时间, 80 = 极速挑战.
    uint8_t question_time_pct;

    // 答题用时统计, 用于星级、单关结算和整周目评级。
    uint32_t answer_total_ms;
    uint16_t fastest_answer_ms;
    uint8_t answer_count;

    // Extra score percentage added to each answered question. Endless runs
    // raise it every time the player chooses to keep going.
    uint8_t score_bonus_pct;

    int32_t phase_ms;
    int32_t question_ms;
    int32_t question_remaining_ms;

    uint8_t cursor;
    math_question_t question;
    math_question_t history[MATH_QUESTION_HISTORY];
    uint8_t history_count;
    uint8_t question_index;
    math_question_t review[BATTLE_REVIEW_SLOTS];
    uint8_t review_count;
    uint8_t review_used;
    math_rng_t rng;

    uint8_t crit_marker;
    uint8_t crit_center;
    uint8_t crit_window;
    // The effective window may be widened by answering quickly. The inner
    // half of it is the perfect zone. Both flags describe the last critical.
    uint8_t crit_window_effective;
    bool crit_bonus;
    bool crit_perfect;

    uint16_t beam_elapsed_ms;
    uint8_t guard_dir;      // 0 = up, 1 = down
    bool guard_resolved;

    beam_rhythm_t beam;
    ultimate_t ult;
} battle_t;

// 周目挑战对单场战斗的轻量变体。monster 只影响表现与图鉴。
typedef struct {
    uint8_t monster;
    uint8_t question_style;
    uint8_t max_wrong;
    uint8_t time_pct;
    uint8_t boss_bonus_hp;
} battle_variant_t;

void battle_start(battle_t *battle, uint8_t stage, uint8_t difficulty, uint8_t hero,
                  uint32_t seed);
// 百减模式使用同一套战斗状态机, 只是第一题起就生成 100 减法题。
void battle_start_mode(battle_t *battle, uint8_t stage, uint8_t difficulty,
                       uint8_t hero, uint32_t seed, bool hundred_sub);
// 周目战役入口: variant 为 NULL 时与 battle_start_mode 完全一致。
void battle_start_variant(battle_t *battle, uint8_t stage, uint8_t difficulty,
                          uint8_t hero, uint32_t seed, bool hundred_sub,
                          const battle_variant_t *variant);
uint32_t battle_average_answer_ms(const battle_t *battle);
uint32_t battle_clear_ms(const battle_t *battle);

battle_event_t battle_move_cursor(battle_t *battle, int delta);
battle_event_t battle_confirm(battle_t *battle);
battle_event_t battle_tick(battle_t *battle, uint32_t elapsed_ms);
battle_event_t battle_use_hint(battle_t *battle);
battle_event_t battle_use_ultimate(battle_t *battle);
battle_event_t battle_ultimate_press(battle_t *battle, uint8_t input);
battle_event_t battle_guard_press(battle_t *battle, uint8_t input);
void battle_pause(battle_t *battle);
void battle_resume(battle_t *battle);

// Queues up to BATTLE_REVIEW_SLOTS questions that replace the 2nd and 5th
// question of the battle. Call after battle_start().
void battle_queue_review(battle_t *battle, const math_question_t *questions,
                         uint8_t count);
bool battle_question_is_review(const battle_t *battle);
void battle_set_low_age(battle_t *battle, bool low_age);
bool battle_low_age(const battle_t *battle);
void battle_set_score_bonus(battle_t *battle, uint8_t percent);

bool battle_can_ultimate(const battle_t *battle);
bool battle_is_over(const battle_t *battle);
uint8_t battle_stars(const battle_t *battle);
uint8_t battle_crit_half_window(const battle_t *battle);
const stage_def_t *battle_stage_def(const battle_t *battle);
const hero_def_t *battle_hero_def(const battle_t *battle);
const difficulty_def_t *battle_difficulty_def(const battle_t *battle);

uint8_t battle_beam_force(const battle_t *battle);
uint8_t battle_beam_beats_left(const battle_t *battle);
uint16_t battle_beam_elapsed_ms(const battle_t *battle);
uint32_t battle_beam_next_beat_ms(const battle_t *battle);
uint32_t battle_beam_beat_ms(uint8_t index);
beam_judge_t battle_beam_judge(const battle_t *battle, uint8_t index);
uint8_t battle_beam_clean(const battle_t *battle);
uint8_t battle_beam_goal(const battle_t *battle);

uint8_t battle_ultimate_steps(const battle_t *battle);
uint8_t battle_ultimate_step(const battle_t *battle);
uint8_t battle_ultimate_hits(const battle_t *battle);
uint8_t battle_ultimate_input(const battle_t *battle);
uint16_t battle_ultimate_step_remaining_ms(const battle_t *battle);
uint16_t battle_ultimate_total_ms(const battle_t *battle);

uint16_t battle_guard_remaining_ms(const battle_t *battle);
uint8_t battle_guard_dir(const battle_t *battle);

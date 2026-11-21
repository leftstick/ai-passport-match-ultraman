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
#define BATTLE_CRITICAL_MS 800
#define BATTLE_ATTACK_MS 520
#define BATTLE_ENEMY_MS 760
#define BATTLE_GUARD_MS 1100
// Three-second count-in plus five one-second beats, with a short tail after the
// final beat so the last marker can leave the track.
#define BATTLE_BEAM_MS 7600
#define BATTLE_CLEAR_MS 1200

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

void battle_start(battle_t *battle, uint8_t stage, uint8_t difficulty, uint8_t hero,
                  uint32_t seed);

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

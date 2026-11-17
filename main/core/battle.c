#include "battle.h"

#include <stddef.h>
#include <string.h>

static uint32_t rng_range(math_rng_t *rng, uint32_t low, uint32_t high)
{
    if (high <= low) {
        return low;
    }
    return low + math_rng_below(rng, high - low + 1u);
}

static uint8_t gauge_gain(const battle_t *battle, uint8_t base)
{
    if (campaign_hero(battle->hero)->perk == HERO_PERK_GAUGE) {
        return (uint8_t)(base * 5u / 4u);
    }
    return base;
}

static void add_gauge(battle_t *battle, uint8_t amount)
{
    uint16_t next = (uint16_t)battle->gauge + amount;
    if (next > BATTLE_GAUGE_MAX) {
        next = BATTLE_GAUGE_MAX;
    }
    battle->gauge = (uint8_t)next;
}

static uint8_t calc_stars(const battle_t *battle)
{
    if (battle->wrong_count == 0 && battle->hero_hp >= battle->hero_hp_max) {
        return 3;
    }
    if (battle->wrong_count <= 1 && battle->hero_hp > 0) {
        return 2;
    }
    return 1;
}

static void record_history(battle_t *battle)
{
    if (battle->history_count < MATH_QUESTION_HISTORY) {
        battle->history[battle->history_count++] = battle->question;
        return;
    }
    for (uint8_t i = 1; i < MATH_QUESTION_HISTORY; i++) {
        battle->history[i - 1] = battle->history[i];
    }
    battle->history[MATH_QUESTION_HISTORY - 1] = battle->question;
}

static uint16_t current_question_ms(const battle_t *battle)
{
    int32_t ms = campaign_question_ms(battle->stage, battle->difficulty, battle->hero);
    if (battle->assist_active) {
        ms += 2000;
    }
    if (battle->last_stand_bonus) {
        ms += 1000;
    }
    if (ms < 2000) {
        ms = 2000;
    }
    if (ms > 15000) {
        ms = 15000;
    }
    return (uint16_t)ms;
}

static void next_question(battle_t *battle)
{
    battle->assist_active = battle->miss_streak >= 2;
    battle->question_ms = current_question_ms(battle);
    battle->question_remaining_ms = battle->question_ms;
    battle->last_stand_bonus = false;
    battle->hint_used = false;
    battle->phase = BATTLE_PHASE_QUESTION;
    battle->phase_ms = 0;
    battle->cursor = BATTLE_CURSOR_NONE;

    math_question_generate(&battle->question,
                           campaign_stage(battle->stage)->math_stage,
                           battle->assist_active, &battle->rng,
                           battle->history, battle->history_count);
    record_history(battle);
}

static bool apply_enemy_hit(battle_t *battle)
{
    if (campaign_hero(battle->hero)->perk == HERO_PERK_SHIELD && !battle->shield_used) {
        battle->shield_used = true;
        return false;
    }
    if (battle->hero_hp > 0) {
        battle->hero_hp--;
    }
    if (battle->hero_hp == 1 && !battle->last_stand_used) {
        battle->last_stand_used = true;
        battle->last_stand_bonus = true;
    }
    return true;
}

static battle_event_t enter_defeat_or_clear(battle_t *battle)
{
    if (battle->monster_hp > 0) {
        return BATTLE_EV_NONE;
    }
    if (battle->boss) {
        battle->phase = BATTLE_PHASE_BEAM;
        battle->phase_ms = BATTLE_BEAM_MS;
        battle->beam_force = 0;
        battle->beam_presses = 0;
        return BATTLE_EV_BEAM_START;
    }
    battle->stars = calc_stars(battle);
    battle->score += 500 + (int32_t)battle->hero_hp * 100;
    battle->phase = BATTLE_PHASE_CLEAR;
    battle->phase_ms = BATTLE_CLEAR_MS;
    battle->clear_reported = false;
    return BATTLE_EV_MONSTER_DEFEATED;
}

static battle_event_t resolve_critical(battle_t *battle, bool hit)
{
    int damage = 1;
    if (hit) {
        damage = 2;
        if (campaign_hero(battle->hero)->perk == HERO_PERK_CRIT_DAMAGE) {
            damage = 3;
        }
        battle->score += 100;
        add_gauge(battle, gauge_gain(battle, 4));
    }
    if (damage >= (int)battle->monster_hp) {
        battle->monster_hp = 0;
    } else {
        battle->monster_hp = (uint16_t)(battle->monster_hp - (uint16_t)damage);
    }
    battle->phase = BATTLE_PHASE_ATTACK;
    battle->phase_ms = BATTLE_ATTACK_MS;
    return hit ? BATTLE_EV_CRITICAL_HIT : BATTLE_EV_CRITICAL_MISS;
}

static battle_event_t answer_wrong(battle_t *battle, battle_event_t event)
{
    battle->combo = 0;
    battle->miss_streak++;
    battle->wrong_count++;
    (void)apply_enemy_hit(battle);
    battle->phase = BATTLE_PHASE_ENEMY;
    battle->phase_ms = BATTLE_ENEMY_MS;
    return event;
}

void battle_start(battle_t *battle, uint8_t stage, uint8_t difficulty, uint8_t hero,
                  uint32_t seed)
{
    memset(battle, 0, sizeof(*battle));
    if (stage >= CAMPAIGN_STAGE_COUNT) {
        stage = 0;
    }
    if (difficulty >= CAMPAIGN_DIFFICULTY_COUNT) {
        difficulty = 0;
    }
    if (hero >= CAMPAIGN_HERO_COUNT) {
        hero = 0;
    }
    battle->stage = stage;
    battle->difficulty = difficulty;
    battle->hero = hero;
    battle->boss = campaign_stage(stage)->boss;
    battle->hero_hp_max = campaign_hero_hp(difficulty);
    battle->hero_hp = battle->hero_hp_max;
    battle->monster_hp_max = campaign_monster_hp(stage, difficulty);
    battle->monster_hp = battle->monster_hp_max;
    battle->crit_window = campaign_crit_window(difficulty, hero);
    math_rng_seed(&battle->rng, seed ^ (0x9E3779B9u + stage * 131u + hero * 17u));
    next_question(battle);
}

battle_event_t battle_move_cursor(battle_t *battle, int delta)
{
    if (battle->phase != BATTLE_PHASE_QUESTION || delta == 0) {
        return BATTLE_EV_NONE;
    }
    if (battle->cursor == BATTLE_CURSOR_NONE) {
        // First press picks the top option regardless of direction, matching
        // how the menu pages behave.
        battle->cursor = 0;
        return BATTLE_EV_MOVED;
    }
    int next = (int)battle->cursor + delta;
    while (next < 0) {
        next += MATH_QUESTION_OPTION_COUNT;
    }
    battle->cursor = (uint8_t)(next % MATH_QUESTION_OPTION_COUNT);
    return BATTLE_EV_MOVED;
}

battle_event_t battle_confirm(battle_t *battle)
{
    if (battle->phase == BATTLE_PHASE_QUESTION) {
        if (battle->cursor == BATTLE_CURSOR_NONE) {
            return BATTLE_EV_NEED_SELECT;
        }
        if (battle->cursor == battle->question.correct_index) {
            battle->combo++;
            if (battle->combo > battle->best_combo) {
                battle->best_combo = battle->combo;
            }
            battle->correct_count++;
            battle->miss_streak = 0;
            battle->score += 100 + (int32_t)battle->combo * 20 +
                             battle->question_remaining_ms / 100;
            add_gauge(battle, gauge_gain(battle, 4));
            battle->phase = BATTLE_PHASE_CRITICAL;
            battle->phase_ms = BATTLE_CRITICAL_MS;
            battle->crit_marker = 0;
            battle->crit_center = (uint8_t)rng_range(
                &battle->rng, battle->crit_window / 2u,
                100u - battle->crit_window / 2u);
            return BATTLE_EV_CORRECT;
        }
        return answer_wrong(battle, BATTLE_EV_WRONG);
    }
    if (battle->phase == BATTLE_PHASE_CRITICAL) {
        const int half = battle->crit_window / 2;
        const int diff = (int)battle->crit_marker - (int)battle->crit_center;
        return resolve_critical(battle, diff >= -half && diff <= half);
    }
    if (battle->phase == BATTLE_PHASE_BEAM) {
        uint16_t force = (uint16_t)battle->beam_force + 12u;
        if (force > 100u) {
            force = 100u;
        }
        battle->beam_force = (uint8_t)force;
        battle->beam_presses++;
        return BATTLE_EV_BEAM_PRESS;
    }
    return BATTLE_EV_NONE;
}

battle_event_t battle_tick(battle_t *battle, uint32_t elapsed_ms)
{
    if (elapsed_ms == 0 || battle->phase == BATTLE_PHASE_PAUSED ||
        battle->phase == BATTLE_PHASE_OVER) {
        return BATTLE_EV_NONE;
    }

    if (battle->phase == BATTLE_PHASE_QUESTION) {
        if ((int32_t)elapsed_ms >= battle->question_remaining_ms) {
            battle->question_remaining_ms = 0;
            return answer_wrong(battle, BATTLE_EV_TIMEOUT);
        }
        battle->question_remaining_ms -= (int32_t)elapsed_ms;
        return BATTLE_EV_NONE;
    }

    if (battle->phase == BATTLE_PHASE_BEAM) {
        int32_t force = (int32_t)battle->beam_force -
                        (int32_t)((elapsed_ms * 30u) / 1000u);
        if (force < 0) {
            force = 0;
        }
        battle->beam_force = (uint8_t)force;
    }

    if ((int32_t)elapsed_ms < battle->phase_ms) {
        battle->phase_ms -= (int32_t)elapsed_ms;
        if (battle->phase == BATTLE_PHASE_CRITICAL) {
            const int32_t elapsed = BATTLE_CRITICAL_MS - battle->phase_ms;
            battle->crit_marker = (uint8_t)(elapsed * 100 / BATTLE_CRITICAL_MS);
        }
        return BATTLE_EV_NONE;
    }

    battle->phase_ms = 0;

    switch (battle->phase) {
    case BATTLE_PHASE_CRITICAL:
        return resolve_critical(battle, false);
    case BATTLE_PHASE_ATTACK: {
        const battle_event_t event = enter_defeat_or_clear(battle);
        if (event != BATTLE_EV_NONE) {
            return event;
        }
        next_question(battle);
        return BATTLE_EV_ATTACK_DONE;
    }
    case BATTLE_PHASE_ENEMY:
        if (battle->hero_hp == 0) {
            battle->phase = BATTLE_PHASE_OVER;
            return BATTLE_EV_HERO_DEFEATED;
        }
        next_question(battle);
        return BATTLE_EV_ENEMY_DONE;
    case BATTLE_PHASE_ULTIMATE: {
        const battle_event_t event = enter_defeat_or_clear(battle);
        if (event != BATTLE_EV_NONE) {
            return event;
        }
        next_question(battle);
        return BATTLE_EV_ULTIMATE_DONE;
    }
    case BATTLE_PHASE_BEAM:
        if (battle->beam_force >= 35 || battle->beam_presses >= 6) {
            battle->stars = calc_stars(battle);
            battle->score += 800 + (int32_t)battle->hero_hp * 100;
            battle->phase = BATTLE_PHASE_CLEAR;
            battle->phase_ms = BATTLE_CLEAR_MS;
            battle->clear_reported = false;
            return BATTLE_EV_BEAM_WIN;
        }
        battle->monster_hp = 2;
        next_question(battle);
        return BATTLE_EV_BEAM_LOSE;
    case BATTLE_PHASE_CLEAR:
        if (!battle->clear_reported) {
            battle->clear_reported = true;
            return BATTLE_EV_STAGE_CLEAR;
        }
        return BATTLE_EV_NONE;
    default:
        return BATTLE_EV_NONE;
    }
}

battle_event_t battle_use_hint(battle_t *battle)
{
    if (battle->phase != BATTLE_PHASE_QUESTION || battle->hint_used) {
        return BATTLE_EV_NONE;
    }
    battle->hint_used = true;
    if (battle->question_remaining_ms > 1500) {
        battle->question_remaining_ms -= 1000;
    } else {
        battle->question_remaining_ms = 500;
    }
    return BATTLE_EV_HINT;
}

battle_event_t battle_use_ultimate(battle_t *battle)
{
    if (battle->phase != BATTLE_PHASE_QUESTION || battle->gauge < BATTLE_GAUGE_MAX) {
        return BATTLE_EV_NONE;
    }
    battle->gauge = 0;
    if (battle->monster_hp <= 3) {
        battle->monster_hp = 0;
    } else {
        battle->monster_hp = (uint16_t)(battle->monster_hp - 3);
    }
    battle->phase = BATTLE_PHASE_ULTIMATE;
    battle->phase_ms = BATTLE_ULTIMATE_MS;
    return BATTLE_EV_ULTIMATE_START;
}

void battle_pause(battle_t *battle)
{
    if (battle->phase == BATTLE_PHASE_QUESTION) {
        battle->phase = BATTLE_PHASE_PAUSED;
    }
}

void battle_resume(battle_t *battle)
{
    if (battle->phase == BATTLE_PHASE_PAUSED) {
        battle->phase = BATTLE_PHASE_QUESTION;
    }
}

bool battle_can_ultimate(const battle_t *battle)
{
    return battle->phase == BATTLE_PHASE_QUESTION &&
           battle->gauge >= BATTLE_GAUGE_MAX;
}

bool battle_is_over(const battle_t *battle)
{
    return battle->phase == BATTLE_PHASE_OVER;
}

uint8_t battle_stars(const battle_t *battle)
{
    return battle->stars;
}

uint8_t battle_crit_half_window(const battle_t *battle)
{
    return (uint8_t)(battle->crit_window / 2u);
}

const stage_def_t *battle_stage_def(const battle_t *battle)
{
    return campaign_stage(battle->stage);
}

const hero_def_t *battle_hero_def(const battle_t *battle)
{
    return campaign_hero(battle->hero);
}

const difficulty_def_t *battle_difficulty_def(const battle_t *battle)
{
    return campaign_difficulty(battle->difficulty);
}

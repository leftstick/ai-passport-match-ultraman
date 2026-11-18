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
    // The final boss gets faster below half health. This is computed from the
    // live HP so it can flip mid-stage without extra bookkeeping.
    if (campaign_stage_has_rule(battle->stage, STAGE_RULE_ENRAGE) &&
        battle->monster_hp * 2 <= battle->monster_hp_max) {
        ms -= 1000;
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
    battle->enraged =
        campaign_stage_has_rule(battle->stage, STAGE_RULE_ENRAGE) &&
        battle->monster_hp * 2 <= battle->monster_hp_max;
    battle->question_ms = current_question_ms(battle);
    battle->question_remaining_ms = battle->question_ms;
    battle->last_stand_bonus = false;
    battle->hint_used = false;
    battle->phase = BATTLE_PHASE_QUESTION;
    battle->phase_ms = 0;
    battle->cursor = BATTLE_CURSOR_NONE;

    // Wrong-answer reviews replace the 2nd and 5th question so the player has
    // settled into the round before seeing a familiar expression again.
    const bool review_slot =
        battle->question_index == 1 || battle->question_index == 4;
    if (review_slot && battle->review_used < battle->review_count) {
        battle->question = battle->review[battle->review_used++];
        battle->current_review = true;
    } else {
        math_question_generate(&battle->question,
                               campaign_stage(battle->stage)->math_stage,
                               battle->assist_active, &battle->rng,
                               battle->history, battle->history_count);
        battle->current_review = false;
    }
    record_history(battle);
    battle->question_index++;
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

static void apply_monster_damage(battle_t *battle, int damage)
{
    if (damage <= 0) {
        return;
    }
    if (damage >= (int)battle->monster_hp) {
        battle->monster_hp = 0;
    } else {
        battle->monster_hp = (uint16_t)(battle->monster_hp - (uint16_t)damage);
    }
}

static uint8_t beam_start_force(const battle_t *battle)
{
    // A clean blue ultimate leaves the boss beam already leaning the hero's
    // way for the rhythm duel.
    if (campaign_hero(battle->hero)->perk == HERO_PERK_GAUGE &&
        ultimate_all_hit(&battle->ult)) {
        return 25;
    }
    return 0;
}

static battle_event_t enter_defeat_or_clear(battle_t *battle)
{
    if (battle->monster_hp > 0) {
        return BATTLE_EV_NONE;
    }
    if (battle->boss) {
        battle->phase = BATTLE_PHASE_BEAM;
        battle->phase_ms = BATTLE_BEAM_MS;
        battle->beam_elapsed_ms = 0;
        beam_rhythm_start(&battle->beam, beam_start_force(battle));
        return BATTLE_EV_BEAM_START;
    }
    battle->stars = calc_stars(battle);
    battle->score += 500 + (int32_t)battle->hero_hp * 100;
    battle->phase = BATTLE_PHASE_CLEAR;
    battle->phase_ms = BATTLE_CLEAR_MS;
    battle->clear_reported = false;
    return BATTLE_EV_MONSTER_DEFEATED;
}

static battle_event_t resolve_critical(battle_t *battle, bool hit, bool perfect)
{
    battle->crit_perfect = perfect;
    battle->crit_pursuit = false;
    battle->combo_finisher = false;

    int damage = 1;
    if (hit) {
        damage = perfect ? 3 : 2;
        if (campaign_hero(battle->hero)->perk == HERO_PERK_CRIT_DAMAGE) {
            damage++;
        }
        battle->score += 100;
        if (perfect) {
            battle->score += 150;
        }
        add_gauge(battle, gauge_gain(battle, 4));

        // Every third consecutive correct strike chains a pursuit hit. From
        // six upward the finisher also pays gauge back.
        if (battle->combo > 0 && battle->combo % 3 == 0) {
            damage++;
            battle->crit_pursuit = true;
            battle->score += 50;
        }
        if (battle->combo >= 6 && battle->combo % 3 == 0) {
            add_gauge(battle, gauge_gain(battle, 1));
            battle->combo_finisher = true;
        }

        if (campaign_stage_has_rule(battle->stage, STAGE_RULE_HALF_NONCRIT) &&
            !perfect && damage > 1) {
            damage = 1;
        }
        if (campaign_stage_has_rule(battle->stage, STAGE_RULE_ARMOR_BREAK)) {
            if (!battle->armor_broken) {
                if (perfect) {
                    // The perfect window is the only way through the armor.
                    damage++;
                    battle->armor_broken = true;
                } else if (damage > 1) {
                    damage = 1;
                }
            } else {
                damage++;
            }
        }
    }
    apply_monster_damage(battle, damage);
    battle->phase = BATTLE_PHASE_ATTACK;
    battle->phase_ms = BATTLE_ATTACK_MS;
    if (!hit) {
        return BATTLE_EV_CRITICAL_MISS;
    }
    return perfect ? BATTLE_EV_CRITICAL_PERFECT : BATTLE_EV_CRITICAL_HIT;
}

static battle_event_t answer_wrong(battle_t *battle, battle_event_t event)
{
    battle->combo = 0;
    battle->miss_streak++;
    battle->wrong_count++;

    // The late-game ambush stages turn a mistake into a short block prompt
    // instead of immediate damage.
    if (battle->hero_hp > 0 && !battle->low_age &&
        campaign_stage_has_rule(battle->stage, STAGE_RULE_GUARD_QTE)) {
        battle->phase = BATTLE_PHASE_GUARD;
        battle->phase_ms = BATTLE_GUARD_MS;
        battle->guard_dir = (uint8_t)math_rng_below(&battle->rng, 2);
        battle->guard_resolved = false;
        return BATTLE_EV_GUARD_START;
    }

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

void battle_queue_review(battle_t *battle, const math_question_t *questions,
                         uint8_t count)
{
    if (questions == NULL) {
        count = 0;
    }
    if (count > BATTLE_REVIEW_SLOTS) {
        count = BATTLE_REVIEW_SLOTS;
    }
    battle->review_count = count;
    battle->review_used = 0;
    for (uint8_t i = 0; i < count; i++) {
        battle->review[i] = questions[i];
    }
}

bool battle_question_is_review(const battle_t *battle)
{
    return battle->current_review;
}

void battle_set_low_age(battle_t *battle, bool low_age)
{
    battle->low_age = low_age;
}

bool battle_low_age(const battle_t *battle)
{
    return battle->low_age;
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
            int32_t gain = 100 + (int32_t)battle->combo * 20 +
                           battle->question_remaining_ms / 100;
            if (battle->current_review) {
                // Reviews deliberately pay more: getting an old mistake right
                // is the behaviour the loop wants to reinforce.
                gain += gain / 2;
                add_gauge(battle, 1);
            }
            battle->score += gain;
            add_gauge(battle, gauge_gain(battle, 4));
            if (campaign_stage_has_rule(battle->stage, STAGE_RULE_FAST_GAUGE)) {
                add_gauge(battle, 1);
            }
            battle->phase = BATTLE_PHASE_CRITICAL;
            battle->phase_ms = BATTLE_CRITICAL_MS;
            battle->crit_marker = 0;
            battle->crit_perfect = false;
            battle->crit_bonus = false;
            battle->crit_window_effective = battle->crit_window;
            // Answering with at least 60% of the clock left widens the
            // critical window by 25%: fast answers are rewarded with a
            // larger timing target.
            if (battle->question_ms > 0 &&
                battle->question_remaining_ms * 10 >= battle->question_ms * 6) {
                const uint16_t widened = (uint16_t)battle->crit_window * 5u / 4u;
                battle->crit_window_effective =
                    (uint8_t)(widened > 100u ? 100u : widened);
                battle->crit_bonus = true;
            }
            if (battle->low_age) {
                const uint16_t widened =
                    (uint16_t)battle->crit_window_effective * 5u / 4u;
                battle->crit_window_effective =
                    (uint8_t)(widened > 100u ? 100u : widened);
            }
            const uint8_t half = (uint8_t)(battle->crit_window_effective / 2u);
            battle->crit_center = (uint8_t)rng_range(
                &battle->rng, half, 100u - half);
            return BATTLE_EV_CORRECT;
        }
        return answer_wrong(battle, BATTLE_EV_WRONG);
    }
    if (battle->phase == BATTLE_PHASE_CRITICAL) {
        const int half = battle->crit_window_effective / 2;
        const int perfect_half = battle->crit_window_effective / 4;
        const int diff = (int)battle->crit_marker - (int)battle->crit_center;
        const int distance = diff < 0 ? -diff : diff;
        if (distance > half) {
            return resolve_critical(battle, false, false);
        }
        // The inner half of the window is perfect: more damage and score.
        return resolve_critical(battle, true, distance <= perfect_half);
    }
    if (battle->phase == BATTLE_PHASE_BEAM) {
        const beam_judge_t judge =
            beam_rhythm_press(&battle->beam, battle->beam_elapsed_ms);
        // The fifth beat settles the duel immediately. Missing the last beat
        // must not leave the player waiting for the timer to drain.
        if (beam_rhythm_beats_left(&battle->beam) == 0) {
            if (beam_rhythm_won(&battle->beam)) {
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
        }
        if (judge == BEAM_JUDGE_PERFECT) {
            return BATTLE_EV_BEAM_PERFECT;
        }
        if (judge == BEAM_JUDGE_HIT) {
            return BATTLE_EV_BEAM_HIT;
        }
        return BATTLE_EV_BEAM_MISS;
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

    if (battle->phase == BATTLE_PHASE_ULTIMATE) {
        const ult_judge_t judge = ultimate_tick(&battle->ult, elapsed_ms);
        if (battle->monster_hp == 0) {
            battle->phase_ms = 0;
            return enter_defeat_or_clear(battle);
        }
        // Once every beat is spent only the release tail remains, so the
        // cinematic does not keep the player waiting after an early finish.
        if (ultimate_done(&battle->ult) &&
            battle->phase_ms > (int32_t)ULT_TAIL_MS) {
            battle->phase_ms = (int32_t)ULT_TAIL_MS;
        }
        if ((int32_t)elapsed_ms < battle->phase_ms) {
            battle->phase_ms -= (int32_t)elapsed_ms;
        } else {
            battle->phase_ms = 0;
        }
        if (!ultimate_done(&battle->ult) || battle->phase_ms > 0) {
            return judge == ULT_JUDGE_MISS ? BATTLE_EV_ULTIMATE_MISS
                                           : BATTLE_EV_NONE;
        }
        // A clean run pays the hero's signature bonus.
        if (ultimate_all_hit(&battle->ult)) {
            const hero_perk_t perk = (hero_perk_t)campaign_hero(battle->hero)->perk;
            if (perk == HERO_PERK_CRIT_DAMAGE) {
                add_gauge(battle, 2);
            } else if (perk == HERO_PERK_SHIELD) {
                apply_monster_damage(battle, 2);
                battle->shield_used = false;
            }
        }
        battle->phase_ms = 0;
        const battle_event_t event = enter_defeat_or_clear(battle);
        if (event != BATTLE_EV_NONE) {
            return event;
        }
        next_question(battle);
        return BATTLE_EV_ULTIMATE_DONE;
    }

    if (battle->phase == BATTLE_PHASE_BEAM) {
        uint32_t advance = elapsed_ms;
        if ((uint32_t)battle->beam_elapsed_ms + advance > BATTLE_BEAM_MS) {
            advance = BATTLE_BEAM_MS - battle->beam_elapsed_ms;
        }
        battle->beam_elapsed_ms =
            (uint16_t)(battle->beam_elapsed_ms + advance);
        beam_rhythm_tick(&battle->beam, advance);

        if ((int32_t)elapsed_ms < battle->phase_ms) {
            battle->phase_ms -= (int32_t)elapsed_ms;
            return BATTLE_EV_NONE;
        }
        battle->phase_ms = 0;
        if (beam_rhythm_won(&battle->beam)) {
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
        return resolve_critical(battle, false, false);
    case BATTLE_PHASE_ATTACK: {
        const battle_event_t event = enter_defeat_or_clear(battle);
        if (event != BATTLE_EV_NONE) {
            return event;
        }
        next_question(battle);
        return BATTLE_EV_ATTACK_DONE;
    }
    case BATTLE_PHASE_ENEMY: {
        if (battle->hero_hp == 0) {
            battle->phase = BATTLE_PHASE_OVER;
            return BATTLE_EV_HERO_DEFEATED;
        }
        // A violet reflected hit can finish the monster during the enemy turn.
        const battle_event_t event = enter_defeat_or_clear(battle);
        if (event != BATTLE_EV_NONE) {
            return event;
        }
        next_question(battle);
        return BATTLE_EV_ENEMY_DONE;
    }
    case BATTLE_PHASE_GUARD:
        if (!battle->guard_resolved) {
            battle->guard_resolved = true;
            (void)apply_enemy_hit(battle);
            battle->phase = BATTLE_PHASE_ENEMY;
            battle->phase_ms = BATTLE_ENEMY_MS;
        }
        return BATTLE_EV_GUARD_FAIL;
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
    // The tutorial stage hands out hints without taxing the clock.
    if (campaign_stage_has_rule(battle->stage, STAGE_RULE_HINT_FREE)) {
        return BATTLE_EV_HINT;
    }
    if (battle->question_remaining_ms > 1500) {
        battle->question_remaining_ms -= 1000;
    } else {
        battle->question_remaining_ms = 500;
    }
    return BATTLE_EV_HINT;
}

battle_event_t battle_use_ultimate(battle_t *battle)
{
    if (battle->phase != BATTLE_PHASE_QUESTION ||
        battle->gauge < BATTLE_GAUGE_MAX) {
        return BATTLE_EV_NONE;
    }
    battle->gauge = 0;
    battle->crit_pursuit = false;
    battle->combo_finisher = false;
    ultimate_start(&battle->ult, battle->hero);
    battle->phase = BATTLE_PHASE_ULTIMATE;
    battle->phase_ms = (int32_t)ultimate_total_ms(battle->hero);
    return BATTLE_EV_ULTIMATE_START;
}

battle_event_t battle_ultimate_press(battle_t *battle, uint8_t input)
{
    if (battle->phase != BATTLE_PHASE_ULTIMATE) {
        return BATTLE_EV_NONE;
    }
    const ult_judge_t judge = ultimate_press(&battle->ult, input);
    if (judge == ULT_JUDGE_NONE) {
        return BATTLE_EV_NONE;
    }
    if (judge == ULT_JUDGE_MISS) {
        return BATTLE_EV_ULTIMATE_MISS;
    }

    const bool perfect = judge == ULT_JUDGE_PERFECT;
    const int damage = ultimate_step_damage(battle->hero, perfect);
    apply_monster_damage(battle, damage);
    battle->score += perfect ? 200 : 100;
    return perfect ? BATTLE_EV_ULTIMATE_PERFECT : BATTLE_EV_ULTIMATE_HIT;
}

battle_event_t battle_guard_press(battle_t *battle, uint8_t input)
{
    if (battle->phase != BATTLE_PHASE_GUARD || battle->guard_resolved) {
        return BATTLE_EV_NONE;
    }
    battle->guard_resolved = true;
    const uint8_t want =
        battle->guard_dir == 0 ? (uint8_t)ULT_INPUT_UP : (uint8_t)ULT_INPUT_DOWN;
    if (input == want) {
        apply_monster_damage(battle, 1);
        battle->score += 80;
        battle->phase = BATTLE_PHASE_ENEMY;
        battle->phase_ms = BATTLE_ENEMY_MS;
        return BATTLE_EV_GUARD_BLOCK;
    }
    (void)apply_enemy_hit(battle);
    battle->phase = BATTLE_PHASE_ENEMY;
    battle->phase_ms = BATTLE_ENEMY_MS;
    return BATTLE_EV_GUARD_FAIL;
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
    return (uint8_t)(battle->crit_window_effective / 2u);
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

uint8_t battle_beam_force(const battle_t *battle)
{
    return battle->beam.force;
}

uint8_t battle_beam_beats_left(const battle_t *battle)
{
    return beam_rhythm_beats_left(&battle->beam);
}

uint16_t battle_beam_elapsed_ms(const battle_t *battle)
{
    return battle->beam_elapsed_ms;
}

uint32_t battle_beam_next_beat_ms(const battle_t *battle)
{
    return beam_rhythm_next_beat_ms(&battle->beam);
}

uint32_t battle_beam_beat_ms(uint8_t index)
{
    return beam_rhythm_beat_ms(index);
}

uint8_t battle_ultimate_steps(const battle_t *battle)
{
    return ultimate_steps(&battle->ult);
}

uint8_t battle_ultimate_step(const battle_t *battle)
{
    return ultimate_step_index(&battle->ult);
}

uint8_t battle_ultimate_hits(const battle_t *battle)
{
    return battle->ult.hits;
}

uint8_t battle_ultimate_input(const battle_t *battle)
{
    return ultimate_step_input(&battle->ult);
}

uint16_t battle_ultimate_step_remaining_ms(const battle_t *battle)
{
    return ultimate_step_remaining_ms(&battle->ult);
}

uint16_t battle_ultimate_total_ms(const battle_t *battle)
{
    return ultimate_total_ms(battle->hero);
}

uint16_t battle_guard_remaining_ms(const battle_t *battle)
{
    if (battle->phase_ms <= 0) {
        return 0;
    }
    return battle->phase_ms > 65535 ? 65535 : (uint16_t)battle->phase_ms;
}

uint8_t battle_guard_dir(const battle_t *battle)
{
    return battle->guard_dir;
}

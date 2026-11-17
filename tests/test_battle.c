// Host tests for the pure battle state machine.
#include "battle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void answer_correct(battle_t *b)
{
    b->cursor = b->question.correct_index;
    const battle_event_t ev = battle_confirm(b);
    assert(ev == BATTLE_EV_CORRECT);
}

static void answer_wrong(battle_t *b)
{
    b->cursor = b->question.correct_index == 0 ? 1 : 0;
    const battle_event_t ev = battle_confirm(b);
    assert(ev == BATTLE_EV_WRONG);
}

static void crit_hit(battle_t *b)
{
    // Land inside the critical window but outside the perfect core.
    const int perfect = b->crit_window_effective / 4;
    const int offset = perfect + 1;
    b->crit_marker = (uint8_t)(b->crit_center + offset);
    assert(battle_confirm(b) == BATTLE_EV_CRITICAL_HIT);
}

static void crit_perfect(battle_t *b)
{
    b->crit_marker = b->crit_center;
    assert(battle_confirm(b) == BATTLE_EV_CRITICAL_PERFECT);
}

static void finish_attack(battle_t *b)
{
    assert(b->phase == BATTLE_PHASE_ATTACK);
    (void)battle_tick(b, BATTLE_ATTACK_MS);
}

static void test_start_and_question(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 1234);
    assert(b.stage == 0);
    assert(b.difficulty == 1);
    assert(b.hero == 0);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.hero_hp == 4);
    assert(b.hero_hp_max == 4);
    assert(b.monster_hp == 4);
    assert(b.monster_hp_max == 4);
    assert(b.gauge == 0);
    assert(b.question_remaining_ms == b.question_ms);
    assert(b.cursor == BATTLE_CURSOR_NONE);
    assert(battle_stage_def(&b) == campaign_stage(0));
    assert(battle_hero_def(&b) == campaign_hero(0));
    assert(battle_difficulty_def(&b) == campaign_difficulty(1));

    // OK without a selection must not answer; the first UP/DOWN picks an
    // option, then the cursor wraps.
    assert(battle_confirm(&b) == BATTLE_EV_NEED_SELECT);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.wrong_count == 0);
    assert(battle_move_cursor(&b, -1) == BATTLE_EV_MOVED);
    assert(b.cursor == 0);
    assert(battle_move_cursor(&b, 1) == BATTLE_EV_MOVED);
    assert(b.cursor == 1);
    assert(battle_move_cursor(&b, 1) == BATTLE_EV_MOVED);
    assert(b.cursor == 2);
    assert(battle_move_cursor(&b, 1) == BATTLE_EV_MOVED);
    assert(b.cursor == 0);
    assert(battle_move_cursor(&b, -1) == BATTLE_EV_MOVED);
    assert(b.cursor == 2);
}

/* Regression: blind OK presses used to answer by accident. Without any
 * up/down input nothing may change. */
static void test_ok_mashing_without_selection(void)
{
    battle_t b;
    battle_start(&b, 0, 0, 0, 2020);
    for (int n = 0; n < 40; n++) {
        assert(battle_confirm(&b) == BATTLE_EV_NEED_SELECT);
        assert(battle_tick(&b, 20) == BATTLE_EV_NONE);
    }
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.hero_hp == b.hero_hp_max);
    assert(b.monster_hp == b.monster_hp_max);
    assert(b.wrong_count == 0);
}

static void test_correct_critical_and_history(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 2222);

    answer_correct(&b);
    assert(b.phase == BATTLE_PHASE_CRITICAL);
    assert(b.combo == 1);
    assert(b.correct_count == 1);
    assert(b.gauge == 5);  // blue hero: 4 * 5 / 4
    assert(b.score >= 100);

    crit_hit(&b);
    assert(b.phase == BATTLE_PHASE_ATTACK);
    assert(b.monster_hp == 2);
    assert(b.gauge == 10);

    finish_attack(&b);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.history_count == 2);
    assert(b.combo == 1);
}

static void test_critical_miss(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 3333);
    answer_correct(&b);
    b.crit_marker = 0;
    b.crit_center = 99;
    assert(battle_confirm(&b) == BATTLE_EV_CRITICAL_MISS);
    assert(b.monster_hp == 3);
    assert(b.phase == BATTLE_PHASE_ATTACK);
}

static void test_critical_timeout(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 3334);
    answer_correct(&b);
    const battle_event_t ev = battle_tick(&b, BATTLE_CRITICAL_MS);
    assert(ev == BATTLE_EV_CRITICAL_MISS);
    assert(b.monster_hp == 3);
}

static void test_perfect_critical_and_speed_bonus(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 2500);
    const uint8_t base_window = b.crit_window;
    answer_correct(&b);
    // Answering immediately earns the widened window.
    assert(b.crit_bonus);
    assert(b.crit_window_effective == (uint8_t)(base_window * 5u / 4u));
    assert(b.crit_window_effective > base_window);

    const int32_t after_answer = b.score;
    crit_perfect(&b);
    assert(b.crit_perfect);
    assert(b.monster_hp == 1);  // 4 - 3
    assert(b.score == after_answer + 250);  // 100 base + 150 perfect
    assert(b.phase == BATTLE_PHASE_ATTACK);

    // A slow answer keeps the base window and cannot be perfect at the edge.
    battle_start(&b, 0, 1, 0, 2501);
    const uint8_t slow_window = b.crit_window;
    const int32_t slow_step = b.question_ms * 41 / 100;
    assert(battle_tick(&b, (uint32_t)slow_step) == BATTLE_EV_NONE);
    answer_correct(&b);
    assert(!b.crit_bonus);
    assert(b.crit_window_effective == slow_window);
}

static void test_red_hero_perfect_damage(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 1, 2502);
    answer_correct(&b);
    crit_perfect(&b);
    assert(b.monster_hp == 0);  // 4 - (3 + 1 red hero perk)
}

static void test_wrong_and_timeout(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 4444);
    answer_wrong(&b);
    assert(b.phase == BATTLE_PHASE_ENEMY);
    assert(b.hero_hp == 3);
    assert(b.wrong_count == 1);
    assert(b.miss_streak == 1);
    assert(b.combo == 0);
    assert(battle_tick(&b, BATTLE_ENEMY_MS) == BATTLE_EV_ENEMY_DONE);
    assert(b.phase == BATTLE_PHASE_QUESTION);

    const int32_t timeout = b.question_remaining_ms + 1;
    assert(battle_tick(&b, (uint32_t)timeout) == BATTLE_EV_TIMEOUT);
    assert(b.hero_hp == 2);
    assert(b.wrong_count == 2);
}

static void test_adaptive_assist_and_last_stand(void)
{
    battle_t b;
    battle_start(&b, 3, 1, 0, 5555);
    assert(b.question.math_stage == 2);

    answer_wrong(&b);
    (void)battle_tick(&b, BATTLE_ENEMY_MS);
    assert(!b.assist_active);

    answer_wrong(&b);
    (void)battle_tick(&b, BATTLE_ENEMY_MS);
    assert(b.assist_active);
    assert(b.question.math_stage == 1);
    assert(b.question_ms == 6800 + 2000);

    b.hero_hp = 2;
    answer_wrong(&b);
    assert(b.hero_hp == 1);
    (void)battle_tick(&b, BATTLE_ENEMY_MS);
    assert(b.question_ms == 6800 + 2000 + 1000);
}

static void test_shield_hero(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 4, 6666);
    answer_wrong(&b);
    assert(b.hero_hp == 4);
    assert(b.shield_used);
    (void)battle_tick(&b, BATTLE_ENEMY_MS);
    answer_wrong(&b);
    assert(b.hero_hp == 3);
}

static void test_red_crit_damage(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 1, 7777);
    answer_correct(&b);
    assert(b.gauge == 4);
    crit_hit(&b);
    assert(b.monster_hp == 1);
}

static void test_hint(void)
{
    battle_t b;
    battle_start(&b, 0, 0, 0, 8888);
    const int32_t before = b.question_remaining_ms;
    assert(battle_use_hint(&b) == BATTLE_EV_HINT);
    assert(b.hint_used);
    assert(b.question_remaining_ms == before - 1000);
    assert(battle_use_hint(&b) == BATTLE_EV_NONE);
}

static void test_ultimate(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 9999);
    assert(!battle_can_ultimate(&b));
    assert(battle_use_ultimate(&b) == BATTLE_EV_NONE);
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_can_ultimate(&b));
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);
    assert(b.gauge == 0);
    assert(b.monster_hp == 1);
    assert(b.phase == BATTLE_PHASE_ULTIMATE);
    assert(battle_tick(&b, BATTLE_ULTIMATE_MS) == BATTLE_EV_ULTIMATE_DONE);
    assert(b.phase == BATTLE_PHASE_QUESTION);
}

static void test_clear_and_stars(void)
{
    battle_t b;
    battle_start(&b, 0, 0, 0, 1111);
    assert(b.monster_hp == 3);
    answer_correct(&b);
    crit_hit(&b);
    finish_attack(&b);
    assert(b.phase == BATTLE_PHASE_QUESTION);

    answer_correct(&b);
    crit_hit(&b);
    assert(b.monster_hp == 0);
    assert(battle_tick(&b, BATTLE_ATTACK_MS) == BATTLE_EV_MONSTER_DEFEATED);
    assert(b.phase == BATTLE_PHASE_CLEAR);
    assert(b.stars == 3);
    assert(battle_tick(&b, BATTLE_CLEAR_MS) == BATTLE_EV_STAGE_CLEAR);
    assert(b.phase == BATTLE_PHASE_CLEAR);
    assert(battle_tick(&b, BATTLE_CLEAR_MS) == BATTLE_EV_NONE);
}

static void test_star_tiers(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 1212);
    b.wrong_count = 1;
    b.monster_hp = 1;
    answer_correct(&b);
    crit_hit(&b);
    (void)battle_tick(&b, BATTLE_ATTACK_MS);
    assert(b.stars == 2);

    battle_start(&b, 0, 1, 0, 1313);
    b.wrong_count = 2;
    b.monster_hp = 1;
    answer_correct(&b);
    crit_hit(&b);
    (void)battle_tick(&b, BATTLE_ATTACK_MS);
    assert(b.stars == 1);
}

static void test_defeat(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 1414);
    b.hero_hp = 1;
    answer_wrong(&b);
    assert(b.hero_hp == 0);
    assert(battle_tick(&b, BATTLE_ENEMY_MS) == BATTLE_EV_HERO_DEFEATED);
    assert(b.phase == BATTLE_PHASE_OVER);
    assert(battle_is_over(&b));
    assert(battle_move_cursor(&b, 1) == BATTLE_EV_NONE);
    assert(battle_confirm(&b) == BATTLE_EV_NONE);
}

static void test_boss_beam_win(void)
{
    battle_t b;
    battle_start(&b, CAMPAIGN_STAGE_COUNT - 1, 1, 0, 1515);
    assert(b.boss);
    b.monster_hp = 2;
    answer_correct(&b);
    crit_hit(&b);
    assert(b.monster_hp == 0);
    assert(battle_tick(&b, BATTLE_ATTACK_MS) == BATTLE_EV_BEAM_START);
    assert(b.phase == BATTLE_PHASE_BEAM);
    assert(b.beam_force == 0);

    for (int i = 0; i < 6; i++) {
        assert(battle_confirm(&b) == BATTLE_EV_BEAM_PRESS);
    }
    assert(b.beam_presses == 6);
    assert(battle_tick(&b, BATTLE_BEAM_MS) == BATTLE_EV_BEAM_WIN);
    assert(b.phase == BATTLE_PHASE_CLEAR);
    assert(b.stars >= 1);
    assert(battle_tick(&b, BATTLE_CLEAR_MS) == BATTLE_EV_STAGE_CLEAR);
}

static void test_boss_beam_lose(void)
{
    battle_t b;
    battle_start(&b, CAMPAIGN_STAGE_COUNT - 1, 1, 0, 1616);
    b.monster_hp = 2;
    answer_correct(&b);
    crit_hit(&b);
    assert(battle_tick(&b, BATTLE_ATTACK_MS) == BATTLE_EV_BEAM_START);
    assert(battle_tick(&b, BATTLE_BEAM_MS) == BATTLE_EV_BEAM_LOSE);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.monster_hp == 2);
}

static void test_beam_force_decay(void)
{
    battle_t b;
    battle_start(&b, CAMPAIGN_STAGE_COUNT - 1, 1, 0, 1717);
    b.monster_hp = 2;
    answer_correct(&b);
    crit_hit(&b);
    (void)battle_tick(&b, BATTLE_ATTACK_MS);
    for (int i = 0; i < 3; i++) {
        (void)battle_confirm(&b);
    }
    assert(b.beam_force == 36);
    (void)battle_tick(&b, 1000);
    assert(b.beam_force == 6);
}

static void test_pause_resume(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 1818);
    const int32_t remaining = b.question_remaining_ms;
    battle_pause(&b);
    assert(b.phase == BATTLE_PHASE_PAUSED);
    assert(battle_tick(&b, 5000) == BATTLE_EV_NONE);
    assert(b.question_remaining_ms == remaining);
    battle_resume(&b);
    assert(b.phase == BATTLE_PHASE_QUESTION);
}

static void test_history_avoids_repeats(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 1919);
    for (int n = 0; n < 12; n++) {
        const math_question_t answered = b.question;
        b.monster_hp = 99;
        answer_correct(&b);
        b.crit_marker = 0;
        b.crit_center = 99;
        assert(battle_confirm(&b) == BATTLE_EV_CRITICAL_MISS);
        (void)battle_tick(&b, BATTLE_ATTACK_MS);
        assert(strcmp(answered.text, b.question.text) != 0);
    }
}

int main(void)
{
    test_start_and_question();
    test_ok_mashing_without_selection();
    test_correct_critical_and_history();
    test_critical_miss();
    test_critical_timeout();
    test_perfect_critical_and_speed_bonus();
    test_red_hero_perfect_damage();
    test_wrong_and_timeout();
    test_adaptive_assist_and_last_stand();
    test_shield_hero();
    test_red_crit_damage();
    test_hint();
    test_ultimate();
    test_clear_and_stars();
    test_star_tiers();
    test_defeat();
    test_boss_beam_win();
    test_boss_beam_lose();
    test_beam_force_decay();
    test_pause_resume();
    test_history_avoids_repeats();
    puts("test_battle: PASS");
    return 0;
}

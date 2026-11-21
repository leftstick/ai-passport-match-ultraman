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
    assert(ev == BATTLE_EV_WRONG || ev == BATTLE_EV_GUARD_START);
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

static void test_critical_sweep_is_readable(void)
{
    battle_t b;
    battle_start(&b, 0, 0, 0, 4444);
    answer_correct(&b);
    // The sweep has to stay on screen long enough for a first-time player.
    assert(BATTLE_CRITICAL_MS >= 5000);
    assert(battle_tick(&b, 2000) == BATTLE_EV_NONE);
    assert(b.phase == BATTLE_PHASE_CRITICAL);
    assert(battle_tick(&b, BATTLE_CRITICAL_MS - 2000) ==
           BATTLE_EV_CRITICAL_MISS);
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
    // Stage 4 is the fast-gauge stage: 10% less clock, +1 gauge on correct.
    assert(b.question_ms == 6800 * 9 / 10);

    answer_wrong(&b);
    (void)battle_tick(&b, BATTLE_ENEMY_MS);
    assert(!b.assist_active);

    answer_wrong(&b);
    (void)battle_tick(&b, BATTLE_ENEMY_MS);
    assert(b.assist_active);
    assert(b.question.math_stage == 1);
    assert(b.question_ms == 6800 * 9 / 10 + 2000);

    b.hero_hp = 2;
    answer_wrong(&b);
    assert(b.hero_hp == 1);
    (void)battle_tick(&b, BATTLE_ENEMY_MS);
    assert(b.question_ms == 6800 * 9 / 10 + 2000 + 1000);
}

static void test_star_difficulty_easing(void)
{
    battle_t star;
    battle_start(&star, 4, 0, 0, 9090);
    // 星光 adds 31.5 s to the clock and drops one curriculum tier.
    assert(star.question_ms == 6200 + 31500);
    assert(star.question.math_stage == 2);
    assert(star.hero_hp == 5);

    battle_t flame;
    battle_start(&flame, 4, 1, 0, 9090);
    assert(flame.question_ms == 6200);
    assert(flame.question.math_stage == 3);

    // The long clock must not inflate the per-answer speed bonus.
    answer_correct(&star);
    assert(star.score == 200);  // 100 base + 20 combo + 80 speed cap
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
    battle_start(&b, 1, 0, 0, 8888);
    const int32_t before = b.question_remaining_ms;
    assert(battle_use_hint(&b) == BATTLE_EV_HINT);
    assert(b.hint_used);
    assert(b.question_remaining_ms == before - 1000);
    assert(battle_use_hint(&b) == BATTLE_EV_NONE);

    // The tutorial stage hands out the hint without a time tax.
    battle_start(&b, 0, 0, 0, 8889);
    const int32_t free_before = b.question_remaining_ms;
    assert(battle_use_hint(&b) == BATTLE_EV_HINT);
    assert(b.question_remaining_ms == free_before);
}

static void ultimate_answer(battle_t *b, uint8_t input)
{
    const uint16_t center = ultimate_press_center_ms(&b->ult);
    assert(battle_tick(b, center) == BATTLE_EV_NONE);
    assert(battle_ultimate_press(b, input) == BATTLE_EV_ULTIMATE_PERFECT);
}

static void test_ultimate_blue_sequence(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 9999);
    assert(!battle_can_ultimate(&b));
    assert(battle_use_ultimate(&b) == BATTLE_EV_NONE);
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_can_ultimate(&b));
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);
    assert(b.gauge == 0);
    assert(b.phase == BATTLE_PHASE_ULTIMATE);
    assert(battle_ultimate_total_ms(&b) == 2120);
    assert(battle_ultimate_steps(&b) == 3);

    for (int i = 0; i < 3; i++) {
        assert(battle_ultimate_step(&b) == i);
        ultimate_answer(&b, ULT_INPUT_OK);
    }
    assert(battle_ultimate_step(&b) == 3);
    assert(battle_ultimate_hits(&b) == 3);
    assert(b.monster_hp == 1);  // 4 - 3

    // 3 * 330 ms of beat time plus the 320 ms tail.
    assert(battle_tick(&b, 1130) == BATTLE_EV_ULTIMATE_DONE);
    assert(b.phase == BATTLE_PHASE_QUESTION);
}

static void test_ultimate_gold_one_shot(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 3, 1001);
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);
    assert(battle_ultimate_steps(&b) == 1);
    assert(battle_ultimate_total_ms(&b) == 1120);

    ultimate_answer(&b, ULT_INPUT_OK);
    assert(b.monster_hp == 0);  // perfect gold hit deals 5
    assert(battle_tick(&b, 1) == BATTLE_EV_MONSTER_DEFEATED);
    assert(b.phase == BATTLE_PHASE_CLEAR);
}

static void test_ultimate_violet_alternating_and_bonus(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 4, 1002);
    b.monster_hp = 99;
    b.monster_hp_max = 99;
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);

    for (int i = 0; i < 4; i++) {
        const uint8_t want = (i & 1) ? ULT_INPUT_DOWN : ULT_INPUT_UP;
        assert(battle_ultimate_input(&b) == want);
        ultimate_answer(&b, want);
    }
    assert(battle_ultimate_hits(&b) == 4);

    // A clean violet run reflects two extra damage and re-arms the shield.
    b.shield_used = true;
    assert(battle_tick(&b, 2400 - 4 * 286) == BATTLE_EV_ULTIMATE_DONE);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.monster_hp == 99 - 4 - 2);
    assert(!b.shield_used);
}

static void test_ultimate_red_refunds_gauge(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 1, 1003);
    b.monster_hp = 99;
    b.monster_hp_max = 99;
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);
    assert(battle_ultimate_total_ms(&b) == 1580);

    for (int i = 0; i < 3; i++) {
        ultimate_answer(&b, ULT_INPUT_OK);
    }
    assert(battle_tick(&b, 1580 - 3 * 231) == BATTLE_EV_ULTIMATE_DONE);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.gauge == 2);  // clean red sequence refund
}

static void test_ultimate_emerald_wide_window(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 2, 1004);
    b.monster_hp = 99;
    b.monster_hp_max = 99;
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);

    // 300 ms off centre is a miss for every other hero but a hit for emerald.
    assert(battle_tick(&b, 418 + 300) == BATTLE_EV_NONE);
    assert(battle_ultimate_press(&b, ULT_INPUT_OK) == BATTLE_EV_ULTIMATE_HIT);
    assert(b.ult.hits == 1);
    assert(b.ult.perfects == 0);
}

static void test_ultimate_misses(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 1005);
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);
    // Pressing immediately is far too early.
    assert(battle_ultimate_press(&b, ULT_INPUT_OK) == BATTLE_EV_ULTIMATE_MISS);
    assert(battle_ultimate_step(&b) == 1);
    assert(b.monster_hp == b.monster_hp_max);

    // Running the next beat out is also a miss.
    assert(battle_tick(&b, b.ult.step_ms) == BATTLE_EV_ULTIMATE_MISS);
    assert(battle_ultimate_step(&b) == 2);

    // Violet punishes the wrong direction even on time.
    battle_start(&b, 0, 1, 4, 1006);
    b.monster_hp = 99;
    b.monster_hp_max = 99;
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);
    assert(battle_ultimate_press(&b, ULT_INPUT_DOWN) == BATTLE_EV_ULTIMATE_MISS);
    assert(battle_ultimate_step(&b) == 1);
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

static void enter_boss_beam(battle_t *b)
{
    battle_start(b, CAMPAIGN_STAGE_COUNT - 1, 1, 0, 1515);
    assert(b->boss);
    b->monster_hp = 2;
    answer_correct(b);
    crit_hit(b);
    assert(b->monster_hp == 0);
    assert(battle_tick(b, BATTLE_ATTACK_MS) == BATTLE_EV_BEAM_START);
    assert(b->phase == BATTLE_PHASE_BEAM);
}

static battle_event_t beam_reach_and_press(battle_t *b, uint16_t target_ms)
{
    const uint16_t now = battle_beam_elapsed_ms(b);
    assert(target_ms >= now);
    if (target_ms > now) {
        assert(battle_tick(b, target_ms - now) == BATTLE_EV_NONE);
    }
    return battle_confirm(b);
}

static void test_boss_beam_five_perfect_beats(void)
{
    battle_t b;
    enter_boss_beam(&b);
    assert(battle_beam_force(&b) == 0);
    assert(battle_beam_beats_left(&b) == 5);

    for (uint8_t i = 0; i < BEAM_BEAT_COUNT - 1; i++) {
        assert(beam_reach_and_press(&b, (uint16_t)battle_beam_beat_ms(i)) ==
               BATTLE_EV_BEAM_PERFECT);
    }
    assert(beam_reach_and_press(&b, (uint16_t)battle_beam_beat_ms(
                                      BEAM_BEAT_COUNT - 1)) ==
           BATTLE_EV_BEAM_WIN);
    assert(b.phase == BATTLE_PHASE_CLEAR);
    assert(b.stars >= 1);
    assert(battle_tick(&b, BATTLE_CLEAR_MS) == BATTLE_EV_STAGE_CLEAR);
}

static void test_boss_beam_lose(void)
{
    battle_t b;
    enter_boss_beam(&b);
    assert(battle_tick(&b, BATTLE_BEAM_MS) == BATTLE_EV_BEAM_LOSE);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.monster_hp == 2);
}

static void test_beam_force_decay_and_hit_window(void)
{
    battle_t b;
    enter_boss_beam(&b);
    assert(beam_reach_and_press(&b, 3000) == BATTLE_EV_BEAM_PERFECT);
    assert(battle_beam_force(&b) == 20);
    // 1200 ms later the second perfect beat lands after two decay ticks.
    assert(beam_reach_and_press(&b, 4200) == BATTLE_EV_BEAM_PERFECT);
    assert(battle_beam_force(&b) == 38);

    assert(beam_reach_and_press(&b, 5400) == BATTLE_EV_BEAM_PERFECT);
    assert(battle_beam_force(&b) == 56);
    assert(beam_reach_and_press(&b, 6600) == BATTLE_EV_BEAM_PERFECT);
    assert(battle_beam_force(&b) == 73);
    assert(beam_reach_and_press(&b, 7800) == BATTLE_EV_BEAM_WIN);
}

static void test_beam_four_hits_one_miss_wins(void)
{
    battle_t b;
    enter_boss_beam(&b);
    // 150 ms late is still a hit; 300 ms late is a miss.
    assert(beam_reach_and_press(&b, 3150) == BATTLE_EV_BEAM_HIT);
    assert(beam_reach_and_press(&b, 4350) == BATTLE_EV_BEAM_HIT);
    assert(beam_reach_and_press(&b, 5550) == BATTLE_EV_BEAM_HIT);
    assert(beam_reach_and_press(&b, 6750) == BATTLE_EV_BEAM_HIT);
    assert(beam_reach_and_press(&b, 8100) == BATTLE_EV_BEAM_WIN);
    assert(b.phase == BATTLE_PHASE_CLEAR);
}

static void test_beam_three_misses_lose(void)
{
    battle_t b;
    enter_boss_beam(&b);
    assert(beam_reach_and_press(&b, 3500) == BATTLE_EV_BEAM_MISS);
    assert(beam_reach_and_press(&b, 4700) == BATTLE_EV_BEAM_MISS);
    assert(beam_reach_and_press(&b, 5900) == BATTLE_EV_BEAM_MISS);
    assert(beam_reach_and_press(&b, 6600) == BATTLE_EV_BEAM_PERFECT);
    assert(beam_reach_and_press(&b, 7800) == BATTLE_EV_BEAM_LOSE);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(b.monster_hp == 2);
}

static void test_beam_timeout_keeps_three_hits(void)
{
    battle_t b;
    enter_boss_beam(&b);
    for (uint8_t i = 0; i < BEAM_WIN_HITS; i++) {
        assert(beam_reach_and_press(&b, (uint16_t)battle_beam_beat_ms(i)) ==
               BATTLE_EV_BEAM_PERFECT);
    }
    // Let the timer run out instead of touching the remaining beats: three
    // clean beats already satisfy the win condition.
    assert(battle_tick(&b, BATTLE_BEAM_MS - 5400) == BATTLE_EV_BEAM_WIN);
    assert(b.phase == BATTLE_PHASE_CLEAR);
}

static void test_blue_ultimate_feeds_beam_start_force(void)
{
    battle_t b;
    battle_start(&b, CAMPAIGN_STAGE_COUNT - 1, 1, 0, 1717);
    b.gauge = BATTLE_GAUGE_MAX;
    assert(battle_use_ultimate(&b) == BATTLE_EV_ULTIMATE_START);
    for (int i = 0; i < 3; i++) {
        ultimate_answer(&b, ULT_INPUT_OK);
    }
    assert(battle_tick(&b, 1130) == BATTLE_EV_ULTIMATE_DONE);
    assert(b.monster_hp == 5);

    // Finish the boss with a normal crit; the clean ultimate leaves the beam
    // leaning the hero's way.
    b.monster_hp = 1;
    answer_correct(&b);
    crit_hit(&b);
    assert(b.monster_hp == 0);
    assert(battle_tick(&b, BATTLE_ATTACK_MS) == BATTLE_EV_BEAM_START);
    assert(battle_beam_force(&b) == 25);
}

static void test_guard_qte(void)
{
    battle_t b;
    battle_start(&b, 5, 1, 0, 2001);
    const uint16_t hp = b.hero_hp;
    answer_wrong(&b);
    assert(b.phase == BATTLE_PHASE_GUARD);
    assert(b.hero_hp == hp);  // damage is deferred until the prompt resolves

    const uint8_t block =
        battle_guard_dir(&b) == 0 ? ULT_INPUT_UP : ULT_INPUT_DOWN;
    const uint16_t monster_before = b.monster_hp;
    assert(battle_guard_press(&b, block) == BATTLE_EV_GUARD_BLOCK);
    assert(b.hero_hp == hp);
    assert(b.monster_hp == monster_before - 1);
    assert(b.phase == BATTLE_PHASE_ENEMY);
    assert(battle_tick(&b, BATTLE_ENEMY_MS) == BATTLE_EV_ENEMY_DONE);
    assert(b.phase == BATTLE_PHASE_QUESTION);

    // Pressing the wrong direction fails the block and takes the hit.
    answer_wrong(&b);
    assert(b.phase == BATTLE_PHASE_GUARD);
    const uint8_t fail =
        battle_guard_dir(&b) == 0 ? ULT_INPUT_DOWN : ULT_INPUT_UP;
    assert(battle_guard_press(&b, fail) == BATTLE_EV_GUARD_FAIL);
    assert(b.hero_hp == hp - 1);
    assert(b.phase == BATTLE_PHASE_ENEMY);

    // Standing still also fails.
    assert(battle_tick(&b, BATTLE_ENEMY_MS) == BATTLE_EV_ENEMY_DONE);
    answer_wrong(&b);
    assert(b.phase == BATTLE_PHASE_GUARD);
    assert(battle_tick(&b, BATTLE_GUARD_MS) == BATTLE_EV_GUARD_FAIL);
    assert(b.hero_hp == hp - 2);
    assert(b.phase == BATTLE_PHASE_ENEMY);
}

static void test_armor_break_stage(void)
{
    battle_t b;
    battle_start(&b, 6, 1, 0, 2101);
    assert(b.monster_hp == 7);

    // Non-perfect crits only chip the armor.
    answer_correct(&b);
    crit_hit(&b);
    assert(b.monster_hp == 6);
    assert(!b.armor_broken);
    finish_attack(&b);

    // The perfect window breaks it and adds bonus damage.
    answer_correct(&b);
    crit_perfect(&b);
    assert(b.monster_hp == 2);
    assert(b.armor_broken);
    finish_attack(&b);

    answer_correct(&b);
    crit_hit(&b);
    assert(b.monster_hp == 0);
    assert(battle_tick(&b, BATTLE_ATTACK_MS) == BATTLE_EV_MONSTER_DEFEATED);
}

static void test_half_noncrit_stage(void)
{
    battle_t b;
    battle_start(&b, 2, 1, 0, 2102);
    assert(b.monster_hp == 5);

    answer_correct(&b);
    crit_hit(&b);
    assert(b.monster_hp == 4);  // 2 damage would be 3
    finish_attack(&b);

    answer_correct(&b);
    crit_perfect(&b);
    assert(b.monster_hp == 1);  // perfect keeps full damage
}

static void test_fast_gauge_stage(void)
{
    battle_t b;
    battle_start(&b, 3, 1, 0, 2103);
    assert(b.question_ms == 6800 * 9 / 10);
    answer_correct(&b);
    assert(b.gauge == 6);  // blue 5 + stage bonus 1
}

static void test_enrage_stage(void)
{
    battle_t b;
    battle_start(&b, CAMPAIGN_STAGE_COUNT - 1, 1, 0, 2104);
    assert(b.question_ms == 5200);
    b.monster_hp = 4;  // exactly half of 8
    answer_wrong(&b);
    assert(b.phase == BATTLE_PHASE_ENEMY);
    assert(battle_tick(&b, BATTLE_ENEMY_MS) == BATTLE_EV_ENEMY_DONE);
    assert(b.enraged);
    assert(b.question_ms == 4200);
}

static void test_combo_pursuit_and_finisher(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 2105);
    b.monster_hp = 99;
    b.monster_hp_max = 99;

    for (int i = 1; i <= 6; i++) {
        answer_correct(&b);
        crit_hit(&b);
        assert(b.combo == (uint8_t)i);
        assert(b.crit_pursuit == (i % 3 == 0));
        assert(b.combo_finisher == (i >= 6 && i % 3 == 0));
        finish_attack(&b);
    }
    // Combo three adds a pursuit strike, so six hits remove more than 6 * 2.
    assert(99 - b.monster_hp == 14);
}

static void test_review_injection_and_bonus(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 4242);
    b.monster_hp = 99;
    b.monster_hp_max = 99;

    math_question_t review[BATTLE_REVIEW_SLOTS];
    memset(review, 0, sizeof(review));
    for (int i = 0; i < BATTLE_REVIEW_SLOTS; i++) {
        snprintf(review[i].text, sizeof(review[i].text), "复习%d", i + 1);
        review[i].options[0] = (int16_t)(i + 1);
        review[i].options[1] = (int16_t)(i + 2);
        review[i].options[2] = (int16_t)(i + 3);
        review[i].answer = (int16_t)(i + 1);
        review[i].correct_index = 0;
        review[i].kind = QUESTION_ADD;
        review[i].math_stage = 1;
    }
    battle_queue_review(&b, review, BATTLE_REVIEW_SLOTS);
    assert(!battle_question_is_review(&b));

    // The first review replaces the second question.
    answer_correct(&b);
    crit_hit(&b);
    finish_attack(&b);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(battle_question_is_review(&b));
    assert(strcmp(b.question.text, "复习1") == 0);

    const int32_t before = b.score;
    answer_correct(&b);
    const int32_t speed =
        b.question_remaining_ms * 80 / b.question_ms;
    const int32_t expected = (100 + (int32_t)b.combo * 20 + speed) * 3 / 2;
    assert(b.score - before == expected);

    // Two more exchanges bring the fifth question, which carries review two.
    for (int i = 0; i < 2; i++) {
        crit_hit(&b);
        finish_attack(&b);
        answer_correct(&b);
    }
    crit_hit(&b);
    finish_attack(&b);
    assert(b.phase == BATTLE_PHASE_QUESTION);
    assert(battle_question_is_review(&b));
    assert(strcmp(b.question.text, "复习2") == 0);
}

static void test_low_age_hides_guard_and_widens_crit(void)
{
    battle_t b;
    battle_start(&b, 5, 1, 0, 5555);
    battle_set_low_age(&b, true);
    assert(battle_low_age(&b));
    b.cursor = b.question.correct_index == 0 ? 1 : 0;
    assert(battle_confirm(&b) == BATTLE_EV_WRONG);
    assert(b.phase == BATTLE_PHASE_ENEMY);

    battle_t normal;
    battle_start(&normal, 0, 1, 0, 6666);
    answer_correct(&normal);
    const uint8_t fast_window = normal.crit_window_effective;

    battle_t low;
    battle_start(&low, 0, 1, 0, 6666);
    battle_set_low_age(&low, true);
    answer_correct(&low);
    assert(low.crit_window_effective > fast_window);
    assert(low.crit_window_effective == 62);  // 40 -> 50 (fast) -> 62 (low age)
}

static void test_score_bonus(void)
{
    battle_t b;
    battle_start(&b, 0, 1, 0, 4242);
    battle_set_score_bonus(&b, 20);
    assert(b.score_bonus_pct == 20);

    const int32_t before = b.score;
    answer_correct(&b);
    const int32_t speed =
        b.question_remaining_ms * 80 / b.question_ms;
    const int32_t base = 100 + (int32_t)b.combo * 20 + speed;
    assert(b.score - before == base + base * 20 / 100);

    // battle_start clears the bonus for the next round.
    battle_t fresh;
    battle_start(&fresh, 0, 1, 0, 4242);
    assert(fresh.score_bonus_pct == 0);
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
    test_critical_sweep_is_readable();
    test_perfect_critical_and_speed_bonus();
    test_red_hero_perfect_damage();
    test_wrong_and_timeout();
    test_adaptive_assist_and_last_stand();
    test_star_difficulty_easing();
    test_shield_hero();
    test_red_crit_damage();
    test_hint();
    test_ultimate_blue_sequence();
    test_ultimate_gold_one_shot();
    test_ultimate_violet_alternating_and_bonus();
    test_ultimate_red_refunds_gauge();
    test_ultimate_emerald_wide_window();
    test_ultimate_misses();
    test_clear_and_stars();
    test_star_tiers();
    test_defeat();
    test_boss_beam_five_perfect_beats();
    test_boss_beam_lose();
    test_beam_force_decay_and_hit_window();
    test_beam_four_hits_one_miss_wins();
    test_beam_three_misses_lose();
    test_beam_timeout_keeps_three_hits();
    test_blue_ultimate_feeds_beam_start_force();
    test_guard_qte();
    test_armor_break_stage();
    test_half_noncrit_stage();
    test_fast_gauge_stage();
    test_enrage_stage();
    test_combo_pursuit_and_finisher();
    test_review_injection_and_bonus();
    test_low_age_hides_guard_and_widens_crit();
    test_score_bonus();
    test_pause_resume();
    test_history_avoids_repeats();
    puts("test_battle: PASS");
    return 0;
}

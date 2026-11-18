// Host tests for the per-hero ultimate beat patterns.
#include "ultimate.h"

#include <assert.h>
#include <stdio.h>

static void test_layout(void)
{
    assert(ultimate_total_ms(0) == 3 * 600 + ULT_TAIL_MS);
    assert(ultimate_total_ms(1) == 3 * 420 + ULT_TAIL_MS);
    assert(ultimate_total_ms(2) == 3 * 760 + ULT_TAIL_MS);
    assert(ultimate_total_ms(3) == 1 * 800 + ULT_TAIL_MS);
    assert(ultimate_total_ms(4) == 4 * 520 + ULT_TAIL_MS);
    // Out-of-range heroes fall back to blue.
    assert(ultimate_total_ms(99) == ultimate_total_ms(0));
}

static void test_blue_pattern(void)
{
    ultimate_t u;
    ultimate_start(&u, 0);
    assert(u.steps == 3);
    assert(ultimate_step_ms(&u) == 600);
    assert(ultimate_press_center_ms(&u) == 330);
    assert(ultimate_step_input(&u) == ULT_INPUT_OK);
    assert(!ultimate_done(&u));

    for (int i = 0; i < 3; i++) {
        assert(ultimate_step_index(&u) == (uint8_t)i);
        assert(ultimate_tick(&u, 330) == ULT_JUDGE_NONE);
        assert(ultimate_press(&u, ULT_INPUT_OK) == ULT_JUDGE_PERFECT);
    }
    assert(ultimate_all_hit(&u));
    assert(u.perfects == 3);
    assert(ultimate_step_index(&u) == 3);
    assert(!ultimate_done(&u));  // tail still playing
    assert(ultimate_tick(&u, 1130) == ULT_JUDGE_NONE);
    assert(ultimate_done(&u));
}

static void test_early_and_late_presses_miss(void)
{
    ultimate_t u;
    ultimate_start(&u, 0);
    // Too early: the marker has not reached the window yet.
    assert(ultimate_press(&u, ULT_INPUT_OK) == ULT_JUDGE_MISS);
    assert(ultimate_step_index(&u) == 1);

    // Letting the next beat run out also misses.
    assert(ultimate_tick(&u, 600) == ULT_JUDGE_MISS);
    assert(ultimate_step_index(&u) == 2);

    // Pressing the wrong button on the alternating hero misses immediately.
    ultimate_start(&u, 4);
    assert(ultimate_step_input(&u) == ULT_INPUT_UP);
    assert(ultimate_press(&u, ULT_INPUT_DOWN) == ULT_JUDGE_MISS);
    assert(ultimate_step_index(&u) == 1);
    assert(ultimate_step_input(&u) == ULT_INPUT_DOWN);
}

static void test_windows(void)
{
    ultimate_t u;
    // Emerald: 300 ms off centre is still a hit.
    ultimate_start(&u, 2);
    assert(ultimate_press_center_ms(&u) == 418);
    assert(ultimate_tick(&u, 418 + 300) == ULT_JUDGE_NONE);
    assert(ultimate_press(&u, ULT_INPUT_OK) == ULT_JUDGE_HIT);
    assert(u.hits == 1);
    assert(u.perfects == 0);

    // Gold: one heavy beat.
    ultimate_start(&u, 3);
    assert(u.steps == 1);
    assert(ultimate_step_damage(3, false) == 4);
    assert(ultimate_step_damage(3, true) == 5);

    // Red: fast beats, heavy damage.
    ultimate_start(&u, 1);
    assert(ultimate_step_damage(1, false) == 2);
    assert(ultimate_step_damage(1, true) == 3);
    assert(ultimate_total_ms(1) == 1580);
}

static void test_violet_alternation_and_clean_run(void)
{
    ultimate_t u;
    ultimate_start(&u, 4);
    assert(u.steps == 4);
    for (int i = 0; i < 4; i++) {
        const uint8_t want = (i & 1) ? ULT_INPUT_DOWN : ULT_INPUT_UP;
        assert(ultimate_step_input(&u) == want);
        assert(ultimate_tick(&u, 286) == ULT_JUDGE_NONE);
        assert(ultimate_press(&u, want) == ULT_JUDGE_PERFECT);
    }
    assert(ultimate_all_hit(&u));
    assert(u.perfects == 4);
}

static void test_timeout_crosses_multiple_beats(void)
{
    ultimate_t u;
    ultimate_start(&u, 0);
    // A stalled frame can cross two beats; both are consumed as misses.
    assert(ultimate_tick(&u, 1200) == ULT_JUDGE_MISS);
    assert(ultimate_step_index(&u) == 2);

    // Presses after all beats are spent do nothing.
    ultimate_start(&u, 3);
    assert(ultimate_tick(&u, 800) == ULT_JUDGE_MISS);
    (void)ultimate_tick(&u, ULT_TAIL_MS);
    assert(ultimate_done(&u));
    assert(ultimate_press(&u, ULT_INPUT_OK) == ULT_JUDGE_NONE);
}

int main(void)
{
    test_layout();
    test_blue_pattern();
    test_early_and_late_presses_miss();
    test_windows();
    test_violet_alternation_and_clean_run();
    test_timeout_crosses_multiple_beats();
    puts("test_ultimate: PASS");
    return 0;
}
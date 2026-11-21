// Host tests for the five-beat boss beam duel.
#include "beam_rhythm.h"

#include <assert.h>
#include <stdio.h>

static const uint16_t BEATS[BEAM_BEAT_COUNT] = {3000, 4200, 5400, 6600, 7800};

static void test_layout(void)
{
    assert(BEAM_BEAT_COUNT == 5);
    assert(BEAM_LEAD_IN_MS == 3000);
    assert(BEAM_BEAT_INTERVAL_MS == 1200);
    assert(beam_rhythm_beat_ms(0) == 3000);
    assert(beam_rhythm_beat_ms(4) == 7800);
    assert(beam_rhythm_beat_ms(5) == UINT32_MAX);
}

static void test_perfect_run_wins(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    assert(r.force == 0);
    assert(beam_rhythm_beats_left(&r) == 5);
    assert(beam_rhythm_next_beat_ms(&r) == 3000);

    uint32_t elapsed = 0;
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        beam_rhythm_tick(&r, BEATS[i] - elapsed);
        elapsed = BEATS[i];
        assert(beam_rhythm_press(&r, elapsed) == BEAM_JUDGE_PERFECT);
        assert(beam_rhythm_judge(&r, (uint8_t)i) == BEAM_JUDGE_PERFECT);
    }
    assert(beam_rhythm_beats_left(&r) == 0);
    assert(beam_rhythm_won(&r));
    assert(r.perfects == 5);
    assert(beam_rhythm_clean(&r) == 5);
    assert(beam_rhythm_goal(&r) == BEAM_WIN_HITS);
}

static void test_count_in_presses_are_ignored(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    // A child tapping along with the count-in must not spend a beat or lose
    // force before the first real beat.
    assert(beam_rhythm_press(&r, 1000) == BEAM_JUDGE_NONE);
    assert(beam_rhythm_press(&r, 2000) == BEAM_JUDGE_NONE);
    assert(beam_rhythm_beats_left(&r) == 5);
    assert(r.force == 0);

    // Once the window opens an early press counts.
    assert(beam_rhythm_press(&r, 2800) == BEAM_JUDGE_HIT);
    assert(beam_rhythm_beats_left(&r) == 4);
}

static void test_one_miss_still_wins(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    uint32_t elapsed = 0;
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        // Tap long after the fourth beat: the beat is spent and costs force.
        const uint32_t press_at = i == 3 ? BEATS[i] + 400 : BEATS[i];
        beam_rhythm_tick(&r, press_at - elapsed);
        elapsed = press_at;
        assert(beam_rhythm_press(&r, press_at) ==
               (i == 3 ? BEAM_JUDGE_MISS : BEAM_JUDGE_PERFECT));
    }
    assert(beam_rhythm_beats_left(&r) == 0);
    assert(beam_rhythm_won(&r));
}

static void test_three_misses_lose(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    uint32_t elapsed = 0;
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        const uint32_t press_at = i >= 2 ? BEATS[i] + 400 : BEATS[i];
        beam_rhythm_tick(&r, press_at - elapsed);
        elapsed = press_at;
        assert(beam_rhythm_press(&r, press_at) ==
               (i >= 2 ? BEAM_JUDGE_MISS : BEAM_JUDGE_PERFECT));
    }
    assert(beam_rhythm_beats_left(&r) == 0);
    assert(!beam_rhythm_won(&r));
}

static void test_expire_spends_untouched_beats(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    for (int i = 0; i < BEAM_WIN_HITS; i++) {
        beam_rhythm_tick(&r, BEATS[i] - (i == 0 ? 0 : BEATS[i - 1]));
        assert(beam_rhythm_press(&r, BEATS[i]) == BEAM_JUDGE_PERFECT);
    }
    assert(beam_rhythm_beats_left(&r) == BEAM_BEAT_COUNT - BEAM_WIN_HITS);

    // Never touching the remaining beats must not throw away three clean hits.
    beam_rhythm_expire(&r, 8400);
    assert(beam_rhythm_beats_left(&r) == 0);
    assert(beam_rhythm_clean(&r) == BEAM_WIN_HITS);
    assert(beam_rhythm_won(&r));
}

static void test_decay(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 50);
    beam_rhythm_tick(&r, 2000);
    assert(r.force == 46);  // 2 force per 1000 ms

    // Force never underflows.
    beam_rhythm_tick(&r, 60000);
    assert(r.force == 0);
}

static void test_extra_presses_bleed_force(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 100);
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        (void)beam_rhythm_press(&r, 999999);
    }
    assert(beam_rhythm_beats_left(&r) == 0);
    const uint8_t before = r.force;
    assert(beam_rhythm_press(&r, 3000) == BEAM_JUDGE_MISS);
    assert(r.force == before - BEAM_FORCE_EXTRA_MISS);
}

static void test_start_force_and_wrap(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 25);
    assert(r.force == 25);
    assert(!beam_rhythm_won(&r));
    beam_rhythm_start(&r, 200);
    assert(r.force == 100);

    uint32_t elapsed = 0;
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        beam_rhythm_tick(&r, BEATS[i] - elapsed);
        elapsed = BEATS[i];
        (void)beam_rhythm_press(&r, elapsed);
    }
    assert(beam_rhythm_won(&r));
}

static void test_judge_accessor_bounds(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    assert(beam_rhythm_judge(&r, 0) == BEAM_JUDGE_NONE);
    assert(beam_rhythm_judge(&r, BEAM_BEAT_COUNT) == BEAM_JUDGE_NONE);
}

int main(void)
{
    test_layout();
    test_perfect_run_wins();
    test_count_in_presses_are_ignored();
    test_one_miss_still_wins();
    test_three_misses_lose();
    test_expire_spends_untouched_beats();
    test_decay();
    test_extra_presses_bleed_force();
    test_start_force_and_wrap();
    test_judge_accessor_bounds();
    puts("test_beam_rhythm: PASS");
    return 0;
}
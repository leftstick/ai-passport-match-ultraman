// Host tests for the five-beat boss beam duel.
#include "beam_rhythm.h"

#include <assert.h>
#include <stdio.h>

static void test_layout(void)
{
    assert(BEAM_BEAT_COUNT == 5);
    assert(beam_rhythm_beat_ms(0) == 500);
    assert(beam_rhythm_beat_ms(4) == 2900);
    assert(beam_rhythm_beat_ms(5) == UINT32_MAX);
}

static void test_perfect_run_wins(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    assert(r.force == 0);
    assert(beam_rhythm_beats_left(&r) == 5);
    assert(beam_rhythm_next_beat_ms(&r) == 500);

    uint32_t elapsed = 0;
    const uint16_t beats[BEAM_BEAT_COUNT] = {500, 1100, 1700, 2300, 2900};
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        beam_rhythm_tick(&r, beats[i] - elapsed);
        elapsed = beats[i];
        assert(beam_rhythm_press(&r, elapsed) == BEAM_JUDGE_PERFECT);
    }
    assert(beam_rhythm_beats_left(&r) == 0);
    assert(beam_rhythm_won(&r));
    assert(r.perfects == 5);
    assert(r.force > BEAM_WIN_FORCE);  // five perfects minus a few decay ticks
}

static void test_four_hits_one_miss_loses(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 0);
    assert(beam_rhythm_press(&r, 600) == BEAM_JUDGE_HIT);   // 100 ms late
    assert(beam_rhythm_press(&r, 1200) == BEAM_JUDGE_HIT);
    assert(beam_rhythm_press(&r, 1800) == BEAM_JUDGE_HIT);
    assert(beam_rhythm_press(&r, 2400) == BEAM_JUDGE_HIT);
    assert(beam_rhythm_press(&r, 2600) == BEAM_JUDGE_MISS);  // 300 ms early
    assert(beam_rhythm_beats_left(&r) == 0);
    assert(!beam_rhythm_won(&r));
}

static void test_decay(void)
{
    beam_rhythm_t r;
    beam_rhythm_start(&r, 50);
    beam_rhythm_tick(&r, 2000);
    assert(r.force == 44);  // 3 force per 1000 ms

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
    const uint16_t beats[BEAM_BEAT_COUNT] = {500, 1100, 1700, 2300, 2900};
    for (int i = 0; i < BEAM_BEAT_COUNT; i++) {
        beam_rhythm_tick(&r, beats[i] - elapsed);
        elapsed = beats[i];
        (void)beam_rhythm_press(&r, elapsed);
    }
    assert(beam_rhythm_won(&r));
}

int main(void)
{
    test_layout();
    test_perfect_run_wins();
    test_four_hits_one_miss_loses();
    test_decay();
    test_extra_presses_bleed_force();
    test_start_force_and_wrap();
    puts("test_beam_rhythm: PASS");
    return 0;
}
#include "beam_rhythm.h"

#include <stddef.h>

static const uint16_t BEAT_MS[BEAM_BEAT_COUNT] = {500, 1100, 1700, 2300, 2900};

static uint8_t clamp_force(int32_t force)
{
    if (force < 0) {
        return 0;
    }
    if (force > 100) {
        return 100;
    }
    return (uint8_t)force;
}

void beam_rhythm_start(beam_rhythm_t *rhythm, uint8_t start_force)
{
    rhythm->force = clamp_force(start_force);
    rhythm->next_beat = 0;
    rhythm->perfects = 0;
    rhythm->hits = 0;
    rhythm->decay_acc = 0;
}

beam_judge_t beam_rhythm_press(beam_rhythm_t *rhythm, uint32_t elapsed_ms)
{
    if (rhythm->next_beat >= BEAM_BEAT_COUNT) {
        // Mashing after the fifth beat only bleeds force away.
        rhythm->force = clamp_force((int32_t)rhythm->force - BEAM_FORCE_EXTRA_MISS);
        return BEAM_JUDGE_MISS;
    }

    const int32_t beat = (int32_t)BEAT_MS[rhythm->next_beat];
    const int32_t delta = (int32_t)elapsed_ms - beat;
    const int32_t distance = delta < 0 ? -delta : delta;
    rhythm->next_beat++;

    if (distance <= BEAM_PERFECT_MS) {
        rhythm->force = clamp_force((int32_t)rhythm->force + BEAM_FORCE_PERFECT);
        rhythm->perfects++;
        return BEAM_JUDGE_PERFECT;
    }
    if (distance <= BEAM_WINDOW_MS) {
        rhythm->force = clamp_force((int32_t)rhythm->force + BEAM_FORCE_HIT);
        rhythm->hits++;
        return BEAM_JUDGE_HIT;
    }

    // An early or late press still consumes the beat so rapid mashing cannot
    // walk through the whole pattern without timing.
    rhythm->force = clamp_force((int32_t)rhythm->force - BEAM_FORCE_MISS);
    return BEAM_JUDGE_MISS;
}

void beam_rhythm_tick(beam_rhythm_t *rhythm, uint32_t elapsed_ms)
{
    if (elapsed_ms == 0 || rhythm->force == 0) {
        return;
    }
    uint32_t acc = (uint32_t)rhythm->decay_acc +
                   elapsed_ms * (uint32_t)BEAM_DECAY_PER_1000MS;
    while (acc >= 1000u && rhythm->force > 0) {
        acc -= 1000u;
        rhythm->force--;
    }
    rhythm->decay_acc = (uint16_t)acc;
}

bool beam_rhythm_won(const beam_rhythm_t *rhythm)
{
    return rhythm->next_beat >= BEAM_BEAT_COUNT &&
           rhythm->force >= BEAM_WIN_FORCE;
}

uint8_t beam_rhythm_beats_left(const beam_rhythm_t *rhythm)
{
    if (rhythm->next_beat >= BEAM_BEAT_COUNT) {
        return 0;
    }
    return (uint8_t)(BEAM_BEAT_COUNT - rhythm->next_beat);
}

uint32_t beam_rhythm_next_beat_ms(const beam_rhythm_t *rhythm)
{
    if (rhythm->next_beat >= BEAM_BEAT_COUNT) {
        return UINT32_MAX;
    }
    return BEAT_MS[rhythm->next_beat];
}

uint32_t beam_rhythm_beat_ms(uint8_t index)
{
    if (index >= BEAM_BEAT_COUNT) {
        return UINT32_MAX;
    }
    return BEAT_MS[index];
}
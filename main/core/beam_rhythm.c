#include "beam_rhythm.h"

#include <stddef.h>

static uint32_t beat_time(uint8_t index)
{
    return (uint32_t)BEAM_LEAD_IN_MS + (uint32_t)index * BEAM_BEAT_INTERVAL_MS;
}

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
    rhythm->grace = start_force >= BEAM_GRACE_START_FORCE ? 1 : 0;
    rhythm->decay_acc = 0;
    for (uint8_t i = 0; i < BEAM_BEAT_COUNT; i++) {
        rhythm->judges[i] = BEAM_JUDGE_NONE;
    }
}

beam_judge_t beam_rhythm_press(beam_rhythm_t *rhythm, uint32_t elapsed_ms)
{
    if (rhythm->next_beat >= BEAM_BEAT_COUNT) {
        // Mashing after the fifth beat only bleeds force away.
        rhythm->force = clamp_force((int32_t)rhythm->force - BEAM_FORCE_EXTRA_MISS);
        return BEAM_JUDGE_MISS;
    }

    const uint8_t index = rhythm->next_beat;
    const int32_t beat = (int32_t)beat_time(index);
    const int32_t delta = (int32_t)elapsed_ms - beat;
    const int32_t distance = delta < 0 ? -delta : delta;

    // Before the window opens the press is a rehearsal: keep the beat intact.
    if (delta < -(int32_t)BEAM_WINDOW_MS) {
        return BEAM_JUDGE_NONE;
    }

    const beam_judge_t judge = distance <= BEAM_PERFECT_MS  ? BEAM_JUDGE_PERFECT
                               : distance <= BEAM_WINDOW_MS ? BEAM_JUDGE_HIT
                                                            : BEAM_JUDGE_MISS;
    rhythm->next_beat++;
    rhythm->judges[index] = (uint8_t)judge;

    if (judge == BEAM_JUDGE_PERFECT) {
        rhythm->force = clamp_force((int32_t)rhythm->force + BEAM_FORCE_PERFECT);
        rhythm->perfects++;
    } else if (judge == BEAM_JUDGE_HIT) {
        rhythm->force = clamp_force((int32_t)rhythm->force + BEAM_FORCE_HIT);
        rhythm->hits++;
    } else {
        // An early or late press still consumes the beat so rapid mashing
        // cannot walk through the whole pattern without timing.
        rhythm->force = clamp_force((int32_t)rhythm->force - BEAM_FORCE_MISS);
    }
    return judge;
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

void beam_rhythm_expire(beam_rhythm_t *rhythm, uint32_t elapsed_ms)
{
    while (rhythm->next_beat < BEAM_BEAT_COUNT) {
        (void)beam_rhythm_press(rhythm, elapsed_ms);
    }
}

bool beam_rhythm_won(const beam_rhythm_t *rhythm)
{
    return rhythm->next_beat >= BEAM_BEAT_COUNT &&
           (uint8_t)(rhythm->perfects + rhythm->hits + rhythm->grace) >=
               BEAM_WIN_HITS;
}

uint8_t beam_rhythm_clean(const beam_rhythm_t *rhythm)
{
    return (uint8_t)(rhythm->perfects + rhythm->hits);
}

uint8_t beam_rhythm_goal(const beam_rhythm_t *rhythm)
{
    return (uint8_t)(BEAM_WIN_HITS - rhythm->grace);
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
    return beat_time(rhythm->next_beat);
}

uint32_t beam_rhythm_beat_ms(uint8_t index)
{
    if (index >= BEAM_BEAT_COUNT) {
        return UINT32_MAX;
    }
    return beat_time(index);
}

beam_judge_t beam_rhythm_judge(const beam_rhythm_t *rhythm, uint8_t index)
{
    if (index >= BEAM_BEAT_COUNT) {
        return BEAM_JUDGE_NONE;
    }
    return (beam_judge_t)rhythm->judges[index];
}
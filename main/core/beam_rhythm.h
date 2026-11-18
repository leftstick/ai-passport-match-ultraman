// Pure five-beat beam duel timing. No LVGL/ESP-IDF dependencies.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BEAM_BEAT_COUNT 5
// All five beats must be spent before the duel resolves. Five clean hits
// (5 * 16) clear the bar, while four hits plus one miss (4 * 16 - 10 minus
// decay) do not.
#define BEAM_WIN_FORCE 60
#define BEAM_PERFECT_MS 70
#define BEAM_WINDOW_MS 180
#define BEAM_FORCE_PERFECT 20
#define BEAM_FORCE_HIT 16
#define BEAM_FORCE_MISS 10
#define BEAM_FORCE_EXTRA_MISS 5
#define BEAM_DECAY_PER_1000MS 3

typedef enum {
    BEAM_JUDGE_NONE = 0,
    BEAM_JUDGE_PERFECT,
    BEAM_JUDGE_HIT,
    BEAM_JUDGE_MISS,
} beam_judge_t;

typedef struct {
    uint8_t force;
    uint8_t next_beat;
    uint8_t perfects;
    uint8_t hits;
    uint16_t decay_acc;
} beam_rhythm_t;

void beam_rhythm_start(beam_rhythm_t *rhythm, uint8_t start_force);
beam_judge_t beam_rhythm_press(beam_rhythm_t *rhythm, uint32_t elapsed_ms);
void beam_rhythm_tick(beam_rhythm_t *rhythm, uint32_t elapsed_ms);

bool beam_rhythm_won(const beam_rhythm_t *rhythm);
uint8_t beam_rhythm_beats_left(const beam_rhythm_t *rhythm);
// Absolute time of the next beat, or UINT32_MAX when all beats were consumed.
uint32_t beam_rhythm_next_beat_ms(const beam_rhythm_t *rhythm);
uint32_t beam_rhythm_beat_ms(uint8_t index);
// Pure five-beat beam duel timing. No LVGL/ESP-IDF dependencies.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BEAM_BEAT_COUNT 5
// The first beat waits behind a three-second count-in so the player can hear
// the metronome and find the gold band before timing counts.
#define BEAM_LEAD_IN_MS 3000
#define BEAM_BEAT_INTERVAL_MS 1200
// The duel is won by landing three clean beats out of five. A charged start
// (the blue ultimate) grants one grace beat, so two clean beats can win.
#define BEAM_WIN_HITS 3
#define BEAM_GRACE_START_FORCE 25
#define BEAM_PERFECT_MS 120
#define BEAM_WINDOW_MS 260
#define BEAM_FORCE_PERFECT 20
#define BEAM_FORCE_HIT 16
#define BEAM_FORCE_MISS 10
#define BEAM_FORCE_EXTRA_MISS 5
#define BEAM_DECAY_PER_1000MS 2

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
    uint8_t grace;
    uint8_t judges[BEAM_BEAT_COUNT];
    uint16_t decay_acc;
} beam_rhythm_t;

void beam_rhythm_start(beam_rhythm_t *rhythm, uint8_t start_force);
// Presses before the hit window opens are ignored rather than consuming the
// beat: the count-in is a practice run, not a hidden penalty.
beam_judge_t beam_rhythm_press(beam_rhythm_t *rhythm, uint32_t elapsed_ms);
void beam_rhythm_tick(beam_rhythm_t *rhythm, uint32_t elapsed_ms);
// Spend every beat the player never touched as a miss. Used when the duel
// timer runs out so a run that already landed enough clean beats still wins.
void beam_rhythm_expire(beam_rhythm_t *rhythm, uint32_t elapsed_ms);

bool beam_rhythm_won(const beam_rhythm_t *rhythm);
uint8_t beam_rhythm_beats_left(const beam_rhythm_t *rhythm);
// Absolute time of the next beat, or UINT32_MAX when all beats were consumed.
uint32_t beam_rhythm_next_beat_ms(const beam_rhythm_t *rhythm);
uint32_t beam_rhythm_beat_ms(uint8_t index);
beam_judge_t beam_rhythm_judge(const beam_rhythm_t *rhythm, uint8_t index);
// Clean beats landed so far and how many are needed to win.
uint8_t beam_rhythm_clean(const beam_rhythm_t *rhythm);
uint8_t beam_rhythm_goal(const beam_rhythm_t *rhythm);
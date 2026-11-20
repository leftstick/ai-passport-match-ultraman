// Pure per-hero ultimate input sequences. No LVGL/ESP-IDF dependencies.
//
// Each hero owns a short rhythm pattern instead of one shared cutscene:
//   blue    three beats, steady 600 ms window, one damage each
//   red     three fast beats, high damage, gauge refund on a clean run
//   emerald three slow beats with a much wider timing window
//   gold    one heavy beat, the biggest single hit in the game
//   violet  four beats alternating up/down, shield refresh on a clean run
//   dark    three lightning-fast beats with heavy per-beat damage
//   royal   two slow, wide-window beats that hit the hardest
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ULT_MAX_STEPS 4
#define ULT_TAIL_MS 320
#define ULT_CENTER_PERMILLE 550

typedef enum {
    ULT_INPUT_OK = 0,
    ULT_INPUT_UP,
    ULT_INPUT_DOWN,
} ult_input_t;

typedef enum {
    ULT_JUDGE_NONE = 0,
    ULT_JUDGE_PERFECT,
    ULT_JUDGE_HIT,
    ULT_JUDGE_MISS,
} ult_judge_t;

typedef struct {
    uint8_t hero;
    uint8_t steps;
    uint8_t step;           // current beat; == steps once all beats were used
    uint8_t hits;           // hit or perfect
    uint8_t perfects;
    uint8_t last_judge;     // ult_judge_t of the most recent resolution
    uint8_t last_step;      // beat that produced last_judge
    uint16_t step_ms;
    uint16_t step_elapsed;
    uint16_t elapsed_ms;    // whole sequence, including the tail
    uint16_t total_ms;
} ultimate_t;

void ultimate_start(ultimate_t *ult, uint8_t hero);

// Resolve one press. The caller maps physical buttons to ULT_INPUT_*.
ult_judge_t ultimate_press(ultimate_t *ult, uint8_t input);

// Advance time. Returns ULT_JUDGE_MISS each time a beat times out; callers
// ticking in small steps (the 33 ms game clock) see one miss per beat.
ult_judge_t ultimate_tick(ultimate_t *ult, uint32_t elapsed_ms);

bool ultimate_done(const ultimate_t *ult);
bool ultimate_all_hit(const ultimate_t *ult);

uint8_t ultimate_steps(const ultimate_t *ult);
uint8_t ultimate_step_index(const ultimate_t *ult);
uint8_t ultimate_step_input(const ultimate_t *ult);
uint16_t ultimate_step_ms(const ultimate_t *ult);
uint16_t ultimate_step_remaining_ms(const ultimate_t *ult);
uint16_t ultimate_press_center_ms(const ultimate_t *ult);

uint16_t ultimate_total_ms(uint8_t hero);
uint8_t ultimate_step_damage(uint8_t hero, bool perfect);
#include "ultimate.h"

#include "campaign.h"

#include <stddef.h>

// HERO_PERK order: gauge, crit damage, time, crit zone, shield.
typedef struct {
    uint8_t steps;
    uint16_t step_ms;
    uint8_t alternating;
    uint16_t perfect_ms;
    uint16_t window_ms;
    uint8_t damage_hit;
    uint8_t damage_perfect;
} ult_def_t;

static const ult_def_t DEFS[CAMPAIGN_HERO_COUNT] = {
    {3, 600, 0, 70, 180, 1, 1},    // blue: steady three-beat beam
    {3, 420, 0, 60, 150, 2, 3},    // red: fast, heavy hits
    {3, 760, 0, 126, 324, 1, 1},   // emerald: wide, forgiving beats
    {1, 800, 0, 90, 220, 4, 5},    // gold: one charged strike
    {4, 520, 1, 70, 170, 1, 1},    // violet: alternating up/down
};

static const ult_def_t *def_for(uint8_t hero)
{
    if (hero >= CAMPAIGN_HERO_COUNT) {
        hero = 0;
    }
    return &DEFS[hero];
}

void ultimate_start(ultimate_t *ult, uint8_t hero)
{
    if (hero >= CAMPAIGN_HERO_COUNT) {
        hero = 0;
    }
    const ult_def_t *def = def_for(hero);
    ult->hero = hero;
    ult->steps = def->steps;
    ult->step = 0;
    ult->hits = 0;
    ult->perfects = 0;
    ult->last_judge = ULT_JUDGE_NONE;
    ult->last_step = 0xFF;
    ult->step_ms = def->step_ms;
    ult->step_elapsed = 0;
    ult->elapsed_ms = 0;
    ult->total_ms = ultimate_total_ms(hero);
}

static void consume_step(ultimate_t *ult, ult_judge_t judge)
{
    ult->last_judge = (uint8_t)judge;
    ult->last_step = ult->step;
    if (judge == ULT_JUDGE_HIT) {
        ult->hits++;
    } else if (judge == ULT_JUDGE_PERFECT) {
        ult->hits++;
        ult->perfects++;
    }
    ult->step++;
    ult->step_elapsed = 0;
}

ult_judge_t ultimate_press(ultimate_t *ult, uint8_t input)
{
    if (ultimate_done(ult) || ult->step >= ult->steps) {
        return ULT_JUDGE_NONE;
    }
    const ult_def_t *def = def_for(ult->hero);

    if (input != ultimate_step_input(ult)) {
        consume_step(ult, ULT_JUDGE_MISS);
        return ULT_JUDGE_MISS;
    }

    const int32_t center = (int32_t)((uint32_t)ult->step_ms *
                                     ULT_CENTER_PERMILLE / 1000u);
    int32_t distance = (int32_t)ult->step_elapsed - center;
    if (distance < 0) {
        distance = -distance;
    }

    ult_judge_t judge = ULT_JUDGE_MISS;
    if (distance <= (int32_t)def->perfect_ms) {
        judge = ULT_JUDGE_PERFECT;
    } else if (distance <= (int32_t)def->window_ms) {
        judge = ULT_JUDGE_HIT;
    }
    consume_step(ult, judge);
    return judge;
}

ult_judge_t ultimate_tick(ultimate_t *ult, uint32_t elapsed_ms)
{
    if (elapsed_ms == 0 || ultimate_done(ult)) {
        return ULT_JUDGE_NONE;
    }

    uint32_t add = elapsed_ms;
    if ((uint32_t)ult->elapsed_ms + add > ult->total_ms) {
        add = (uint32_t)ult->total_ms - ult->elapsed_ms;
    }
    ult->elapsed_ms = (uint16_t)(ult->elapsed_ms + add);

    bool missed = false;
    uint32_t left = add;
    // A large tick (host tests, a stalled frame) may cross several beats. Each
    // crossed beat is a miss; the caller sees one miss event for the tick.
    while (left > 0 && ult->step < ult->steps) {
        const uint32_t need =
            (uint32_t)ult->step_ms - (uint32_t)ult->step_elapsed;
        if (left < need) {
            ult->step_elapsed = (uint16_t)(ult->step_elapsed + left);
            left = 0;
        } else {
            left -= need;
            consume_step(ult, ULT_JUDGE_MISS);
            missed = true;
        }
    }
    return missed ? ULT_JUDGE_MISS : ULT_JUDGE_NONE;
}

bool ultimate_done(const ultimate_t *ult)
{
    return ult->elapsed_ms >= ult->total_ms;
}

bool ultimate_all_hit(const ultimate_t *ult)
{
    return ult->steps > 0 && ult->hits == ult->steps;
}

uint8_t ultimate_steps(const ultimate_t *ult)
{
    return ult->steps;
}

uint8_t ultimate_step_index(const ultimate_t *ult)
{
    return ult->step;
}

uint8_t ultimate_step_input(const ultimate_t *ult)
{
    if (ult->step >= ult->steps) {
        return ULT_INPUT_OK;
    }
    const ult_def_t *def = def_for(ult->hero);
    if (!def->alternating) {
        return ULT_INPUT_OK;
    }
    // Up, down, up, down...
    return (ult->step & 1u) ? ULT_INPUT_DOWN : ULT_INPUT_UP;
}

uint16_t ultimate_step_ms(const ultimate_t *ult)
{
    return ult->step_ms;
}

uint16_t ultimate_step_remaining_ms(const ultimate_t *ult)
{
    if (ult->step >= ult->steps) {
        return 0;
    }
    return (uint16_t)(ult->step_ms - ult->step_elapsed);
}

uint16_t ultimate_press_center_ms(const ultimate_t *ult)
{
    return (uint16_t)((uint32_t)ult->step_ms * ULT_CENTER_PERMILLE / 1000u);
}

uint16_t ultimate_total_ms(uint8_t hero)
{
    const ult_def_t *def = def_for(hero);
    return (uint16_t)((uint32_t)def->steps * def->step_ms + ULT_TAIL_MS);
}

uint8_t ultimate_step_damage(uint8_t hero, bool perfect)
{
    const ult_def_t *def = def_for(hero);
    return perfect ? def->damage_perfect : def->damage_hit;
}
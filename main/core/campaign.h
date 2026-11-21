// Static campaign content: heroes, stages, difficulties.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAMPAIGN_HERO_COUNT 16
#define CAMPAIGN_STAGE_COUNT 9
#define CAMPAIGN_DIFFICULTY_COUNT 3

// Per-stage twists. Each stage keeps at most one or two so the rule stays
// readable on the stage list and in the battle footer.
#define STAGE_RULE_HINT_FREE    0x01u  // hints cost no question time
#define STAGE_RULE_HALF_NONCRIT 0x02u  // non-perfect crits deal 1 damage
#define STAGE_RULE_FAST_GAUGE   0x04u  // -10% question time, +1 gauge on correct
#define STAGE_RULE_GUARD_QTE    0x08u  // wrong answers open a block prompt
#define STAGE_RULE_ARMOR_BREAK  0x10u  // only a perfect crit breaks the armor
#define STAGE_RULE_ENRAGE       0x20u  // boss speeds up below half HP

typedef enum {
    HERO_PERK_GAUGE = 0,
    HERO_PERK_CRIT_DAMAGE,
    HERO_PERK_TIME,
    HERO_PERK_CRIT_ZONE,
    HERO_PERK_SHIELD,
} hero_perk_t;

typedef struct {
    const char *name;
    const char *short_name;
    const char *perk_text;
    const char *sprite_prefix;
    uint32_t accent_color;
    uint8_t perk;
} hero_def_t;

typedef struct {
    const char *name;
    const char *monster_name;
    const char *domain;
    const char *sprite_name;
    uint8_t math_stage;
    uint16_t question_ms;
    uint8_t monster_hp;
    uint8_t rules;
    bool boss;
} stage_def_t;

typedef struct {
    const char *name;
    const char *detail;
    int16_t time_bonus_ms;
    uint8_t hero_hp;
    int8_t monster_hp_delta;
    // Shifts the curriculum tier per difficulty. 星光 runs one tier easier so
    // the first difficulty never jumps straight into 100以内 regrouping.
    int8_t math_stage_delta;
    uint8_t crit_window;
} difficulty_def_t;

const hero_def_t *campaign_hero(uint8_t index);
const stage_def_t *campaign_stage(uint8_t index);
const difficulty_def_t *campaign_difficulty(uint8_t index);

uint16_t campaign_question_ms(uint8_t stage, uint8_t difficulty, uint8_t hero);
// Curriculum tier (1..4) actually used for a stage on this difficulty.
uint8_t campaign_math_stage(uint8_t stage, uint8_t difficulty);
uint8_t campaign_hero_hp(uint8_t difficulty);
uint8_t campaign_monster_hp(uint8_t stage, uint8_t difficulty);
uint8_t campaign_crit_window(uint8_t difficulty, uint8_t hero);
bool campaign_stage_has_rule(uint8_t stage, uint8_t rule);

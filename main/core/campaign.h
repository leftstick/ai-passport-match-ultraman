// Static campaign content: heroes, stages, difficulties.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAMPAIGN_HERO_COUNT 5
#define CAMPAIGN_STAGE_COUNT 8
#define CAMPAIGN_DIFFICULTY_COUNT 3

typedef enum {
    HERO_PERK_GAUGE = 0,
    HERO_PERK_CRIT_DAMAGE,
    HERO_PERK_TIME,
    HERO_PERK_CRIT_ZONE,
    HERO_PERK_SHIELD,
} hero_perk_t;

typedef struct {
    const char *name;
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
    bool boss;
} stage_def_t;

typedef struct {
    const char *name;
    const char *detail;
    int16_t time_bonus_ms;
    uint8_t hero_hp;
    int8_t monster_hp_delta;
    uint8_t crit_window;
} difficulty_def_t;

const hero_def_t *campaign_hero(uint8_t index);
const stage_def_t *campaign_stage(uint8_t index);
const difficulty_def_t *campaign_difficulty(uint8_t index);

uint16_t campaign_question_ms(uint8_t stage, uint8_t difficulty, uint8_t hero);
uint8_t campaign_hero_hp(uint8_t difficulty);
uint8_t campaign_monster_hp(uint8_t stage, uint8_t difficulty);
uint8_t campaign_crit_window(uint8_t difficulty, uint8_t hero);

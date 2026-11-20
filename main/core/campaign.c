#include "campaign.h"

#include <stddef.h>

static const hero_def_t HEROES[CAMPAIGN_HERO_COUNT] = {
    { .name = "赛罗奥特曼", .perk_text = "必杀槽 +25%", .sprite_prefix = "blue",
      .accent_color = 0x4C9DFF, .perk = HERO_PERK_GAUGE },
    { .name = "泰罗奥特曼", .perk_text = "暴击伤害 +1", .sprite_prefix = "red",
      .accent_color = 0xFF5A5A, .perk = HERO_PERK_CRIT_DAMAGE },
    { .name = "高斯奥特曼", .perk_text = "答题时间 +0.8秒", .sprite_prefix = "emerald",
      .accent_color = 0x35D07F, .perk = HERO_PERK_TIME },
    { .name = "奥特之王", .perk_text = "暴击区 +30%", .sprite_prefix = "gold",
      .accent_color = 0xFFC53D, .perk = HERO_PERK_CRIT_ZONE },
    { .name = "迪迦奥特曼", .perk_text = "每关首次受伤免疫", .sprite_prefix = "violet",
      .accent_color = 0xB07CFF, .perk = HERO_PERK_SHIELD },
};

static const stage_def_t STAGES[CAMPAIGN_STAGE_COUNT] = {
    { .name = "街区警报", .monster_name = "巴尔坦星人", .domain = "20以内加减 · 2~5乘法",
      .sprite_name = "spiker", .math_stage = 1, .question_ms = 8000, .monster_hp = 4,
      .rules = STAGE_RULE_HINT_FREE, .boss = false },
    { .name = "重装对决", .monster_name = "雷德王", .domain = "20以内加减 · 2~5乘法",
      .sprite_name = "brute", .math_stage = 1, .question_ms = 7800, .monster_hp = 4,
      .rules = 0, .boss = false },
    { .name = "深海来袭", .monster_name = "雷伊洛斯", .domain = "100以内加减 · 6~9乘法",
      .sprite_name = "abyss", .math_stage = 2, .question_ms = 7000, .monster_hp = 5,
      .rules = STAGE_RULE_HALF_NONCRIT, .boss = false },
    { .name = "夜翼危机", .monster_name = "多拉考", .domain = "100以内加减 · 6~9乘法",
      .sprite_name = "moth", .math_stage = 2, .question_ms = 6800, .monster_hp = 5,
      .rules = STAGE_RULE_FAST_GAUGE, .boss = false },
    { .name = "森林巨兽", .monster_name = "哥莫拉", .domain = "退位减 · 乘除 · 填空",
      .sprite_name = "ape", .math_stage = 3, .question_ms = 6200, .monster_hp = 6,
      .rules = 0, .boss = false },
    { .name = "钢刃追猎", .monster_name = "加拉特隆", .domain = "退位减 · 乘除 · 填空",
      .sprite_name = "bladehead", .math_stage = 3, .question_ms = 6000, .monster_hp = 6,
      .rules = STAGE_RULE_GUARD_QTE, .boss = false },
    { .name = "赤甲虫王", .monster_name = "杰顿", .domain = "混合两步 · 100以内加减",
      .sprite_name = "beetle", .math_stage = 4, .question_ms = 5600, .monster_hp = 7,
      .rules = STAGE_RULE_ARMOR_BREAK, .boss = false },
    { .name = "终焉飞龙", .monster_name = "魔格大蛇", .domain = "混合两步 · 乘除 · 填空",
      .sprite_name = "wyvern", .math_stage = 4, .question_ms = 5200, .monster_hp = 8,
      .rules = STAGE_RULE_ENRAGE, .boss = true },
};

static const difficulty_def_t DIFFICULTIES[CAMPAIGN_DIFFICULTY_COUNT] = {
    { .name = "星光", .detail = "时间 +1.5秒 · 5颗心", .time_bonus_ms = 1500,
      .hero_hp = 5, .monster_hp_delta = -1, .crit_window = 50 },
    { .name = "烈焰", .detail = "标准节奏 · 4颗心", .time_bonus_ms = 0,
      .hero_hp = 4, .monster_hp_delta = 0, .crit_window = 40 },
    { .name = "究极", .detail = "时间 -1秒 · 3颗心", .time_bonus_ms = -1000,
      .hero_hp = 3, .monster_hp_delta = 1, .crit_window = 32 },
};

const hero_def_t *campaign_hero(uint8_t index)
{
    if (index >= CAMPAIGN_HERO_COUNT) {
        index = 0;
    }
    return &HEROES[index];
}

const stage_def_t *campaign_stage(uint8_t index)
{
    if (index >= CAMPAIGN_STAGE_COUNT) {
        index = 0;
    }
    return &STAGES[index];
}

const difficulty_def_t *campaign_difficulty(uint8_t index)
{
    if (index >= CAMPAIGN_DIFFICULTY_COUNT) {
        index = 0;
    }
    return &DIFFICULTIES[index];
}

uint16_t campaign_question_ms(uint8_t stage, uint8_t difficulty, uint8_t hero)
{
    const stage_def_t *stage_def = campaign_stage(stage);
    const difficulty_def_t *diff = campaign_difficulty(difficulty);
    int32_t ms = (int32_t)stage_def->question_ms + diff->time_bonus_ms;
    if ((stage_def->rules & STAGE_RULE_FAST_GAUGE) != 0) {
        ms = ms * 9 / 10;
    }
    if (campaign_hero(hero)->perk == HERO_PERK_TIME) {
        ms += 800;
    }
    if (ms < 2500) {
        ms = 2500;
    }
    if (ms > 12000) {
        ms = 12000;
    }
    return (uint16_t)ms;
}

uint8_t campaign_hero_hp(uint8_t difficulty)
{
    return campaign_difficulty(difficulty)->hero_hp;
}

uint8_t campaign_monster_hp(uint8_t stage, uint8_t difficulty)
{
    const stage_def_t *stage_def = campaign_stage(stage);
    int hp = (int)stage_def->monster_hp +
             (int)campaign_difficulty(difficulty)->monster_hp_delta;
    if (hp < 2) {
        hp = 2;
    }
    return (uint8_t)hp;
}

uint8_t campaign_crit_window(uint8_t difficulty, uint8_t hero)
{
    int window = campaign_difficulty(difficulty)->crit_window;
    if (campaign_hero(hero)->perk == HERO_PERK_CRIT_ZONE) {
        window = window * 13 / 10;
    }
    if (window > 70) {
        window = 70;
    }
    return (uint8_t)window;
}

bool campaign_stage_has_rule(uint8_t stage, uint8_t rule)
{
    return (campaign_stage(stage)->rules & rule) != 0;
}

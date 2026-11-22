#include "cycle.h"

#include <stddef.h>
#include <string.h>

#include "math_question.h"

#define SECRET_STAGE_CYCLE 2
#define SECRET_BOSS_CYCLE 5
#define RARE_POOL_CYCLE 10

static uint32_t cycle_rng_next(math_rng_t *rng)
{
    return math_rng_next(rng);
}

static uint32_t cycle_rng_below(math_rng_t *rng, uint32_t bound)
{
    return bound == 0 ? 0 : cycle_rng_next(rng) % bound;
}

static void shuffle_u8(uint8_t *values, uint8_t count, math_rng_t *rng)
{
    for (uint8_t i = count; i > 1; i--) {
        const uint8_t j =
            (uint8_t)cycle_rng_below(rng, (uint32_t)i);
        const uint8_t tmp = values[i - 1];
        values[i - 1] = values[j];
        values[j] = tmp;
    }
}

void cycle_plan_build(cycle_plan_t *plan, uint16_t cycle, uint32_t seed,
                      bool variation)
{
    if (plan == NULL) {
        return;
    }
    memset(plan, 0, sizeof(*plan));
    if (cycle == 0) {
        cycle = 1;
    }
    plan->cycle = cycle;
    plan->seed = seed;
    for (uint8_t i = 0; i < CYCLE_STAGE_COUNT; i++) {
        plan->monster[i] = i;
        plan->style[i] = CYCLE_STYLE_NORMAL;
    }

    if (!variation || cycle <= 1) {
        return;
    }

    math_rng_t rng;
    math_rng_seed(&rng, seed ^ (0xC1C1u + (uint32_t)cycle * 7919u));

    uint8_t pool[CYCLE_MONSTER_COUNT];
    uint8_t pool_count = 8;
    for (uint8_t i = 0; i < 8; i++) {
        pool[i] = i;
    }
    if (cycle >= 3) {
        // 第三周目起开放全部 11 只怪兽, 普通关也可能遇到新面孔。
        for (uint8_t i = 8; i < CYCLE_MONSTER_COUNT; i++) {
            pool[i] = i;
        }
        pool_count = CYCLE_MONSTER_COUNT;
    }
    shuffle_u8(pool, pool_count, &rng);
    if (cycle >= RARE_POOL_CYCLE) {
        // 稀有池: 保证本轮普通关至少出现一只第十周目专属怪兽。
        bool has_rare = false;
        for (uint8_t i = 0; i < 8; i++) {
            if (pool[i] == 9 || pool[i] == 10) {
                has_rare = true;
            }
        }
        if (!has_rare) {
            pool[7] = (cycle & 1u) != 0 ? 9 : 10;
        }
    }
    for (uint8_t stage = 0; stage < 8; stage++) {
        plan->monster[stage] = pool[stage];
    }
    // 第 9 关永远是最终 BOSS, 保证周目仍然有收尾。
    plan->monster[8] = 8;

    // 专项关卡只替换普通关, BOSS 关保持完整对拼节奏。
    // 第 2 周目整轮进入混合题型, 第 3 周目起改为随机专项关。
    if (cycle == 2) {
        for (uint8_t stage = 0; stage < 8; stage++) {
            plan->style[stage] = CYCLE_STYLE_MIXED;
        }
    } else {
        uint8_t slots[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        shuffle_u8(slots, 8, &rng);
        uint8_t special = 2;
        if (cycle >= 6) {
            special++;
        }
        if (cycle >= 10) {
            special++;
        }
        for (uint8_t i = 0; i < special; i++) {
            const uint8_t stage = slots[i];
            plan->style[stage] =
                (i & 1u) != 0 ? CYCLE_STYLE_MIXED : CYCLE_STYLE_HUNDRED;
        }
    }

    // 每周目随机 1~2 条轻量挑战, 不叠加互斥规则。
    uint8_t mods[4] = {CYCLE_MOD_SPEED, CYCLE_MOD_GUARD, CYCLE_MOD_MIXED,
                       CYCLE_MOD_BOSS};
    shuffle_u8(mods, 4, &rng);
    const uint8_t mod_count =
        cycle <= 2 ? 1u : (uint8_t)(1u + cycle_rng_below(&rng, 2));
    for (uint8_t i = 0; i < mod_count; i++) {
        plan->mods |= mods[i];
    }
}

bool cycle_plan_valid(const cycle_plan_t *plan)
{
    if (plan == NULL || plan->cycle == 0) {
        return false;
    }
    for (uint8_t i = 0; i < CYCLE_STAGE_COUNT; i++) {
        if (plan->monster[i] >= CYCLE_MONSTER_COUNT ||
            plan->style[i] > CYCLE_STYLE_MIXED) {
            return false;
        }
    }
    return plan->monster[8] == 8 && (plan->mods & ~CYCLE_MOD_ALL) == 0;
}

const char *cycle_style_name(uint8_t style)
{
    switch (style) {
    case CYCLE_STYLE_HUNDRED:
        return "百减专项";
    case CYCLE_STYLE_MIXED:
        return "混合题型";
    default:
        return "基础题";
    }
}

const char *cycle_mod_name(uint8_t bit)
{
    switch (bit) {
    case CYCLE_MOD_SPEED:
        return "极速挑战";
    case CYCLE_MOD_GUARD:
        return "错题守护";
    case CYCLE_MOD_MIXED:
        return "混合题型";
    case CYCLE_MOD_BOSS:
        return "BOSS强化";
    default:
        return "";
    }
}

const char *cycle_mod_detail(uint8_t bit)
{
    switch (bit) {
    case CYCLE_MOD_SPEED:
        return "答题时间 -20% · 硬币 +50%";
    case CYCLE_MOD_GUARD:
        return "累计 2 次错误本关失败";
    case CYCLE_MOD_MIXED:
        return "100减法与常规题随机切换";
    case CYCLE_MOD_BOSS:
        return "最终BOSS多2点血";
    default:
        return "";
    }
}

uint8_t cycle_mod_count(uint8_t mods)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if ((mods & (1u << i)) != 0) {
            count++;
        }
    }
    return count;
}

bool cycle_has_secret_stage(uint16_t cycle, bool variation)
{
    return variation && cycle >= SECRET_STAGE_CYCLE;
}

bool cycle_has_secret_boss(uint16_t cycle, bool variation)
{
    return variation && cycle >= SECRET_BOSS_CYCLE;
}

bool cycle_has_rare_pool(uint16_t cycle, bool variation)
{
    return variation && cycle >= RARE_POOL_CYCLE;
}

uint8_t cycle_style_for_stage(const cycle_plan_t *plan, uint8_t stage)
{
    if (plan == NULL || stage >= CYCLE_STAGE_COUNT) {
        return CYCLE_STYLE_NORMAL;
    }
    if ((plan->mods & CYCLE_MOD_MIXED) != 0) {
        return CYCLE_STYLE_MIXED;
    }
    return plan->style[stage];
}

uint16_t cycle_question_time_pct(uint8_t mods)
{
    return (mods & CYCLE_MOD_SPEED) != 0 ? 80 : 100;
}

uint8_t cycle_max_wrong(uint8_t mods)
{
    return (mods & CYCLE_MOD_GUARD) != 0 ? 2 : 0;
}

uint8_t cycle_boss_bonus_hp(uint8_t mods)
{
    return (mods & CYCLE_MOD_BOSS) != 0 ? 2 : 0;
}

bool cycle_force_mixed(uint8_t mods)
{
    return (mods & CYCLE_MOD_MIXED) != 0;
}

uint8_t cycle_stage_coin_reward(uint8_t stars, bool boss, bool secret,
                                uint8_t mods)
{
    int coins = 10 + (int)stars * 8;
    if (secret) {
        coins += 15;
    }
    if (boss) {
        coins *= 2;
    }
    if ((mods & CYCLE_MOD_SPEED) != 0) {
        coins = coins * 3 / 2;
    }
    if ((mods & CYCLE_MOD_GUARD) != 0) {
        coins += 5;
    }
    if (coins > 250) {
        coins = 250;
    }
    return (uint8_t)coins;
}

cycle_grade_t cycle_grade(uint32_t correct, uint32_t wrong, uint16_t fastest_ms)
{
    const uint32_t total = correct + wrong;
    if (total == 0) {
        return CYCLE_GRADE_C;
    }
    const uint32_t accuracy = correct * 100u / total;
    if (accuracy >= 95u && fastest_ms > 0 && fastest_ms <= 3000u) {
        return CYCLE_GRADE_SS;
    }
    if (accuracy >= 90u) {
        return CYCLE_GRADE_S;
    }
    if (accuracy >= 78u) {
        return CYCLE_GRADE_A;
    }
    if (accuracy >= 62u) {
        return CYCLE_GRADE_B;
    }
    return CYCLE_GRADE_C;
}

const char *cycle_grade_name(uint8_t grade)
{
    switch (grade) {
    case CYCLE_GRADE_SS:
        return "SS";
    case CYCLE_GRADE_S:
        return "S";
    case CYCLE_GRADE_A:
        return "A";
    case CYCLE_GRADE_B:
        return "B";
    default:
        return "C";
    }
}
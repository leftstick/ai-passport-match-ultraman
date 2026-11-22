// Host tests for the roguelite cycle planner and reward rules.
#include "cycle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_fixed_plan(void)
{
    cycle_plan_t plan;
    cycle_plan_build(&plan, 1, 0x1234u, true);
    assert(plan.cycle == 1);
    assert(plan.mods == 0);
    for (uint8_t i = 0; i < CYCLE_STAGE_COUNT; i++) {
        assert(plan.monster[i] == i);
        assert(plan.style[i] == CYCLE_STYLE_NORMAL);
    }
    assert(cycle_plan_valid(&plan));

    // 家长关闭周目变化后, 即使周目数很高也回到固定内容。
    cycle_plan_build(&plan, 8, 0x9999u, false);
    assert(plan.cycle == 8);
    assert(plan.mods == 0);
    for (uint8_t i = 0; i < CYCLE_STAGE_COUNT; i++) {
        assert(plan.monster[i] == i);
        assert(plan.style[i] == CYCLE_STYLE_NORMAL);
    }
}

static void test_deterministic_rotation(void)
{
    cycle_plan_t a;
    cycle_plan_t b;
    cycle_plan_build(&a, 3, 0xABCD1234u, true);
    cycle_plan_build(&b, 3, 0xABCD1234u, true);
    assert(memcmp(&a, &b, sizeof(a)) == 0);

    cycle_plan_build(&b, 4, 0xABCD1234u, true);
    assert(memcmp(&a, &b, sizeof(a)) != 0);
}

static void test_cycle_two_specials(void)
{
    cycle_plan_t plan;
    cycle_plan_build(&plan, 2, 0x55AAu, true);
    assert(plan.monster[8] == 8);
    assert(cycle_mod_count(plan.mods) == 1);
    // 第 2 周目整轮采用混合题型, BOSS 关保持原规则。
    for (uint8_t i = 0; i < 8; i++) {
        assert(plan.style[i] == CYCLE_STYLE_MIXED);
    }
    assert(plan.style[8] == CYCLE_STYLE_NORMAL);

    cycle_plan_build(&plan, 3, 0x55AAu, true);
    uint8_t hundred = 0;
    uint8_t mixed = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (plan.style[i] == CYCLE_STYLE_HUNDRED) hundred++;
        if (plan.style[i] == CYCLE_STYLE_MIXED) mixed++;
    }
    assert(hundred == 1);
    assert(mixed == 1);
}

static void test_high_cycle_plan(void)
{
    for (uint16_t cycle = 3; cycle <= 60; cycle++) {
        cycle_plan_t plan;
        cycle_plan_build(&plan, cycle, 0x1000u + cycle, true);
        assert(cycle_plan_valid(&plan));
        assert(plan.monster[8] == 8);
        const uint8_t mods = cycle_mod_count(plan.mods);
        assert(mods >= 1 && mods <= 2);

        // 普通关不会出现重复怪兽; 最终 BOSS 固定为第 9 只。
        bool seen[CYCLE_MONSTER_COUNT] = {false};
        for (uint8_t i = 0; i < 8; i++) {
            assert(plan.monster[i] < CYCLE_MONSTER_COUNT);
            assert(!seen[plan.monster[i]]);
            seen[plan.monster[i]] = true;
        }
        if (cycle >= 10) {
            // 稀有池: 普通关保证至少出现一只第十周目专属怪兽 (9 或 10)。
            assert(seen[9] || seen[10]);
        }
    }
}

static void test_secret_thresholds(void)
{
    assert(!cycle_has_secret_stage(1, true));
    assert(cycle_has_secret_stage(2, true));
    assert(!cycle_has_secret_stage(9, false));

    assert(!cycle_has_secret_boss(4, true));
    assert(cycle_has_secret_boss(5, true));
    assert(!cycle_has_secret_boss(9, false));

    assert(!cycle_has_rare_pool(9, true));
    assert(cycle_has_rare_pool(10, true));
}

static void test_modifiers(void)
{
    assert(cycle_question_time_pct(CYCLE_MOD_SPEED) == 80);
    assert(cycle_question_time_pct(0) == 100);
    assert(cycle_max_wrong(CYCLE_MOD_GUARD) == 2);
    assert(cycle_max_wrong(0) == 0);
    assert(cycle_boss_bonus_hp(CYCLE_MOD_BOSS) == 2);
    assert(cycle_force_mixed(CYCLE_MOD_MIXED));

    cycle_plan_t plan;
    cycle_plan_build(&plan, 3, 7u, true);
    plan.mods = CYCLE_MOD_MIXED;
    for (uint8_t i = 0; i < CYCLE_STAGE_COUNT; i++) {
        assert(cycle_style_for_stage(&plan, i) == CYCLE_STYLE_MIXED);
    }
}

static void test_coin_reward(void)
{
    assert(cycle_stage_coin_reward(3, false, false, 0) == 34);
    assert(cycle_stage_coin_reward(1, false, false, 0) == 18);
    assert(cycle_stage_coin_reward(3, true, false, 0) == 68);
    assert(cycle_stage_coin_reward(3, false, false, CYCLE_MOD_SPEED) == 51);
    assert(cycle_stage_coin_reward(3, false, false, CYCLE_MOD_GUARD) == 39);
    assert(cycle_stage_coin_reward(3, false, true, 0) == 49);
}

static void test_grade(void)
{
    assert(cycle_grade(20, 0, 2500) == CYCLE_GRADE_SS);
    assert(cycle_grade(20, 0, 4000) == CYCLE_GRADE_S);
    assert(cycle_grade(18, 2, 2000) == CYCLE_GRADE_S);
    assert(cycle_grade(16, 4, 5000) == CYCLE_GRADE_A);
    assert(cycle_grade(13, 7, 5000) == CYCLE_GRADE_B);
    assert(cycle_grade(10, 10, 5000) == CYCLE_GRADE_C);
    assert(cycle_grade(0, 0, 0) == CYCLE_GRADE_C);
    assert(strcmp(cycle_grade_name(CYCLE_GRADE_SS), "SS") == 0);
    assert(strcmp(cycle_grade_name(CYCLE_GRADE_C), "C") == 0);
}

static void test_names(void)
{
    assert(strcmp(cycle_mod_name(CYCLE_MOD_SPEED), "极速挑战") == 0);
    assert(strcmp(cycle_mod_name(CYCLE_MOD_GUARD), "错题守护") == 0);
    assert(strcmp(cycle_mod_name(CYCLE_MOD_MIXED), "混合题型") == 0);
    assert(strcmp(cycle_mod_name(CYCLE_MOD_BOSS), "BOSS强化") == 0);
    assert(cycle_mod_detail(CYCLE_MOD_SPEED)[0] != '\0');
    assert(cycle_mod_count(CYCLE_MOD_SPEED | CYCLE_MOD_GUARD) == 2);
}

int main(void)
{
    test_fixed_plan();
    test_deterministic_rotation();
    test_cycle_two_specials();
    test_high_cycle_plan();
    test_secret_thresholds();
    test_modifiers();
    test_coin_reward();
    test_grade();
    test_names();
    puts("test_cycle: PASS");
    return 0;
}
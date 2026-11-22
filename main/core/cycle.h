// 周目动态变化: 怪兽轮换、题型轮换、挑战 buff、奖励与评级。
// 纯 C, 不依赖 LVGL/ESP-IDF, 可用主机测试覆盖。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CYCLE_STAGE_COUNT 9
#define CYCLE_MONSTER_COUNT 11

typedef enum {
    CYCLE_STYLE_NORMAL = 0,
    CYCLE_STYLE_HUNDRED,
    CYCLE_STYLE_MIXED,
} cycle_style_t;

typedef enum {
    CYCLE_GRADE_C = 0,
    CYCLE_GRADE_B,
    CYCLE_GRADE_A,
    CYCLE_GRADE_S,
    CYCLE_GRADE_SS,
} cycle_grade_t;

#define CYCLE_MOD_SPEED 0x01u  // 极速挑战: 答题时间缩短, 硬币加成
#define CYCLE_MOD_GUARD 0x02u  // 错题守护: 累计 2 次错误本关失败
#define CYCLE_MOD_MIXED 0x04u  // 混合题型: 100 减法与常规题随机切换
#define CYCLE_MOD_BOSS  0x08u  // BOSS 强化: 最终 BOSS 多 2 点血
#define CYCLE_MOD_ALL   0x0Fu

typedef struct {
    uint16_t cycle;
    uint32_t seed;
    uint8_t monster[CYCLE_STAGE_COUNT];
    uint8_t style[CYCLE_STAGE_COUNT];
    uint8_t mods;
} cycle_plan_t;

// cycle 从 1 开始。variation=false 时固定为第 1 周目内容, 供家长关闭动态变化。
void cycle_plan_build(cycle_plan_t *plan, uint16_t cycle, uint32_t seed,
                      bool variation);
bool cycle_plan_valid(const cycle_plan_t *plan);

const char *cycle_style_name(uint8_t style);
const char *cycle_mod_name(uint8_t bit);
const char *cycle_mod_detail(uint8_t bit);
uint8_t cycle_mod_count(uint8_t mods);

bool cycle_has_secret_stage(uint16_t cycle, bool variation);
bool cycle_has_secret_boss(uint16_t cycle, bool variation);
bool cycle_has_rare_pool(uint16_t cycle, bool variation);

// 实际生效的关卡题型: 混合 buff 会把普通关卡也变成混合题型。
uint8_t cycle_style_for_stage(const cycle_plan_t *plan, uint8_t stage);

uint16_t cycle_question_time_pct(uint8_t mods);
uint8_t cycle_max_wrong(uint8_t mods);
uint8_t cycle_boss_bonus_hp(uint8_t mods);
bool cycle_force_mixed(uint8_t mods);

uint8_t cycle_stage_coin_reward(uint8_t stars, bool boss, bool secret,
                                uint8_t mods);
cycle_grade_t cycle_grade(uint32_t correct, uint32_t wrong, uint16_t fastest_ms);
const char *cycle_grade_name(uint8_t grade);
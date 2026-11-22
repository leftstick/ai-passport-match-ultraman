// Pure math-question generation for the battle game.
// No LVGL/ESP-IDF dependencies so it can be covered by host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MATH_QUESTION_OPTION_COUNT 3
#define MATH_QUESTION_TEXT_SIZE 24
#define MATH_QUESTION_HINT_SIZE 48
#define MATH_QUESTION_HISTORY 4

typedef struct {
    uint32_t state;
} math_rng_t;

typedef enum {
    QUESTION_ADD = 0,
    QUESTION_SUB,
    QUESTION_MUL,
    QUESTION_DIV,
    QUESTION_FILL,
    QUESTION_MIX,
    // 百减模式两种题型: typeA 求结果, typeB 求被藏起来的减数.
    QUESTION_HUNDRED_A,
    QUESTION_HUNDRED_B,
} question_kind_t;

typedef enum {
    HUNDRED_SUB_TYPE_A = 0,  // 100 - n = ?
    HUNDRED_SUB_TYPE_B,      // 100 - ? = n
} hundred_sub_type_t;

typedef struct {
    char text[MATH_QUESTION_TEXT_SIZE];
    char hint[MATH_QUESTION_HINT_SIZE];
    int16_t options[MATH_QUESTION_OPTION_COUNT];
    int16_t answer;
    uint8_t correct_index;
    uint8_t kind;
    uint8_t math_stage;
} math_question_t;

void math_rng_seed(math_rng_t *rng, uint32_t seed);
uint32_t math_rng_next(math_rng_t *rng);
uint32_t math_rng_below(math_rng_t *rng, uint32_t bound);

// stage: 1..4 curriculum tier. assist: generate one tier easier (min tier 1).
// history/history_len: recent questions used to avoid immediate repeats.
void math_question_generate(math_question_t *question, uint8_t stage, bool assist,
                            math_rng_t *rng, const math_question_t *history,
                            uint8_t history_len);

// 百减模式: 被减数固定 100, 减数 n 取 1..99, 结果 100-n 也落在 1..99。
// questionType 记录在 question->kind: QUESTION_HUNDRED_A / QUESTION_HUNDRED_B。
// 两种题型各 50% 概率, 保证不会出现 100-0 或 100-100。
void generateHundredSubQuestion(math_question_t *question, math_rng_t *rng);

// 指定减数和题型的确定性构造, 供测试与回归样例使用。
// typeA: "100 - n = ?" (answer 100-n)
// typeB: "100 - ? = n" (answer 100-n)
void buildHundredSubQuestion(math_question_t *question, int minus,
                             hundred_sub_type_t type, math_rng_t *rng);

// Exposed for tests and for UI hints that need the multiplication table phrase.
void math_chinese_number(int value, char *out, size_t size);
void math_multiplication_phrase(int a, int b, char *out, size_t size);

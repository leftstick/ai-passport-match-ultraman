// Host tests for the 100-subtraction ("百减") question generator.
#include "math_question.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_options(const math_question_t *q)
{
    assert(q->correct_index < MATH_QUESTION_OPTION_COUNT);
    assert(q->options[q->correct_index] == q->answer);
    for (int i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        if (i != q->correct_index) {
            assert(q->options[i] != q->answer);
        }
    }
}

static void test_type_a_samples(void)
{
    math_rng_t rng;
    math_rng_seed(&rng, 0x1234u);

    math_question_t q;
    buildHundredSubQuestion(&q, 29, HUNDRED_SUB_TYPE_A, &rng);
    assert(strcmp(q.text, "100 - 29 = ?") == 0);
    assert(q.answer == 71);
    assert(q.kind == QUESTION_HUNDRED_A);
    check_options(&q);

    buildHundredSubQuestion(&q, 8, HUNDRED_SUB_TYPE_A, &rng);
    assert(strcmp(q.text, "100 - 8 = ?") == 0);
    assert(q.answer == 92);
    assert(q.kind == QUESTION_HUNDRED_A);
    check_options(&q);
}

static void test_type_b_samples(void)
{
    math_rng_t rng;
    math_rng_seed(&rng, 0x5678u);

    math_question_t q;
    buildHundredSubQuestion(&q, 29, HUNDRED_SUB_TYPE_B, &rng);
    assert(strcmp(q.text, "100 - ? = 29") == 0);
    assert(q.answer == 71);
    assert(q.kind == QUESTION_HUNDRED_B);
    check_options(&q);

    buildHundredSubQuestion(&q, 8, HUNDRED_SUB_TYPE_B, &rng);
    assert(strcmp(q.text, "100 - ? = 8") == 0);
    assert(q.answer == 92);
    assert(q.kind == QUESTION_HUNDRED_B);
    check_options(&q);
}

static void test_generator_range_and_mix(void)
{
    math_rng_t rng;
    math_rng_seed(&rng, 0xBEEFu);
    unsigned type_a = 0;
    unsigned type_b = 0;

    for (int i = 0; i < 4000; i++) {
        math_question_t q;
        generateHundredSubQuestion(&q, &rng);

        int n = 0;
        if (q.kind == QUESTION_HUNDRED_A) {
            assert(sscanf(q.text, "100 - %d = ?", &n) == 1);
            type_a++;
        } else {
            assert(q.kind == QUESTION_HUNDRED_B);
            assert(sscanf(q.text, "100 - ? = %d", &n) == 1);
            type_b++;
        }
        // 减数与结果都必须在 1..99, 不允许 100-0 / 100-100。
        assert(n >= 1 && n <= 99);
        assert(q.answer >= 1 && q.answer <= 99);
        assert(q.answer == 100 - n);
        check_options(&q);
    }

    // 两种题型都应出现, 且大致均衡。
    assert(type_a > 1600 && type_b > 1600);
    assert(type_a + type_b == 4000);
}

static void test_clamping(void)
{
    math_rng_t rng;
    math_rng_seed(&rng, 7u);

    math_question_t q;
    buildHundredSubQuestion(&q, 0, HUNDRED_SUB_TYPE_A, &rng);
    assert(strcmp(q.text, "100 - 1 = ?") == 0);
    assert(q.answer == 99);

    buildHundredSubQuestion(&q, 100, HUNDRED_SUB_TYPE_B, &rng);
    assert(strcmp(q.text, "100 - ? = 99") == 0);
    assert(q.answer == 1);
}

int main(void)
{
    test_type_a_samples();
    test_type_b_samples();
    test_generator_range_and_mix();
    test_clamping();
    puts("test_hundred_sub: PASS");
    return 0;
}
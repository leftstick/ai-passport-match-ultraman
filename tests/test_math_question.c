// Host tests for the pure question generator.
#include "math_question.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_options(const math_question_t *q)
{
    assert(q->text[0] != '\0');
    assert(q->hint[0] != '\0');
    assert(q->correct_index < MATH_QUESTION_OPTION_COUNT);
    assert(q->options[q->correct_index] == q->answer);
    assert(q->options[0] != q->options[1]);
    assert(q->options[0] != q->options[2]);
    assert(q->options[1] != q->options[2]);
    assert(q->answer >= 0);
}

// sscanf format strings cannot reliably match multi-byte operators on every
// host libc, so normalise the two UTF-8 operators to ASCII first.
static void normalize(const char *src, char *dst, size_t size)
{
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < size; i++) {
        const unsigned char ch = (unsigned char)src[i];
        const unsigned char next = (unsigned char)src[i + 1];
        if (ch == 0xC3 && next == 0x97) {  // ×
            dst[out++] = 'x';
            i++;
        } else if (ch == 0xC3 && next == 0xB7) {  // ÷
            dst[out++] = '/';
            i++;
        } else {
            dst[out++] = (char)ch;
        }
    }
    dst[out] = '\0';
}

static void verify_text(const math_question_t *q)
{
    int a = 0;
    int b = 0;
    int c = 0;
    char text[64];
    normalize(q->text, text, sizeof(text));

    if (sscanf(text, "%d + %d = ?", &a, &b) == 2) {
        assert(q->answer == a + b);
        return;
    }
    if (sscanf(text, "%d - %d = ?", &a, &b) == 2) {
        assert(q->answer == a - b);
        return;
    }
    // Three-operand patterns must be tried first: sscanf returns the number of
    // converted items even when a later literal fails to match.
    if (sscanf(text, "%d x %d + %d = ?", &a, &b, &c) == 3) {
        assert(q->answer == a * b + c);
        return;
    }
    if (sscanf(text, "%d x %d - %d = ?", &a, &b, &c) == 3) {
        assert(q->answer == a * b - c);
        return;
    }
    if (sscanf(text, "%d x %d = ?", &a, &b) == 2) {
        assert(q->answer == a * b);
        return;
    }
    if (sscanf(text, "%d / %d = ?", &a, &b) == 2) {
        assert(b != 0);
        assert(q->answer == a / b);
        return;
    }
    if (sscanf(text, "%d + ? = %d", &a, &b) == 2) {
        assert(q->answer == b - a);
        return;
    }
    if (sscanf(text, "? + %d = %d", &a, &b) == 2) {
        assert(q->answer == b - a);
        return;
    }
    if (sscanf(text, "%d - ? = %d", &a, &b) == 2) {
        assert(q->answer == a - b);
        return;
    }
    if (sscanf(text, "? - %d = %d", &a, &b) == 2) {
        assert(q->answer == a + b);
        return;
    }
    if (sscanf(text, "%d x ? = %d", &a, &b) == 2) {
        assert(a != 0);
        assert(q->answer == b / a);
        return;
    }
    if (sscanf(text, "? x %d = %d", &a, &b) == 2) {
        assert(a != 0);
        assert(q->answer == b / a);
        return;
    }
    if (sscanf(text, "%d / ? = %d", &a, &b) == 2) {
        assert(b != 0);
        assert(q->answer == a / b);
        return;
    }
    if (sscanf(text, "? / %d = %d", &a, &b) == 2) {
        assert(q->answer == a * b);
        return;
    }
    fprintf(stderr, "unrecognised question text: %s\n", text);
    assert(0);
}

static void test_generation(void)
{
    for (uint32_t seed = 1; seed <= 25; seed++) {
        for (uint8_t stage = 1; stage <= 4; stage++) {
            math_rng_t rng;
            math_rng_seed(&rng, seed * 7919u + stage);
            math_question_t history[MATH_QUESTION_HISTORY] = { 0 };
            uint8_t history_len = 0;

            for (int n = 0; n < 40; n++) {
                math_question_t q;
                memset(&q, 0, sizeof(q));
                math_question_generate(&q, stage, false, &rng, history,
                                       history_len);
                assert(q.math_stage == stage);
                expect_options(&q);
                verify_text(&q);

                if (history_len < MATH_QUESTION_HISTORY) {
                    history[history_len++] = q;
                } else {
                    for (int i = 1; i < MATH_QUESTION_HISTORY; i++) {
                        history[i - 1] = history[i];
                    }
                    history[MATH_QUESTION_HISTORY - 1] = q;
                }
            }
        }
    }
}

static void test_assist_tier(void)
{
    math_rng_t rng;
    math_rng_seed(&rng, 12345);
    math_question_t q;
    math_question_generate(&q, 4, true, &rng, NULL, 0);
    assert(q.math_stage == 3);

    math_question_generate(&q, 1, true, &rng, NULL, 0);
    assert(q.math_stage == 1);

    math_question_generate(&q, 9, false, &rng, NULL, 0);
    assert(q.math_stage == 4);
}

static void test_chinese_number(void)
{
    char buf[16];
    math_chinese_number(0, buf, sizeof(buf));
    assert(strcmp(buf, "零") == 0);
    math_chinese_number(7, buf, sizeof(buf));
    assert(strcmp(buf, "七") == 0);
    math_chinese_number(10, buf, sizeof(buf));
    assert(strcmp(buf, "十") == 0);
    math_chinese_number(12, buf, sizeof(buf));
    assert(strcmp(buf, "十二") == 0);
    math_chinese_number(20, buf, sizeof(buf));
    assert(strcmp(buf, "二十") == 0);
    math_chinese_number(42, buf, sizeof(buf));
    assert(strcmp(buf, "四十二") == 0);
}

static void test_phrase(void)
{
    char buf[24];
    math_multiplication_phrase(2, 3, buf, sizeof(buf));
    assert(strcmp(buf, "二三得六") == 0);
    math_multiplication_phrase(6, 7, buf, sizeof(buf));
    assert(strcmp(buf, "六七四十二") == 0);
    math_multiplication_phrase(9, 9, buf, sizeof(buf));
    assert(strcmp(buf, "九九八十一") == 0);
}

static void test_rng(void)
{
    math_rng_t rng;
    math_rng_seed(&rng, 0);
    const uint32_t first = math_rng_next(&rng);
    assert(first != 0);
    for (int i = 0; i < 1000; i++) {
        assert(math_rng_below(&rng, 7) < 7);
    }
    assert(math_rng_below(&rng, 0) == 0);
}

int main(void)
{
    test_generation();
    test_assist_tier();
    test_chinese_number();
    test_phrase();
    test_rng();
    puts("test_math_question: PASS");
    return 0;
}

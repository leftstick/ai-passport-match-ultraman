#include "math_question.h"

#include <stdio.h>
#include <string.h>

static const char *const CN_DIGIT[] = {
    "零", "一", "二", "三", "四", "五", "六", "七", "八", "九",
};

static uint32_t rng_range(math_rng_t *rng, uint32_t low, uint32_t high)
{
    if (high <= low) {
        return low;
    }
    return low + math_rng_below(rng, high - low + 1u);
}

void math_rng_seed(math_rng_t *rng, uint32_t seed)
{
    rng->state = seed == 0 ? 0x9E3779B9u : seed;
}

uint32_t math_rng_next(math_rng_t *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0) {
        x = 0x9E3779B9u;
    }
    rng->state = x;
    return x;
}

uint32_t math_rng_below(math_rng_t *rng, uint32_t bound)
{
    return bound == 0 ? 0 : math_rng_next(rng) % bound;
}

void math_chinese_number(int value, char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return;
    }
    if (value < 0) {
        value = 0;
    }
    if (value < 10) {
        snprintf(out, size, "%s", CN_DIGIT[value]);
    } else if (value < 20) {
        if (value == 10) {
            snprintf(out, size, "十");
        } else {
            snprintf(out, size, "十%s", CN_DIGIT[value % 10]);
        }
    } else {
        const int tens = value / 10;
        const int ones = value % 10;
        if (ones == 0) {
            snprintf(out, size, "%s十", CN_DIGIT[tens]);
        } else {
            snprintf(out, size, "%s十%s", CN_DIGIT[tens], CN_DIGIT[ones]);
        }
    }
}

void math_multiplication_phrase(int a, int b, char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return;
    }
    if (a < 1 || a > 9 || b < 1 || b > 9) {
        snprintf(out, size, "乘法口诀");
        return;
    }
    int low = a < b ? a : b;
    int high = a < b ? b : a;
    char product[16];
    math_chinese_number(low * high, product, sizeof(product));
    if (low * high < 10) {
        snprintf(out, size, "%s%s得%s", CN_DIGIT[low], CN_DIGIT[high], product);
    } else {
        snprintf(out, size, "%s%s%s", CN_DIGIT[low], CN_DIGIT[high], product);
    }
}

static void add_candidate(int *list, int *count, int value, int answer)
{
    if (value < 0 || value > 200 || value == answer) {
        return;
    }
    for (int i = 0; i < *count; i++) {
        if (list[i] == value) {
            return;
        }
    }
    if (*count < 16) {
        list[(*count)++] = value;
    }
}

static void shuffle_ints(int *values, int count, math_rng_t *rng)
{
    for (int i = count - 1; i > 0; i--) {
        const int j = (int)math_rng_below(rng, (uint32_t)(i + 1));
        const int tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
}

// Build 3 options with plausible mistakes, then shuffle. Context values are the
// visible operands; c is unused except for mixed two-step questions.
static void finish_question(math_question_t *q, int answer, uint8_t base_kind,
                            int a, int b, int c, math_rng_t *rng)
{
    q->answer = (int16_t)answer;
    q->kind = base_kind;

    int candidates[16];
    int count = 0;
    add_candidate(candidates, &count, answer - 1, answer);
    add_candidate(candidates, &count, answer + 1, answer);
    add_candidate(candidates, &count, answer - 10, answer);
    add_candidate(candidates, &count, answer + 10, answer);

    if (base_kind == QUESTION_ADD) {
        const int no_carry = (a % 10 + b % 10) % 10 + (a / 10 + b / 10) * 10;
        add_candidate(candidates, &count, no_carry, answer);
        add_candidate(candidates, &count, no_carry + 10, answer);
    } else if (base_kind == QUESTION_SUB) {
        const int forgot_borrow = ((a / 10) - (b / 10)) * 10 +
                                  ((a % 10 - b % 10 + 10) % 10);
        add_candidate(candidates, &count, forgot_borrow, answer);
        add_candidate(candidates, &count, b - a, answer);
    } else if (base_kind == QUESTION_MUL) {
        add_candidate(candidates, &count, a * (b - 1), answer);
        add_candidate(candidates, &count, a * (b + 1), answer);
        add_candidate(candidates, &count, (a - 1) * b, answer);
        add_candidate(candidates, &count, (a + 1) * b, answer);
    } else if (base_kind == QUESTION_DIV) {
        add_candidate(candidates, &count, answer - 2, answer);
        add_candidate(candidates, &count, answer + 2, answer);
    } else if (base_kind == QUESTION_MIX) {
        const int product = a * b;
        add_candidate(candidates, &count, product, answer);
        add_candidate(candidates, &count, answer + (c < 0 ? -c : c), answer);
        add_candidate(candidates, &count, answer - (c < 0 ? -c : c), answer);
    }

    // Fallback so exactly three options always exist.
    for (int step = 2; count < 6; step++) {
        add_candidate(candidates, &count, answer + step, answer);
        add_candidate(candidates, &count, answer - step, answer);
    }

    shuffle_ints(candidates, count, rng);

    int options[3];
    options[0] = answer;
    for (int i = 1; i < 3; i++) {
        options[i] = candidates[i - 1];
    }
    shuffle_ints(options, 3, rng);

    for (int i = 0; i < 3; i++) {
        q->options[i] = (int16_t)options[i];
        if (options[i] == answer) {
            q->correct_index = (uint8_t)i;
        }
    }
}

static void hint_for(math_question_t *q, uint8_t base_kind, int a, int b)
{
    switch (base_kind) {
    case QUESTION_ADD:
        if ((a % 10 + b % 10) >= 10) {
            snprintf(q->hint, sizeof(q->hint), "个位相加满十，要进位");
        } else {
            snprintf(q->hint, sizeof(q->hint), "先算个位，再算十位");
        }
        break;
    case QUESTION_SUB:
        if ((a % 10) < (b % 10)) {
            snprintf(q->hint, sizeof(q->hint), "个位不够减，向十位借一");
        } else {
            snprintf(q->hint, sizeof(q->hint), "先算个位，再算十位");
        }
        break;
    case QUESTION_MUL: {
        char phrase[24];
        math_multiplication_phrase(a, b, phrase, sizeof(phrase));
        snprintf(q->hint, sizeof(q->hint), "口诀：%s", phrase);
        break;
    }
    case QUESTION_DIV: {
        char phrase[24];
        math_multiplication_phrase(b, a / b, phrase, sizeof(phrase));
        snprintf(q->hint, sizeof(q->hint), "想：%s", phrase);
        break;
    }
    case QUESTION_MIX:
        snprintf(q->hint, sizeof(q->hint), "先算乘法，再算加减");
        break;
    default:
        snprintf(q->hint, sizeof(q->hint), "用逆运算想一想");
        break;
    }
}

static void make_add(math_question_t *q, uint8_t stage, math_rng_t *rng)
{
    int a = 0;
    int b = 0;
    if (stage <= 1) {
        for (int attempt = 0; attempt < 32; attempt++) {
            a = (int)rng_range(rng, 1, 10);
            b = (int)rng_range(rng, 1, 10);
            if (a + b <= 20 && (attempt > 8 || a % 10 + b % 10 >= 10)) {
                break;
            }
        }
    } else {
        for (int attempt = 0; attempt < 48; attempt++) {
            a = (int)rng_range(rng, 10, 89);
            b = (int)rng_range(rng, 10, (uint32_t)(100 - a));
            if (a + b <= 100 && (attempt > 8 || a % 10 + b % 10 >= 10)) {
                break;
            }
        }
    }
    snprintf(q->text, sizeof(q->text), "%d + %d = ?", a, b);
    finish_question(q, a + b, QUESTION_ADD, a, b, 0, rng);
    hint_for(q, QUESTION_ADD, a, b);
}

static void make_sub(math_question_t *q, uint8_t stage, math_rng_t *rng)
{
    int a = 0;
    int b = 0;
    if (stage <= 1) {
        for (int attempt = 0; attempt < 32; attempt++) {
            a = (int)rng_range(rng, 2, 20);
            b = (int)rng_range(rng, 1, (uint32_t)(a - 1));
            if (attempt > 8 || (a > 10 && a % 10 < b % 10)) {
                break;
            }
        }
    } else {
        for (int attempt = 0; attempt < 48; attempt++) {
            a = (int)rng_range(rng, 21, 99);
            b = (int)rng_range(rng, 10, (uint32_t)(a - 1));
            if (attempt > 8 || a % 10 < b % 10) {
                break;
            }
        }
    }
    snprintf(q->text, sizeof(q->text), "%d - %d = ?", a, b);
    finish_question(q, a - b, QUESTION_SUB, a, b, 0, rng);
    hint_for(q, QUESTION_SUB, a, b);
}

static void make_mul(math_question_t *q, uint8_t stage, math_rng_t *rng)
{
    int a = 0;
    int b = 0;
    if (stage <= 1) {
        a = (int)rng_range(rng, 2, 5);
        b = (int)rng_range(rng, 1, 9);
    } else if (stage == 2) {
        a = (int)rng_range(rng, 6, 9);
        b = (int)rng_range(rng, 1, 9);
    } else {
        a = (int)rng_range(rng, 2, 9);
        b = (int)rng_range(rng, 2, 9);
    }
    snprintf(q->text, sizeof(q->text), "%d × %d = ?", a, b);
    finish_question(q, a * b, QUESTION_MUL, a, b, 0, rng);
    hint_for(q, QUESTION_MUL, a, b);
}

static void make_div(math_question_t *q, uint8_t stage, math_rng_t *rng)
{
    (void)stage;
    const int divisor = (int)rng_range(rng, 2, 9);
    const int quotient = (int)rng_range(rng, 2, 9);
    const int dividend = divisor * quotient;
    snprintf(q->text, sizeof(q->text), "%d ÷ %d = ?", dividend, divisor);
    finish_question(q, quotient, QUESTION_DIV, dividend, divisor, 0, rng);
    hint_for(q, QUESTION_DIV, dividend, divisor);
}

static void make_fill(math_question_t *q, math_rng_t *rng)
{
    const int family = (int)rng_range(rng, 0, 3);
    if (family == 0) {
        const int a = (int)rng_range(rng, 1, 80);
        const int b = (int)rng_range(rng, 1, (uint32_t)(100 - a));
        const int sum = a + b;
        if (math_rng_below(rng, 2) == 0) {
            snprintf(q->text, sizeof(q->text), "? + %d = %d", b, sum);
            finish_question(q, a, QUESTION_FILL, a, b, 0, rng);
        } else {
            snprintf(q->text, sizeof(q->text), "%d + ? = %d", a, sum);
            finish_question(q, b, QUESTION_FILL, a, b, 0, rng);
        }
        snprintf(q->hint, sizeof(q->hint), "用减法想一想");
    } else if (family == 1) {
        const int a = (int)rng_range(rng, 21, 99);
        const int b = (int)rng_range(rng, 1, (uint32_t)(a - 1));
        const int diff = a - b;
        if (math_rng_below(rng, 2) == 0) {
            snprintf(q->text, sizeof(q->text), "? - %d = %d", b, diff);
            finish_question(q, a, QUESTION_FILL, a, b, 0, rng);
        } else {
            snprintf(q->text, sizeof(q->text), "%d - ? = %d", a, diff);
            finish_question(q, b, QUESTION_FILL, a, b, 0, rng);
        }
        snprintf(q->hint, sizeof(q->hint), "用加法想一想");
    } else if (family == 2) {
        const int a = (int)rng_range(rng, 2, 9);
        const int b = (int)rng_range(rng, 1, 9);
        const int product = a * b;
        char phrase[24];
        if (math_rng_below(rng, 2) == 0) {
            snprintf(q->text, sizeof(q->text), "? × %d = %d", b, product);
            finish_question(q, a, QUESTION_FILL, a, b, 0, rng);
        } else {
            snprintf(q->text, sizeof(q->text), "%d × ? = %d", a, product);
            finish_question(q, b, QUESTION_FILL, a, b, 0, rng);
        }
        math_multiplication_phrase(a, b, phrase, sizeof(phrase));
        snprintf(q->hint, sizeof(q->hint), "口诀：%s", phrase);
    } else {
        const int divisor = (int)rng_range(rng, 2, 9);
        const int quotient = (int)rng_range(rng, 2, 9);
        const int dividend = divisor * quotient;
        if (math_rng_below(rng, 2) == 0) {
            snprintf(q->text, sizeof(q->text), "? ÷ %d = %d", divisor, quotient);
            finish_question(q, dividend, QUESTION_FILL, dividend, divisor, 0, rng);
        } else {
            snprintf(q->text, sizeof(q->text), "%d ÷ ? = %d", dividend, quotient);
            finish_question(q, divisor, QUESTION_FILL, dividend, divisor, 0, rng);
        }
        snprintf(q->hint, sizeof(q->hint), "用乘法口诀想一想");
    }
}

// 百减模式的两个题型共用同一组数值: 减数 n 与结果 100-n。
// 题型只决定问号放在等号右侧还是减数位置, 答案始终是 100-n。
void buildHundredSubQuestion(math_question_t *question, int minus,
                             hundred_sub_type_t type, math_rng_t *rng)
{
    if (question == NULL || rng == NULL) {
        return;
    }
    if (minus < 1) {
        minus = 1;
    }
    if (minus > 99) {
        minus = 99;
    }
    const int answer = 100 - minus;
    if (type == HUNDRED_SUB_TYPE_B) {
        snprintf(question->text, sizeof(question->text), "100 - ? = %d", minus);
        snprintf(question->hint, sizeof(question->hint), "想：100 - 结果 = 减数");
    } else {
        snprintf(question->text, sizeof(question->text), "100 - %d = ?", minus);
        if (minus % 10 != 0) {
            snprintf(question->hint, sizeof(question->hint), "个位不够减，从100借一");
        } else {
            snprintf(question->hint, sizeof(question->hint), "先算十位，再算个位");
        }
    }
    // base_kind 用 QUESTION_SUB, 让干扰项沿用“忘退位”等既有策略,
    // 之后再覆盖成题目类型, 便于 UI 或统计区分。
    finish_question(question, answer, QUESTION_SUB, 100, minus, 0, rng);
    question->kind = (uint8_t)(type == HUNDRED_SUB_TYPE_B ? QUESTION_HUNDRED_B
                                                          : QUESTION_HUNDRED_A);
    question->math_stage = 4;
}

void generateHundredSubQuestion(math_question_t *question, math_rng_t *rng)
{
    if (question == NULL || rng == NULL) {
        return;
    }
    // 1..99: 排除 100-0 与 100-100 两个边界, 结果同样保持 1..99。
    const int minus = 1 + (int)math_rng_below(rng, 99);
    const hundred_sub_type_t type =
        math_rng_below(rng, 2) == 0 ? HUNDRED_SUB_TYPE_A : HUNDRED_SUB_TYPE_B;
    buildHundredSubQuestion(question, minus, type, rng);
}

static void make_mix(math_question_t *q, math_rng_t *rng)
{
    const int a = (int)rng_range(rng, 2, 9);
    const int b = (int)rng_range(rng, 2, 9);
    const bool plus = math_rng_below(rng, 2) == 0;
    const int product = a * b;
    if (plus) {
        const int c = (int)rng_range(rng, 1, (uint32_t)(100 - product));
        snprintf(q->text, sizeof(q->text), "%d × %d + %d = ?", a, b, c);
        finish_question(q, product + c, QUESTION_MIX, a, b, c, rng);
    } else {
        if (product <= 1) {
            make_mix(q, rng);
            return;
        }
        const int c = (int)rng_range(rng, 1, (uint32_t)(product - 1));
        snprintf(q->text, sizeof(q->text), "%d × %d - %d = ?", a, b, c);
        finish_question(q, product - c, QUESTION_MIX, a, b, -c, rng);
    }
    snprintf(q->hint, sizeof(q->hint), "先算乘法，再算加减");
}

static uint8_t pick_kind(uint8_t stage, math_rng_t *rng)
{
    const uint32_t roll = math_rng_below(rng, 100);
    if (stage <= 1) {
        if (roll < 45) return QUESTION_ADD;
        if (roll < 75) return QUESTION_SUB;
        return QUESTION_MUL;
    }
    if (stage == 2) {
        if (roll < 35) return QUESTION_ADD;
        if (roll < 65) return QUESTION_SUB;
        return QUESTION_MUL;
    }
    if (stage == 3) {
        if (roll < 25) return QUESTION_ADD;
        if (roll < 50) return QUESTION_SUB;
        if (roll < 70) return QUESTION_MUL;
        if (roll < 85) return QUESTION_DIV;
        return QUESTION_FILL;
    }
    if (roll < 20) return QUESTION_ADD;
    if (roll < 40) return QUESTION_SUB;
    if (roll < 55) return QUESTION_DIV;
    if (roll < 75) return QUESTION_FILL;
    return QUESTION_MIX;
}

static bool seen_before(const math_question_t *history, uint8_t history_len,
                        const char *text)
{
    for (uint8_t i = 0; i < history_len; i++) {
        if (strcmp(history[i].text, text) == 0) {
            return true;
        }
    }
    return false;
}

static void generate_once(math_question_t *q, uint8_t stage, math_rng_t *rng)
{
    switch (pick_kind(stage, rng)) {
    case QUESTION_ADD:
        make_add(q, stage, rng);
        break;
    case QUESTION_SUB:
        make_sub(q, stage, rng);
        break;
    case QUESTION_MUL:
        make_mul(q, stage, rng);
        break;
    case QUESTION_DIV:
        make_div(q, stage, rng);
        break;
    case QUESTION_FILL:
        make_fill(q, rng);
        break;
    default:
        make_mix(q, rng);
        break;
    }
    q->math_stage = stage;
}

void math_question_generate(math_question_t *question, uint8_t stage, bool assist,
                            math_rng_t *rng, const math_question_t *history,
                            uint8_t history_len)
{
    if (question == NULL || rng == NULL) {
        return;
    }
    uint8_t effective = stage;
    if (effective < 1) {
        effective = 1;
    }
    if (effective > 4) {
        effective = 4;
    }
    if (assist && effective > 1) {
        effective--;
    }

    for (int attempt = 0; attempt < 24; attempt++) {
        generate_once(question, effective, rng);
        if (history == NULL || history_len == 0 ||
            !seen_before(history, history_len, question->text)) {
            return;
        }
    }
}

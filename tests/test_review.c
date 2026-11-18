// Host tests for the wrong-answer review pool.
#include "review.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static math_question_t make_question(const char *text, int16_t answer)
{
    math_question_t question;
    memset(&question, 0, sizeof(question));
    snprintf(question.text, sizeof(question.text), "%s", text);
    snprintf(question.hint, sizeof(question.hint), "hint %s", text);
    question.options[0] = answer;
    question.options[1] = (int16_t)(answer + 1);
    question.options[2] = (int16_t)(answer - 1);
    question.answer = answer;
    question.correct_index = 0;
    question.kind = QUESTION_ADD;
    question.math_stage = 1;
    return question;
}

static void test_record_and_collect(void)
{
    review_store_t store;
    review_store_init(&store);
    assert(store.count == 0);

    const math_question_t a = make_question("3+4", 7);
    const math_question_t b = make_question("8+2", 10);
    const math_question_t c = make_question("9-3", 6);
    assert(review_store_record(&store, &a, 1));
    assert(review_store_record(&store, &b, 2));
    assert(review_store_record(&store, &c, 1));
    assert(store.count == 3);

    review_record_t out[3];
    uint8_t n = review_store_collect(&store, 1, out, 3);
    assert(n == 3);
    // Newest same-stage question first, then the older one, then other stages.
    assert(strcmp(out[0].question.text, "9-3") == 0);
    assert(strcmp(out[1].question.text, "3+4") == 0);
    assert(strcmp(out[2].question.text, "8+2") == 0);
    assert(out[0].stage == 1);
    assert(out[1].mistakes == 1);

    // A limit smaller than the pool truncates from the front.
    n = review_store_collect(&store, 1, out, 1);
    assert(n == 1);
    assert(strcmp(out[0].question.text, "9-3") == 0);
}

static void test_duplicate_moves_to_newest(void)
{
    review_store_t store;
    review_store_init(&store);
    const math_question_t a = make_question("2+2", 4);
    const math_question_t b = make_question("5+5", 10);

    assert(review_store_record(&store, &a, 1));
    assert(review_store_record(&store, &b, 2));
    assert(review_store_record(&store, &a, 1));
    assert(store.count == 2);

    review_record_t out[2];
    const uint8_t n = review_store_collect(&store, 1, out, 2);
    assert(n == 2);
    assert(strcmp(out[0].question.text, "2+2") == 0);
    assert(out[0].mistakes == 2);
    assert(strcmp(out[1].question.text, "5+5") == 0);
}

static void test_overflow_drops_oldest(void)
{
    review_store_t store;
    review_store_init(&store);

    for (int i = 0; i < REVIEW_CAPACITY + 1; i++) {
        char text[16];
        snprintf(text, sizeof(text), "1+%d", i);
        const math_question_t q = make_question(text, (int16_t)(1 + i));
        assert(review_store_record(&store, &q, 1));
    }
    assert(store.count == REVIEW_CAPACITY);

    // The very first record has been evicted; the newest is still present.
    review_record_t out[REVIEW_CAPACITY];
    const uint8_t n = review_store_collect(&store, 1, out, REVIEW_CAPACITY);
    assert(n == REVIEW_CAPACITY);
    assert(strcmp(out[0].question.text, "1+40") == 0);
    bool found_first = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].question.text, "1+0") == 0) {
            found_first = true;
        }
    }
    assert(!found_first);
}

static void test_clear(void)
{
    review_store_t store;
    review_store_init(&store);
    const math_question_t q = make_question("6+6", 12);
    assert(review_store_record(&store, &q, 3));
    assert(store.count == 1);
    review_store_clear(&store);
    assert(store.count == 0);

    review_record_t out[1];
    assert(review_store_collect(&store, 3, out, 1) == 0);
}

int main(void)
{
    test_record_and_collect();
    test_duplicate_moves_to_newest();
    test_overflow_drops_oldest();
    test_clear();
    puts("test_review: PASS");
    return 0;
}
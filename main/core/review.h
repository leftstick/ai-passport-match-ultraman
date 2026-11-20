// Wrong-answer review pool. Pure C so it can be covered by host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math_question.h"

// Keep the pool small enough for a single NVS blob while still holding a
// useful slice of recent mistakes across several stages.
#define REVIEW_CAPACITY 40

typedef struct {
    math_question_t question;
    uint8_t stage;
    uint8_t mistakes;
} review_record_t;

typedef struct {
    uint8_t count;
    review_record_t items[REVIEW_CAPACITY];
} review_store_t;

void review_store_init(review_store_t *store);

// Adds or refreshes a mistake. The most recent mistake is always last so the
// collect helper can walk backwards without extra bookkeeping.
bool review_store_record(review_store_t *store, const math_question_t *question,
                         uint8_t stage);

// Newest first. Records from `stage` come first; the remaining slots are
// filled with other stages so an empty stage still has material to review.
// Questions above `max_math_stage` are skipped; pass 0 to disable the cap.
uint8_t review_store_collect(const review_store_t *store, uint8_t stage,
                             uint8_t max_math_stage, review_record_t *out,
                             uint8_t max);

// A correct review answer lowers the mistake count. The record leaves the pool
// once its count reaches zero. Returns true when the question was removed.
bool review_store_mastered(review_store_t *store,
                           const math_question_t *question);

void review_store_clear(review_store_t *store);
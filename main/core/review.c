#include "review.h"

#include <string.h>

static bool same_question(const math_question_t *a, const math_question_t *b)
{
    return a->answer == b->answer &&
           strcmp(a->text, b->text) == 0;
}

static void move_to_newest(review_store_t *store, uint8_t index)
{
    if (index + 1u >= store->count) {
        return;
    }
    const review_record_t record = store->items[index];
    memmove(&store->items[index], &store->items[index + 1],
            (size_t)(store->count - index - 1u) * sizeof(store->items[0]));
    store->items[store->count - 1u] = record;
}

void review_store_init(review_store_t *store)
{
    memset(store, 0, sizeof(*store));
}

bool review_store_record(review_store_t *store, const math_question_t *question,
                         uint8_t stage)
{
    for (uint8_t i = 0; i < store->count; i++) {
        if (same_question(&store->items[i].question, question)) {
            if (store->items[i].mistakes < 0xFFu) {
                store->items[i].mistakes++;
            }
            move_to_newest(store, i);
            return true;
        }
    }

    review_record_t record;
    memset(&record, 0, sizeof(record));
    record.question = *question;
    record.stage = stage;
    record.mistakes = 1;

    if (store->count < REVIEW_CAPACITY) {
        store->items[store->count++] = record;
        return true;
    }

    memmove(&store->items[0], &store->items[1],
            (REVIEW_CAPACITY - 1u) * sizeof(store->items[0]));
    store->items[REVIEW_CAPACITY - 1u] = record;
    return true;
}

uint8_t review_store_collect(const review_store_t *store, uint8_t stage,
                             review_record_t *out, uint8_t max)
{
    uint8_t written = 0;
    for (int i = (int)store->count - 1; i >= 0 && written < max; i--) {
        if (store->items[i].stage == stage) {
            out[written++] = store->items[i];
        }
    }
    for (int i = (int)store->count - 1; i >= 0 && written < max; i--) {
        if (store->items[i].stage != stage) {
            out[written++] = store->items[i];
        }
    }
    return written;
}

void review_store_clear(review_store_t *store)
{
    store->count = 0;
}
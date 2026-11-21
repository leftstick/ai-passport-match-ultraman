#include "time_attack.h"

#include <string.h>

static void record_history(time_attack_t *run)
{
    if (run->history_count < MATH_QUESTION_HISTORY) {
        run->history[run->history_count++] = run->question;
        return;
    }
    for (uint8_t i = 1; i < MATH_QUESTION_HISTORY; i++) {
        run->history[i - 1] = run->history[i];
    }
    run->history[MATH_QUESTION_HISTORY - 1] = run->question;
}

static uint8_t pick_other(math_rng_t *rng, uint8_t avoid_a, uint8_t avoid_b)
{
    uint8_t candidates[MATH_QUESTION_OPTION_COUNT];
    uint8_t count = 0;
    for (uint8_t i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        if (i != avoid_a && i != avoid_b) {
            candidates[count++] = i;
        }
    }
    if (count == 0) {
        return 0xFF;
    }
    return candidates[math_rng_below(rng, count)];
}

// 怪兽换位:打乱三个选项,并重新定位正确答案。
static void shuffle_options(time_attack_t *run)
{
    int16_t options[MATH_QUESTION_OPTION_COUNT];
    memcpy(options, run->question.options, sizeof(options));
    for (uint8_t i = MATH_QUESTION_OPTION_COUNT - 1; i > 0; i--) {
        const uint8_t j = (uint8_t)math_rng_below(&run->rng, (uint32_t)i + 1u);
        const int16_t tmp = options[i];
        options[i] = options[j];
        options[j] = tmp;
    }
    memcpy(run->question.options, options, sizeof(options));
    for (uint8_t i = 0; i < MATH_QUESTION_OPTION_COUNT; i++) {
        if (run->question.options[i] == run->question.answer) {
            run->question.correct_index = i;
            break;
        }
    }
}

static void next_question(time_attack_t *run)
{
    uint8_t tier = (uint8_t)(1u + run->question_index / 6u);
    if (tier > 4) {
        tier = 4;
    }
    run->tier = tier;
    math_question_generate(&run->question, tier, run->assist, &run->rng,
                           run->history, run->history_count);
    record_history(run);
    run->question_index++;
    run->phase = TIME_PHASE_QUESTION;
    run->cursor = TIME_ATTACK_CURSOR_NONE;
    run->mask_index = 0xFF;
    run->lock_index = 0xFF;
    run->lock_open = false;
    run->swap_done = false;
    run->swap_at_ms = 0;
    run->question_elapsed_ms = 0;
    run->feedback_ms = 0;
    run->last_correct = 0xFF;
    run->last_wrong = 0xFF;

    if (!run->hazards) {
        return;
    }
    if (run->combo >= TIME_ATTACK_MASK_COMBO) {
        run->mask_index = (uint8_t)math_rng_below(&run->rng,
                                                  MATH_QUESTION_OPTION_COUNT);
    }
    if (run->combo >= TIME_ATTACK_LOCK_COMBO) {
        run->lock_index = pick_other(&run->rng, run->mask_index, 0xFF);
    }
    if (run->combo >= TIME_ATTACK_SWAP_COMBO) {
        // 给玩家一点时间看清题目,再让怪兽把选项换位。
        run->swap_at_ms = 1200;
    }
}

void time_attack_start(time_attack_t *run, uint32_t seed, bool assist, bool hazards)
{
    memset(run, 0, sizeof(*run));
    run->assist = assist;
    run->hazards = hazards;
    run->remaining_ms = TIME_ATTACK_MS;
    math_rng_seed(&run->rng, seed);
    next_question(run);
}

time_event_t time_attack_move(time_attack_t *run, int delta)
{
    if (run->phase != TIME_PHASE_QUESTION || run->finished) {
        return TIME_EV_NONE;
    }
    if (run->cursor == TIME_ATTACK_CURSOR_NONE) {
        run->cursor = delta < 0 ? (MATH_QUESTION_OPTION_COUNT - 1u) : 0u;
        return TIME_EV_MOVED;
    }
    int next = (int)run->cursor + delta;
    while (next < 0) {
        next += MATH_QUESTION_OPTION_COUNT;
    }
    run->cursor = (uint8_t)(next % MATH_QUESTION_OPTION_COUNT);
    return TIME_EV_MOVED;
}

static void finish(time_attack_t *run)
{
    run->phase = TIME_PHASE_FINISHED;
    run->finished = true;
    run->remaining_ms = 0;
    run->cursor = TIME_ATTACK_CURSOR_NONE;
}

time_event_t time_attack_confirm(time_attack_t *run)
{
    if (run->finished || run->phase == TIME_PHASE_FINISHED) {
        return TIME_EV_NONE;
    }
    if (run->phase == TIME_PHASE_FEEDBACK) {
        next_question(run);
        return TIME_EV_NONE;
    }
    if (run->cursor == TIME_ATTACK_CURSOR_NONE) {
        return TIME_EV_NEED_SELECT;
    }
    // 冰冻选项要按两次:第一次解冻,第二次才算作答。
    if (run->lock_index != 0xFF && run->cursor == run->lock_index &&
        !run->lock_open) {
        run->lock_open = true;
        return TIME_EV_UNLOCKED;
    }

    run->last_correct = run->question.correct_index;
    if (run->cursor == run->question.correct_index) {
        run->combo++;
        if (run->combo > run->best_combo) {
            run->best_combo = run->combo;
        }
        run->correct_count++;
        int32_t points = 10 + (int32_t)(run->combo > 10 ? 10 : run->combo) * 2;
        if (run->mask_index != 0xFF || run->lock_index != 0xFF) {
            points += 5;
        }
        run->score += points;
        run->last_wrong = 0xFF;
        run->feedback_ms = TIME_ATTACK_CORRECT_MS;
        run->phase = TIME_PHASE_FEEDBACK;
        return TIME_EV_CORRECT;
    }

    run->combo = 0;
    run->wrong_count++;
    run->last_wrong = run->cursor;
    run->mask_index = 0xFF;
    run->lock_index = 0xFF;
    run->lock_open = false;
    run->feedback_ms = TIME_ATTACK_WRONG_MS;
    run->phase = TIME_PHASE_FEEDBACK;
    if (run->remaining_ms <= TIME_ATTACK_PENALTY_MS) {
        finish(run);
        return TIME_EV_FINISHED;
    }
    run->remaining_ms -= TIME_ATTACK_PENALTY_MS;
    return TIME_EV_WRONG;
}

time_event_t time_attack_tick(time_attack_t *run, uint32_t elapsed_ms)
{
    if (run->finished || elapsed_ms == 0) {
        return TIME_EV_NONE;
    }

    if (elapsed_ms >= run->remaining_ms) {
        finish(run);
        return TIME_EV_FINISHED;
    }
    run->remaining_ms -= elapsed_ms;

    if (run->phase == TIME_PHASE_FEEDBACK) {
        if (elapsed_ms >= run->feedback_ms) {
            next_question(run);
        } else {
            run->feedback_ms = (uint16_t)(run->feedback_ms - elapsed_ms);
        }
        return TIME_EV_NONE;
    }

    if (run->phase == TIME_PHASE_QUESTION) {
        run->question_elapsed_ms =
            (uint16_t)(run->question_elapsed_ms + elapsed_ms);
        if (run->swap_at_ms > 0 && !run->swap_done &&
            run->question_elapsed_ms >= run->swap_at_ms) {
            run->swap_done = true;
            shuffle_options(run);
            return TIME_EV_SWAP;
        }
    }
    return TIME_EV_NONE;
}

bool time_attack_option_hidden(const time_attack_t *run, uint8_t index)
{
    return run->phase == TIME_PHASE_QUESTION &&
           run->mask_index != 0xFF && index == run->mask_index;
}

bool time_attack_option_locked(const time_attack_t *run, uint8_t index)
{
    return run->phase == TIME_PHASE_QUESTION &&
           run->lock_index != 0xFF && index == run->lock_index &&
           !run->lock_open;
}

const char *time_attack_hazard_text(const time_attack_t *run)
{
    if (run->phase != TIME_PHASE_QUESTION) {
        return "";
    }
    if (run->lock_index != 0xFF && !run->lock_open) {
        return "冰冻选项 · 再按一次OK";
    }
    if (run->mask_index != 0xFF) {
        return "怪兽吐烟 · 有个选项看不见";
    }
    return "";
}

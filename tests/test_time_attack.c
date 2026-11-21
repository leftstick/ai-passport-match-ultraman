// Host tests for the 60-second time-attack mode.
#include "time_attack.h"

#include <assert.h>
#include <stdio.h>

static void select_correct(time_attack_t *run)
{
    const uint8_t correct = run->question.correct_index;
    assert(correct < MATH_QUESTION_OPTION_COUNT);
    run->cursor = correct;
    time_event_t ev = time_attack_confirm(run);
    if (ev == TIME_EV_UNLOCKED) {
        // 冰冻选项的第二次确认才是作答。
        ev = time_attack_confirm(run);
    }
    assert(ev == TIME_EV_CORRECT);
    assert(run->phase == TIME_PHASE_FEEDBACK);
}

static void answer_correct(time_attack_t *run)
{
    select_correct(run);
    assert(time_attack_tick(run, TIME_ATTACK_CORRECT_MS) == TIME_EV_NONE);
    assert(run->phase == TIME_PHASE_QUESTION);
}

static void test_start_and_selection(void)
{
    time_attack_t run;
    time_attack_start(&run, 1234, false, true);
    assert(run.phase == TIME_PHASE_QUESTION);
    assert(run.remaining_ms == TIME_ATTACK_MS);
    assert(run.cursor == TIME_ATTACK_CURSOR_NONE);
    assert(run.question.correct_index < MATH_QUESTION_OPTION_COUNT);

    // 未选答案时按 OK 不算作答。
    assert(time_attack_confirm(&run) == TIME_EV_NEED_SELECT);
    assert(run.phase == TIME_PHASE_QUESTION);

    // 第一次按 ↓ 直接落在第一个选项。
    assert(time_attack_move(&run, 1) == TIME_EV_MOVED);
    assert(run.cursor == 0);
    assert(time_attack_move(&run, -1) == TIME_EV_MOVED);
    assert(run.cursor == MATH_QUESTION_OPTION_COUNT - 1);
    assert(time_attack_move(&run, 1) == TIME_EV_MOVED);
    assert(run.cursor == 0);
}

static void test_correct_and_wrong(void)
{
    time_attack_t run;
    time_attack_start(&run, 77, false, true);
    answer_correct(&run);
    assert(run.combo == 1);
    assert(run.correct_count == 1);
    assert(run.score > 0);
    assert(run.question_index == 2);

    // 故意答错:扣 3 秒并清空连击。
    const uint8_t wrong =
        (uint8_t)((run.question.correct_index + 1u) % MATH_QUESTION_OPTION_COUNT);
    run.cursor = wrong;
    const uint32_t before = run.remaining_ms;
    assert(time_attack_confirm(&run) == TIME_EV_WRONG);
    assert(run.combo == 0);
    assert(run.wrong_count == 1);
    assert(run.remaining_ms + TIME_ATTACK_PENALTY_MS == before);
    assert(run.phase == TIME_PHASE_FEEDBACK);
    assert(time_attack_tick(&run, TIME_ATTACK_WRONG_MS) == TIME_EV_NONE);
    assert(run.phase == TIME_PHASE_QUESTION);
    assert(run.cursor == TIME_ATTACK_CURSOR_NONE);
}

static void test_timer_finish(void)
{
    time_attack_t run;
    time_attack_start(&run, 9, false, true);
    assert(time_attack_tick(&run, 59000) == TIME_EV_NONE);
    assert(run.remaining_ms == 1000);
    assert(time_attack_tick(&run, 1000) == TIME_EV_FINISHED);
    assert(run.finished);
    assert(run.phase == TIME_PHASE_FINISHED);
    assert(run.remaining_ms == 0);
    // 结束后输入不再生效。
    assert(time_attack_move(&run, 1) == TIME_EV_NONE);
    assert(time_attack_confirm(&run) == TIME_EV_NONE);
    assert(time_attack_tick(&run, 100) == TIME_EV_NONE);
}

static void test_penalty_can_finish(void)
{
    time_attack_t run;
    time_attack_start(&run, 5, false, true);
    run.remaining_ms = 2000;
    run.cursor = (uint8_t)((run.question.correct_index + 1u) %
                           MATH_QUESTION_OPTION_COUNT);
    assert(time_attack_confirm(&run) == TIME_EV_FINISHED);
    assert(run.finished);
}

static void test_hazards_unlock_with_combo(void)
{
    time_attack_t run;
    time_attack_start(&run, 4242, false, true);
    for (int i = 0; i < TIME_ATTACK_MASK_COMBO; i++) {
        answer_correct(&run);
    }
    assert(run.combo == TIME_ATTACK_MASK_COMBO);
    assert(run.mask_index < MATH_QUESTION_OPTION_COUNT);
    assert(run.lock_index == 0xFF);

    // 烟雾遮住的选项在作答阶段不可见,反馈阶段恢复。
    assert(time_attack_option_hidden(&run, run.mask_index));
    assert(!time_attack_option_hidden(&run,
                                      (uint8_t)((run.mask_index + 1u) %
                                                MATH_QUESTION_OPTION_COUNT)));

    for (int i = run.combo; i < TIME_ATTACK_LOCK_COMBO; i++) {
        answer_correct(&run);
    }
    assert(run.lock_index < MATH_QUESTION_OPTION_COUNT);
    assert(run.lock_index != run.mask_index);

    // 冰冻选项要按两次:第一次解冻,第二次才作答。
    const uint8_t frozen = run.lock_index;
    run.cursor = frozen;
    assert(time_attack_confirm(&run) == TIME_EV_UNLOCKED);
    assert(run.lock_open);
    assert(!time_attack_option_locked(&run, frozen));
    const time_event_t second = time_attack_confirm(&run);
    assert(second == TIME_EV_CORRECT || second == TIME_EV_WRONG);
}

static void test_swap_event(void)
{
    time_attack_t run;
    time_attack_start(&run, 31337, false, true);
    for (int i = 0; i < TIME_ATTACK_SWAP_COMBO; i++) {
        answer_correct(&run);
    }
    assert(run.combo == TIME_ATTACK_SWAP_COMBO);
    assert(run.swap_at_ms > 0);
    const uint8_t correct = run.question.correct_index;
    assert(time_attack_tick(&run, run.swap_at_ms) == TIME_EV_SWAP);
    assert(run.swap_done);
    // 换位后正确答案仍然可被找到。
    assert(run.question.options[run.question.correct_index] ==
           run.question.answer);
    (void)correct;
}

static void test_hazards_disabled(void)
{
    time_attack_t run;
    time_attack_start(&run, 2024, true, false);
    for (int i = 0; i < TIME_ATTACK_LOCK_COMBO + 2; i++) {
        answer_correct(&run);
    }
    assert(run.mask_index == 0xFF);
    assert(run.lock_index == 0xFF);
    assert(run.swap_at_ms == 0);
    assert(time_attack_option_hidden(&run, 0) == false);
    assert(time_attack_option_locked(&run, 0) == false);
}

int main(void)
{
    test_start_and_selection();
    test_correct_and_wrong();
    test_timer_finish();
    test_penalty_can_finish();
    test_hazards_unlock_with_combo();
    test_swap_event();
    test_hazards_disabled();
    puts("test_time_attack: PASS");
    return 0;
}

// 限时猎手:60 秒连续答题,连击越高,怪兽的干扰越强。
// 纯 C,不依赖 LVGL/ESP-IDF,可在主机上测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math_question.h"

#define TIME_ATTACK_MS 60000
#define TIME_ATTACK_PENALTY_MS 3000
#define TIME_ATTACK_CORRECT_MS 420
#define TIME_ATTACK_WRONG_MS 760
#define TIME_ATTACK_CURSOR_NONE 0xFF

// 连击门槛:4 连起烟雾遮住一个选项,8 连起冰冻一个选项,12 连起怪兽换位。
#define TIME_ATTACK_MASK_COMBO 4
#define TIME_ATTACK_LOCK_COMBO 8
#define TIME_ATTACK_SWAP_COMBO 12

typedef enum {
    TIME_EV_NONE = 0,
    TIME_EV_MOVED,
    TIME_EV_NEED_SELECT,
    TIME_EV_UNLOCKED,
    TIME_EV_SWAP,
    TIME_EV_CORRECT,
    TIME_EV_WRONG,
    TIME_EV_FINISHED,
} time_event_t;

typedef enum {
    TIME_PHASE_QUESTION = 0,
    TIME_PHASE_FEEDBACK,
    TIME_PHASE_FINISHED,
} time_phase_t;

typedef struct {
    uint8_t phase;
    math_question_t question;
    math_question_t history[MATH_QUESTION_HISTORY];
    uint8_t history_count;
    math_rng_t rng;

    uint8_t tier;
    uint8_t cursor;
    uint32_t remaining_ms;
    int32_t score;
    uint8_t combo;
    uint8_t best_combo;
    uint8_t correct_count;
    uint8_t wrong_count;
    uint8_t question_index;

    // 干扰状态。0xFF 表示该干扰本回合未生效。
    uint8_t mask_index;
    uint8_t lock_index;
    bool lock_open;
    bool hazards;
    bool swap_done;
    bool assist;

    uint8_t last_correct;
    uint8_t last_wrong;
    uint16_t feedback_ms;
    uint16_t question_elapsed_ms;
    uint16_t swap_at_ms;
    bool finished;
} time_attack_t;

// hazards: allow mask/lock/swap. 低龄模式传 false。
void time_attack_start(time_attack_t *run, uint32_t seed, bool assist, bool hazards);

time_event_t time_attack_move(time_attack_t *run, int delta);
time_event_t time_attack_confirm(time_attack_t *run);
time_event_t time_attack_tick(time_attack_t *run, uint32_t elapsed_ms);

// 界面用来决定是否隐藏/锁住某个选项。
bool time_attack_option_hidden(const time_attack_t *run, uint8_t index);
bool time_attack_option_locked(const time_attack_t *run, uint8_t index);
// 当前生效的干扰提示,没有干扰时返回空串。
const char *time_attack_hazard_text(const time_attack_t *run);

// 达到这个分数即解锁「限时猎手」徽章。
#define TIME_ATTACK_BADGE_SCORE 300

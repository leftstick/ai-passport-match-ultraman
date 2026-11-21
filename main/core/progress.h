// 收藏与成就进度。纯 C,可主机测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PROGRESS_MONSTER_COUNT 9
#define PROGRESS_HERO_COUNT 16

typedef enum {
    PROGRESS_BADGE_FLAWLESS = 0,   // 无伤通关任意关卡
    PROGRESS_BADGE_COMBO,          // 单局 8 连击
    PROGRESS_BADGE_ENDLESS,        // 无尽模式撑到第 5 关
    PROGRESS_BADGE_ALL_CLEAR,      // 打通全部关卡
    PROGRESS_BADGE_TIME,           // 限时猎手得分 300
    PROGRESS_BADGE_THREE_STAR,     // 全部关卡 3 星
    PROGRESS_BADGE_COUNT
} progress_badge_t;

typedef struct {
    uint16_t monsters;  // bit i = 已遭遇第 i 关怪兽
    uint16_t heroes;    // bit i = 已使用第 i 位英雄
    uint16_t badges;    // bit i = 已获得第 i 枚徽章
    uint32_t time_best;
} progress_t;

void progress_init(progress_t *progress);

// 返回 true 表示这是首次收录。
bool progress_mark_monster(progress_t *progress, uint8_t stage);
bool progress_mark_hero(progress_t *progress, uint8_t hero);
bool progress_award(progress_t *progress, uint8_t badge);
bool progress_has_badge(const progress_t *progress, uint8_t badge);

// 只有更高的分数才会覆盖最佳成绩。
bool progress_record_time(progress_t *progress, uint32_t score);

uint8_t progress_count16(uint16_t value);
uint8_t progress_three_star_count(uint32_t stars);

const char *progress_badge_name(uint8_t badge);
const char *progress_badge_detail(uint8_t badge);

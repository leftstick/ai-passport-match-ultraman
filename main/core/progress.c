#include "progress.h"

#include <string.h>

static const char *const BADGE_NAMES[PROGRESS_BADGE_COUNT] = {
    "无伤通关",
    "连击高手",
    "无尽五战",
    "全关制霸",
    "限时猎手",
    "三星收藏",
};

static const char *const BADGE_DETAILS[PROGRESS_BADGE_COUNT] = {
    "满血通过任意一关",
    "单局打出 8 连击",
    "无尽模式到达第 5 关",
    "打通全部 9 关",
    "限时猎手得分 300",
    "9 关全部拿到 3 星",
};

void progress_init(progress_t *progress)
{
    memset(progress, 0, sizeof(*progress));
}

bool progress_mark_monster(progress_t *progress, uint8_t stage)
{
    if (stage >= PROGRESS_MONSTER_COUNT) {
        return false;
    }
    const uint16_t bit = (uint16_t)(1u << stage);
    if ((progress->monsters & bit) != 0) {
        return false;
    }
    progress->monsters |= bit;
    return true;
}

bool progress_mark_hero(progress_t *progress, uint8_t hero)
{
    if (hero >= PROGRESS_HERO_COUNT) {
        return false;
    }
    const uint16_t bit = (uint16_t)(1u << hero);
    if ((progress->heroes & bit) != 0) {
        return false;
    }
    progress->heroes |= bit;
    return true;
}

bool progress_award(progress_t *progress, uint8_t badge)
{
    if (badge >= PROGRESS_BADGE_COUNT) {
        return false;
    }
    const uint16_t bit = (uint16_t)(1u << badge);
    if ((progress->badges & bit) != 0) {
        return false;
    }
    progress->badges |= bit;
    return true;
}

bool progress_has_badge(const progress_t *progress, uint8_t badge)
{
    if (badge >= PROGRESS_BADGE_COUNT) {
        return false;
    }
    return (progress->badges & (uint16_t)(1u << badge)) != 0;
}

bool progress_record_time(progress_t *progress, uint32_t score)
{
    if (score <= progress->time_best) {
        return false;
    }
    progress->time_best = score;
    return true;
}

uint8_t progress_count16(uint16_t value)
{
    uint8_t count = 0;
    while (value != 0) {
        value = (uint16_t)(value & (uint16_t)(value - 1u));
        count++;
    }
    return count;
}


uint8_t progress_three_star_count(uint32_t stars)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < PROGRESS_MONSTER_COUNT; i++) {
        if (((stars >> (i * 2u)) & 3u) == 3u) {
            count++;
        }
    }
    return count;
}

const char *progress_badge_name(uint8_t badge)
{
    return badge < PROGRESS_BADGE_COUNT ? BADGE_NAMES[badge] : "";
}

const char *progress_badge_detail(uint8_t badge)
{
    return badge < PROGRESS_BADGE_COUNT ? BADGE_DETAILS[badge] : "";
}

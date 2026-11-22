// Host tests for the collection / achievement progress store.
#include "progress.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_monsters(void)
{
    progress_t progress;
    progress_init(&progress);
    assert(progress.monsters == 0);
    assert(progress_mark_monster(&progress, 0));
    assert(progress_mark_monster(&progress, 8));
    assert(progress_mark_monster(&progress, 9));
    assert(progress_mark_monster(&progress, 10));
    assert(!progress_mark_monster(&progress, 8));
    assert(!progress_mark_monster(&progress, PROGRESS_MONSTER_COUNT));
    assert(progress_count16(progress.monsters) == 4);
    assert(progress.monsters == 0x0701);
}

static void test_heroes(void)
{
    progress_t progress;
    progress_init(&progress);
    assert(progress_mark_hero(&progress, 6));
    assert(progress_mark_hero(&progress, 15));
    assert(progress_mark_hero(&progress, 16));
    assert(progress_mark_hero(&progress, 17));
    assert(!progress_mark_hero(&progress, 17));
    assert(!progress_mark_hero(&progress, PROGRESS_HERO_COUNT));
    assert(progress_count32(progress.heroes) == 4);
    assert(progress.heroes == 0x00038040u);
}

static void test_legacy_migration(void)
{
    // 旧存档: 16 位英雄掩码, 不能因为升级丢失已有收藏。
    struct {
        uint16_t monsters;
        uint16_t heroes;
        uint16_t badges;
        uint32_t time_best;
    } legacy = {0x0005, 0x8040, 0x0003, 777};
    progress_t progress;
    assert(progress_load(&progress, &legacy, sizeof(legacy)));
    assert(progress.monsters == 0x0005);
    assert(progress.heroes == 0x00008040u);
    assert(progress.badges == 0x0003);
    assert(progress.time_best == 777);

    // 新布局直接拷贝; 空 blob 或者未知长度则回到默认值。
    progress_t fresh;
    assert(progress_load(&fresh, &progress, sizeof(progress)));
    assert(fresh.heroes == progress.heroes);
    assert(!progress_load(&fresh, NULL, 0));
    assert(fresh.monsters == 0 && fresh.heroes == 0);
}

static void test_badges(void)
{
    progress_t progress;
    progress_init(&progress);
    assert(!progress_has_badge(&progress, PROGRESS_BADGE_FLAWLESS));
    assert(progress_award(&progress, PROGRESS_BADGE_FLAWLESS));
    assert(!progress_award(&progress, PROGRESS_BADGE_FLAWLESS));
    assert(progress_has_badge(&progress, PROGRESS_BADGE_FLAWLESS));
    assert(!progress_has_badge(&progress, PROGRESS_BADGE_TIME));
    assert(!progress_award(&progress, PROGRESS_BADGE_COUNT));

    for (uint8_t i = 0; i < PROGRESS_BADGE_COUNT; i++) {
        assert(strlen(progress_badge_name(i)) > 0);
        assert(strlen(progress_badge_detail(i)) > 0);
    }
    assert(strcmp(progress_badge_name(PROGRESS_BADGE_TIME), "限时猎手") == 0);
}

static void test_time_best(void)
{
    progress_t progress;
    progress_init(&progress);
    assert(progress_record_time(&progress, 120));
    assert(progress.time_best == 120);
    assert(!progress_record_time(&progress, 120));
    assert(!progress_record_time(&progress, 90));
    assert(progress.time_best == 120);
    assert(progress_record_time(&progress, 301));
    assert(progress.time_best == 301);
}

static void test_three_stars(void)
{
    // 9 关,每关 2 bit。前 3 关满星,第 4 关 2 星。
    uint32_t stars = 0;
    for (uint8_t i = 0; i < 3; i++) {
        stars |= 3u << (i * 2u);
    }
    stars |= 2u << (3u * 2u);
    assert(progress_three_star_count(stars) == 3);
    assert(progress_three_star_count(0) == 0);
    assert(progress_three_star_count(0x3FFFFu) == PROGRESS_STAGE_COUNT);
}

int main(void)
{
    test_monsters();
    test_heroes();
    test_legacy_migration();
    test_badges();
    test_time_best();
    test_three_stars();
    puts("test_progress: PASS");
    return 0;
}

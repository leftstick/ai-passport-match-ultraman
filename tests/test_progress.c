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
    assert(progress_mark_monster(&progress, 7));
    assert(!progress_mark_monster(&progress, 7));
    assert(!progress_mark_monster(&progress, PROGRESS_MONSTER_COUNT));
    assert(progress_count16(progress.monsters) == 2);
    assert(progress.monsters == 0x0081);
}

static void test_heroes(void)
{
    progress_t progress;
    progress_init(&progress);
    assert(progress_mark_hero(&progress, 6));
    assert(!progress_mark_hero(&progress, 6));
    assert(!progress_mark_hero(&progress, PROGRESS_HERO_COUNT));
    assert(progress_count8(progress.heroes) == 1);
    assert(progress.heroes == 0x40);
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
    // 8 关,每关 2 bit。前 3 关满星,第 4 关 2 星。
    uint32_t stars = 0;
    for (uint8_t i = 0; i < 3; i++) {
        stars |= 3u << (i * 2u);
    }
    stars |= 2u << (3u * 2u);
    assert(progress_three_star_count(stars) == 3);
    assert(progress_three_star_count(0) == 0);
    assert(progress_three_star_count(0xFFFFu) == PROGRESS_MONSTER_COUNT);
}

int main(void)
{
    test_monsters();
    test_heroes();
    test_badges();
    test_time_best();
    test_three_stars();
    puts("test_progress: PASS");
    return 0;
}

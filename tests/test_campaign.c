// Host tests for campaign content and derived difficulty numbers.
#include "campaign.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_content(void)
{
    for (uint8_t i = 0; i < CAMPAIGN_HERO_COUNT; i++) {
        const hero_def_t *hero = campaign_hero(i);
        assert(hero->name[0] != '\0');
        assert(hero->perk_text[0] != '\0');
        assert(hero->perk <= HERO_PERK_SHIELD);
    }
    assert(strcmp(campaign_hero(0)->name, "赛罗奥特曼") == 0);
    assert(strcmp(campaign_hero(4)->name, "迪迦奥特曼") == 0);
    assert(strcmp(campaign_hero(5)->name, "欧布奥特曼·暗耀") == 0);
    assert(strcmp(campaign_hero(5)->sprite_prefix, "dark") == 0);
    assert(campaign_hero(5)->perk == HERO_PERK_CRIT_DAMAGE);
    assert(strcmp(campaign_hero(6)->name, "捷德奥特曼·尊皇") == 0);
    assert(strcmp(campaign_hero(6)->sprite_prefix, "royal") == 0);
    assert(campaign_hero(6)->perk == HERO_PERK_SHIELD);

    for (uint8_t i = 0; i < CAMPAIGN_STAGE_COUNT; i++) {
        const stage_def_t *stage = campaign_stage(i);
        assert(stage->name[0] != '\0');
        assert(stage->monster_name[0] != '\0');
        assert(stage->math_stage >= 1 && stage->math_stage <= 4);
        assert(stage->question_ms >= 4000 && stage->question_ms <= 9000);
        assert(stage->monster_hp >= 2);
    }
    assert(campaign_stage(0)->boss == false);
    assert(campaign_stage(CAMPAIGN_STAGE_COUNT - 1)->boss == true);

    for (uint8_t i = 0; i < CAMPAIGN_DIFFICULTY_COUNT; i++) {
        assert(campaign_difficulty(i)->name[0] != '\0');
    }
}

static void test_clamps(void)
{
    assert(campaign_question_ms(0, 0, 0) == 9500);
    assert(campaign_question_ms(0, 0, 2) == 10300);
    assert(campaign_question_ms(7, 2, 0) == 4200);
    assert(campaign_question_ms(0, 2, 0) == 7000);

    assert(campaign_hero_hp(0) == 5);
    assert(campaign_hero_hp(1) == 4);
    assert(campaign_hero_hp(2) == 3);

    assert(campaign_monster_hp(0, 0) == 3);
    assert(campaign_monster_hp(0, 2) == 5);
    assert(campaign_monster_hp(0, 9) == 3);
    assert(campaign_monster_hp(99, 0) >= 2);

    assert(campaign_crit_window(1, 0) == 40);
    assert(campaign_crit_window(1, 3) == 52);
    assert(campaign_crit_window(0, 3) == 65);
    assert(campaign_crit_window(2, 3) == 41);
    assert(campaign_crit_window(2, 9) == 32);
}

static void test_out_of_range(void)
{
    assert(campaign_hero(99) == campaign_hero(0));
    assert(campaign_stage(99) == campaign_stage(0));
    assert(campaign_difficulty(99) == campaign_difficulty(0));
}

int main(void)
{
    test_content();
    test_clamps();
    test_out_of_range();
    puts("test_campaign: PASS");
    return 0;
}

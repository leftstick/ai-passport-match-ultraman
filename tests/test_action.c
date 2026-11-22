// Host tests for the generated action configuration tables.
#include "action_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_counts(void)
{
    assert(action_hero_count() == 18);
    assert(action_monster_count() == 11);
}

static void test_every_binding(void)
{
    // 每个角色都声明了全部 10 个动作; 配置总数 = (18 + 11) * 10。
    uint32_t bindings = 0;
    for (uint8_t i = 0; i < action_hero_count(); i++) {
        for (int a = 0; a < ACTION_COUNT; a++) {
            assert(action_supported(ACTION_CHARACTER_HERO, i, (action_id_t)a));
            const action_seq_t *seq =
                action_sequence(ACTION_CHARACTER_HERO, i, (action_id_t)a);
            assert(seq != NULL);
            assert(seq->count > 0);
            assert(seq->total_ms > 0);
            bindings++;
        }
    }
    for (uint8_t i = 0; i < action_monster_count(); i++) {
        for (int a = 0; a < ACTION_COUNT; a++) {
            assert(action_supported(ACTION_CHARACTER_MONSTER, i,
                                    (action_id_t)a));
            const action_seq_t *seq =
                action_sequence(ACTION_CHARACTER_MONSTER, i, (action_id_t)a);
            assert(seq != NULL);
            assert(seq->count > 0);
            bindings++;
        }
    }
    assert(bindings == 290);
}

static void test_unlock_cycles(void)
{
    // 第 1 周目: 待机/准备/必杀/受击/彩蛋。
    assert(action_is_unlocked(ACTION_IDLE, 1));
    assert(action_is_unlocked(ACTION_READY, 1));
    assert(action_is_unlocked(ACTION_ULTIMATE, 1));
    assert(action_is_unlocked(ACTION_HURT, 1));
    assert(action_is_unlocked(ACTION_EASTER_EGG, 1));
    assert(!action_is_unlocked(ACTION_ANSWER_CORRECT, 1));
    assert(!action_is_unlocked(ACTION_ANSWER_WRONG, 1));
    assert(!action_is_unlocked(ACTION_COMBO, 1));
    assert(!action_is_unlocked(ACTION_VICTORY, 1));

    // 第 2 周目: 答对/答错/挑衅。
    assert(action_unlock_cycle(ACTION_ANSWER_CORRECT) == 2);
    assert(action_unlock_cycle(ACTION_ANSWER_WRONG) == 2);
    assert(action_unlock_cycle(ACTION_TAUNT) == 2);
    assert(action_is_unlocked(ACTION_TAUNT, 2));

    // 第 3 周目: 连击。
    assert(action_unlock_cycle(ACTION_COMBO) == 3);
    assert(!action_is_unlocked(ACTION_COMBO, 2));
    assert(action_is_unlocked(ACTION_COMBO, 3));

    // 第 4 周目: 胜利。
    assert(action_unlock_cycle(ACTION_VICTORY) == 4);
    assert(!action_is_unlocked(ACTION_VICTORY, 3));
    assert(action_is_unlocked(ACTION_VICTORY, 4));

    // cycle 0 视为第 1 周目; 非法动作不解锁。
    assert(action_is_unlocked(ACTION_IDLE, 0));
    assert(!action_is_unlocked(ACTION_COUNT, 99));
}

static void test_preview_order(void)
{
    // 仅看已解锁动作; 顺序固定, 便于图鉴逐帧预览。
    assert(action_preview_count(ACTION_CHARACTER_HERO, 0, 1) == 5);
    assert(action_preview_count(ACTION_CHARACTER_HERO, 0, 2) == 8);
    assert(action_preview_count(ACTION_CHARACTER_HERO, 0, 3) == 9);
    assert(action_preview_count(ACTION_CHARACTER_HERO, 0, 4) == 10);
    assert(action_preview_count(ACTION_CHARACTER_HERO, 17, 4) == 10);
    assert(action_preview_count(ACTION_CHARACTER_MONSTER, 10, 4) == 10);

    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 1, 0) == ACTION_IDLE);
    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 1, 1) == ACTION_READY);
    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 1, 2) == ACTION_ULTIMATE);
    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 1, 3) == ACTION_HURT);
    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 1, 4) ==
           ACTION_EASTER_EGG);

    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 2, 2) ==
           ACTION_ANSWER_CORRECT);
    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 3, 4) == ACTION_COMBO);
    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 4, 6) == ACTION_VICTORY);

    // 越界 ordinal 回落到待机, 不会返回脏枚举。
    assert(action_preview_at(ACTION_CHARACTER_HERO, 0, 4, 99) == ACTION_IDLE);
}

static void test_idle_fallback(void)
{
    const action_seq_t *idle =
        action_sequence(ACTION_CHARACTER_HERO, 0, ACTION_IDLE);
    assert(idle != NULL);

    // 非法动作与非法角色下标都回落到待机序列, 不返回 NULL。
    assert(action_sequence(ACTION_CHARACTER_HERO, 0, ACTION_COUNT) == idle);
    assert(action_sequence(ACTION_CHARACTER_HERO, 200, ACTION_IDLE) == idle);
    assert(action_sequence(ACTION_CHARACTER_MONSTER, 200, ACTION_IDLE) != NULL);
    assert(!action_supported(ACTION_CHARACTER_HERO, 200, ACTION_IDLE));
    assert(!action_supported(ACTION_CHARACTER_HERO, 0, ACTION_COUNT));
    assert(strcmp(action_label(ACTION_COUNT), "") == 0);
}

static void test_timing(void)
{
    // 反馈动作 0.4~0.8s, 必杀 0.7~1.2s。
    const action_seq_t *hero_ok =
        action_sequence(ACTION_CHARACTER_HERO, 0, ACTION_ANSWER_CORRECT);
    const action_seq_t *hero_bad =
        action_sequence(ACTION_CHARACTER_HERO, 0, ACTION_ANSWER_WRONG);
    const action_seq_t *mon_ok =
        action_sequence(ACTION_CHARACTER_MONSTER, 0, ACTION_ANSWER_CORRECT);
    const action_seq_t *mon_bad =
        action_sequence(ACTION_CHARACTER_MONSTER, 0, ACTION_ANSWER_WRONG);
    const action_seq_t *ult =
        action_sequence(ACTION_CHARACTER_HERO, 0, ACTION_ULTIMATE);
    assert(hero_ok->total_ms >= 400 && hero_ok->total_ms <= 800);
    assert(hero_bad->total_ms >= 400 && hero_bad->total_ms <= 800);
    assert(mon_ok->total_ms >= 400 && mon_ok->total_ms <= 800);
    assert(mon_bad->total_ms >= 400 && mon_bad->total_ms <= 800);
    assert(ult->total_ms >= 700 && ult->total_ms <= 1200);

    // 小彩蛋间隔 8~12s, 周目 5 起概率提升。
    assert(action_easter_egg_min_ms() == 8000);
    assert(action_easter_egg_max_ms() == 12000);
    assert(action_easter_egg_percent(1) == 6);
    assert(action_easter_egg_percent(4) == 6);
    assert(action_easter_egg_percent(5) == 18);
    assert(action_easter_egg_percent(20) == 18);
}

static void test_labels(void)
{
    for (int a = 0; a < ACTION_COUNT; a++) {
        assert(strlen(action_label((action_id_t)a)) > 0);
    }
}

int main(void)
{
    test_counts();
    test_every_binding();
    test_unlock_cycles();
    test_preview_order();
    test_idle_fallback();
    test_timing();
    test_labels();
    puts("test_action: PASS");
    return 0;
}

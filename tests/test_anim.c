// Host tests for the presentation timeline.
#include "anim.h"

#include <assert.h>
#include <stdio.h>

#define HERO_ATTACK_MS 520u
#define ENEMY_ATTACK_MS 760u
#define ULTIMATE_MS 1600u

static void test_idle_is_neutral(void)
{
    const anim_frame_t f = anim_sample(ANIM_IDLE, 0, 1000);
    assert(f.hero_dx == 0);
    assert(f.monster_dx == 0);
    assert(f.hero_flash == 0);
    assert(f.monster_flash == 0);
    assert(f.beam == 0);
    assert(f.hero_pose == ANIM_POSE_FIGHT);
    assert(f.monster_pose == ANIM_MONSTER_IDLE);
}

static void test_hero_attack_impact_alignment(void)
{
    const uint32_t impact = anim_impact_ms(ANIM_HERO_ATTACK, HERO_ATTACK_MS);
    assert(impact == 229u);

    const anim_frame_t windup = anim_sample(ANIM_HERO_ATTACK, 78, HERO_ATTACK_MS);
    assert(windup.hero_pose == ANIM_POSE_FIGHT);
    assert(windup.hero_dx == -7);
    assert(windup.monster_flash == 0);

    const anim_frame_t fly = anim_sample(ANIM_HERO_ATTACK, 180, HERO_ATTACK_MS);
    assert(fly.hero_pose == ANIM_POSE_FLY);
    assert(fly.hero_dx > 0 && fly.hero_dx < 68);
    assert(fly.monster_flash == 0);

    const anim_frame_t hit = anim_sample(ANIM_HERO_ATTACK, impact, HERO_ATTACK_MS);
    assert(hit.hero_pose == ANIM_POSE_KICK);
    assert(hit.hero_dx == 68);
    assert(hit.monster_pose == ANIM_MONSTER_HURT);
    assert(hit.monster_dx == 8);
    assert(hit.monster_flash == 100);
    assert(hit.hero_flash == 0);
    assert(hit.shake == 5);

    // Six frames of hit-stop keep the pose pinned on the target.
    const anim_frame_t hold = anim_sample(ANIM_HERO_ATTACK, impact + 50,
                                          HERO_ATTACK_MS);
    assert(hold.hero_pose == ANIM_POSE_KICK);
    assert(hold.hero_dx == 68);

    const anim_frame_t last = anim_sample(ANIM_HERO_ATTACK, HERO_ATTACK_MS,
                                          HERO_ATTACK_MS);
    assert(last.hero_pose == ANIM_POSE_FIGHT);
    assert(last.hero_dx == 0);
    assert(last.monster_flash == 0);
    assert(last.shake == 0);
}

static void test_enemy_attack_only_hurts_hero_on_impact(void)
{
    const uint32_t impact = anim_impact_ms(ANIM_ENEMY_ATTACK, ENEMY_ATTACK_MS);
    assert(impact == 361u);

    const anim_frame_t roar = anim_sample(ANIM_ENEMY_ATTACK, 200, ENEMY_ATTACK_MS);
    assert(roar.monster_pose == ANIM_MONSTER_ROAR);
    assert(roar.hero_pose == ANIM_POSE_FIGHT);
    assert(roar.hero_flash == 0);

    const anim_frame_t hit = anim_sample(ANIM_ENEMY_ATTACK, impact, ENEMY_ATTACK_MS);
    assert(hit.monster_pose == ANIM_MONSTER_ATTACK);
    assert(hit.monster_dx == -68);
    assert(hit.hero_pose == ANIM_POSE_GUARD);
    assert(hit.hero_dx == -8);
    assert(hit.hero_flash == 100);
    assert(hit.monster_flash == 0);
    assert(hit.shake == 5);
}

static void test_ultimate_charges_then_fires(void)
{
    const uint32_t impact = anim_impact_ms(ANIM_ULTIMATE, ULTIMATE_MS);
    assert(impact == 704u);

    const anim_frame_t charge = anim_sample(ANIM_ULTIMATE, 400, ULTIMATE_MS);
    assert(charge.hero_pose == ANIM_POSE_CHARGE);
    assert(charge.beam == 0);
    assert(charge.monster_flash == 0);

    const anim_frame_t fire = anim_sample(ANIM_ULTIMATE, impact, ULTIMATE_MS);
    assert(fire.hero_pose == ANIM_POSE_BEAM);
    assert(fire.beam == 100);
    assert(fire.monster_pose == ANIM_MONSTER_HURT);
    assert(fire.monster_flash == 100);
    assert(fire.shake == 5);

    const anim_frame_t fade = anim_sample(ANIM_ULTIMATE, ULTIMATE_MS, ULTIMATE_MS);
    assert(fade.beam == 0);
    assert(fade.hero_pose == ANIM_POSE_FIGHT);
}

static void test_variant_attacks_share_impact_frame(void)
{
    // Variants change the silhouette, never the moment damage lands.
    assert(anim_impact_ms(ANIM_HERO_ATTACK_PUNCH, HERO_ATTACK_MS) == 229u);
    assert(anim_impact_ms(ANIM_HERO_ATTACK_HEAVY, HERO_ATTACK_MS) == 229u);
    assert(anim_impact_ms(ANIM_HERO_ATTACK_ARC, HERO_ATTACK_MS) == 229u);
    assert(anim_impact_ms(ANIM_ENEMY_ATTACK_CHARGE, ENEMY_ATTACK_MS) == 361u);

    const anim_frame_t punch =
        anim_sample(ANIM_HERO_ATTACK_PUNCH, 229, HERO_ATTACK_MS);
    assert(punch.hero_pose == ANIM_POSE_FLY);
    assert(punch.hero_dx == 72);
    assert(punch.hero_dy == -6);
    assert(punch.monster_pose == ANIM_MONSTER_HURT);
    assert(punch.monster_dx == 8);
    assert(punch.monster_flash == 100);
    assert(punch.shake == 5);

    const anim_frame_t gather =
        anim_sample(ANIM_HERO_ATTACK_HEAVY, 150, HERO_ATTACK_MS);
    assert(gather.hero_pose == ANIM_POSE_CHARGE);

    const anim_frame_t heavy =
        anim_sample(ANIM_HERO_ATTACK_HEAVY, 229, HERO_ATTACK_MS);
    assert(heavy.hero_pose == ANIM_POSE_KICK);
    assert(heavy.hero_dx == 78);
    assert(heavy.monster_pose == ANIM_MONSTER_HURT);
    assert(heavy.monster_dx == 12);
    assert(heavy.monster_flash == 100);

    const anim_frame_t arc =
        anim_sample(ANIM_HERO_ATTACK_ARC, 229, HERO_ATTACK_MS);
    assert(arc.hero_pose == ANIM_POSE_KICK);
    assert(arc.hero_dx == 58);
    assert(arc.hero_dy == -16);
    assert(arc.monster_pose == ANIM_MONSTER_HURT);
    assert(arc.monster_dx == 10);

    const anim_frame_t roar =
        anim_sample(ANIM_ENEMY_ATTACK_CHARGE, 200, ENEMY_ATTACK_MS);
    assert(roar.monster_pose == ANIM_MONSTER_CHARGE);
    assert(roar.hero_flash == 0);

    const anim_frame_t maul =
        anim_sample(ANIM_ENEMY_ATTACK_CHARGE, 361, ENEMY_ATTACK_MS);
    assert(maul.monster_pose == ANIM_MONSTER_ATTACK);
    assert(maul.monster_dx == -68);
    assert(maul.hero_pose == ANIM_POSE_GUARD);
    assert(maul.hero_dx == -8);
    assert(maul.hero_flash == 100);
    assert(maul.shake == 5);
}

static void test_hero_strikes_rotate_every_hit(void)
{
    // Regression: perfect criticals used to force one fixed action, so a
    // player who always hit the centre saw nine identical strikes.
    const anim_id_t first = anim_hero_strike(0, false, false);
    bool seen[ANIM_COUNT] = {false};
    for (uint8_t i = 0; i < 9; i++) {
        const anim_id_t id = anim_hero_strike(i, false, false);
        seen[id] = true;
        if (i > 0) {
            assert(id != anim_hero_strike((uint8_t)(i - 1), false, false));
        }
    }
    assert(seen[ANIM_HERO_ATTACK]);
    assert(seen[ANIM_HERO_ATTACK_PUNCH]);
    assert(seen[ANIM_HERO_ATTACK_HEAVY]);
    assert(seen[ANIM_HERO_ATTACK_ARC]);

    // The sequence wraps, so long runs keep cycling instead of sticking.
    assert(anim_hero_strike(4, false, false) == first);

    // Combo rewards keep their dedicated moves.
    assert(anim_hero_strike(5, true, false) == ANIM_COMBO_FINISH);
    assert(anim_hero_strike(5, false, true) == ANIM_HERO_PURSUIT);
    assert(anim_hero_strike(5, true, true) == ANIM_COMBO_FINISH);

    assert(anim_enemy_strike(0) != anim_enemy_strike(1));
    assert(anim_enemy_strike(0) == anim_enemy_strike(2));
}

static void test_beam_and_terminal_actions(void)
{
    assert(anim_impact_ms(ANIM_BEAM, 3000) == 0);
    assert(anim_impact_ms(ANIM_CLEAR, 1200) == 0);
    assert(anim_impact_ms(ANIM_OVER, 1200) == 0);

    const anim_frame_t beam = anim_sample(ANIM_BEAM, 400, 3000);
    assert(beam.hero_pose == ANIM_POSE_BEAM);
    assert(beam.monster_pose == ANIM_MONSTER_CHARGE);
    assert(beam.beam == 100);

    const anim_frame_t clear = anim_sample(ANIM_CLEAR, 200, 1200);
    assert(clear.hero_pose == ANIM_POSE_VICTORY);

    const anim_frame_t over = anim_sample(ANIM_OVER, 500, 1200);
    assert(over.hero_pose == ANIM_POSE_IDLE);
    assert(over.hero_dy == 8);
    assert(over.monster_pose == ANIM_MONSTER_ROAR);
}

static void test_elapsed_is_clamped(void)
{
    const anim_frame_t f = anim_sample(ANIM_HERO_ATTACK, 99999, HERO_ATTACK_MS);
    assert(f.hero_dx == 0);
    assert(f.monster_dx == 0);
    assert(f.monster_flash == 0);
    assert(f.shake == 0);
}

static void test_sample_stays_in_range(void)
{
    for (uint32_t ms = 0; ms <= 2000; ms += 13) {
        const anim_frame_t f = anim_sample(ANIM_ENEMY_ATTACK, ms, ENEMY_ATTACK_MS);
        assert(f.hero_flash <= 100);
        assert(f.monster_flash <= 100);
        assert(f.beam <= 100);
        assert(f.shake <= 5);
        assert(f.hero_dx >= -16 && f.hero_dx <= 72);
        assert(f.monster_dx >= -72 && f.monster_dx <= 16);
    }
}

static void test_ko_sequence_dissolves_monster(void)
{
    // 普通怪: 光线先亮, 怪兽闪白抖动, 后半段才淡出。
    const anim_frame_t start = anim_sample(ANIM_KO, 0, 900);
    assert(start.monster_fade == 0);
    assert(start.beam == 0);

    const anim_frame_t beam = anim_sample(ANIM_KO, 72, 900);
    assert(beam.beam == 100);
    assert(beam.monster_fade == 0);

    const anim_frame_t flash = anim_sample(ANIM_KO, 207, 900);
    assert(flash.monster_flash == 100);
    assert(flash.monster_fade == 0);

    const anim_frame_t dissolve = anim_sample(ANIM_KO, 450, 900);
    assert(dissolve.monster_fade > 0);
    assert(dissolve.monster_fade < 100);

    const anim_frame_t end = anim_sample(ANIM_KO, 900, 900);
    assert(end.monster_fade == 100);
    assert(end.beam == 0);

    // BOSS: 更长、更重, 同样以完全淡出收尾。
    const anim_frame_t boss_mid = anim_sample(ANIM_BOSS_KO, 600, 1200);
    assert(boss_mid.monster_fade > 0 && boss_mid.monster_fade < 100);
    assert(boss_mid.monster_flash <= 100);
    const anim_frame_t boss_end = anim_sample(ANIM_BOSS_KO, 1200, 1200);
    assert(boss_end.monster_fade == 100);

    // 惨叫卡在最后一帧附近。
    assert(anim_ko_cry_ms(ANIM_KO, 900) == 675);
    assert(anim_ko_cry_ms(ANIM_BOSS_KO, 1200) == 960);
    assert(anim_ko_cry_ms(ANIM_IDLE, 900) == 0);
}

static void test_breathe_triangle(void)
{
    assert(anim_breathe(0, 0, 2000) == 0);
    assert(anim_breathe(250, 0, 2000) == 0);
    assert(anim_breathe(500, 0, 2000) == 1);
    assert(anim_breathe(999, 0, 2000) == 1);
    assert(anim_breathe(1000, 0, 2000) == 2);
    assert(anim_breathe(1500, 0, 2000) == 1);
    assert(anim_breathe(1999, 0, 2000) == 0);

    // Phase offset desynchronises the two actors.
    assert(anim_breathe(0, 1000, 2000) == 2);
    assert(anim_breathe(0, 0, 2) == 0);
}

int main(void)
{
    test_idle_is_neutral();
    test_hero_attack_impact_alignment();
    test_enemy_attack_only_hurts_hero_on_impact();
    test_ultimate_charges_then_fires();
    test_variant_attacks_share_impact_frame();
    test_hero_strikes_rotate_every_hit();
    test_beam_and_terminal_actions();
    test_elapsed_is_clamped();
    test_sample_stays_in_range();
    test_ko_sequence_dissolves_monster();
    test_breathe_triangle();
    puts("test_anim: PASS");
    return 0;
}

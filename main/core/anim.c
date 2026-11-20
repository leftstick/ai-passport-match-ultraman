#include "anim.h"

#include <stddef.h>

// Keyframes use a normalised 0..1000 timeline so phase durations can be tuned
// without rewriting the tables. Discrete fields (poses) take the value of the
// segment's starting keyframe; continuous fields are interpolated.
typedef struct {
    uint16_t t;
    int8_t hero_dx;
    int8_t hero_dy;
    int8_t monster_dx;
    int8_t monster_dy;
    uint8_t hero_pose;
    uint8_t monster_pose;
    uint8_t hero_flash;
    uint8_t monster_flash;
    uint8_t beam;
    uint8_t shake;
} anim_key_t;

static const anim_key_t IDLE_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Windup -> fly -> kick at the impact frame, then a short hit-stop and a
// recoil. The impact frame is 440/1000 so it lands 8 px from the monster.
static const anim_key_t HERO_ATTACK_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {150, -7, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {240, 0, 0, 0, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {439, 68, 0, 8, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {440, 68, 0, 8, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 100, 0, 5},
    {555, 68, 0, 8, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 60, 0, 3},
    {750, 18, 0, 2, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Flying punch: a quick hop with a vertical offset, so the silhouette differs
// from the flat flying kick even before the impact pose. The impact frame
// stays at 440/1000 so damage pops on contact.
static const anim_key_t HERO_ATTACK_PUNCH_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {90, -4, -4, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {170, 0, 0, 0, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {439, 72, -6, 8, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {440, 72, -6, 8, 0, ANIM_POSE_FLY, ANIM_MONSTER_HURT, 0, 100, 0, 5},
    {560, 72, -6, 8, 0, ANIM_POSE_FLY, ANIM_MONSTER_HURT, 0, 60, 0, 3},
    {760, 16, 0, 2, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Perfect-timing strike: gather energy, then a longer, heavier flying kick.
// It shares the impact frame with the ordinary attack so the damage number
// still lands on contact.
static const anim_key_t HERO_ATTACK_HEAVY_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {140, -10, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {260, -4, 0, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {360, 0, 0, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {400, 20, 0, 0, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {439, 78, 0, 12, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {440, 78, 0, 12, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 100, 0, 5},
    {590, 78, 0, 12, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 80, 0, 4},
    {800, 24, 0, 4, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_HURT, 0, 30, 0, 2},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Rising arc kick: the hero leaps higher than the punch, then lands a kick
// from above. The larger vertical travel makes it a third recognisable beat.
static const anim_key_t HERO_ATTACK_ARC_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {120, -10, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {200, 0, 0, 0, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {320, 20, -8, 0, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {439, 58, -16, 10, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {440, 58, -16, 10, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 100, 0, 5},
    {570, 58, -16, 10, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 80, 0, 4},
    {780, 20, -4, 3, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_HURT, 0, 20, 0, 1},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Roar -> pounce. The hero only guards and flashes on the impact frame.
static const anim_key_t ENEMY_ATTACK_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {220, 0, 0, -8, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
    {320, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_ATTACK, 0, 0, 0, 0},
    {474, -8, 0, -68, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_ATTACK, 0, 0, 0, 0},
    {475, -8, 0, -68, 0, ANIM_POSE_GUARD, ANIM_MONSTER_ATTACK, 100, 0, 0, 5},
    {555, -8, 0, -68, 0, ANIM_POSE_GUARD, ANIM_MONSTER_ATTACK, 70, 0, 0, 3},
    {760, 0, 0, -10, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Alternate enemy attack: the monster gathers itself before the pounce. The
// hit still lands on the same 475/1000 frame.
static const anim_key_t ENEMY_ATTACK_CHARGE_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {200, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_CHARGE, 0, 0, 0, 0},
    {320, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_CHARGE, 0, 0, 0, 0},
    {400, 0, 0, -20, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_ATTACK, 0, 0, 0, 0},
    {474, -8, 0, -68, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_ATTACK, 0, 0, 0, 0},
    {475, -8, 0, -68, 0, ANIM_POSE_GUARD, ANIM_MONSTER_ATTACK, 100, 0, 0, 5},
    {555, -8, 0, -68, 0, ANIM_POSE_GUARD, ANIM_MONSTER_ATTACK, 70, 0, 0, 3},
    {760, 0, 0, -10, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Charge, snap into the beam pose, then fire at the impact frame and hold the
// beam long enough to read before fading out.
static const anim_key_t ULTIMATE_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {280, 0, -6, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
    {330, 0, -4, 0, 0, ANIM_POSE_BEAM, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
    {439, 10, -4, 12, 0, ANIM_POSE_BEAM, ANIM_MONSTER_ROAR, 0, 0, 99, 0},
    {440, 10, -4, 12, 0, ANIM_POSE_BEAM, ANIM_MONSTER_HURT, 0, 100, 100, 5},
    {560, 10, -4, 12, 0, ANIM_POSE_BEAM, ANIM_MONSTER_HURT, 0, 100, 100, 3},
    {750, 6, -2, 6, 0, ANIM_POSE_BEAM, ANIM_MONSTER_HURT, 0, 50, 100, 0},
    {880, 0, 0, 2, 0, ANIM_POSE_BEAM, ANIM_MONSTER_IDLE, 0, 0, 70, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Boss struggle: the beam is driven by the core force value, this table only
// keeps both actors in the right poses and ramps the visual in.
static const anim_key_t BEAM_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_BEAM, ANIM_MONSTER_CHARGE, 0, 0, 20, 0},
    {120, 0, 0, 0, 0, ANIM_POSE_BEAM, ANIM_MONSTER_CHARGE, 0, 0, 100, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_BEAM, ANIM_MONSTER_CHARGE, 0, 0, 100, 0},
};

static const anim_key_t CLEAR_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_VICTORY, ANIM_MONSTER_HURT, 0, 60, 0, 0},
    {160, 0, -6, 0, 0, ANIM_POSE_VICTORY, ANIM_MONSTER_HURT, 0, 30, 0, 0},
    {320, 0, 0, 0, 0, ANIM_POSE_VICTORY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {480, 0, -4, 0, 0, ANIM_POSE_VICTORY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {640, 0, 0, 0, 0, ANIM_POSE_VICTORY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_VICTORY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

static const anim_key_t OVER_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_GUARD, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
    {220, 0, 8, 0, 0, ANIM_POSE_IDLE, ANIM_MONSTER_ROAR, 30, 0, 0, 2},
    {400, 0, 8, 0, 0, ANIM_POSE_IDLE, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
    {1000, 0, 8, 0, 0, ANIM_POSE_IDLE, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
};

// Combo pursuit: the hero chases the knocked-back monster for a second strike.
static const anim_key_t HERO_PURSUIT_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {120, -10, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {220, 0, 0, 0, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {439, 76, 0, 8, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {440, 76, 0, 8, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 100, 0, 5},
    {555, 76, 0, 8, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 70, 0, 4},
    {700, 46, 0, 4, 0, ANIM_POSE_FLY, ANIM_MONSTER_HURT, 0, 30, 0, 1},
    {860, 10, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Combo finisher: deeper windup before the same impact frame, with a bigger
// recoil so the third hit reads as the end of a sentence.
static const anim_key_t COMBO_FINISH_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {180, -12, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {300, 0, 0, 0, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {439, 82, 0, 10, 0, ANIM_POSE_FLY, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {440, 82, 0, 10, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 100, 0, 5},
    {580, 82, 0, 10, 0, ANIM_POSE_KICK, ANIM_MONSTER_HURT, 0, 80, 0, 5},
    {760, 30, 0, 4, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_HURT, 0, 30, 0, 2},
    {900, 6, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

// Interactive ultimate, part one: hold the charge while the beat marker runs.
static const anim_key_t ULTIMATE_CHARGE_KEYS[] = {
    {0, 0, 0, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
    {180, 0, 0, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
    {420, 0, -4, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_ROAR, 0, 0, 0, 0},
    {760, 0, -7, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_CHARGE, 0, 0, 0, 0},
    {1000, 0, -7, 0, 0, ANIM_POSE_CHARGE, ANIM_MONSTER_CHARGE, 0, 0, 0, 0},
};

// Interactive ultimate, part two: the tail after the final beat.
static const anim_key_t ULTIMATE_FIRE_KEYS[] = {
    {0, 0, -4, 0, 0, ANIM_POSE_BEAM, ANIM_MONSTER_HURT, 0, 100, 100, 3},
    {180, 0, -4, 8, 0, ANIM_POSE_BEAM, ANIM_MONSTER_HURT, 0, 80, 100, 5},
    {420, 0, -2, 4, 0, ANIM_POSE_BEAM, ANIM_MONSTER_HURT, 0, 40, 100, 2},
    {760, 0, 0, 0, 0, ANIM_POSE_BEAM, ANIM_MONSTER_IDLE, 0, 0, 60, 0},
    {1000, 0, 0, 0, 0, ANIM_POSE_FIGHT, ANIM_MONSTER_IDLE, 0, 0, 0, 0},
};

#define KEY_COUNT(t) (sizeof(t) / sizeof((t)[0]))

typedef struct {
    const anim_key_t *keys;
    uint8_t count;
    uint16_t impact_permille;
} anim_def_t;

static const anim_def_t DEFS[ANIM_COUNT] = {
    [ANIM_IDLE] = {IDLE_KEYS, (uint8_t)KEY_COUNT(IDLE_KEYS), 0},
    [ANIM_HERO_ATTACK] = {HERO_ATTACK_KEYS, (uint8_t)KEY_COUNT(HERO_ATTACK_KEYS), 440},
    [ANIM_ENEMY_ATTACK] = {ENEMY_ATTACK_KEYS, (uint8_t)KEY_COUNT(ENEMY_ATTACK_KEYS), 475},
    [ANIM_ULTIMATE] = {ULTIMATE_KEYS, (uint8_t)KEY_COUNT(ULTIMATE_KEYS), 440},
    [ANIM_BEAM] = {BEAM_KEYS, (uint8_t)KEY_COUNT(BEAM_KEYS), 0},
    [ANIM_CLEAR] = {CLEAR_KEYS, (uint8_t)KEY_COUNT(CLEAR_KEYS), 0},
    [ANIM_OVER] = {OVER_KEYS, (uint8_t)KEY_COUNT(OVER_KEYS), 0},
    [ANIM_HERO_PURSUIT] = {HERO_PURSUIT_KEYS,
                           (uint8_t)KEY_COUNT(HERO_PURSUIT_KEYS), 440},
    [ANIM_COMBO_FINISH] = {COMBO_FINISH_KEYS,
                           (uint8_t)KEY_COUNT(COMBO_FINISH_KEYS), 440},
    [ANIM_ULTIMATE_CHARGE] = {ULTIMATE_CHARGE_KEYS,
                              (uint8_t)KEY_COUNT(ULTIMATE_CHARGE_KEYS), 0},
    [ANIM_ULTIMATE_FIRE] = {ULTIMATE_FIRE_KEYS,
                            (uint8_t)KEY_COUNT(ULTIMATE_FIRE_KEYS), 0},
    [ANIM_HERO_ATTACK_PUNCH] = {HERO_ATTACK_PUNCH_KEYS,
                                (uint8_t)KEY_COUNT(HERO_ATTACK_PUNCH_KEYS), 440},
    [ANIM_HERO_ATTACK_HEAVY] = {HERO_ATTACK_HEAVY_KEYS,
                                (uint8_t)KEY_COUNT(HERO_ATTACK_HEAVY_KEYS), 440},
    [ANIM_HERO_ATTACK_ARC] = {HERO_ATTACK_ARC_KEYS,
                              (uint8_t)KEY_COUNT(HERO_ATTACK_ARC_KEYS), 440},
    [ANIM_ENEMY_ATTACK_CHARGE] = {ENEMY_ATTACK_CHARGE_KEYS,
                                  (uint8_t)KEY_COUNT(ENEMY_ATTACK_CHARGE_KEYS),
                                  475},
};

// Linear interpolation, used for flash and shake where an immediate onset
// reads better than an ease-in.
static int32_t lerp_i32(int32_t a, int32_t b, uint32_t num, uint32_t den)
{
    if (den == 0) {
        return b;
    }
    return a + (int32_t)(((int64_t)(b - a) * (int64_t)num) / (int64_t)den);
}

// Smoothstep interpolation (3t^2 - 2t^3) for offsets and the beam so the
// windup and recoil do not look mechanical.
static int32_t smooth_i32(int32_t a, int32_t b, uint32_t num, uint32_t den)
{
    if (den == 0) {
        return b;
    }
    const int64_t n = (int64_t)num;
    const int64_t d = (int64_t)den;
    const int64_t w = n * n * (3 * d - 2 * n) / (d * d);  // 0..den
    return a + (int32_t)(((int64_t)(b - a) * w) / d);
}

static uint8_t clamp_u8(int32_t v, int32_t max)
{
    if (v < 0) {
        return 0;
    }
    if (v > max) {
        return (uint8_t)max;
    }
    return (uint8_t)v;
}

anim_frame_t anim_sample(anim_id_t id, uint32_t elapsed_ms, uint32_t duration_ms)
{
    anim_frame_t out = {0};
    if (id < 0 || id >= ANIM_COUNT) {
        return out;
    }
    const anim_def_t *def = &DEFS[id];
    if (def->keys == NULL || def->count < 2) {
        return out;
    }

    uint32_t p = 0;
    if (duration_ms != 0) {
        if (elapsed_ms >= duration_ms) {
            p = 1000;
        } else {
            p = (uint32_t)(((uint64_t)elapsed_ms * 1000u) / duration_ms);
        }
    }

    const anim_key_t *keys = def->keys;
    uint8_t idx = 0;
    // A keyframe takes effect on its own timestamp, so poses snap exactly at
    // the impact frame instead of one segment later.
    while (idx + 1 < def->count && p >= keys[idx + 1].t) {
        idx++;
    }
    if (idx + 1 >= def->count) {
        idx = (uint8_t)(def->count - 1);
    }

    const anim_key_t *a = &keys[idx];
    const anim_key_t *b = (idx + 1 < def->count) ? &keys[idx + 1] : a;
    const uint32_t span = (uint32_t)(b->t - a->t);
    const uint32_t local = (span == 0) ? span : (p - a->t);
    const uint32_t den = (span == 0) ? 1u : span;

    // Discrete fields snap to the segment start.
    out.hero_pose = a->hero_pose;
    out.monster_pose = a->monster_pose;

    out.hero_dx = (int8_t)smooth_i32(a->hero_dx, b->hero_dx, local, den);
    out.hero_dy = (int8_t)smooth_i32(a->hero_dy, b->hero_dy, local, den);
    out.monster_dx = (int8_t)smooth_i32(a->monster_dx, b->monster_dx, local, den);
    out.monster_dy = (int8_t)smooth_i32(a->monster_dy, b->monster_dy, local, den);
    out.beam = clamp_u8(smooth_i32(a->beam, b->beam, local, den), 100);

    // Impact feedback decays linearly so the flash is strongest on frame one.
    out.hero_flash = clamp_u8(lerp_i32(a->hero_flash, b->hero_flash, local, den), 100);
    out.monster_flash =
        clamp_u8(lerp_i32(a->monster_flash, b->monster_flash, local, den), 100);
    out.shake = clamp_u8(lerp_i32(a->shake, b->shake, local, den), 5);

    return out;
}

uint32_t anim_impact_ms(anim_id_t id, uint32_t duration_ms)
{
    if (id < 0 || id >= ANIM_COUNT) {
        return 0;
    }
    const anim_def_t *def = &DEFS[id];
    if (def->impact_permille == 0) {
        return 0;
    }
    // Round up so the sampled frame at this timestamp has already reached the
    // impact keyframe.
    return (uint32_t)(((uint64_t)duration_ms * def->impact_permille + 999u) / 1000u);
}

anim_id_t anim_hero_strike(uint8_t sequence, bool finisher, bool pursuit)
{
    if (finisher) {
        return ANIM_COMBO_FINISH;
    }
    if (pursuit) {
        return ANIM_HERO_PURSUIT;
    }
    static const anim_id_t ROTATION[] = {
        ANIM_HERO_ATTACK,
        ANIM_HERO_ATTACK_PUNCH,
        ANIM_HERO_ATTACK_HEAVY,
        ANIM_HERO_ATTACK_ARC,
    };
    const uint8_t count = (uint8_t)(sizeof(ROTATION) / sizeof(ROTATION[0]));
    return ROTATION[sequence % count];
}

anim_id_t anim_enemy_strike(uint8_t sequence)
{
    return (sequence & 1u) != 0 ? ANIM_ENEMY_ATTACK_CHARGE : ANIM_ENEMY_ATTACK;
}

int anim_breathe(uint32_t time_ms, uint32_t phase_ms, uint32_t period_ms)
{
    if (period_ms < 4) {
        return 0;
    }
    const uint32_t half = period_ms / 2u;
    const uint32_t t = (time_ms + phase_ms) % period_ms;
    uint32_t v;
    if (t < half) {
        v = (t * 2u) / half;
    } else {
        v = ((period_ms - t) * 2u) / half;
    }
    if (v > 2u) {
        v = 2u;
    }
    return (int)v;
}

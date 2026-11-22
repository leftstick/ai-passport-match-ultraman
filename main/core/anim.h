// Presentation timeline for battle actions. Pure C, no LVGL/ESP-IDF.
//
// The state machine owns phase durations; this module maps elapsed time inside
// a phase to a small set of keyframes (pose, offset, flash, beam width, shake).
// Keyframes are normalised to 0..1000 so the same table works when a phase
// duration is tuned. Discrete values (poses) come from the segment's start
// keyframe, continuous values are interpolated.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ANIM_IDLE = 0,
    ANIM_HERO_ATTACK,
    ANIM_ENEMY_ATTACK,
    ANIM_ULTIMATE,
    ANIM_BEAM,
    ANIM_CLEAR,
    ANIM_OVER,
    // Combo variants and the interactive ultimate split.
    ANIM_HERO_PURSUIT,
    ANIM_COMBO_FINISH,
    ANIM_ULTIMATE_CHARGE,
    ANIM_ULTIMATE_FIRE,
    // Ordinary-strike variants so one hero does not repeat a single action.
    ANIM_HERO_ATTACK_PUNCH,
    ANIM_HERO_ATTACK_HEAVY,
    ANIM_HERO_ATTACK_ARC,
    ANIM_ENEMY_ATTACK_CHARGE,
    // KO 演出: 光线扫过 -> 怪兽抖动闪白 -> 碎裂淡出。BOSS 版更长更重。
    ANIM_KO,
    ANIM_BOSS_KO,
    ANIM_COUNT,
} anim_id_t;

// Values intentionally match sprite_pose_t in assets/sprites.h so the app can
// cast between them. Keep both enums in the same order.
typedef enum {
    ANIM_POSE_IDLE = 0,
    ANIM_POSE_FIGHT,
    ANIM_POSE_FLY,
    ANIM_POSE_BEAM,
    ANIM_POSE_CHARGE,
    ANIM_POSE_KICK,
    ANIM_POSE_GUARD,
    ANIM_POSE_VICTORY,
    ANIM_POSE_COUNT,
} anim_hero_pose_t;

// Values intentionally match sprite_monster_pose_t in assets/sprites.h.
typedef enum {
    ANIM_MONSTER_IDLE = 0,
    ANIM_MONSTER_ATTACK,
    ANIM_MONSTER_ROAR,
    ANIM_MONSTER_CHARGE,
    ANIM_MONSTER_HURT,
    ANIM_MONSTER_COUNT,
} anim_monster_pose_t;

typedef struct {
    int8_t hero_dx;
    int8_t hero_dy;
    int8_t monster_dx;
    int8_t monster_dy;
    uint8_t hero_pose;
    uint8_t monster_pose;
    uint8_t hero_flash;     // 0..100, 0 = no flash
    uint8_t monster_flash;  // 0..100
    uint8_t beam;           // 0..100, 0 = beam hidden
    uint8_t shake;          // 0..5 px
    // 0 = fully visible, 100 = fully dissolved. Only the KO timelines raise
    // this; every other action leaves the monster opaque.
    uint8_t monster_fade;
} anim_frame_t;

// Sample an action timeline. elapsed_ms is clamped to duration_ms.
anim_frame_t anim_sample(anim_id_t id, uint32_t elapsed_ms, uint32_t duration_ms);

// Time inside the phase at which the hit lands. Returns 0 for actions without
// a hit frame.
uint32_t anim_impact_ms(anim_id_t id, uint32_t duration_ms);

// KO 惨叫与最后一帧同步: 普通怪 0.75 时长, BOSS 0.8 时长。
uint32_t anim_ko_cry_ms(anim_id_t id, uint32_t duration_ms);

// Pick the next ordinary hero strike. `sequence` advances on every strike so
// the four silhouettes rotate even when the player hits the perfect zone every
// time. Combo rewards keep their dedicated moves and take priority.
anim_id_t anim_hero_strike(uint8_t sequence, bool finisher, bool pursuit);

// Enemy pounces alternate between a direct roar and a charge-up.
anim_id_t anim_enemy_strike(uint8_t sequence);

// Slow two-step breathing value in 0..2. Callers pass a phase offset so the
// hero and monster do not bob in lockstep.
int anim_breathe(uint32_t time_ms, uint32_t phase_ms, uint32_t period_ms);

// Generated PCM sound-effect table (16 kHz, mono, signed 16-bit).
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SFX_MOVE = 0,
    SFX_CONFIRM,
    SFX_CORRECT,
    SFX_WRONG,
    SFX_HINT,
    SFX_HIT,
    SFX_CRITICAL,
    SFX_HURT,
    SFX_CHARGE,
    SFX_BEAM,
    SFX_MONSTER_DOWN,
    SFX_VICTORY,
    SFX_DEFEAT,
    SFX_PAUSE,
    // Procedurally synthesised cues live in sfx_extra.c.
    SFX_SWING,
    SFX_GROWL,
    // Monster defeat cries built from CC0 animal recordings in monster_cries.c.
    SFX_CRY_HEAVY,
    SFX_CRY_WINGED,
    SFX_CRY_MECH,
    SFX_CRY_ALIEN,
    SFX_COUNT,
} sfx_id_t;

typedef struct {
    const int16_t *samples;
    uint32_t count;
} sfx_clip_t;

const sfx_clip_t *sfx_clip(sfx_id_t id);

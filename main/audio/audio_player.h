// Two-voice SFX mixer built on the BSP ES8311/I2S output.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sfx.h"

esp_err_t audio_player_start(void);
void audio_player_play(sfx_id_t id);
void audio_player_set_muted(bool muted);
bool audio_player_is_muted(void);

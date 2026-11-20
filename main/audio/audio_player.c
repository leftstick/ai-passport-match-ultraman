#include "audio_player.h"

#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "monster_cries.h"
#include "sfx_extra.h"

static const char *TAG = "audio";

#define AUDIO_RATE 16000
#define AUDIO_BLOCK_SAMPLES 160  // 10 ms
#define AUDIO_QUEUE_DEPTH 8
#define AUDIO_VOLUME 70
#define AUDIO_VOICE_GAIN 90

typedef struct {
    const sfx_clip_t *clip;
    uint32_t position;
    bool active;
} voice_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_ready;
static volatile bool s_muted;
static voice_t s_voices[2];

static bool is_priority(sfx_id_t id)
{
    return id == SFX_CRITICAL || id == SFX_BEAM || id == SFX_MONSTER_DOWN ||
           id == SFX_VICTORY || id == SFX_DEFEAT || id == SFX_CHARGE ||
           id == SFX_WRONG;
}

static const sfx_clip_t *resolve_clip(sfx_id_t id)
{
    const sfx_clip_t *cry = monster_cry_clip(id);
    if (cry != NULL && cry->count > 0) {
        return cry;
    }
    const sfx_clip_t *extra = sfx_extra_clip(id);
    if (extra != NULL && extra->count > 0) {
        return extra;
    }
    return sfx_clip(id);
}

static void start_voice(sfx_id_t id)
{
    const sfx_clip_t *clip = resolve_clip(id);
    if (clip == NULL || clip->count == 0 || clip->samples == NULL) {
        return;
    }
    const int index = is_priority(id) ? 0 : 1;
    s_voices[index].clip = clip;
    s_voices[index].position = 0;
    s_voices[index].active = true;
}

static void mix_block(int16_t *out)
{
    int32_t mix[AUDIO_BLOCK_SAMPLES];
    memset(mix, 0, sizeof(mix));

    bool any = false;
    for (int v = 0; v < 2; v++) {
        voice_t *voice = &s_voices[v];
        if (!voice->active || voice->clip == NULL) {
            continue;
        }
        any = true;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES && voice->position < voice->clip->count;
             i++, voice->position++) {
            mix[i] += (int32_t)voice->clip->samples[voice->position] * AUDIO_VOICE_GAIN / 100;
        }
        if (voice->position >= voice->clip->count) {
            voice->active = false;
            voice->clip = NULL;
        }
    }

    if (!any) {
        return;
    }

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        int32_t sample = mix[i];
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        out[i] = (int16_t)sample;
    }
    (void)bsp_audio_write(out, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
}

static void audio_task(void *arg)
{
    (void)arg;
    const esp_err_t init_err = bsp_audio_init();
    esp_err_t format_err = ESP_FAIL;
    if (init_err == ESP_OK) {
        format_err = bsp_audio_set_format(AUDIO_RATE, 16, 1);
        if (format_err == ESP_OK) {
            bsp_audio_set_volume(AUDIO_VOLUME);
            s_ready = true;
            ESP_LOGI(TAG, "音频就绪: %d Hz mono", AUDIO_RATE);
        }
    }
    if (!s_ready) {
        ESP_LOGW(TAG, "音频不可用,游戏将静音运行 (init=%s format=%s)",
                 esp_err_to_name(init_err), esp_err_to_name(format_err));
    }

    int16_t block[AUDIO_BLOCK_SAMPLES];
    for (;;) {
        sfx_id_t id;
        if (xQueueReceive(s_queue, &id, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (!s_muted) {
                start_voice(id);
            }
        }
        if (!s_ready || s_muted) {
            continue;
        }
        bool active = s_voices[0].active || s_voices[1].active;
        if (active) {
            mix_block(block);
        }
    }
}

esp_err_t audio_player_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(sfx_id_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "battle_audio", 4096, NULL, 4, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_player_play(sfx_id_t id)
{
    if (id < 0 || id >= SFX_COUNT || s_queue == NULL || s_muted) {
        return;
    }
    (void)xQueueSend(s_queue, &id, 0);
}

void audio_player_set_muted(bool muted)
{
    s_muted = muted;
}

bool audio_player_is_muted(void)
{
    return s_muted;
}

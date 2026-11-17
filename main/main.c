// main/main.c —— 算术超能战启动入口。
//
// 按键语义:
//   首页:上/下切换英雄,长按确定切换难度,确定进入关卡
//   关卡:上/下选关,确定出击,长按确定返回
//   战斗:上/下选答案,确定作答;长按上必杀,长按下提示,长按确定暂停
//   浮层:上/下选择,确定执行,长按确定继续/默认项
#include "battle_app.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "nvs_flash.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 12
// A key may still be held while the chip boots from a GPIO wake. Ignore it
// until it is released; the deadline only protects against a stuck key.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

static key_guard_t s_key_guard;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;

// 按键回调运行在共享 esp_timer 任务里,只能入队后立刻返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Consume the wake guard before the readiness check: a release can arrive
    // after the guard was armed but before the app is ready, and dropping it
    // here would leave the guard blocking the next real press.
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE,
                          esp_timer_get_time())) {
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            battle_app_handle_key(input.btn, input.event);
        }
    }
}

static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新格式化: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "算术超能战启动");

    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    const esp_err_t nvs_err = nvs_init();
    if (nvs_err != ESP_OK) {
        // 最高分无法保存,但游戏本身仍可运行。
        ESP_LOGE(TAG, "NVS 初始化失败,最高分将不会保存: %s",
                 esp_err_to_name(nvs_err));
    }
    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 电量计是可选能力:读不到时界面显示 "--",不影响游戏。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,电量显示为 --");
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "battle_input", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }
    if (s_key_guard.armed) {
        // The wake key may have been released before the driver started, in
        // which case no release event arrives; sample the pin once.
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
    }

    const esp_err_t app_err = battle_app_start();
    if (app_err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(app_err));
        return;
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪:Display/Button/Battery 已初始化");
}

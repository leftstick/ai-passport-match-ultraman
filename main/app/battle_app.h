// Battle math game UI and application state.
//
// All functions update LVGL objects; call them from the input task or the app
// task only. The implementation takes bsp_lvgl_lock() internally.
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

esp_err_t battle_app_start(void);
void battle_app_handle_key(bsp_btn_t btn, bsp_btn_ev_t event);

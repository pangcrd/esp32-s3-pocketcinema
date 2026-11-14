#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include <stdint.h>
#include "ft6336g.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_display_init(lv_display_t **out_disp);
lv_display_t *app_display_get_display(void);
ft6336g_handle_t app_display_get_touch_handle(void);
bool app_display_get_last_touch(int *x, int *y, bool *pressed);

#ifdef __cplusplus
}
#endif
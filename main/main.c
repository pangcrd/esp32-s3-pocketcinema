/* 
* Video player project.
* Youtube: https://www.youtube.com/@pangcrd
* Github: https://github.com/pangcrd
 */
#include "app_display.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "hw_port_usb.h"
#include "lvgl.h"
//#include "lv_examples.h"
#include "ui.h"
#include "gui_event.h"

static const char *TAG = "VIDEO-PLAYER-AVI-FORMAT";

void app_main(void)
{
    lv_display_t *disp = NULL;
    if (app_display_init(&disp) != ESP_OK) {
        ESP_LOGE(TAG, "app_display_init failed - check wiring / run poll first");
        return;
    }
    (void)disp;
    hw_port_usb_init();
    if (!lvgl_port_lock(-1)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        return;
    }
    //lv_example_anim_timeline_1();
    ui_init(); 
    gui_event_init();
    //gui_event_test_usb_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI ready - LVGL port task is running, main returns");
}
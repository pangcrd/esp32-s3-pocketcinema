/* Backlight brightness for the LCD, via LEDC PWM on the BL pin.
 * This is the only place that touches the backlight hardware:
 * gui_event only calls hw_port_brightness_set(0-100) and never
 * includes a driver header. Panel drawing (esp_lcd_ili9341)
 * does not control brightness; the BL pin does.
 * Call order: app_display_init() first (it drives BL high as
 * a plain GPIO), then hw_port_brightness_init/set, which
 * takes the pin over as an LEDC output. Do not call
 * app_display's BL GPIO code again after this.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "gpio_conf.h"
#include "hw_port.h"

#define HW_PORT_BL_DUTY_MAX ((1 << 13) - 1)

static const char *TAG = "hw_port_display";
static bool s_inited = false;
static int s_brightness = 100;

esp_err_t hw_port_brightness_init(void)
{
    esp_err_t err;
    if (s_inited) return ESP_OK;

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BOARD_LCD_BL_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)BOARD_LCD_BL_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    return ESP_OK;
}

/* Set backlight brightness, 0 = off, 100 = full. */
esp_err_t hw_port_brightness_set(int percent)
{
    esp_err_t err;
    uint32_t duty;
    int duty_percent;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    err = hw_port_brightness_init();
    if (err != ESP_OK) return err;

    /* Active-low backlight needs the inverted duty cycle. */
    duty_percent = BOARD_LCD_BL_ACTIVE_HIGH ? percent : (100 - percent);
    duty = (uint32_t)duty_percent * HW_PORT_BL_DUTY_MAX / 100;

    err = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                        (ledc_channel_t)BOARD_LCD_BL_PWM_CHANNEL, duty);
    if (err != ESP_OK) return err;
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE,
                           (ledc_channel_t)BOARD_LCD_BL_PWM_CHANNEL);
    if (err != ESP_OK) return err;

    s_brightness = percent;
    return ESP_OK;
}

int hw_port_brightness_get(void)
{
    return s_brightness;
}

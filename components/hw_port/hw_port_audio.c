/* Audio adapter: volume/mute/PA/codec, via bsp_audio + ES8311.
 * gui_event only calls hw_port_audio_* and never includes
 * bsp_audio.h, es8311.h or driver headers.
 *
 * Shared I2C0: touch (FT6336G, 0x38) and codec (ES8311, 0x18)
 * are on the same wires, so they must share ONE bus handle.
 * app_display owns that bus (it creates touch first), so
 * this file joins it via ft6336g_get_bus_handle(), it never
 * calls bsp_audio_init() which would try to create a second
 * bus on port 0 and fail. Call order: app_display_init()
 * first, then hw_port_audio_init() (or any set_volume call,
 * which lazily inits the same way).
 */

#include <stdbool.h>

#include "app_display.h"
#include "bsp_audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "ft6336g.h"
#include "gpio_conf.h"
#include "hw_port.h"

static const char *TAG = "hw_port_audio";
static bool s_inited = false;

esp_err_t hw_port_audio_init(void)
{
    esp_err_t err;
    ft6336g_handle_t touch;
    i2c_master_bus_handle_t bus;
    bsp_audio_config_t cfg;

    if (s_inited) return ESP_OK;

    touch = app_display_get_touch_handle();
    if (touch == NULL) {
        ESP_LOGE(TAG, "no touch handle: call app_display_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    bus = ft6336g_get_bus_handle(touch);
    if (bus == NULL) {
        ESP_LOGE(TAG, "touch bus handle is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    cfg.i2c_port = BOARD_AUDIO_I2C_PORT;
    cfg.i2c_sda = BOARD_AUDIO_I2C_SDA_GPIO;
    cfg.i2c_scl = BOARD_AUDIO_I2C_SCL_GPIO;
    cfg.i2c_freq_hz = BOARD_AUDIO_I2C_FREQ_HZ;
    cfg.codec_addr = BOARD_AUDIO_CODEC_I2C_ADDR;
    cfg.i2s_mclk = BOARD_AUDIO_I2S_MCLK_GPIO;
    cfg.i2s_bclk = BOARD_AUDIO_I2S_BCLK_GPIO;
    cfg.i2s_ws = BOARD_AUDIO_I2S_WS_GPIO;
    cfg.i2s_dout = BOARD_AUDIO_I2S_DOUT_GPIO;
    cfg.pa_pin = BOARD_AUDIO_PA_EN_GPIO;
    cfg.pa_active_level = BOARD_AUDIO_PA_ACTIVE_LEVEL;

    err = bsp_audio_init_on_bus(bus, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init_on_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

/* Volume 0-100, same scale as the LVGL slider, but REMAPPED.
 * The codec itself spans 80 dB: 100% = 0 dB, 70% = -24 dB,
 * 40% = -48 dB, 1% = -79 dB, 0% = mute. On this speaker/PA,
 * -24 dB and below is below the audible threshold, which
 * is why the slider was silent below ~70% and only audible
 * above it, even though the code was 'working'.
 * Fix in this adapter only (bsp_audio/es8311 untouched):
 * slider 0% -> codec 0% (mute, reg 0x00), slider 1-100%
 * -> codec 62-100%, i.e. -30 dB to 0 dB. Now 1% is quiet
 * but audible, 50% is about -15 dB (medium), 100% is max,
 * and only 0% mutes completely, as the user asked. */
esp_err_t hw_port_audio_set_volume(int percent)
{
    esp_err_t err;
    int codec_percent;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    err = hw_port_audio_init();
    if (err != ESP_OK) return err;
    if (percent == 0) return bsp_audio_set_volume(0);
    codec_percent = 62 + (percent * 38 + 50) / 100;
    return bsp_audio_set_volume(codec_percent);
}

esp_err_t hw_port_audio_mute(bool mute)
{
    esp_err_t err;
    err = hw_port_audio_init();
    if (err != ESP_OK) return err;
    return bsp_audio_mute(mute);
}

esp_err_t hw_port_audio_start(void)
{
    esp_err_t err;
    err = hw_port_audio_init();
    if (err != ESP_OK) return err;
    return bsp_audio_start();
}

esp_err_t hw_port_audio_stop(void)
{
    esp_err_t err;
    err = hw_port_audio_init();
    if (err != ESP_OK) return err;
    return bsp_audio_stop();
}

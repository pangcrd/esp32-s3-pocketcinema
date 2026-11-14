#include "bsp_audio.h"
#include "es8311.h"
/* New I2C driver API (driver_ng). Project no longer uses esp32_display_panel,
 * so we can use the modern i2c_master.h API without conflicts. */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_audio";

#define AUDIO_MCLK_MULTIPLE  384   /* must match I2S_MCLK_MULTIPLE_384 below */
#define AUDIO_MCLK_FREQ_HZ   (BSP_AUDIO_SAMPLE_RATE * AUDIO_MCLK_MULTIPLE)

static bsp_audio_config_t       s_cfg;
static i2c_master_bus_handle_t  s_i2c_bus;
static i2s_chan_handle_t        s_tx_handle;
static es8311_handle_t          s_es_handle;
static bool                     s_inited;

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = s_cfg.i2c_port,
        .sda_io_num = s_cfg.i2c_sda,
        .scl_io_num = s_cfg.i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Bus already exists: reuse it. This keeps bsp_audio_init() working
         * if the caller tears the codec down with es8311_delete() and re-inits. */
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "i2c new master bus failed");
    return ESP_OK;
}

static esp_err_t i2s_tx_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* out of data -> play silence instead of repeating the last DMA buffer */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL), TAG, "i2s new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = BSP_AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_384,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            (BSP_AUDIO_CHANNELS == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = s_cfg.i2s_mclk,
            .bclk = s_cfg.i2s_bclk,
            .ws   = s_cfg.i2s_ws,
            .dout = s_cfg.i2s_dout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    /* Mono: ES8311 on this board reads the left slot. If audio is silent or distorted,
     * try I2S_STD_SLOT_RIGHT, or switch to stereo and duplicate the samples. */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "i2s enable failed");
    return ESP_OK;
}

static esp_err_t pa_enable_init(void)
{
    if (s_cfg.pa_pin < 0) return ESP_OK;   /* board has no PA control pin */

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_cfg.pa_pin,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "pa gpio config failed");
    return gpio_set_level(s_cfg.pa_pin, !s_cfg.pa_active_level);   /* start with PA off */
}

esp_err_t bsp_audio_pa_set(bool on)
{
    if (s_cfg.pa_pin < 0) return ESP_OK;
    return gpio_set_level(s_cfg.pa_pin, on ? s_cfg.pa_active_level : !s_cfg.pa_active_level);
}

static esp_err_t bsp_audio_init_inner(i2c_master_bus_handle_t bus,
                                      const bsp_audio_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "null config");
    ESP_RETURN_ON_FALSE(!s_inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    s_cfg = *cfg;

    if (bus != NULL) {
        s_i2c_bus = bus;
    } else {
        ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c init failed");
    }
    ESP_RETURN_ON_ERROR(i2s_tx_init(), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(pa_enable_init(), TAG, "pa enable failed");

    s_es_handle = es8311_create(s_i2c_bus, s_cfg.codec_addr, s_cfg.i2c_freq_hz);
    ESP_RETURN_ON_FALSE(s_es_handle, ESP_FAIL, TAG, "es8311 create failed");

    const es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = AUDIO_MCLK_FREQ_HZ,
        .sample_frequency = BSP_AUDIO_SAMPLE_RATE,
    };

    ESP_RETURN_ON_ERROR(
        es8311_init(s_es_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
        TAG, "es8311 init failed");
    ESP_RETURN_ON_ERROR(
        es8311_sample_frequency_config(s_es_handle, AUDIO_MCLK_FREQ_HZ, BSP_AUDIO_SAMPLE_RATE),
        TAG, "es8311 sample rate config failed");

    /* Volume mặc định khi init: 0 dB (mức sạch tối đa của codec). Muốn nhỏ hơn thì
     * gọi bsp_audio_set_volume(); > 100 vào es8311_voice_volume_set() vẫn bị kẹp về
     * mức trần. Đừng hạ số này xuống thấp vì thang là dB, giảm rất nhanh. */
    ESP_RETURN_ON_ERROR(
        es8311_voice_volume_set(s_es_handle, BSP_AUDIO_VOLUME_DEFAULT, NULL),
        TAG, "es8311 volume set failed");

    s_inited = true;
    ESP_LOGI(TAG, "audio init OK: %dHz mono 16-bit, mclk=%dHz", BSP_AUDIO_SAMPLE_RATE, AUDIO_MCLK_FREQ_HZ);
    return ESP_OK;
}

esp_err_t bsp_audio_init(const bsp_audio_config_t *cfg)
{
    return bsp_audio_init_inner(NULL, cfg);
}

esp_err_t bsp_audio_init_on_bus(i2c_master_bus_handle_t bus,
                                const bsp_audio_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "null bus");
    return bsp_audio_init_inner(bus, cfg);
}

esp_err_t bsp_audio_write(const uint8_t *pcm, size_t len, size_t *bytes_written, TickType_t timeout)
{
    if (!s_tx_handle) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(s_tx_handle, pcm, len, bytes_written, timeout);
}

esp_err_t bsp_audio_flush_silence(int ms)
{
    if (!s_tx_handle) return ESP_ERR_INVALID_STATE;

    static const int16_t zeros[256] = {0};   /* 256 samples = 16 ms @ 16 kHz */
    int blocks = ms / 16 + 1;
    for (int i = 0; i < blocks; i++) {
        size_t w = 0;
        ESP_RETURN_ON_ERROR(
            bsp_audio_write((const uint8_t *)zeros, sizeof(zeros), &w, portMAX_DELAY),
            TAG, "flush write failed");
    }
    return ESP_OK;
}

esp_err_t bsp_audio_start(void)
{
    if (!s_es_handle) return ESP_ERR_INVALID_STATE;

    /* Order matters: unmute codec first, enable PA second, so the codec's pop does not hit the amp */
    ESP_RETURN_ON_ERROR(es8311_voice_mute(s_es_handle, false), TAG, "unmute failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return bsp_audio_pa_set(true);
}

esp_err_t bsp_audio_stop(void)
{
    if (!s_es_handle) return ESP_ERR_INVALID_STATE;

    /* Reverse order: silence -> PA off -> mute codec */
    bsp_audio_flush_silence(100);
    bsp_audio_pa_set(false);
    return es8311_voice_mute(s_es_handle, true);
}

esp_err_t bsp_audio_set_volume(int volume_percent)
{
    if (!s_es_handle) return ESP_ERR_INVALID_STATE;
    return es8311_voice_volume_set(s_es_handle, volume_percent, NULL);
}

esp_err_t bsp_audio_mute(bool mute)
{
    if (!s_es_handle) return ESP_ERR_INVALID_STATE;
    return es8311_voice_mute(s_es_handle, mute);
}
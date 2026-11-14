#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_AUDIO_SAMPLE_RATE   16000
#define BSP_AUDIO_CHANNELS      1       /* mono */
#define BSP_AUDIO_BITS          16

/* Volume lúc init, 0-100. 100 = 0 dB (mức sạch tối đa, xem es8311.c phần 0x32).
 * Giảm rất nhanh theo dB, nên hạ từng chút một: 80% đã là -16 dB. */
#define BSP_AUDIO_VOLUME_DEFAULT    100

typedef struct {
    int      i2c_port;
    int      i2c_sda, i2c_scl;
    uint32_t i2c_freq_hz;
    uint16_t codec_addr;                /* ES8311: 0x18 (CE low) / 0x19 (CE high) */
    int      i2s_mclk, i2s_bclk, i2s_ws, i2s_dout;
    int      pa_pin;                    /* < 0 = board has no PA control pin */
    int      pa_active_level;           /* 0 = active low, 1 = active high */
} bsp_audio_config_t;

/* Init I2C, I2S, ES8311. The PA is left OFF: call bsp_audio_start() before playing. */
esp_err_t bsp_audio_init(const bsp_audio_config_t *cfg);

/* Same as bsp_audio_init, but joins an I2C bus the caller already
 * owns instead of creating a second bus on the same port.
 * REQUIRED when touch (FT6336G) already owns I2C0: ESP-IDF
 * allows only one bus handle per port, so a second
 * i2c_new_master_bus() on port 0 fails and es8311_create()
 * on a NULL bus fails. Pass the bus from app_display's
 * touch handle (ft6336g_get_bus_handle()). */
esp_err_t bsp_audio_init_on_bus(i2c_master_bus_handle_t bus,
                                const bsp_audio_config_t *cfg);

/* Unmute codec, then enable PA. Call before playback. */
esp_err_t bsp_audio_start(void);

/* Flush silence, disable PA, then mute codec. Call after playback. */
esp_err_t bsp_audio_stop(void);

/* Write PCM 16-bit mono 16 kHz to the speaker. Blocks until accepted or timeout. */
esp_err_t bsp_audio_write(const uint8_t *pcm, size_t len, size_t *bytes_written, TickType_t timeout);

/* Push `ms` milliseconds of zero samples to flush the DMA buffers. */
esp_err_t bsp_audio_flush_silence(int ms);

esp_err_t bsp_audio_pa_set(bool on);
esp_err_t bsp_audio_set_volume(int volume_percent);   /* 0-100; 100 = 0 dB, 0 = mute */
esp_err_t bsp_audio_mute(bool mute);

#ifdef __cplusplus
}
#endif
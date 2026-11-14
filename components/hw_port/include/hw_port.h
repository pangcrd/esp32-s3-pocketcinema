#ifndef HW_PORT_H
#define HW_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hw_port_brightness_init(void);
esp_err_t hw_port_brightness_set(int percent);
int hw_port_brightness_get(void);

/* Audio: ES8311 codec + speaker PA, see hw_port_audio.c.
 * Init joins the touch I2C bus, so app_display_init() must
 * run first. set_volume() lazily inits the same way. */
esp_err_t hw_port_audio_init(void);
esp_err_t hw_port_audio_set_volume(int percent);
esp_err_t hw_port_audio_mute(bool mute);
esp_err_t hw_port_audio_start(void);
esp_err_t hw_port_audio_stop(void);

/* Battery: ADC1 GPIO9, 200k/200k, see hw_port_battery.c.
 * Level is 20, 50 or 100, matching the three icons. */
esp_err_t hw_port_battery_init(void);
esp_err_t hw_port_battery_read_mv(int *vbat_mv);
int hw_port_battery_level(void);

/* SD storage, wrapping bsp_sd + bsp_fs. All paths are full
 * paths ("/sdcard/<name>") ready for avi_player_play(). */
#define HW_PORT_VIDEO_MAX 64

esp_err_t hw_port_storage_mount(void);
esp_err_t hw_port_storage_unmount(void);
int hw_port_storage_is_mounted(void);
int hw_port_storage_refresh_list(void);
int hw_port_storage_video_count(void);
const char *hw_port_storage_video_name(int index);
esp_err_t hw_port_storage_video_path(int index, char *out, size_t out_size);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif

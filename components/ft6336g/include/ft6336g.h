#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FT6336G_VERSION "1.0.17"
#define FT6336G_MAX_POINTS 2

/* Touch event flags, register P*_XH bits 7:6 (normal-mode table). */
typedef enum {
    FT6336G_EVENT_PRESS_DOWN = 0,
    FT6336G_EVENT_LIFT_UP = 1,
    FT6336G_EVENT_CONTACT = 2,
    FT6336G_EVENT_NO_EVENT = 3,
} ft6336g_event_t;

typedef struct {
    uint8_t id;
    uint16_t x;
    uint16_t y;
    ft6336g_event_t event;
} ft6336g_point_t;

typedef struct {
    uint8_t count;
    ft6336g_point_t points[FT6336G_MAX_POINTS];
} ft6336g_touch_t;

/* Chip identification, read from the normal-mode ID registers. */
typedef struct {
    uint8_t vendor_id;    /* 0xA8, expected 0x11 */
    uint8_t cipher_high;  /* 0xA3, expected 0x64 */
    uint8_t cipher_mid;   /* 0x9F, expected 0x26 */
    uint8_t cipher_low;   /* 0xA0, 0x01 = FT6336G (0x00 FT6236G, 0x02 FT6336U) */
    uint8_t lib_ver_high; /* 0xA1 */
    uint8_t lib_ver_low;  /* 0xA2 */
    uint8_t firmware_id;  /* 0xA6 */
} ft6336g_chip_id_t;

typedef struct {
    int i2c_port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint16_t dev_addr;
    uint32_t clk_speed_hz;
    bool i2c_internal_pullup;
    gpio_num_t rst_gpio; /* (gpio_num_t)-1 = not wired */
    gpio_num_t int_gpio; /* (gpio_num_t)-1 = polling only */
    uint16_t disp_width;
    uint16_t disp_height;
    bool swap_xy;
    bool invert_x;
    bool invert_y;
} ft6336g_config_t;

typedef struct ft6336g *ft6336g_handle_t;

ft6336g_config_t ft6336g_config_default(void);
esp_err_t ft6336g_init_default(ft6336g_handle_t *out_handle);
esp_err_t ft6336g_init(const ft6336g_config_t *cfg, ft6336g_handle_t *out_handle);
esp_err_t ft6336g_init_on_bus(i2c_master_bus_handle_t bus, const ft6336g_config_t *cfg,
                              ft6336g_handle_t *out_handle);
   /* Return the I2C bus this touch handle owns or shares, so a
    * second device on the same bus (ES8311) can join it. */
i2c_master_bus_handle_t ft6336g_get_bus_handle(ft6336g_handle_t handle);                              
esp_err_t ft6336g_deinit(ft6336g_handle_t handle);
esp_err_t ft6336g_reset(ft6336g_handle_t handle);
esp_err_t ft6336g_read(ft6336g_handle_t handle, ft6336g_touch_t *out);
esp_err_t ft6336g_get_chip_id(ft6336g_handle_t handle, ft6336g_chip_id_t *out);
esp_err_t ft6336g_read_reg(ft6336g_handle_t handle, uint8_t reg, uint8_t *out);
esp_err_t ft6336g_write_reg(ft6336g_handle_t handle, uint8_t reg, uint8_t value);
esp_err_t ft6336g_set_display_size(ft6336g_handle_t handle, uint16_t width, uint16_t height);
esp_err_t ft6336g_set_transform(ft6336g_handle_t handle, bool swap_xy, bool invert_x, bool invert_y);
const char *ft6336g_version(void);

#ifdef __cplusplus
}
#endif
#include "ft6336g.h"
#include "ft6336g_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "FT6336G";

/* Normal-mode registers (supplied register table, sheet "普通模式"). */
#define FT6336G_REG_MODE_SWITCH 0x00
#define FT6336G_REG_TD_STATUS 0x02
#define FT6336G_REG_P1_XH 0x03
#define FT6336G_REG_CIPHER_MID 0x9F
#define FT6336G_REG_CIPHER_LOW 0xA0
#define FT6336G_REG_LIB_VER_H 0xA1
#define FT6336G_REG_LIB_VER_L 0xA2
#define FT6336G_REG_CIPHER_HIGH 0xA3
#define FT6336G_REG_FIRMID 0xA6
#define FT6336G_REG_VENDOR_ID 0xA8

#define FT6336G_TOUCH_FRAME_LEN 13 /* 0x02..0x0E: status + 2 x 6 bytes */
#define FT6336G_VENDOR_ID_EXPECTED 0x11
#define FT6336G_CIPHER_LOW_FT6336G 0x01

struct ft6336g {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool bus_owned;
    gpio_num_t rst_gpio;
    gpio_num_t int_gpio;
    uint16_t disp_width;
    uint16_t disp_height;
    bool swap_xy;
    bool invert_x;
    bool invert_y;
};

const char *ft6336g_version(void)
{
    return FT6336G_VERSION;
}

ft6336g_config_t ft6336g_config_default(void)
{
    ft6336g_config_t cfg = {
        .i2c_port = FT6336G_I2C_PORT,
        .sda_gpio = (gpio_num_t)FT6336G_SDA_GPIO,
        .scl_gpio = (gpio_num_t)FT6336G_SCL_GPIO,
        .dev_addr = FT6336G_I2C_ADDR,
        .clk_speed_hz = FT6336G_I2C_SPEED_HZ,
        .i2c_internal_pullup = FT6336G_I2C_INTERNAL_PULLUP != 0,
        .rst_gpio = (gpio_num_t)FT6336G_RST_GPIO,
        .int_gpio = (gpio_num_t)FT6336G_INT_GPIO,
        .disp_width = FT6336G_DISP_WIDTH,
        .disp_height = FT6336G_DISP_HEIGHT,
        .swap_xy = FT6336G_SWAP_XY != 0,
        .invert_x = FT6336G_INVERT_X != 0,
        .invert_y = FT6336G_INVERT_Y != 0,
    };
    return cfg;
}

/* Reset pulse per datasheet Table 3-5: RST low >= 5 ms, first valid
 * report >= 300 ms after reset. */
esp_err_t ft6336g_reset(ft6336g_handle_t handle)
{
    if (handle == NULL || handle->rst_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_reset_pin(handle->rst_gpio);
    gpio_set_direction(handle->rst_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(handle->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(handle->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
}

static esp_err_t ft6336g_init_inner(i2c_master_bus_handle_t bus, bool bus_owned,
                                    const ft6336g_config_t *cfg, ft6336g_handle_t *out_handle)
{
    if (bus == NULL || cfg == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->dev_addr > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }

    ft6336g_handle_t h = (ft6336g_handle_t)calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->bus = bus;
    h->bus_owned = bus_owned;
    h->rst_gpio = cfg->rst_gpio;
    h->int_gpio = cfg->int_gpio;
    h->disp_width = cfg->disp_width;
    h->disp_height = cfg->disp_height;
    h->swap_xy = cfg->swap_xy;
    h->invert_x = cfg->invert_x;
    h->invert_y = cfg->invert_y;

    if (h->rst_gpio >= 0) {
        esp_err_t err = ft6336g_reset(h);
        if (err != ESP_OK) {
            free(h);
            return err;
        }
    }
    if (h->int_gpio >= 0) {
        gpio_reset_pin(h->int_gpio);
        gpio_set_direction(h->int_gpio, GPIO_MODE_INPUT);
        gpio_set_pull_mode(h->int_gpio, GPIO_PULLUP_ONLY);
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->dev_addr,
        .scl_speed_hz = cfg->clk_speed_hz ? cfg->clk_speed_hz : 400000,
        .scl_wait_us = 0,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02x failed: %s", cfg->dev_addr, esp_err_to_name(err));
        free(h);
        return err;
    }

    /* Probe only: an I2C failure here means no chip answers, so fail
     * fast instead of returning empty touches forever. A surprising
     * ID value only warns, so FT6236G/FT6336U variants still work. */
    ft6336g_chip_id_t id;
    err = ft6336g_get_chip_id(h, &id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip ID read failed (check wiring / 0x%02x address)", cfg->dev_addr);
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return err;
    }
    if (id.vendor_id != FT6336G_VENDOR_ID_EXPECTED) {
        ESP_LOGW(TAG, "unexpected vendor 0x%02x (expected 0x%02x)", id.vendor_id,
                 FT6336G_VENDOR_ID_EXPECTED);
    }
    if (id.cipher_low != FT6336G_CIPHER_LOW_FT6336G) {
        ESP_LOGW(TAG, "cipher_low 0x%02x, not FT6336G (0x01) - continuing as FT family",
                 id.cipher_low);
    }
    ESP_LOGI(TAG, "v%s ready: vendor 0x%02x cipher %02x-%02x-%02x lib %d.%d fw 0x%02x",
             FT6336G_VERSION, id.vendor_id, id.cipher_high, id.cipher_mid, id.cipher_low,
             id.lib_ver_high, id.lib_ver_low, id.firmware_id);
    *out_handle = h;
    return ESP_OK;
}

esp_err_t ft6336g_init(const ft6336g_config_t *cfg, ft6336g_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ft6336g_config_t def = ft6336g_config_default();
    if (cfg == NULL) {
        cfg = &def;
    }
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = cfg->i2c_port,
        .scl_io_num = cfg->scl_gpio,
        .sda_io_num = cfg->sda_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = cfg->i2c_internal_pullup,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new master bus port %d failed: %s", cfg->i2c_port, esp_err_to_name(err));
        return err;
    }
    err = ft6336g_init_inner(bus, true, cfg, out_handle);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus);
    }
    return err;
}

esp_err_t ft6336g_init_default(ft6336g_handle_t *out_handle)
{
    return ft6336g_init(NULL, out_handle);
}

/* Attach to a bus the application already owns (e.g. shared with AHT20).
 * The caller keeps owning and deleting that bus; deinit only removes
 * this device. Never call ft6336g_init() and this on the same port. */
esp_err_t ft6336g_init_on_bus(i2c_master_bus_handle_t bus, const ft6336g_config_t *cfg,
                              ft6336g_handle_t *out_handle)
{
    if (bus == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ft6336g_config_t def = ft6336g_config_default();
    if (cfg == NULL) {
        cfg = &def;
    }
    return ft6336g_init_inner(bus, false, cfg, out_handle);
}
i2c_master_bus_handle_t ft6336g_get_bus_handle(ft6336g_handle_t handle)
   {
       if (handle == NULL) return NULL;
       return handle->bus;
   }

esp_err_t ft6336g_deinit(ft6336g_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->dev != NULL) {
        i2c_master_bus_rm_device(handle->dev);
    }
    if (handle->bus_owned && handle->bus != NULL) {
        i2c_del_master_bus(handle->bus);
    }
    free(handle);
    return ESP_OK;
}

esp_err_t ft6336g_read_reg(ft6336g_handle_t handle, uint8_t reg, uint8_t *out)
{
    if (handle == NULL || handle->dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(handle->dev, &reg, 1, out, 1,
                                       FT6336G_I2C_TIMEOUT_MS);
}

esp_err_t ft6336g_write_reg(ft6336g_handle_t handle, uint8_t reg, uint8_t value)
{
    if (handle == NULL || handle->dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(handle->dev, buf, sizeof(buf), FT6336G_I2C_TIMEOUT_MS);
}

esp_err_t ft6336g_get_chip_id(ft6336g_handle_t handle, ft6336g_chip_id_t *out)
{
    if (handle == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    err = ft6336g_read_reg(handle, FT6336G_REG_VENDOR_ID, &out->vendor_id);
    if (err != ESP_OK) return err;
    err = ft6336g_read_reg(handle, FT6336G_REG_CIPHER_HIGH, &out->cipher_high);
    if (err != ESP_OK) return err;
    err = ft6336g_read_reg(handle, FT6336G_REG_CIPHER_MID, &out->cipher_mid);
    if (err != ESP_OK) return err;
    err = ft6336g_read_reg(handle, FT6336G_REG_CIPHER_LOW, &out->cipher_low);
    if (err != ESP_OK) return err;
    err = ft6336g_read_reg(handle, FT6336G_REG_LIB_VER_H, &out->lib_ver_high);
    if (err != ESP_OK) return err;
    err = ft6336g_read_reg(handle, FT6336G_REG_LIB_VER_L, &out->lib_ver_low);
    if (err != ESP_OK) return err;
    err = ft6336g_read_reg(handle, FT6336G_REG_FIRMID, &out->firmware_id);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

/* Parse one 13-byte frame and apply the single transform layer. */
static void ft6336g_parse(const uint8_t *buf, ft6336g_handle_t h, ft6336g_touch_t *out)
{
    uint8_t count = buf[0] & 0x0F;
    if (count > FT6336G_MAX_POINTS) {
        count = FT6336G_MAX_POINTS;
    }
    out->count = count;
    for (int i = 0; i < FT6336G_MAX_POINTS; i++) {
        const uint8_t *p = &buf[1 + i * 6];
        uint16_t x = (uint16_t)(((p[0] & 0x0F) << 8) | p[1]);
        uint16_t y = (uint16_t)(((p[2] & 0x0F) << 8) | p[3]);
        out->points[i].id = (uint8_t)((p[2] >> 4) & 0x0F);
        out->points[i].event = (ft6336g_event_t)((p[0] >> 6) & 0x03);
        if (h->swap_xy) {
            uint16_t t = x;
            x = y;
            y = t;
        }
        /* Clamp before inverting so width-1-x can never go negative. */
        if (h->disp_width > 0 && x >= h->disp_width) {
            x = h->disp_width - 1;
        }
        if (h->disp_height > 0 && y >= h->disp_height) {
            y = h->disp_height - 1;
        }
        if (h->invert_x && h->disp_width > 0) {
            x = h->disp_width - 1 - x;
        }
        if (h->invert_y && h->disp_height > 0) {
            y = h->disp_height - 1 - y;
        }
        out->points[i].x = x;
        out->points[i].y = y;
    }
    /* Points past count are stale by definition: mark them NO_EVENT so
     * a caller that ignores count cannot mistake them for touches. */
    for (int i = count; i < FT6336G_MAX_POINTS; i++) {
        out->points[i].event = FT6336G_EVENT_NO_EVENT;
    }
}

esp_err_t ft6336g_read(ft6336g_handle_t handle, ft6336g_touch_t *out)
{
    if (handle == NULL || handle->dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg = FT6336G_REG_TD_STATUS;
    uint8_t buf[FT6336G_TOUCH_FRAME_LEN];
    esp_err_t err = i2c_master_transmit_receive(handle->dev, &reg, 1, buf, sizeof(buf),
                                                FT6336G_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    ft6336g_parse(buf, handle, out);
    return ESP_OK;
}

esp_err_t ft6336g_set_display_size(ft6336g_handle_t handle, uint16_t width, uint16_t height)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->disp_width = width;
    handle->disp_height = height;
    return ESP_OK;
}

esp_err_t ft6336g_set_transform(ft6336g_handle_t handle, bool swap_xy, bool invert_x, bool invert_y)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->swap_xy = swap_xy;
    handle->invert_x = invert_x;
    handle->invert_y = invert_y;
    return ESP_OK;
}
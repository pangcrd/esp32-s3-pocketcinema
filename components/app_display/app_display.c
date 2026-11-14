/* Display + touch module: ILI9341 over SPI2 plus FT6336G over I2C0,
 */
#include "app_display.h"
#include "display_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "ft6336g.h"
#include "lvgl.h"

static const char *TAG = "APP_DISPLAY";
static lv_display_t *s_disp = NULL;
static ft6336g_handle_t s_touch = NULL;
static int s_last_x = -1, s_last_y = -1;
static bool s_last_pressed = false;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    ft6336g_touch_t t = {0};
    if (s_touch == NULL || ft6336g_read(s_touch, &t) != ESP_OK || t.count == 0) {
        /* Keep the last point on release: LVGL expects it. */
        data->point.x = s_last_x < 0 ? 0 : s_last_x;
        data->point.y = s_last_y < 0 ? 0 : s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        s_last_pressed = false;
        return;
    }
    s_last_x = t.points[0].x;
    s_last_y = t.points[0].y;
    s_last_pressed = (t.points[0].event != FT6336G_EVENT_LIFT_UP);
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = s_last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

typedef struct {
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} app_madctl_t;

/* One ROTATE value -> one panel MADCTL. Returns false for any
 * value other than 0/90/180/270, so a typo fails fast instead
 * of silently showing a wrong orientation. */
static bool rotate_to_madctl(app_madctl_t *out)
{
    switch (APP_DISP_ROTATE) {
        case APP_DISP_ROTATE_0:
            *out = (app_madctl_t){.swap_xy = false, .mirror_x = false, .mirror_y = true};
            return true;
        case APP_DISP_ROTATE_90:
            *out = (app_madctl_t){.swap_xy = true, .mirror_x = false, .mirror_y = false};
            return true;
        case APP_DISP_ROTATE_180:
            *out = (app_madctl_t){.swap_xy = false, .mirror_x = true, .mirror_y = false};
            return true;
        case APP_DISP_ROTATE_270:
            *out = (app_madctl_t){.swap_xy = true, .mirror_x = true, .mirror_y = true};
            return true;
        default:
            ESP_LOGE(TAG, "APP_DISP_ROTATE must be 0/90/180/270, got %d", APP_DISP_ROTATE);
            return false;
    }
}

/* SPI bus + ILI9341 panel. The port registers its own panel-IO
 * transfer-done callback when the display is added, so this IO
 * config intentionally leaves on_color_trans_done NULL. */
static esp_err_t panel_init(const app_madctl_t *madctl,
                            int disp_w,
                            int flush_lines,
                            esp_lcd_panel_io_handle_t *out_io,
                            esp_lcd_panel_handle_t *out_panel)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = APP_DISP_MOSI_GPIO,
        .miso_io_num = APP_DISP_MISO_GPIO,
        .sclk_io_num = APP_DISP_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Logical width, not native H_RES: a landscape flush is
         * disp_w wide and must fit in one SPI transfer. For a
         * full-frame buffer flush_lines is the whole height,
         * so max_transfer is the whole frame (153600 bytes
         * for 240x320); for a partial buffer it is one chunk
         * (disp_w * BUF_LINES * 2). If this is too small,
         * draw_bitmap fails with an invalid-size error. */
        .max_transfer_sz = disp_w * flush_lines * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = APP_DISP_CS_GPIO,
        .dc_gpio_num = APP_DISP_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = APP_DISP_SPI_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, out_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new panel io failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = APP_DISP_RST_GPIO,
        /* APP_DISP_BGR=1 for this module (v1.0.13 proof in
         * README): RGB order showed RED->yellow etc., which
         * is R/B swap, so BGR cancels it. */
        .rgb_ele_order = APP_DISP_BGR ? LCD_RGB_ELEMENT_ORDER_BGR
                                      : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_ili9341(*out_io, &panel_cfg, out_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new panel ili9341 failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_reset(*out_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel reset failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_init(*out_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Panel hardware rotation from ROTATE. Setters, not toggles. */
    err = esp_lcd_panel_swap_xy(*out_panel, madctl->swap_xy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel swap_xy failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_mirror(*out_panel, madctl->mirror_x, madctl->mirror_y);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel mirror failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_invert_color(*out_panel, APP_DISP_INVERT_COLOR != 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel invert_color failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_disp_on_off(*out_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel disp_on failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_reset_pin((gpio_num_t)APP_DISP_BL_GPIO);
    gpio_set_direction((gpio_num_t)APP_DISP_BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)APP_DISP_BL_GPIO, APP_DISP_BL_ON_LEVEL);
    return ESP_OK;
}

esp_err_t app_display_init(lv_display_t **out_disp)
{
    if (s_disp != NULL) {
        if (out_disp != NULL) {
            *out_disp = s_disp;
        }
        return ESP_OK;
    }

    app_madctl_t madctl;
    if (!rotate_to_madctl(&madctl)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* LVGL logical size follows ROTATE, not H_RES/V_RES by hand:
     * 90/270 are landscape, 0/180 are portrait. */
    const int disp_w = madctl.swap_xy ? APP_DISP_V_RES : APP_DISP_H_RES;
    const int disp_h = madctl.swap_xy ? APP_DISP_H_RES : APP_DISP_V_RES;


    int flush_lines = APP_DISP_FULL_BUFFER ? disp_h : APP_DISP_BUF_LINES;
    int buffer_size = APP_DISP_FULL_BUFFER ? disp_w * disp_h
                                           : disp_w * APP_DISP_BUF_LINES;
    bool effective_full = APP_DISP_FULL_BUFFER != 0;
    bool effective_spiram = APP_DISP_USE_SPIRAM != 0;
    /* v1.0.16 fix: esp_psram_get_size() does NOT exist in
     * esp_psram.h in the user's IDF build, so do not use
     * it. heap_caps_get_total_size(MALLOC_CAP_SPIRAM)
     * is the stable heap API for the same question:
     * total PSRAM heap size, 0 when there is no PSRAM
     * or PSRAM is not enabled in sdkconfig. */
    const size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "heap before display: psram_size=%u psram_free=%u dma_free=%u dma_largest=%u",
             (unsigned)psram_size, (unsigned)psram_free,
             (unsigned)dma_free, (unsigned)dma_largest);
    if (effective_spiram) {
        const size_t need = (size_t)buffer_size * sizeof(uint16_t)
                            * (APP_DISP_DOUBLE_BUFFER ? 2 : 1);
        if (psram_size == 0 || psram_free < need) {
            ESP_LOGW(TAG, "USE_SPIRAM=1 but PSRAM cannot hold %u bytes (psram_size=%u psram_free=%u): check sdkconfig PSRAM + module PSRAM, falling back to internal DMA partial buffer, display will still work",
                     (unsigned)need, (unsigned)psram_size, (unsigned)psram_free);
            effective_spiram = false;
            effective_full = false;
            flush_lines = APP_DISP_BUF_LINES;
            buffer_size = disp_w * APP_DISP_BUF_LINES;
        }
    }
    if (effective_full && !effective_spiram && APP_DISP_DOUBLE_BUFFER) {
        /* Full double buffer in internal RAM is 307200
         * bytes for 240x320: do not let the port assert
         * on it, fall back to the partial buffer that
         * is known to work on this board. */
        const size_t need = (size_t)buffer_size * sizeof(uint16_t) * 2;
        if (need > dma_largest) {
            ESP_LOGW(TAG, "FULL_BUFFER double in internal RAM needs %u bytes but largest DMA block is %u: falling back to partial %d lines",
                     (unsigned)need, (unsigned)dma_largest, APP_DISP_BUF_LINES);
            effective_full = false;
            flush_lines = APP_DISP_BUF_LINES;
            buffer_size = disp_w * APP_DISP_BUF_LINES;
        }
    }
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = panel_init(&madctl, disp_w, flush_lines, &io, &panel);
    if (err != ESP_OK) {
        return err;
    }

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = buffer_size,
        .double_buffer = APP_DISP_DOUBLE_BUFFER,
        .hres = disp_w,
        .vres = disp_h,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* SAME rotation as the panel: the port applies this to
         * the panel at add_disp time. v1.0.6 passed 0 here and
         * the port overwrote the panel MADCTL back to 0 -- that
         * is what reversed the picture and clipped landscape
         * flushes. Same values twice is idempotent, not double. */
        .rotation = {
            .swap_xy = madctl.swap_xy,
            .mirror_x = madctl.mirror_x,
            .mirror_y = madctl.mirror_y,
        },
        .flags = {
            /* v1.0.15: buff_dma and buff_spiram are
             * mutually exclusive. buff_dma=true is the
             * old working mode (internal DMA RAM).
             * buff_spiram=true means buff_dma=false:
             * a PSRAM buffer is not DMA-capable
             * internal RAM, and asking for both caps
             * is what hung v1.0.14. */
            .buff_dma = !effective_spiram,
            .buff_spiram = effective_spiram,
            .swap_bytes = APP_DISP_SWAP_BYTES != 0,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LVGL buffer: %s, buffer_size=%d px (%d bytes/buf), double=%d, spiram=%d (requested spiram=%d full=%d), dma=%d, spi max_transfer=%d bytes",
             effective_full ? "FULL frame" : "PARTIAL lines",
             buffer_size, buffer_size * (int)sizeof(uint16_t),
             APP_DISP_DOUBLE_BUFFER, effective_spiram,
             APP_DISP_USE_SPIRAM, APP_DISP_FULL_BUFFER,
             effective_spiram ? 0 : 1,
             disp_w * flush_lines * (int)sizeof(uint16_t));

    err = ft6336g_init_default(&s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s - run main_poll.c first", esp_err_to_name(err));
        return err;
    }
    /* Touch = inverse MADCTL from the same ROTATE value (see
     * header comment): mirror axes cross when swapped. No
     * separate touch config, and no touch code to edit. */
    ft6336g_set_display_size(s_touch, disp_w, disp_h);
    /* Touch = corrected table (see header comment): for
     * swapped rotations the Y inversion is the opposite of
     * the MADCTL mirror_x. Do NOT "simplify" this back to
     * the textbook inverse -- the ROTATE_90 log proves it. */
    ft6336g_set_transform(s_touch,
                          madctl.swap_xy,
                          madctl.swap_xy ? madctl.mirror_y : madctl.mirror_x,
                          madctl.swap_xy ? !madctl.mirror_x : madctl.mirror_y);

    if (!lvgl_port_lock(-1)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        return ESP_FAIL;
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, s_disp);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lvgl_port_unlock();

    if (out_disp != NULL) {
        *out_disp = s_disp;
    }
    ESP_LOGI(TAG, "display + touch ready, rotate %d, logical %dx%d, madctl swap=%d mx=%d my=%d, touch driver %s",
             APP_DISP_ROTATE, disp_w, disp_h,
             madctl.swap_xy, madctl.mirror_x, madctl.mirror_y, ft6336g_version());
    return ESP_OK;
}

lv_display_t *app_display_get_display(void)
{
    return s_disp;
}

ft6336g_handle_t app_display_get_touch_handle(void)
{
    return s_touch;
}

bool app_display_get_last_touch(int *x, int *y, bool *pressed)
{
    if (s_last_x < 0) {
        return false;
    }
    if (x != NULL) {
        *x = s_last_x;
    }
    if (y != NULL) {
        *y = s_last_y;
    }
    if (pressed != NULL) {
        *pressed = s_last_pressed;
    }
    return true;
}
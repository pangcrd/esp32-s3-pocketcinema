/* SD storage adapter: mount + video list, for gui_event playlist.
 * Wraps the tested bsp_sd / bsp_fs, does not rewrite them.
 * NOTE: the bsp_sd in this project is SDIO-only (no SPI mode).
 * If the board's SD slot is wired SPI, this file cannot work
 * and the SPI bsp_sd variant must be used instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "bsp_fs.h"
#include "bsp_sd.h"
#include "esp_err.h"
#include "esp_log.h"
#include "gpio_conf.h"
#include "hw_port.h"

static const char *TAG = "hw_port_storage";
static char s_names[HW_PORT_VIDEO_MAX][256];
static int s_count = 0;
static bool s_list_valid = false;

/* Mount the SD card once. Calling again is OK, not an error. */
esp_err_t hw_port_storage_mount(void)
{
    esp_err_t err;
    if (bsp_sd_get_card() != NULL) return ESP_OK;

    /* Safety: if gpio_conf.h ever goes back to -1 for an SD
     * pin, refuse here with a clear log instead of passing
     * -1 to bsp_sd_mount and failing obscurely in SDMMC. */
    if (BOARD_SD_CLK_GPIO < 0 || BOARD_SD_CMD_GPIO < 0 ||
        BOARD_SD_D0_GPIO < 0 || BOARD_SD_D1_GPIO < 0 ||
        BOARD_SD_D2_GPIO < 0 || BOARD_SD_D3_GPIO < 0) {
        ESP_LOGE(TAG, "SD pins not filled in gpio_conf.h (all -1)");
        return ESP_ERR_INVALID_STATE;
    }

    bsp_sd_config_t cfg = {
        .clk = BOARD_SD_CLK_GPIO,
        .cmd = BOARD_SD_CMD_GPIO,
        .d0 = BOARD_SD_D0_GPIO,
        .d1 = BOARD_SD_D1_GPIO,
        .d2 = BOARD_SD_D2_GPIO,
        .d3 = BOARD_SD_D3_GPIO,
        .width = BOARD_SD_BUS_WIDTH,
        .max_freq_khz = BOARD_SD_MAX_FREQ_KHZ,
        .internal_pullup = BOARD_SD_INTERNAL_PULLUP ? true : false,
    };
    err = bsp_sd_mount(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already mounted by someone else: treat as mounted. */
        return bsp_sd_get_card() != NULL ? ESP_OK : err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sd_mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_list_valid = false;
    return ESP_OK;
}

esp_err_t hw_port_storage_unmount(void)
{
    esp_err_t err;
    if (bsp_sd_get_card() == NULL) return ESP_OK;
    err = bsp_sd_unmount();
    if (err != ESP_OK) return err;
    s_count = 0;
    s_list_valid = false;
    return ESP_OK;
}

int hw_port_storage_is_mounted(void)
{
    return bsp_sd_get_card() != NULL ? 1 : 0;
}

/* Refresh the .avi list from the SD root. Returns the count,
 * or -1 if the card is not mounted. Names are sorted by
 * bsp_fs_list_by_ext and the match is case-insensitive, so
 * .AVI files are included. An empty list after a successful
 * mount means no .avi files, not a mount failure. */
int hw_port_storage_refresh_list(void)
{
    if (bsp_sd_get_card() == NULL) return -1;
    s_count = bsp_fs_list_by_ext(BSP_SD_MOUNT_POINT, ".avi",
                                 s_names, HW_PORT_VIDEO_MAX);
    s_list_valid = true;
    return s_count;
}

int hw_port_storage_video_count(void)
{
    if (!s_list_valid) {
        if (hw_port_storage_refresh_list() < 0) return 0;
    }
    return s_count;
}

const char *hw_port_storage_video_name(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return s_names[index];
}

esp_err_t hw_port_storage_video_path(int index, char *out, size_t out_size)
{
    int n;
    if (out == NULL || out_size == 0) return ESP_ERR_INVALID_ARG;
    if (index < 0 || index >= s_count) return ESP_ERR_INVALID_ARG;
    n = snprintf(out, out_size, "%s/%s", BSP_SD_MOUNT_POINT, s_names[index]);
    if (n < 0 || (size_t)n >= out_size) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

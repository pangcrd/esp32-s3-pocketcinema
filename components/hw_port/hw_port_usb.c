/* USB MSC adapter: card ownership + player stop.
 *
 * The owner callback runs in bsp_usb_msc's owner task,
 * not in the TinyUSB task and not in the LVGL task, so
 * it may block (bounded) but must NOT touch LVGL.
 * It therefore only stops the player and waits for it
 * to let go of the card, then sets a flag. gui_event's
 * 500 ms poll timer, running on the LVGL task, sees
 * that flag and switches screens there.
 *
 * OWNER_USB: stop every file access NOW, before the
 * driver unmounts. avi_player_stop() is a direct flag,
 * avi_player_wait_idle() waits for the file to close.
 * If the player does not go idle in 2 s, the driver
 * still exposes the raw sectors, but a file that was
 * open at that moment can corrupt, so the 2 s bound
 * is a real limit, not a suggestion: do not raise it
 * without also raising the player's cleanup timeout.
 * OWNER_APP: VFS is usable again at this point, the
 * poll timer will refresh the list and auto-play.
 */

#include "avi_player.h"
#include "bsp_sd.h"
#include "bsp_usb_msc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio_conf.h"
#include "hw_port.h"
#include "hw_port_player.h"
#include "hw_port_usb.h"

static const char *TAG = "hw_port_usb";
static bool s_enabled = false;

static void owner_cb(bsp_usb_msc_owner_t owner, void *ctx)
{
    (void)ctx;
    if (owner == BSP_USB_MSC_OWNER_USB) {
        ESP_LOGI(TAG, "USB takes card, stopping player");
        hw_port_player_stop();
        if (!avi_player_wait_idle(2000)) {
            ESP_LOGE(TAG, "player did not go idle in 2 s");
        }
    } else {
        ESP_LOGI(TAG, "card back in app");
    }
}

esp_err_t hw_port_usb_init(void)
{
    esp_err_t err;
    bsp_sd_config_t sd_cfg;
    bsp_usb_msc_config_t usb_cfg;
    if (s_enabled) return ESP_OK;

    if (BOARD_SD_CLK_GPIO < 0 || BOARD_SD_CMD_GPIO < 0 ||
        BOARD_SD_D0_GPIO < 0 || BOARD_SD_D1_GPIO < 0 ||
        BOARD_SD_D2_GPIO < 0 || BOARD_SD_D3_GPIO < 0) {
        ESP_LOGE(TAG, "SD pins not filled in gpio_conf.h");
        return ESP_ERR_INVALID_STATE;
    }
    sd_cfg.clk = BOARD_SD_CLK_GPIO;
    sd_cfg.cmd = BOARD_SD_CMD_GPIO;
    sd_cfg.d0 = BOARD_SD_D0_GPIO;
    sd_cfg.d1 = BOARD_SD_D1_GPIO;
    sd_cfg.d2 = BOARD_SD_D2_GPIO;
    sd_cfg.d3 = BOARD_SD_D3_GPIO;
    sd_cfg.width = BOARD_SD_BUS_WIDTH;
    sd_cfg.max_freq_khz = BOARD_SD_MAX_FREQ_KHZ;
    sd_cfg.internal_pullup = BOARD_SD_INTERNAL_PULLUP ? true : false;

    sdmmc_card_t *card = NULL;
    err = bsp_sd_init_card(&sd_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sd_init_card failed: %s", esp_err_to_name(err));
        return err;
    }
    usb_cfg.card = card;
    usb_cfg.base_path = BSP_SD_MOUNT_POINT;
    usb_cfg.on_owner_change = owner_cb;
    usb_cfg.ctx = NULL;
    err = bsp_usb_msc_init(&usb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_usb_msc_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_enabled = true;
    return ESP_OK;
}

bool hw_port_usb_is_enabled(void)
{
    return s_enabled;
}

bool hw_port_usb_is_app_owner(void)
{
    if (!s_enabled) return false;
    return bsp_usb_msc_is_app_owner();
}

bool hw_port_usb_is_usb_owner(void)
{
    return s_enabled && !bsp_usb_msc_is_app_owner();
}

bool hw_port_usb_is_host_connected(void)
{
    if (!s_enabled) return false;
    return bsp_usb_msc_is_host_connected();
}

bool hw_port_usb_format_required(void)
{
    if (!s_enabled) return false;
    return bsp_usb_msc_format_required();
}

esp_err_t hw_port_usb_eject(void)
{
    bsp_usb_msc_write_stats_t st;
    if (!s_enabled) return ESP_ERR_INVALID_STATE;
    bsp_usb_msc_get_write_stats(&st);
    if (st.active) {
        ESP_LOGW(TAG, "copy in progress, eject ignored, press again after copy finishes");
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_usb_msc_eject();
}

void hw_port_usb_get_write_bytes(uint64_t *bytes, uint32_t *rate_bps, bool *active)
{
    bsp_usb_msc_write_stats_t st;
    if (!s_enabled) {
        if (bytes) *bytes = 0;
        if (rate_bps) *rate_bps = 0;
        if (active) *active = false;
        return;
    }
    bsp_usb_msc_get_write_stats(&st);
    if (bytes) *bytes = st.bytes;
    if (rate_bps) *rate_bps = st.rate_bps;
    if (active) *active = st.active;
}

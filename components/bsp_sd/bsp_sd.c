#include "bsp_sd.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_sd";
static sdmmc_card_t *s_card;

esp_err_t bsp_sd_init_card(const bsp_sd_config_t *cfg, sdmmc_card_t **out_card)
{
    ESP_RETURN_ON_FALSE(cfg && out_card, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(!s_card, ESP_ERR_INVALID_STATE, TAG, "card already initialised");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = cfg->max_freq_khz ? cfg->max_freq_khz : SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = cfg->width;
    slot.clk = cfg->clk;  slot.cmd = cfg->cmd;
    slot.d0 = cfg->d0;    slot.d1 = cfg->d1;
    slot.d2 = cfg->d2;    slot.d3 = cfg->d3;
    if (cfg->internal_pullup) slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init failed");
    esp_err_t ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) { sdmmc_host_deinit(); return ret; }

    sdmmc_card_t *card = calloc(1, sizeof(*card));
    if (!card) { sdmmc_host_deinit(); return ESP_ERR_NO_MEM; }
    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) { free(card); sdmmc_host_deinit(); return ret; }

    sdmmc_card_print_info(stdout, card);
    s_card = card;
    *out_card = card;
    return ESP_OK;
}

esp_err_t bsp_sd_mount(const bsp_sd_config_t *cfg)
{
    if (s_card) return ESP_ERR_INVALID_STATE;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 32 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = cfg->max_freq_khz ? cfg->max_freq_khz : SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = cfg->width;
    slot.clk = cfg->clk;
    slot.cmd = cfg->cmd;
    slot.d0 = cfg->d0;
    slot.d1 = cfg->d1;
    slot.d2 = cfg->d2;
    slot.d3 = cfg->d3;
    if (cfg->internal_pullup) slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        s_card = NULL;
        return ret;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    s_card = NULL;
    return ret;
}

sdmmc_card_t *bsp_sd_get_card(void) { return s_card; }
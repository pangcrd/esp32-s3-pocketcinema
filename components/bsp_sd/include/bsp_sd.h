#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_SD_MOUNT_POINT "/sdcard"

typedef struct {
    int clk, cmd, d0, d1, d2, d3;
    int width;
    int max_freq_khz;
    bool internal_pullup;
} bsp_sd_config_t;

esp_err_t bsp_sd_init_card(const bsp_sd_config_t *cfg, sdmmc_card_t **out_card);
esp_err_t bsp_sd_mount(const bsp_sd_config_t *cfg);
esp_err_t bsp_sd_unmount(void);
sdmmc_card_t *bsp_sd_get_card(void);
esp_err_t bsp_sd_bench(size_t total_bytes, size_t block_size);

void raw_sector_bench(sdmmc_card_t *card);

#ifdef __cplusplus
}
#endif
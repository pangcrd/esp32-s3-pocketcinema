#include "bsp_sd.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "sd_bench";

esp_err_t bsp_sd_bench(size_t total, size_t blk)
{
    if (total == 0 || blk == 0) return ESP_ERR_INVALID_ARG;
    uint8_t *buf = heap_caps_malloc(blk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) return ESP_ERR_NO_MEM;
    memset(buf, 0xA5, blk);

    static uint8_t stream_buf[16 * 1024];   // buffer cho setvbuf, dùng chung cho cả 2 lần mở file

    const char *path = BSP_SD_MOUNT_POINT "/bench.bin";

    FILE *f = fopen(path, "wb");
    if (!f) { heap_caps_free(buf); return ESP_FAIL; }
    setvbuf(f, (char *)stream_buf, _IOFBF, sizeof(stream_buf));

    int64_t t0 = esp_timer_get_time();
    size_t written = 0;
    while (written < total) {
        size_t chunk = total - written < blk ? total - written : blk;
        size_t n = fwrite(buf, 1, chunk, f);
        if (n != chunk) break;
        written += n;
    }
    bool write_error = ferror(f) || written != total;
    if (fclose(f) != 0) write_error = true;
    int64_t t1 = esp_timer_get_time();
    if (write_error) {
        ESP_LOGE(TAG, "write failed: %u/%u bytes", (unsigned)written, (unsigned)total);
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WRITE: %.2f MB/s", written / (double)(t1 - t0));

    f = fopen(path, "rb");
    if (!f) {
        remove(path);
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    setvbuf(f, (char *)stream_buf, _IOFBF, sizeof(stream_buf));

    t0 = esp_timer_get_time();
    size_t read_total = 0;
    while (read_total < total) {
        size_t chunk = total - read_total < blk ? total - read_total : blk;
        size_t n = fread(buf, 1, chunk, f);
        read_total += n;
        if (n != chunk) break;
    }
    bool read_error = ferror(f) || read_total != total;
    if (fclose(f) != 0) read_error = true;
    t1 = esp_timer_get_time();
    if (read_error) {
        ESP_LOGE(TAG, "read failed: %u/%u bytes", (unsigned)read_total, (unsigned)total);
        remove(path);
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "READ:  %.2f MB/s", read_total / (double)(t1 - t0));

    remove(path);
    heap_caps_free(buf);
    return ESP_OK;
}

void raw_sector_bench(sdmmc_card_t *card)
{
    if (!card) return;
    const int n_sectors = 2048;              // 2048 * 512B = 1MB
    uint8_t *buf = (uint8_t *)heap_caps_malloc(512 * 32, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE("raw_bench", "buffer allocation failed");
        return;
    }

    int64_t t0 = esp_timer_get_time();
    int sectors_read = 0;
    for (int i = 0; i < n_sectors; i += 32) {
        esp_err_t ret = sdmmc_read_sectors(card, buf, i, 32);
        if (ret != ESP_OK) {
            ESP_LOGE("raw_bench", "read failed at sector %d: %s", i, esp_err_to_name(ret));
            break;
        }
        sectors_read += 32;
    }
    int64_t t1 = esp_timer_get_time();
    double mb = (sectors_read * 512) / 1e6;
    ESP_LOGI("raw_bench", "RAW READ: %.2f MB/s (%d sectors)", mb / ((t1 - t0) / 1e6), sectors_read);

    heap_caps_free(buf);
}

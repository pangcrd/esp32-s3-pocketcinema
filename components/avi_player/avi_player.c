#include "avi_player.h"
#include <string.h>
#include "avi_parser.h"
#include "jpeg_dec.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/event_groups.h"

static const char *TAG = "av_player";

#define CHUNK_BUF_SIZE            (64 * 1024)   /* biggest video/audio chunk we can hold; 24K dropped 320x240 JPEG frames over ~24K, 64K in PSRAM is still small vs 8 MB PSRAM */
#define AUDIO_STREAM_BYTES        (16 * 1024)   /* ~500 ms of 16 kHz mono 16-bit */
#define AUDIO_TASK_STACK          4096
#define AUDIO_TASK_PRIO           6
#define AUDIO_TASK_CORE           1             /* keep audio off the core that decodes JPEG */
#define AUDIO_WRITE_CHUNK         2048
#define DEFAULT_AUDIO_LATENCY_MS  80
#define LOOP_GAP_SILENCE_BYTES    (BSP_AUDIO_SAMPLE_RATE / 10 * 2)   /* 100 ms */

static volatile bool s_stop;
static volatile bool s_paused;
static TaskHandle_t s_audio_task;
static const uint8_t s_silence[LOOP_GAP_SILENCE_BYTES] = {0};

/* Lets another task tell when the player has really let go of the card, which
 * is what the USB MSC owner callback waits for before the driver unmounts.
 * Created on first use; avi_player_play() is the only writer. */
#define EVT_IDLE  BIT0

static EventGroupHandle_t s_evt;

static EventGroupHandle_t idle_evt(void)
{
    /* No lock: the first call happens from app_main before any other task can
     * reach the player. */
    if (!s_evt) {
        s_evt = xEventGroupCreate();
        if (s_evt) xEventGroupSetBits(s_evt, EVT_IDLE);
    }
    return s_evt;
}

typedef struct {
    StreamBufferHandle_t sb;
    TaskHandle_t         owner;
    volatile bool        quit;
} audio_ctx_t;

/* Pulls PCM from the stream buffer and feeds I2S. i2s write blocks when the DMA is full,
 * so this task naturally runs at the real audio rate. */
static void audio_task(void *arg)
{
    audio_ctx_t *c = (audio_ctx_t *)arg;
    /* SỬA 2: MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM không có vùng nhớ nào thỏa
     * cả 2 caps -> malloc trả NULL -> mất tiếng. Buffer này chỉ là staging
     * cho bsp_audio_write (nó tự copy vào DMA nội của driver) nên không
     * cần DMA cap. */
    uint8_t *buf = heap_caps_malloc(AUDIO_WRITE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t have = 0;   /* 0 or 1 leftover byte, keeps samples 2-byte aligned */

    if (!buf) ESP_LOGE(TAG, "audio buffer alloc failed, no sound");

    while (buf && !c->quit) {
        size_t n = xStreamBufferReceive(c->sb, buf + have, AUDIO_WRITE_CHUNK - have, pdMS_TO_TICKS(50));
        size_t total = have + n;
        size_t even = total & ~(size_t)1;
        if (even) {
            size_t w = 0;
            bsp_audio_write(buf, even, &w, portMAX_DELAY);
        }
        if (total & 1) {
            buf[0] = buf[total - 1];
            have = 1;
        } else {
            have = 0;
        }
    }

    heap_caps_free(buf);
    xTaskNotifyGive(c->owner);
    vTaskDelete(NULL);
}

void avi_player_stop(void)
{
    s_stop = true;
    /* If paused, wake the audio task so cleanup can exit
     * it instead of timing out after 2 s. */
    if (s_audio_task != NULL) vTaskResume(s_audio_task);
}

void avi_player_pause(bool pause)
{
    s_paused = pause;
    if (s_audio_task != NULL) {
        if (pause) vTaskSuspend(s_audio_task);
        else vTaskResume(s_audio_task);
    }
}

bool avi_player_is_paused(void)
{
    return s_paused;
}

bool avi_player_wait_idle(uint32_t timeout_ms)
{
    EventGroupHandle_t evt = idle_evt();
    if (!evt) return true;   /* never started, so nothing holds the card */

    EventBits_t bits = xEventGroupWaitBits(evt, EVT_IDLE, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & EVT_IDLE) != 0;
}

bool avi_player_is_busy(void)
{
    EventGroupHandle_t evt = idle_evt();
    if (!evt) return false;
    return (xEventGroupGetBits(evt) & EVT_IDLE) == 0;
}

esp_err_t avi_player_play(const av_player_config_t *cfg, const char *path)
{
    if (!cfg || !cfg->draw_cb || !path) return ESP_ERR_INVALID_ARG;

    avi_info_t info;
    avi_parser_handle_t avi = NULL;
    uint8_t *chunk_buf = NULL;
    uint8_t *pix[4] = {NULL, NULL, NULL, NULL};
    audio_ctx_t actx = {0};
    bool use_audio = false, audio_started = false, task_started = false;
    esp_err_t ret;

    s_stop = false;
    s_paused = false;

    /* Mark the card busy before the first open() so a USB owner change that
     * races with us still waits for the close below. */
    EventGroupHandle_t evt = idle_evt();
    if (evt) xEventGroupClearBits(evt, EVT_IDLE);

    ret = avi_parser_open(path, &avi, &info);
    if (ret != ESP_OK) {
        /* Fail ngay trước khi vào vòng play (thường là không mở được file hoặc
         * container không hợp lệ). Chi tiết nằm ở log của avi_parser. */
        ESP_LOGE(TAG, "avi_parser_open('%s') failed: %s", path, esp_err_to_name(ret));
        if (evt) xEventGroupSetBits(evt, EVT_IDLE);
        return ret;
    }

    const double fps = info.fps > 0 ? info.fps : 15.0;
    const int64_t frame_us = (int64_t)(1e6 / fps);
    const int latency_ms = cfg->audio_latency_ms > 0 ? cfg->audio_latency_ms : DEFAULT_AUDIO_LATENCY_MS;
    const size_t pix_size = (size_t)info.width * info.height * 2;

    /* SỬA 3: chunk_buf sang PSRAM (cùng lý do SỬA 1). Giữ nguyên vị trí alloc
     * sau khi open thành công để nhánh lỗi open phía trên không bị leak. */
    chunk_buf = heap_caps_malloc(CHUNK_BUF_SIZE, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 4; i++) pix[i] = jpeg_dec_pixbuf_alloc(info.width, info.height);
    if (!chunk_buf || !pix[0] || !pix[1] || !pix[2] || !pix[3]) {
        ESP_LOGE(TAG, "buffer allocation failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* The audio path only accepts the format bsp_audio was configured for */
    use_audio = info.has_audio;
    if (use_audio && (info.audio_format_tag != 1 ||
                      info.audio_channels != BSP_AUDIO_CHANNELS ||
                      info.audio_bits != BSP_AUDIO_BITS ||
                      info.audio_sample_rate != BSP_AUDIO_SAMPLE_RATE)) {
        ESP_LOGW(TAG, "audio is not PCM %d Hz %d ch %d bit, playing video only",
                 BSP_AUDIO_SAMPLE_RATE, BSP_AUDIO_CHANNELS, BSP_AUDIO_BITS);
        use_audio = false;
    }

    if (use_audio) {
        actx.sb = xStreamBufferCreate(AUDIO_STREAM_BYTES, 1);
        actx.owner = xTaskGetCurrentTaskHandle();
        if (!actx.sb || bsp_audio_start() != ESP_OK) {
            ESP_LOGW(TAG, "audio unavailable, playing video only");
            use_audio = false;
        } else {
            audio_started = true;
            if (xTaskCreatePinnedToCore(audio_task, "av_audio", AUDIO_TASK_STACK, &actx,
                                        AUDIO_TASK_PRIO, &s_audio_task, AUDIO_TASK_CORE) == pdPASS) {
                task_started = true;
            } else {
                ESP_LOGW(TAG, "audio task create failed, playing video only");
                use_audio = false;
            }
        }
    }

    ESP_LOGI(TAG, "playing %s: %ux%u @ %.2f fps, audio %s", path, info.width, info.height, fps,
             use_audio ? "on" : "off");

    do {
        avi_parser_rewind(avi);

        uint32_t n = 0, shown = 0, dropped = 0;
        int cur = 0;
        /* Frame 0 is due after the audio latency, so picture and sound line up */
        int64_t t0 = esp_timer_get_time() + (use_audio ? (int64_t)latency_ms * 1000 : 0);

        while (!s_stop) {
            if (s_paused) {
                /* Freeze here, before reading the next chunk,
                 * so the parser stays exactly at the resume
                 * point. Shift t0 by the pause length, or
                 * every remaining frame would be late and
                 * dropped on resume. */
                const int64_t pause_start = esp_timer_get_time();
                while (s_paused && !s_stop) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                t0 += esp_timer_get_time() - pause_start;
                if (s_stop) break;
            }
            avi_chunk_type_t type;
            size_t len = 0;
            esp_err_t r = avi_parser_read_chunk(avi, &type, chunk_buf, CHUNK_BUF_SIZE, &len);
            if (type == AVI_CHUNK_EOF) break;

            if (type == AVI_CHUNK_AUDIO) {
                if (use_audio && r == ESP_OK && len) {
                    /* Blocks if the buffer is full: this is what keeps video from running ahead of audio */
                    size_t sent = xStreamBufferSend(actx.sb, chunk_buf, len, pdMS_TO_TICKS(500));
                    if (sent < len) {
                        ESP_LOGW(TAG, "audio buffer full, dropped %u bytes", (unsigned)(len - sent));
                    }
                }
                continue;
            }

            /* Video chunk */
            const int64_t due = t0 + (int64_t)n * frame_us;
            n++;
            if (r != ESP_OK) { dropped++; continue; }

            /* More than one frame late: skip the decode to catch up */
            if (esp_timer_get_time() > due + frame_us) { dropped++; continue; }

            uint16_t w = 0, h = 0;
            if (jpeg_dec_buffer(chunk_buf, len, cfg->swap_bytes, cfg->rotate,
                                pix[cur], pix_size, &w, &h) != ESP_OK) {
                dropped++;
                continue;
            }

            /* Wait until the frame is due, then show it */
            int64_t now = esp_timer_get_time();
            if (due > now) {
                vTaskDelay(pdMS_TO_TICKS((due - now) / 1000));
            }

            int x = (cfg->screen_w > w) ? (cfg->screen_w - w) / 2 : 0;
            int y = (cfg->screen_h > h) ? (cfg->screen_h - h) / 2 : 0;
            cfg->draw_cb(x, y, w, h, pix[cur], cfg->draw_ctx);
            cur = (cur + 1) % 4;
            shown++;
            if (cfg->progress_cb) {
                cfg->progress_cb((int)shown, (int)info.total_frames, fps, cfg->progress_ctx);
            }
        }

        const double secs = (esp_timer_get_time() - t0) / 1e6;
        ESP_LOGI(TAG, "pass done: %u frames, %u shown, %u dropped, %.2f fps shown",
                 (unsigned)n, (unsigned)shown, (unsigned)dropped, secs > 0 ? shown / secs : 0.0);

        /* Let the tail of the audio play out before restarting so the next pass stays in sync */
        if (use_audio && !s_stop) {
            if (cfg->loop) {
                xStreamBufferSend(actx.sb, s_silence, sizeof(s_silence), pdMS_TO_TICKS(500));
            }
            while (!s_stop && xStreamBufferBytesAvailable(actx.sb) > 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    } while (cfg->loop && !s_stop);

    ret = ESP_OK;

cleanup:
    if (task_started) {
        if (s_audio_task != NULL) vTaskResume(s_audio_task);
        actx.quit = true;
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
            ESP_LOGE(TAG, "audio task did not exit in time");
        }
    }
    if (audio_started) bsp_audio_stop();
    if (actx.sb) vStreamBufferDelete(actx.sb);
    for (int i = 0; i < 4; i++) jpeg_dec_pixbuf_free(pix[i]);
    heap_caps_free(chunk_buf);
    avi_parser_close(avi);
    /* The file is closed: whoever waits for the card can have it now. */
    if (evt) xEventGroupSetBits(evt, EVT_IDLE);
    return ret;
}

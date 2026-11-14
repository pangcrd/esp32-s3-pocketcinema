#include <string.h>

#include "avi_player.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "hw_port.h"
#include "hw_port_player.h"
#include "hw_port_usb.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "hw_port_player";
#define PLAYER_TASK_STACK 8192
#define PLAYER_TASK_PRIO 5
#define PLAYER_TASK_CORE 0

typedef enum { CMD_PLAY_INDEX, CMD_STOP } player_cmd_kind_t;
typedef struct {
    player_cmd_kind_t kind;
    int index;
} player_cmd_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_playing = false;
static volatile int s_index = -1;
static volatile int s_frame = 0;
static volatile int s_total_frames = 0;
static volatile double s_fps = 0;
static lv_obj_t *s_video_img = NULL;
/* LVGL image descriptor, reused for every frame. The data
 * pointer is the pix buffer avi_player just decoded. */
static lv_image_dsc_t s_img_dsc;

void hw_port_player_set_video_obj(struct _lv_obj_t *obj)
{
    s_video_img = (lv_obj_t *)obj;
}

static void progress_cb(int frame, int total_frames, double fps, void *ctx)
{
    (void)ctx;
    s_frame = frame;
    s_total_frames = total_frames;
    s_fps = fps;
}

/* Runs on the player task, after avi_player decoded a frame
 * into rgb565 and waited until it is due. Only swaps the
 * image buffer pointer under the LVGL lock, then returns
 * immediately so the player's frame timing stays exact. */
static void draw_cb(int x, int y, int w, int h, const uint8_t *rgb565, void *ctx)
{
    (void)x;
    (void)y;
    (void)ctx;
    if (s_video_img == NULL) return;
    if (!lvgl_port_lock(-1)) return;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = (uint16_t)w;
    s_img_dsc.header.h = (uint16_t)h;
    s_img_dsc.header.stride = (uint16_t)(w * 2);
    s_img_dsc.data = rgb565;
    s_img_dsc.data_size = (size_t)w * h * 2;
    lv_image_set_src(s_video_img, &s_img_dsc);
    lv_obj_invalidate(s_video_img);
    lvgl_port_unlock();
}

static void player_task(void *arg)
{
    (void)arg;
    player_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd.kind == CMD_STOP) {
            avi_player_stop();
            continue;
        }
        /* CMD_PLAY_INDEX: stop anything running first, wait
         * until it really let go of the card, then start. */
        if (s_playing) {
            avi_player_stop();
            avi_player_wait_idle(2000);
        }
        if (hw_port_storage_is_mounted() == 0) {
            if (hw_port_storage_mount() != ESP_OK) {
                ESP_LOGE(TAG, "SD mount failed, cannot play");
                continue;
            }
        }
        if (hw_port_storage_refresh_list() < 0) {
            ESP_LOGE(TAG, "SD list failed, cannot play");
            continue;
        }
        char path[512];
        if (hw_port_storage_video_path(cmd.index, path, sizeof(path)) != ESP_OK) {
            ESP_LOGE(TAG, "no video at index %d", cmd.index);
            continue;
        }
        s_index = cmd.index;
        s_frame = 0;
        s_total_frames = 0;
        s_fps = 0;
        av_player_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.draw_cb = draw_cb;
        cfg.draw_ctx = NULL;
        /* LVGL path: no byte swap and no decoder rotation.
         * LVGL renders the image into its own draw buffer
         * and applies the panel's swap/rotation in flush,
         * exactly as it does for the UI. Setting either
         * here would double-apply it. */
        cfg.swap_bytes = false;
        cfg.rotate = JPEG_DEC_ROTATE_0;
        /* Loop the CURRENT video only: avi_player already
         * supports this (do..while loop, audio tail + 100 ms
         * silence gap between passes). Next/prev still work:
         * play_index() calls avi_player_stop() first, which
         * exits this loop, then starts the new index with
         * loop=true again, so the new video loops instead.
         * No auto-advance to the next file on end. */
        cfg.loop = true;
        /* 80 ms default inside avi_player (audio_latency_ms
         * 0). Tune only from a lip-sync test, in 20 ms
         * steps: video ahead of sound -> raise it, sound
         * ahead of video -> lower it. */
        cfg.audio_latency_ms = 0;
        cfg.screen_w = 320;
        cfg.screen_h = 240;
        cfg.progress_cb = progress_cb;
        cfg.progress_ctx = NULL;
        s_playing = true;
        /* Blocks until the file ends or stop is posted.
         * The queue is drained after, so a stop posted
         * during play does not start a stale file. */
        avi_player_play(&cfg, path);
        s_playing = false;
        /* Do not invalidate here: s_img_dsc still points to the
         * pix buffer avi_player just freed, redrawing it would
         * show a stale/black frame for one frame on video switch.
         * Keep the last frame until the next file draws its first frame. */
    }
}

esp_err_t hw_port_player_init(void)
{
    if (s_task != NULL) return ESP_OK;
    s_queue = xQueueCreate(4, sizeof(player_cmd_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(player_task, "hw_player", PLAYER_TASK_STACK,
                                NULL, PLAYER_TASK_PRIO, &s_task,
                                PLAYER_TASK_CORE) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t post_cmd(player_cmd_kind_t kind, int index)
{
    player_cmd_t cmd = { .kind = kind, .index = index };
    if (s_queue == NULL) {
        if (hw_port_player_init() != ESP_OK) return ESP_FAIL;
    }
    return xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t hw_port_player_play_index(int index)
{
    /* Never play while the PC owns the card: VFS is
     * not usable then, and reading it would race the
     * host's writes. */
    if (hw_port_usb_is_enabled() && !hw_port_usb_is_app_owner()) {
        ESP_LOGE(TAG, "card owned by USB, cannot play");
        return ESP_ERR_INVALID_STATE;
    }
    /* Direct stop first: the player task is blocked inside
     * avi_player_play() while playing, so it cannot read
     * a queued stop/play command until the file ends.
     * avi_player_stop() sets the flag that makes play
     * return, then the queued play command starts the
     * new file. Without this, stop / next / prev while
     * playing did nothing until end of file. */
    if (s_playing) avi_player_stop();
    return post_cmd(CMD_PLAY_INDEX, index);
}

esp_err_t hw_port_player_stop(void)
{
    /* Direct flag, not a queue command, for the same
     * reason: the player task cannot read the queue
     * while it is inside avi_player_play(). */
    avi_player_stop();
    return ESP_OK;
}

esp_err_t hw_port_player_pause(void)
{
    if (!s_playing) return ESP_ERR_INVALID_STATE;
    avi_player_pause(true);
    return ESP_OK;
}

esp_err_t hw_port_player_resume(void)
{
    avi_player_pause(false);
    return ESP_OK;
}

bool hw_port_player_is_paused(void)
{
    return avi_player_is_paused();
}

esp_err_t hw_port_player_toggle(void)
{
    if (avi_player_is_paused()) return hw_port_player_resume();
    if (s_playing) return hw_port_player_pause();
    int idx = s_index >= 0 ? s_index : 0;
    return hw_port_player_play_index(idx);
}

esp_err_t hw_port_player_next(void)
{
    int count = hw_port_storage_video_count();
    int idx = s_index + 1;
    if (count > 0 && idx >= count) idx = 0;
    return hw_port_player_play_index(idx);
}

esp_err_t hw_port_player_prev(void)
{
    int count = hw_port_storage_video_count();
    int idx = s_index - 1;
    if (idx < 0) idx = count > 0 ? count - 1 : 0;
    return hw_port_player_play_index(idx);
}

bool hw_port_player_is_playing(void)
{
    return s_playing && !avi_player_is_paused();
}

int hw_port_player_get_index(void)
{
    return s_index;
}

int hw_port_player_get_progress_percent(void)
{
    if (s_total_frames <= 0) return 0;
    int p = (int)((int64_t)s_frame * 100 / s_total_frames);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

int hw_port_player_get_frame(void)
{
    return s_frame;
}

int hw_port_player_get_total_frames(void)
{
    return s_total_frames;
}

double hw_port_player_get_fps(void)
{
    return s_fps;
}
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jpeg_dec.h"   /* jpeg_dec_rotate_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per frame to put an RGB565 image on screen. Keeps av_player independent of the LCD library. */
typedef void (*av_draw_cb_t)(int x, int y, int w, int h, const uint8_t *rgb565, void *ctx);

/* Called from the player task after each frame is drawn.
 * frame is 1-based within the current pass, total_frames is
 * 0 when the AVI header does not record it. Used only for
 * the progress slider / time labels; NULL = no progress. */
typedef void (*av_progress_cb_t)(int frame, int total_frames, double fps, void *ctx);

typedef struct {
    av_draw_cb_t draw_cb;
    void        *draw_ctx;
    bool         swap_bytes;        /* RGB565 byte order the panel expects */
    jpeg_dec_rotate_t rotate;       /* frame rotation done inside the JPEG decoder */
    bool         loop;              /* restart from the beginning at end of file */
    int          audio_latency_ms;  /* video start delay to match I2S/DMA latency; 0 = default (80) */
    int          screen_w;          /* used to center the frame; 0 = draw at x = 0 */
    int          screen_h;          /* used to center the frame; 0 = draw at y = 0 */
    av_progress_cb_t progress_cb;
    void        *progress_ctx;
} av_player_config_t;

/* Play an AVI (MJPEG video + optional PCM 16 kHz mono 16-bit audio).
 * Blocks until the file ends (or forever when cfg->loop is set) or av_player_stop() is called.
 * bsp_audio_init() must have been called beforehand if the file has audio. */
esp_err_t avi_player_play(const av_player_config_t *cfg, const char *path);

/* Ask a running av_player_play() to return. Safe to call from another task. */
void avi_player_stop(void);

/* Pause / resume a running play, at the current frame.
 * Pause freezes the video loop and suspends the audio
 * task, so sound stops within one DMA buffer and resumes
 * from exactly the same point, with no audio skip. It
 * does NOT close the file or stop the codec, so there is
 * no pop on resume. Safe to call from another task. */
void avi_player_pause(bool pause);
bool avi_player_is_paused(void);

/* Block until no avi_player_play() is running, i.e. the file handle is closed
 * and the SD card is no longer touched by the player. Returns false on timeout.
 * Pair it with avi_player_stop() before handing the card to a USB host. */
bool avi_player_wait_idle(uint32_t timeout_ms);

/* true while avi_player_play() holds a file open. */
bool avi_player_is_busy(void);

#ifdef __cplusplus
}
#endif
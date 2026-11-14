#ifndef HW_PORT_PLAYER_H
#define HW_PORT_PLAYER_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Player task + command queue. gui_event never calls
 * avi_player_play() directly: it blocks until the file
 * ends, so calling it from an LVGL event would freeze the
 * UI and break A/V timing. gui_event only posts play /
 * stop / next / prev here, and polls status with its
 * 500 ms timer. */

/* Video image object, created by gui_event on the main
 * player screen (full screen, behind the control panel).
 * The player task swaps this object's buffer, it never
 * draws directly to the panel: direct panel writes would
 * race LVGL's flush and tear, and would erase the UI
 * overlay. Set it before the first play, NULL clears it. */
struct _lv_obj_t;
void hw_port_player_set_video_obj(struct _lv_obj_t *obj);

esp_err_t hw_port_player_init(void);
/* Play the file at index in the hw_port_storage list. */
esp_err_t hw_port_player_play_index(int index);
/* Play / pause / resume toggle used by the play/pause
 * buttons: stopped -> play, playing -> pause, paused ->
 * resume. Stop is separate and restarts from the start. */
esp_err_t hw_port_player_toggle(void);
esp_err_t hw_port_player_pause(void);
esp_err_t hw_port_player_resume(void);
bool hw_port_player_is_paused(void);
esp_err_t hw_port_player_stop(void);
esp_err_t hw_port_player_next(void);
esp_err_t hw_port_player_prev(void);

bool hw_port_player_is_playing(void); /* false while paused */
int hw_port_player_get_index(void);
/* 0-100, 0 when total frames is unknown. */
int hw_port_player_get_progress_percent(void);
int hw_port_player_get_frame(void);
int hw_port_player_get_total_frames(void);
double hw_port_player_get_fps(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif

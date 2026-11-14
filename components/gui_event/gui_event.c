/* Independent app events for the SquareLine video-player UI.
 * Keep this file in components/gui_event/, outside the generated
 * ui/ folder. Re-exporting from SquareLine replaces ui/ only,
 * this file survives. Brightness here drives real hardware
 * through hw_port, it is not UI-only anymore.
 * Generated handlers in ui_main_player.c still run first; the handlers
 * here run second, so the volume mute-icon correction below wins.
 * LVGL 9.3, screen 320x240 landscape (size read at runtime, not assumed).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "ui.h"
#include "gui_event.h"
#include "hw_port.h"
#include "hw_port_player.h"
#include "hw_port_usb.h"
#include "esp_log.h"

static const char *TAG = "gui_event";

#define PANEL_HIDE_MS 3000
#define STARTUP_MS 5000
#define EDGE_ZONE_DIV 5
#define GESTURE_STEP 10

typedef enum {
    EDGE_NONE = 0,
    EDGE_LEFT,
    EDGE_RIGHT
} edge_side_t;

static lv_timer_t * s_startup_timer = NULL;
static lv_timer_t * s_panel_timer = NULL;
/* Moved here from the poll section: startup_timer_cb (above)
 * reads this flag, so it must be declared before that
 * function, otherwise 'undeclared (first use)' at build. */
static bool s_on_usb_screen = false;
static edge_side_t s_edge = EDGE_NONE;
static int32_t s_edge_start_y = 0;
static int32_t s_edge_start_value = 0;
static bool s_edge_dragged = false;
static int s_press_count = 0; /* debounce: ignore single-poll phantom touch */
static bool s_panel_shown = false;
static bool s_playing = false;
static bool s_inited = false;

static int32_t clamp_0_100(int32_t v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static int32_t screen_w(void)
{
    lv_display_t * d = lv_display_get_default();
    if (d == NULL) return 320;
    return (int32_t)lv_display_get_horizontal_resolution(d);
}

static int32_t screen_h(void)
{
    lv_display_t * d = lv_display_get_default();
    if (d == NULL) return 240;
    return (int32_t)lv_display_get_vertical_resolution(d);
}

static void set_hidden(lv_obj_t * obj, bool hidden)
{
    if (obj == NULL) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Re-arm the 3 s auto-hide timer without changing what is visible.
 * Use this while dragging or sliding, so the overlay never changes mode
 * or hides in the middle of an interaction. */
static void panel_arm_timer(void)
{
    if (s_panel_timer != NULL) {
        lv_timer_reset(s_panel_timer);
        lv_timer_resume(s_panel_timer);
    }
}

/* Full player UI: header, play controls, footer and system controls. */
static void show_full_panel(void)
{
    if (ui_control_panel == NULL) return;
    set_hidden(ui_control_panel, false);
    set_hidden(ui_header_status, false);
    set_hidden(ui_player_control, false);
    set_hidden(ui_footer, false);
    set_hidden(ui_system_ctrl, true);
    panel_arm_timer();
}

static void panel_hide_now(void)
{
    if (ui_control_panel != NULL) {
        lv_obj_add_flag(ui_control_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_panel_timer != NULL) {
        lv_timer_pause(s_panel_timer);
    }
}

static void panel_timer_cb(lv_timer_t * t)
{
    (void)t;
    panel_hide_now();
}

/* Clean overlay, like a mobile web player: only ui_system_ctrl appears.
 * ui_system_ctrl is a child of ui_control_panel, so the panel itself must
 * stay visible for it to show at all. That is fine visually because the
 * panel has remove_style_all (transparent); the clutter comes from its
 * other children, so this hides header, play controls and footer and
 * leaves only the selected brightness/volume row visible. */
static void show_system_ctrl(bool brightness)
{
    if (ui_control_panel == NULL) return;
    set_hidden(ui_control_panel, false);
    set_hidden(ui_header_status, true);
    set_hidden(ui_player_control, true);
    set_hidden(ui_footer, true);
    set_hidden(ui_system_ctrl, false);
    if (brightness) {
        set_hidden(ui_brightness_ctrl, false);
        set_hidden(ui_volume_ctrl, true);
    } else {
        set_hidden(ui_volume_ctrl, false);
        set_hidden(ui_brightness_ctrl, true);
    }
    panel_arm_timer();
}

/* Update volume label and icon only; never touches the slider itself. */
static void update_volume_label_icon(int32_t v)
{
    if (ui_volume_val != NULL) {
        lv_label_set_text_fmt(ui_volume_val, "%d%%", (int)v);
    }
    if (ui_volume_icon != NULL) {
        /* No separate ui_mute_icon object exists in the generated code:
         * mute is the volume icon swapped to the mute image. */
        lv_image_set_src(ui_volume_icon,
                         (v <= 0) ? &ui_img_icon_mute_png : &ui_img_icon_volume_up_png);
    }
}

static void update_brightness_label(int32_t v)
{
    if (ui_brightness_val != NULL) {
        lv_label_set_text_fmt(ui_brightness_val, "%d%%", (int)v);
    }
}

static void set_volume(int32_t v)
{
    v = clamp_0_100(v);
    if (ui_volume_slide != NULL) {
        lv_slider_set_value(ui_volume_slide, v, LV_ANIM_OFF);
    }
    update_volume_label_icon(v);
    /* Hardware: codec volume follows the same state. If audio
     * init failed, ignore here so the UI still works. */
    (void)hw_port_audio_set_volume((int)v);
}

static void set_brightness(int32_t v)
{
    v = clamp_0_100(v);
    if (ui_brightness_slide != NULL) {
        lv_slider_set_value(ui_brightness_slide, v, LV_ANIM_OFF);
    }
    update_brightness_label(v);
    /* Hardware, via hw_port only. If LEDC init fails, the UI
     * still updates and the next change retries it. */
    (void)hw_port_brightness_set((int)v);
}

/* After 5 s on the startup screen, load the main player screen. */
static void startup_timer_cb(lv_timer_t * t)
{
    (void)t;
    s_startup_timer = NULL; /* repeat_count 1: LVGL frees the timer itself */
    /* Single-screen rule: only one timer may decide the active
     * screen. The poll timer owns USB vs main switching via
     * switch_usb_screen(). If the PC already owns the card
     * when this 5 s timer fires (log proof: USB screen on
     * at 1905 ms, this timer fired at 6435 ms and loaded
     * main_player on top of it, then 'no video at index 0'
     * because the card was not usable), loading main_player
     * here fights the USB screen: s_on_usb_screen stays
     * true, poll returns early, and the visible main_player
     * is never updated (video hidden, panel hidden -> white).
     * Defer instead: when the card comes back, the poll
     * timer's return path loads main_player + auto-play. */
    if (s_on_usb_screen || (hw_port_usb_is_enabled() && hw_port_usb_is_usb_owner())) {
        ESP_LOGI(TAG, "startup deferred, USB owns card, poll will load main player on return");
        return;
    }
    if (ui_main_player == NULL) {
        ui_main_player_screen_init();
    }
    lv_screen_load(ui_main_player);
    /* Auto-play: previously nothing started playback here,
     * so the main screen stayed empty until the user found
     * the hidden play button. Start the first .avi on the
     * card now; the 500 ms poll syncs the play/pause
     * buttons, and a failed start (no card, no .avi) just
     * leaves the play button visible. */
    ESP_LOGI(TAG, "main player loaded, auto-play index 0");
    (void)hw_port_player_play_index(0);
}

/* Screen touch: tap shows the panel, edge drag adjusts value, flick is a fallback. */
static void on_main_player_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_event_get_indev(e);

    if (code == LV_EVENT_PRESSED) {
        s_edge = EDGE_NONE;
        s_edge_dragged = false;
        s_press_count = 1;
        s_panel_shown = false;
        if (indev == NULL) {
            show_full_panel();
            s_panel_shown = true;
            return;
        }
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        /* Ghost touch: FT6336G I2C noise returns (0,0) or out-of-range. */
        if ((pt.x == 0 && pt.y == 0) || pt.x < 0 || pt.y < 0 ||
            pt.x >= screen_w() || pt.y >= screen_h()) {
            ESP_LOGI(TAG, "ghost press ignored (%d,%d)", (int)pt.x, (int)pt.y);
            s_edge = EDGE_NONE;
            s_press_count = 0;
            return;
        }
        int32_t w = screen_w();
        if (pt.x < w / EDGE_ZONE_DIV) {
            s_edge = EDGE_LEFT;
            s_edge_start_y = pt.y;
            s_edge_start_value = (ui_brightness_slide != NULL)
                                     ? lv_slider_get_value(ui_brightness_slide)
                                     : 40;
        } else if (pt.x > w * (EDGE_ZONE_DIV - 1) / EDGE_ZONE_DIV) {
            s_edge = EDGE_RIGHT;
            s_edge_start_y = pt.y;
            s_edge_start_value = (ui_volume_slide != NULL)
                                     ? lv_slider_get_value(ui_volume_slide)
                                     : 40;
        }
        /* Do not show yet: wait for 2nd PRESSING, single-poll ghost is ignored. */
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (indev == NULL) return;
        s_press_count++;
        if (!s_panel_shown && s_press_count >= 2) {
            if (s_edge == EDGE_LEFT) show_system_ctrl(true);
            else if (s_edge == EDGE_RIGHT) show_system_ctrl(false);
            else show_full_panel();
            s_panel_shown = true;
        }
        if (s_edge == EDGE_NONE) return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        int32_t h = screen_h();
        if (h <= 0) return;
        /* Drag up (y decreases) raises the value, drag down lowers it.
         * Full screen height maps to the full 0-100 range. */
        int32_t v = clamp_0_100(s_edge_start_value + (s_edge_start_y - pt.y) * 100 / h);
        s_edge_dragged = true;
        if (s_edge == EDGE_LEFT) {
            set_brightness(v);
        } else {
            set_volume(v);
        }
        panel_arm_timer();
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (!s_panel_shown) {
            /* Single-poll ghost or too-short tap: do not show panel. */
            s_edge = EDGE_NONE;
            s_press_count = 0;
            return;
        }
        s_edge = EDGE_NONE;
        s_press_count = 0;
        panel_arm_timer(); /* arm the 3 s timer from release, not from press */
        return;
    }

    if (code == LV_EVENT_GESTURE) {
        if (indev == NULL) return;
        if (s_press_count < 2 && !s_panel_shown) return; /* ghost gesture */
        s_press_count = 0;
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        int32_t w = screen_w();
        bool brightness;
        if (pt.x < w / EDGE_ZONE_DIV) {
            brightness = true;
        } else if (pt.x > w * (EDGE_ZONE_DIV - 1) / EDGE_ZONE_DIV) {
            brightness = false;
        } else {
            return;
        }
        show_system_ctrl(brightness);
        /* Only step here if the continuous PRESSING drag did not already
         * adjust the value, otherwise a flick would count twice. */
        if (!s_edge_dragged) {
            int32_t step = (dir == LV_DIR_TOP) ? GESTURE_STEP : -GESTURE_STEP;
            if (brightness) {
                int32_t cur = (ui_brightness_slide != NULL)
                                  ? lv_slider_get_value(ui_brightness_slide)
                                  : 40;
                set_brightness(cur + step);
            } else {
                int32_t cur = (ui_volume_slide != NULL)
                                  ? lv_slider_get_value(ui_volume_slide)
                                  : 40;
                set_volume(cur + step);
            }
        }
        /* show_system_ctrl() already re-armed the timer. */
        return;
    }
}

static void on_volume_slide(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (ui_volume_slide == NULL) return;
    update_volume_label_icon(lv_slider_get_value(ui_volume_slide));
    (void)hw_port_audio_set_volume((int)lv_slider_get_value(ui_volume_slide));
    panel_arm_timer();
}

static void on_brightness_slide(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (ui_brightness_slide == NULL) return;
    {
        int32_t v = lv_slider_get_value(ui_brightness_slide);
        update_brightness_label(v);
        (void)hw_port_brightness_set((int)v);
    }
    panel_arm_timer();
}

static lv_obj_t *s_video_img = NULL;
static lv_timer_t *s_poll_timer = NULL;
static int s_poll_count = 0;
static int s_last_title_index = -1;
static int s_last_battery_level = -1;

/* USB screen switch, runs on the LVGL task via the
 * poll timer, never from the MSC owner task.
 * Animation target is ui_arkss, confirmed from
 * ui_usb_upload_scr.c/h (centre image on this screen).
 * See the animation block inside switch_usb_screen(). */
static void on_usb_eject(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    (void)hw_port_usb_eject();
    /* Do NOT switch screens here. The owner task takes
     * the card back, then the poll timer sees APP owner
     * and switches to main_player + auto-play by itself.
     * If eject was ignored (copy in progress), the USB
     * screen simply stays, and pressing again later works. */
}

/* Frame anim for the USB screen, time-based, same timing as the
 * SquareLine animate_Animation() in ui.c: frames 0..N-1, linear,
 * USB_ANIM_LOOP_MS per loop, infinite repeat.
 * The frame index comes from elapsed time, not from a tick counter,
 * so a late timer skips a frame instead of slowing the whole loop,
 * and lv_image_set_src() is only called when the frame really changes.
 * The SquareLine anim is used once, only to read its imgset, then it
 * is deleted so the timer is the sole owner of ui_arkss. */
#define USB_ANIM_LOOP_MS 1200   /* ui.c: lv_anim_set_duration(..., 1200) */
#define USB_ANIM_TICK_MS 20     /* poll rate only; redraw happens on frame change */

static lv_timer_t * s_usb_anim_timer = NULL;
static const lv_image_dsc_t ** s_usb_anim_imgset = NULL;
static int32_t s_usb_anim_imgset_size = 0;
static int32_t s_usb_anim_frame = -1;
static uint32_t s_usb_anim_elapsed = 0;
static uint32_t s_usb_anim_last_tick = 0;
static bool s_usb_anim_running = false;

static void usb_anim_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    if (ui_arkss == NULL || s_usb_anim_imgset == NULL || s_usb_anim_imgset_size <= 0) return;

    uint32_t dt = lv_tick_elaps(s_usb_anim_last_tick);
    s_usb_anim_last_tick = lv_tick_get();
    s_usb_anim_elapsed = (s_usb_anim_elapsed + dt) % USB_ANIM_LOOP_MS;

    int32_t frame = (int32_t)(((uint64_t)s_usb_anim_elapsed * (uint32_t)s_usb_anim_imgset_size)
                              / USB_ANIM_LOOP_MS);
    if (frame >= s_usb_anim_imgset_size) frame = s_usb_anim_imgset_size - 1;
    if (frame == s_usb_anim_frame) return;   /* nothing new to draw */

    s_usb_anim_frame = frame;
    lv_image_set_src(ui_arkss, s_usb_anim_imgset[frame]);
}
/* Pause / resume are idempotent: poll calls them every 500 ms, and a
 * resume while already running must not reset the clock (that would
 * cause a small hitch every poll). */
static void pause_usb_anim(void)
{
    if (s_usb_anim_timer != NULL && s_usb_anim_running) {
        lv_timer_pause(s_usb_anim_timer);
        s_usb_anim_running = false;
    }
}
static void resume_usb_anim(void)
{
    if (s_usb_anim_timer != NULL && !s_usb_anim_running) {
        s_usb_anim_last_tick = lv_tick_get();  /* paused time must not count */
        lv_timer_resume(s_usb_anim_timer);
        s_usb_anim_running = true;
    }
}
/* Correct frame order 1..28. SquareLine imgset in ui.c is scrambled. */
static const lv_image_dsc_t * s_usb_anim_ordered[28] = {
    &ui_img_arknight_ark1_png, &ui_img_arknight_ark2_png,
    &ui_img_arknight_ark3_png, &ui_img_arknight_ark4_png,
    &ui_img_arknight_ark5_png, &ui_img_arknight_ark6_png,
    &ui_img_arknight_ark7_png, &ui_img_arknight_ark8_png,
    &ui_img_arknight_ark9_png, &ui_img_arknight_ark10_png,
    &ui_img_arknight_ark11_png, &ui_img_arknight_ark12_png,
    &ui_img_arknight_ark13_png, &ui_img_arknight_ark14_png,
    &ui_img_arknight_ark15_png, &ui_img_arknight_ark16_png,
    &ui_img_arknight_ark17_png, &ui_img_arknight_ark18_png,
    &ui_img_arknight_ark19_png, &ui_img_arknight_ark20_png,
    &ui_img_arknight_ark21_png, &ui_img_arknight_ark22_png,
    &ui_img_arknight_ark23_png, &ui_img_arknight_ark24_png,
    &ui_img_arknight_ark25_png, &ui_img_arknight_ark26_png,
    &ui_img_arknight_ark27_png, &ui_img_arknight_ark28_png,
};
static void start_usb_anim(void)
{
    if (ui_arkss == NULL) return;
    if (s_usb_anim_timer != NULL) {
        resume_usb_anim();
        return;
    }
    /* Use ordered array, not SquareLine imgset (scrambled in ui.c). */
    s_usb_anim_imgset = s_usb_anim_ordered;
    s_usb_anim_imgset_size = 28;
    s_usb_anim_frame = -1;
    s_usb_anim_elapsed = 0;
    s_usb_anim_last_tick = lv_tick_get();
    s_usb_anim_timer = lv_timer_create(usb_anim_timer_cb, USB_ANIM_TICK_MS, NULL);
    s_usb_anim_running = true;
}
static void stop_usb_anim(void)
{
    pause_usb_anim();
}
static void switch_usb_screen(bool to_usb)
{
    if (to_usb == s_on_usb_screen) return;
    if (to_usb) {
        /* Cancel the startup timer before it can fire on
         * top of the USB screen. It is one-shot, and the
         * poll return path replaces its job. */
        if (s_startup_timer != NULL) {
            lv_timer_delete(s_startup_timer);
            s_startup_timer = NULL;
        }
        /* Load the screen first, animation is started
         * after load, on ui_arkss only (see below). */
        if (ui_usb_upload_scr == NULL) {
            ui_usb_upload_scr_screen_init();
        }
        if (ui_usb_upload_scr == NULL) {
            ESP_LOGE(TAG, "USB screen object is still NULL after init, cannot switch");
            return;
        }
        /* Video image points to avi_player's pixbuf, which is freed
         * when the player stops. Hide it before switching screens,
         * so LVGL never tries to redraw a freed buffer while the
         * PC is copying (that redraw is a suspect for the white
         * panel during copy). Auto-play on return sets a fresh
         * buffer and shows it again. */
        if (s_video_img != NULL) {
            lv_obj_add_flag(s_video_img, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "USB screen load start");
        lv_screen_load(ui_usb_upload_scr);
        ESP_LOGI(TAG, "USB screen on, player should be stopped");
        s_on_usb_screen = true;
        /* Animation, safely: target is ui_arkss, the centre
         * image on this screen (from ui_usb_upload_scr.c:
         * ui_arkss = lv_image, ark1.png, centre, y+15).
         * The other objects here are static: labels,
         * QR images and the eject button, none of which
         * makes sense to animate. The previous crash came
         * from passing the whole screen as the target;
         * animate_Animation() animates a property of its
         * target object, and a screen is not that object.
         * Delete any previous animation on ui_arkss first,
         * so re-entering USB does not stack a second
         * animation on the same object. If ui_arkss is
         * NULL, skip animation entirely, do not call
         * animate with a NULL or screen target. */
        if (ui_arkss != NULL) {
            start_usb_anim();
        } else {
            ESP_LOGW(TAG, "ui_arkss is NULL, animation skipped");
        }
        /* Attach eject once, regardless of whether
         * ui_init() already created this screen at boot.
         * The old 'created' flag missed that case, so
         * eject was never attached when ui_init() had
         * already made the screen. */
        static bool s_eject_attached = false;
        if (!s_eject_attached && ui_eject_btn != NULL) {
            lv_obj_add_event_cb(ui_eject_btn, on_usb_eject, LV_EVENT_CLICKED, NULL);
            s_eject_attached = true;
        }
    } else {
        if (s_startup_timer != NULL) {
            lv_timer_delete(s_startup_timer);
            s_startup_timer = NULL;
        }
        /* Stop the upload animation when leaving: it must
         * not keep invalidating a hidden screen while the
         * player runs again, otherwise it competes with
         * playback and with the next USB copy. Delete only
         * animations on ui_arkss, never a global delete. */
        stop_usb_anim();
        if (ui_usb_upload_scr != NULL) {
            lv_anim_delete(ui_usb_upload_scr, NULL);
        }
        if (ui_main_player == NULL) {
            ui_main_player_screen_init();
        }
        lv_screen_load(ui_main_player);
        s_on_usb_screen = false;
        ESP_LOGI(TAG, "USB screen off, back to player");
        if (s_video_img != NULL) {
            lv_obj_remove_flag(s_video_img, LV_OBJ_FLAG_HIDDEN);
        }
        /* Card is back, list may have changed on the
         * PC: refresh, then auto-play like at boot. */
        (void)hw_port_storage_refresh_list();
        s_last_title_index = -1;
        (void)hw_port_player_play_index(0);
    }
}

/* Title: full name, extension stripped, no truncation.
 * 30 chars is only the scroll threshold: <= 30 shows
 * static, > 30 scrolls circularly inside a fixed 200 px
 * label, so a long name is read over time instead of
 * being cut off. Short names stay centred in the same
 * 200 px box, so the layout does not jump. */
static void set_video_title(const char *name)
{
    char buf[256];
    size_t n;
    if (ui_video_title == NULL || name == NULL) return;
    n = strlen(name);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, name, n);
    buf[n] = '\0';
    if (n > 4 && (strcasecmp(buf + n - 4, ".avi") == 0)) {
        buf[n - 4] = '\0';
        n -= 4;
    }
    lv_obj_set_width(ui_video_title, 200);
    lv_obj_set_style_text_align(ui_video_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui_video_title, buf);
    if (n > 30) {
        lv_label_set_long_mode(ui_video_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    } else {
        lv_label_set_long_mode(ui_video_title, LV_LABEL_LONG_CLIP);
    }
}

static void update_battery_icon(void)
{
    int level;
    if (ui_battery_stt == NULL) return;
    level = hw_port_battery_level();
    if (level == s_last_battery_level) return;
    s_last_battery_level = level;
    if (level >= 100) {
        lv_image_set_src(ui_battery_stt, &ui_img_icon_bat100_png);
    } else if (level >= 50) {
        lv_image_set_src(ui_battery_stt, &ui_img_icon_bat50_png);
    } else {
        lv_image_set_src(ui_battery_stt, &ui_img_icon_bat20_png);
    }
}

/* Poll, do not callback, from the player task: the player
 * task never touches LVGL, so gui_event polls its status
 * here on the LVGL timer. This timer must NOT re-arm the
 * panel hide timer, or the panel would never hide during
 * playback. Progress slider is display-only: avi_player
 * has no seek, so dragging it must not be wired as seek. */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* USB first: when the PC owns the card, show the
     * upload screen and do not try to play. When the
     * card comes back, switch_usb_screen() returns to
     * the player and auto-plays. A plain charger never
     * enumerates, so this stays false and the player
     * keeps running while charging. */
    if (hw_port_usb_is_enabled()) {
        switch_usb_screen(hw_port_usb_is_usb_owner());
        if (s_on_usb_screen) {
            /* Copy-speed guard: a running animation redraws
             * ui_arkss every frame over SPI, which stalls
             * the TinyUSB task in bursts, so the PC sees
             * speed jumping 0-300-600 kb/s instead of a
             * steady ~700 kb/s. While the host is actually
             * writing (write in the last 800 ms, from
             * bsp_usb_msc stats), stop the animation, so
             * copy runs steady. As soon as writing goes
             * idle, restart it, so the waiting screen is
             * animated continuously again. The user is
             * looking at the PC during copy, not at the
             * ESP screen, so pausing then is invisible. */
            uint64_t wr_bytes = 0;
            uint32_t wr_rate = 0;
            bool wr_active = false;
            hw_port_usb_get_write_bytes(&wr_bytes, &wr_rate, &wr_active);
            (void)wr_bytes; (void)wr_rate;
            /* Pause / resume, never delete / restart:
             * waiting = resume, copying = pause. A paused
             * anim does not redraw, so copy stays steady
             * (~700 kb/s, no 0-300-600 stepping). A resumed
             * anim continues from where it paused, and
             * because ui.c already repeats it infinitely
             * it needs no restart from poll at all. This
             * also fixes the duplicate-anim fight: poll
             * no longer creates a new anim on every
             * write-idle tick. */
            /* Pause on any host write, not threshold.
             * Threshold failed: with anim running, copy
             * slows below 16KB/tick, so threshold missed
             * a real copy and kept anim running. Any write
             * in last 800ms means host is writing, even
             * slow. Diag showed wr_delta=0 when idle, so
             * this will not stay paused when not copying. */
            (void)wr_bytes; (void)wr_rate;
            if (wr_active) {
                pause_usb_anim();
            } else {
                if (s_usb_anim_timer == NULL) {
                    start_usb_anim();
                } else {
                    resume_usb_anim();
                }
            }
            return;
        }
    }
    /* Battery every 10 s (20 x 500 ms), title on change.
     * Both run even when paused/stopped, so the header
     * stays correct without a separate timer. */
    s_poll_count++;
    if (s_poll_count % 20 == 1) {
        update_battery_icon();
    }
    {
        int idx = hw_port_player_get_index();
        if (idx >= 0 && idx != s_last_title_index) {
            const char *name = hw_port_storage_video_name(idx);
            if (name != NULL) {
                set_video_title(name);
                s_last_title_index = idx;
            }
        }
    }
    bool playing = hw_port_player_is_playing();
    if (playing != s_playing) {
        s_playing = playing;
        if (ui_play_btn != NULL) {
            if (s_playing) lv_obj_add_flag(ui_play_btn, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(ui_play_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_pause_btn != NULL) {
            if (s_playing) lv_obj_remove_flag(ui_pause_btn, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(ui_pause_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!playing) return;
    int total = hw_port_player_get_total_frames();
    double fps = hw_port_player_get_fps();
    if (total <= 0 || fps <= 0) return;
    int frame = hw_port_player_get_frame();
    int pct = hw_port_player_get_progress_percent();
    if (ui_video_lenght_slide != NULL) {
        lv_slider_set_value(ui_video_lenght_slide, pct, LV_ANIM_OFF);
    }
    if (ui_duration != NULL) {
        int cur_s = (int)(frame / fps);
        lv_label_set_text_fmt(ui_duration, "%02d:%02d", cur_s / 60, cur_s % 60);
    }
    if (ui_video_lenght != NULL) {
        int tot_s = (int)(total / fps);
        lv_label_set_text_fmt(ui_video_lenght, "%02d:%02d", tot_s / 60, tot_s % 60);
    }
}

static void set_playing_ui(bool playing)
{
    s_playing = playing;
    if (ui_play_btn != NULL) {
        if (playing) lv_obj_add_flag(ui_play_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(ui_play_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_pause_btn != NULL) {
        if (playing) lv_obj_remove_flag(ui_pause_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }
    panel_arm_timer();
}

static void on_play_pause(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Pause is a true pause now, not stop: playing ->
     * pause at the current frame, paused -> resume from
     * the same frame, stopped -> play from the start.
     * Optimistic UI flip; the 500 ms poll corrects it
     * if play actually failed (no SD, no file). */
    if (hw_port_player_is_paused()) {
        (void)hw_port_player_resume();
        set_playing_ui(true);
    } else if (hw_port_player_is_playing()) {
        (void)hw_port_player_pause();
        set_playing_ui(false);
    } else {
        int idx = hw_port_player_get_index();
        if (idx < 0) idx = 0;
        (void)hw_port_player_play_index(idx);
        set_playing_ui(true);
    }
}

static void on_next_prev(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *target = lv_event_get_target(e);
    if (target == ui_next_btn) {
        (void)hw_port_player_next();
    } else if (target == ui_previous_btn) {
        (void)hw_port_player_prev();
    } else {
        return;
    }
    set_playing_ui(true);
    panel_arm_timer();
}

/* Attach all app events. Call once, after ui_init(). */
void gui_event_init(void)
{
    if (s_inited) return;

    /* ui_init() normally creates all screens; the NULL guards only cover
     * the case where a screen is created lazily on first load. */
    if (ui_startup_scr == NULL) {
        ui_startup_scr_screen_init();
    }
    if (ui_main_player == NULL) {
        ui_main_player_screen_init();
    }
    if (ui_control_panel == NULL || ui_main_player == NULL) return;

    /* Video image: full screen, behind the control panel.
     * Created here, in gui_event, because it is a UI object;
     * hw_port_player only swaps its buffer. */
    {
        lv_obj_t *video_img = lv_image_create(ui_main_player);
        lv_obj_set_size(video_img, 320, 240);
        lv_obj_center(video_img);
        lv_obj_move_background(video_img);
        s_video_img = video_img;
        hw_port_player_set_video_obj(video_img);
        ESP_LOGI(TAG, "video image created");
    }
    /* Fallback USB init: main() should already have called
     * hw_port_usb_init() at boot, before any SD mount.
     * If main forgot it, this inits it here, before the
     * startup auto-play mounts anything. If SD was
     * already mounted before this point, init will fail
     * with 'bsp_sd_init_card failed' in the log, and
     * that failure itself is the diagnosis: remove the
     * old hw_port_storage_mount() call from boot. */
    if (!hw_port_usb_is_enabled()) {
        esp_err_t usb_err = hw_port_usb_init();
        if (usb_err != ESP_OK) {
            ESP_LOGW(TAG, "USB init failed in gui_event_init: %s, USB screen will not trigger", esp_err_to_name(usb_err));
        } else {
            ESP_LOGI(TAG, "USB init done from gui_event_init");
        }
    }
    (void)hw_port_player_init();
    if (s_poll_timer == NULL) {
        s_poll_timer = lv_timer_create(poll_timer_cb, 500, NULL);
    }

    /* Sliders are indicators only: touch on them is off,
     * volume and brightness are changed by edge gesture
     * only, video length is display-only (no seek yet).
     * Programmatic lv_slider_set_value still works, so
     * gesture and the poll timer still update them. */
    if (ui_volume_slide != NULL) {
        lv_obj_remove_flag(ui_volume_slide, LV_OBJ_FLAG_CLICKABLE);
    }
    if (ui_brightness_slide != NULL) {
        lv_obj_remove_flag(ui_brightness_slide, LV_OBJ_FLAG_CLICKABLE);
    }
    if (ui_video_lenght_slide != NULL) {
        lv_obj_remove_flag(ui_video_lenght_slide, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Event 2: panel hidden by default, shown by touch for 3 s. */
    lv_obj_add_flag(ui_control_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_panel_timer == NULL) {
        s_panel_timer = lv_timer_create(panel_timer_cb, PANEL_HIDE_MS, NULL);
        lv_timer_pause(s_panel_timer);
    }

    /* Sync labels with the generated initial slider values (40%). */
    if (ui_volume_slide != NULL) {
        int32_t v = lv_slider_get_value(ui_volume_slide);
        update_volume_label_icon(v);
        /* Codec follows the same initial value, so boot does
         * not start at bsp_audio's 100% default. */
        (void)hw_port_audio_set_volume((int)v);
    }
    if (ui_brightness_slide != NULL) {
        int32_t v = lv_slider_get_value(ui_brightness_slide);
        update_brightness_label(v);
        /* Take the BL pin over from app_display's plain GPIO
         * high output and set it to the slider's initial
         * value (40% in the generated UI). */
        (void)hw_port_brightness_set((int)v);
    }

    lv_obj_add_event_cb(ui_main_player, on_main_player_event, LV_EVENT_ALL, NULL);
    if (ui_volume_slide != NULL) {
        lv_obj_add_event_cb(ui_volume_slide, on_volume_slide, LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (ui_brightness_slide != NULL) {
        lv_obj_add_event_cb(ui_brightness_slide, on_brightness_slide, LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (ui_play_btn != NULL) {
        lv_obj_add_event_cb(ui_play_btn, on_play_pause, LV_EVENT_CLICKED, NULL);
    }
    if (ui_pause_btn != NULL) {
        lv_obj_add_event_cb(ui_pause_btn, on_play_pause, LV_EVENT_CLICKED, NULL);
    }
    if (ui_next_btn != NULL) {
        lv_obj_add_event_cb(ui_next_btn, on_next_prev, LV_EVENT_CLICKED, NULL);
    }
    if (ui_previous_btn != NULL) {
        lv_obj_add_event_cb(ui_previous_btn, on_next_prev, LV_EVENT_CLICKED, NULL);
    }

    /* Event 1: startup screen first, main player after 5 s. */
    lv_screen_load(ui_startup_scr);
    if (s_startup_timer == NULL) {
        s_startup_timer = lv_timer_create(startup_timer_cb, STARTUP_MS, NULL);
        lv_timer_set_repeat_count(s_startup_timer, 1);
    }

    s_inited = true;
}

int gui_event_get_volume(void)
{
    if (ui_volume_slide == NULL) return 0;
    return (int)lv_slider_get_value(ui_volume_slide);
}

int gui_event_get_brightness(void)
{
    if (ui_brightness_slide == NULL) return 0;
    return (int)lv_slider_get_value(ui_brightness_slide);
}

int gui_event_is_playing(void)
{
    return s_playing ? 1 : 0;
}
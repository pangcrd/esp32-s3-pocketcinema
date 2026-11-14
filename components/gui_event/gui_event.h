#ifndef GUI_EVENT_H
#define GUI_EVENT_H

#ifdef __cplusplus
extern "C" {
#endif

void gui_event_init(void);
void gui_event_test_usb_screen(void); /* test only, remove after test */
void gui_event_test_pos_anim(void); /* test only, remove after test */
int gui_event_get_volume(void);
int gui_event_get_brightness(void);
int gui_event_is_playing(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif

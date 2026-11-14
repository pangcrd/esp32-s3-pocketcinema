idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_display hw_port lvgl ui gui_event esp_lvgl_port esp_timer)
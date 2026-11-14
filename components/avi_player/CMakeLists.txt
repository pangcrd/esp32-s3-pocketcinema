idf_component_register(
    SRCS "avi_player.c"
    INCLUDE_DIRS "include"
    REQUIRES avi_parser jpeg_dec bsp_audio esp_timer
)
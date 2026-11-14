idf_component_register(
    SRCS "bsp_audio.c" "es8311.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2s esp_driver_i2c esp_driver_gpio
)
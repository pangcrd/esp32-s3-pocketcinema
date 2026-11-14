idf_component_register(
    SRCS "bsp_sd.c" "bsp_sd_bench.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_timer vfs
)
idf_component_register(
    SRCS "bsp_usb_msc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_tinyusb sdmmc fatfs esp_timer
)
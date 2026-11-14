idf_component_register(SRCS "app_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ft6336g gpio_config lvgl esp_lcd esp_lcd_ili9341 esp_lvgl_port esp_driver_spi esp_driver_gpio esp_psram)
idf_component_register(SRCS "hw_port_brightness.c" "hw_port_storage.c" "hw_port_audio.c" "hw_port_player.c" "hw_port_battery.c" "hw_port_usb.c"
  INCLUDE_DIRS "include"
  REQUIRES gpio_config bsp_sd bsp_fs bsp_audio avi_player bsp_usb_msc app_display ft6336g lvgl esp_lvgl_port
           esp_driver_ledc esp_driver_gpio esp_driver_i2c esp_adc esp_common log esp_system esp_timer)

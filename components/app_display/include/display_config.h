#pragma once

#include "gpio_conf.h"

#define APP_DISP_ROTATE_0 0
#define APP_DISP_ROTATE_90 90
#define APP_DISP_ROTATE_180 180
#define APP_DISP_ROTATE_270 270

/* TFT pins, ESP32-S3 SPI2 (FSPI). Pins are NOT set here anymore:
 * they come from components/board/include/gpio_conf.h, so a
 * pin change is one edit there, not one edit per config file.
 * LCD RST confirmed GPIO4 by dai ca 2026-10-26. */
#ifndef APP_DISP_BL_GPIO
#define APP_DISP_BL_GPIO BOARD_LCD_BL_GPIO
#endif

#ifndef APP_DISP_MISO_GPIO
#define APP_DISP_MISO_GPIO BOARD_LCD_MISO_GPIO
#endif

#ifndef APP_DISP_MOSI_GPIO
#define APP_DISP_MOSI_GPIO BOARD_LCD_MOSI_GPIO
#endif

#ifndef APP_DISP_SCLK_GPIO
#define APP_DISP_SCLK_GPIO BOARD_LCD_SCLK_GPIO
#endif

#ifndef APP_DISP_CS_GPIO
#define APP_DISP_CS_GPIO BOARD_LCD_CS_GPIO
#endif

#ifndef APP_DISP_DC_GPIO
#define APP_DISP_DC_GPIO BOARD_LCD_DC_GPIO
#endif

#ifndef APP_DISP_RST_GPIO
#define APP_DISP_RST_GPIO BOARD_LCD_RST_GPIO
#endif

/* Panel NATIVE size. Always 240x320 for this panel, even for
 * landscape: ROTATE_90/270 make the LVGL logical size 320x240
 * by themselves. Swapping these two numbers by hand (as in the
 * earlier landscape test) is no longer the way to rotate. */
#ifndef APP_DISP_H_RES
#define APP_DISP_H_RES 240
#endif

#ifndef APP_DISP_V_RES
#define APP_DISP_V_RES 320
#endif

#ifndef APP_DISP_SPI_CLK_HZ
#define APP_DISP_SPI_CLK_HZ 40000000
#endif


#ifndef APP_DISP_BUF_LINES
#define APP_DISP_BUF_LINES 80
#endif

#ifndef APP_DISP_DOUBLE_BUFFER
#define APP_DISP_DOUBLE_BUFFER 1
#endif

#ifndef APP_DISP_USE_SPIRAM
#define APP_DISP_USE_SPIRAM 1
#endif


#ifndef APP_DISP_FULL_BUFFER
#define APP_DISP_FULL_BUFFER 0
#endif

#ifndef APP_DISP_FRAME_DIV
#define APP_DISP_FRAME_DIV 0
#endif

#ifndef APP_DISP_FRAME_RTNA
#define APP_DISP_FRAME_RTNA 0x1B
#endif

/* Backlight level: 1 = active-high (most modules). Set 0 if the
 * module drives BL active-low and the screen stays dark. */
#ifndef APP_DISP_BL_ON_LEVEL
#define APP_DISP_BL_ON_LEVEL BOARD_LCD_BL_ACTIVE_HIGH
#endif


#ifndef APP_DISP_ROTATE
#define APP_DISP_ROTATE APP_DISP_ROTATE_270
#endif

#ifndef APP_DISP_SWAP_BYTES
#define APP_DISP_SWAP_BYTES 1
#endif


#ifndef APP_DISP_BGR
#define APP_DISP_BGR 1
#endif

#ifndef APP_DISP_INVERT_COLOR
#define APP_DISP_INVERT_COLOR 1
#endif

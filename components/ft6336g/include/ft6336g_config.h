#pragma once

#include "gpio_conf.h"


#ifndef FT6336G_I2C_PORT
#define FT6336G_I2C_PORT BOARD_TOUCH_I2C_PORT
#endif

#ifndef FT6336G_SDA_GPIO
#define FT6336G_SDA_GPIO BOARD_TOUCH_I2C_SDA_GPIO
#endif

#ifndef FT6336G_SCL_GPIO
#define FT6336G_SCL_GPIO BOARD_TOUCH_I2C_SCL_GPIO
#endif

/* 7-bit slave address: scan-proven 0x38 on this board. */
#ifndef FT6336G_I2C_ADDR
#define FT6336G_I2C_ADDR BOARD_TOUCH_I2C_ADDR
#endif

/* 400 kHz: the speed in the proving scan log. Datasheet Table 2-2
 * allows 10..400 kHz. */
#ifndef FT6336G_I2C_SPEED_HZ
#define FT6336G_I2C_SPEED_HZ BOARD_TOUCH_I2C_SPEED_HZ
#endif

/* 1 = enable the ESP32 internal pull-ups on SDA/SCL. This is the
 * setting in the proving scan log (pullup 1). */
#ifndef FT6336G_I2C_INTERNAL_PULLUP
#define FT6336G_I2C_INTERNAL_PULLUP BOARD_TOUCH_I2C_INTERNAL_PULLUP
#endif

/* -1 = pin not wired / reset handled externally. */
#ifndef FT6336G_RST_GPIO
#define FT6336G_RST_GPIO BOARD_TOUCH_RST_GPIO
#endif

/* -1 = polling only (this driver polls; INT is not used as an IRQ yet). */
#ifndef FT6336G_INT_GPIO
#define FT6336G_INT_GPIO BOARD_TOUCH_INT_GPIO
#endif

/* Display size in pixels. 0 = unknown: coordinates pass through raw and
 * inversion is a no-op, because x = width-1-x needs a real width.
 * The LVGL example fills this from the real display at runtime. */
#ifndef FT6336G_DISP_WIDTH
#define FT6336G_DISP_WIDTH 0
#endif

#ifndef FT6336G_DISP_HEIGHT
#define FT6336G_DISP_HEIGHT 0
#endif

/* Coordinate transform, applied in exactly one place: ft6336g_read().
 * Leave all three at 0 first, touch the panel, and only then flip the
 * axis that is actually reversed. Never flip the same axis here and
 * again in the LVGL example. */
#ifndef FT6336G_SWAP_XY
#define FT6336G_SWAP_XY 0
#endif

#ifndef FT6336G_INVERT_X
#define FT6336G_INVERT_X 0
#endif

#ifndef FT6336G_INVERT_Y
#define FT6336G_INVERT_Y 0
#endif

#ifndef FT6336G_I2C_TIMEOUT_MS
#define FT6336G_I2C_TIMEOUT_MS 100
#endif

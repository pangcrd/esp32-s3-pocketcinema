#ifndef BOARD_GPIO_CONF_H
#define BOARD_GPIO_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* LCD panel, SPI2. Brightness is backlight PWM on BL, not a
 * panel command (see hw_port_display). */
#define BOARD_LCD_MOSI_GPIO 11
#define BOARD_LCD_MISO_GPIO 13
#define BOARD_LCD_SCLK_GPIO 12
#define BOARD_LCD_CS_GPIO 10
#define BOARD_LCD_DC_GPIO 46
#define BOARD_LCD_BL_GPIO 45
#define BOARD_LCD_RST_GPIO (-1)
#define BOARD_LCD_BL_PWM_FREQ_HZ 1000
#define BOARD_LCD_BL_PWM_CHANNEL 0
#define BOARD_LCD_BL_ACTIVE_HIGH 1

/* Touch + audio codec control, shared I2C0.
 * Touch FT6336G at 0x38, ES8311 codec at 0x18: different
 * addresses, same bus, so they coexist. The 0x18 device
 * found in the 2026-10-25 touch scan is the ES8311. */
#define BOARD_TOUCH_I2C_PORT 0
#define BOARD_TOUCH_I2C_SDA_GPIO 16
#define BOARD_TOUCH_I2C_SCL_GPIO 15
#define BOARD_TOUCH_RST_GPIO 18
#define BOARD_TOUCH_INT_GPIO 17
#define BOARD_TOUCH_I2C_ADDR 0x38
#define BOARD_TOUCH_I2C_SPEED_HZ 400000
#define BOARD_TOUCH_I2C_INTERNAL_PULLUP 1

/* SD card, SDMMC 4-bit, SDIO only (bsp_sd in this project
 * has no SPI mode). Pins from dai ca 2026-10-26. */
#define BOARD_SD_CLK_GPIO 38
#define BOARD_SD_CMD_GPIO 40
#define BOARD_SD_D0_GPIO 39
#define BOARD_SD_D1_GPIO 41
#define BOARD_SD_D2_GPIO 48
#define BOARD_SD_D3_GPIO 47
#define BOARD_SD_BUS_WIDTH 4
#define BOARD_SD_MAX_FREQ_KHZ 40000
#define BOARD_SD_INTERNAL_PULLUP 1

/* Audio codec control, I2C0 shared with touch (see above). */
#define BOARD_AUDIO_I2C_PORT BOARD_TOUCH_I2C_PORT
#define BOARD_AUDIO_I2C_SDA_GPIO BOARD_TOUCH_I2C_SDA_GPIO
#define BOARD_AUDIO_I2C_SCL_GPIO BOARD_TOUCH_I2C_SCL_GPIO
#define BOARD_AUDIO_I2C_FREQ_HZ 400000
#define BOARD_AUDIO_CODEC_I2C_ADDR 0x18

/* Audio data, I2S + ES8311. WS is LRCK. DOUT is ESP data
 * out to the codec (playback only, no microphone input,
 * so DIN stays -1). I2S port was not in the board list;
 * 0 (I2S0) is assumed, confirm if the codec uses I2S1. */
#define BOARD_AUDIO_I2S_PORT 0
#define BOARD_AUDIO_I2S_MCLK_GPIO 4
#define BOARD_AUDIO_I2S_BCLK_GPIO 5
#define BOARD_AUDIO_I2S_WS_GPIO 7
#define BOARD_AUDIO_I2S_DOUT_GPIO 8
#define BOARD_AUDIO_I2S_DIN_GPIO (-1)

/* Speaker power amplifier enable. Active level 0 means
 * drive the pin LOW to enable the speaker, HIGH to mute. */
#define BOARD_AUDIO_PA_EN_GPIO 1
#define BOARD_AUDIO_PA_ACTIVE_LEVEL 0

/* Battery sense, ADC1 on GPIO9, divider 200k / 200k.
 * Pin voltage = VBAT / 2, so VBAT 3.0-4.2 V gives pin
 * 1.5-2.1 V, inside the DB_12 ADC range. GPIO9 is ADC1
 * channel 8 on ESP32-S3, not a strapping pin, and is free
 * on this board. */
#define BOARD_BATT_ADC_GPIO 9
#define BOARD_BATT_ADC_CHANNEL 8
#define BOARD_BATT_DIVIDER_R_TOP_OHM 200000
#define BOARD_BATT_DIVIDER_R_BOTTOM_OHM 200000

/* Sensors / spare I2C. Not in the board list yet. A sensor
 * on I2C0 must use BOARD_TOUCH_I2C_SDA/SCL_GPIO and an
 * address other than 0x38 (touch) and 0x18 (codec). */
#define BOARD_SENSOR_I2C_SDA_GPIO (-1)
#define BOARD_SENSOR_I2C_SCL_GPIO (-1)

/* Spare. */
#define BOARD_USER_BUTTON_GPIO (-1)
#define BOARD_LED_GPIO (-1)

/* USED GPIO, check before adding a pin:
 * LCD: 10, 11, 12, 13, 45, 46, RST tied to board RST (-1)
 * Touch + codec I2C0: 15, 16, 17, 18
 * SD SDMMC: 38, 39, 40, 41, 47, 48
 * I2S: 4 (MCLK), 5, 7, 8
 * Speaker PA: 1
 * Battery ADC: 9 (ADC1 ch8, 200k/200k)
 * Free / unconfirmed: everything else, except the -1 list. */

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif

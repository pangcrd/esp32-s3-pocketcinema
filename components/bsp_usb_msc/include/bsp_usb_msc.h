#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Who currently owns the SD card. A FAT volume must never be mounted by the
 * firmware and the PC at the same time, so ownership is always exclusive. */
typedef enum {
    BSP_USB_MSC_OWNER_APP,   /* mounted in the firmware (VFS path usable), PC sees "no medium" */
    BSP_USB_MSC_OWNER_USB,   /* exposed to the PC, VFS path NOT usable */
} bsp_usb_msc_owner_t;

/* Called from the TinyUSB task when ownership changes.
 *  OWNER_USB: the host is about to take the card -> stop every file access NOW.
 *  OWNER_APP: the card is back in the firmware (VFS path usable again).
 *
 * For OWNER_USB this runs *before* the driver unmounts, so blocking here is
 * what keeps the unmount from racing a reader. Keep the wait bounded. */
typedef void (*bsp_usb_msc_owner_cb_t)(bsp_usb_msc_owner_t owner, void *ctx);

typedef struct {
    sdmmc_card_t           *card;              /* already initialised card (bsp_sd_init_card) */
    const char             *base_path;         /* VFS mount path while the app owns the card, NULL = "/sdcard" */
    bsp_usb_msc_owner_cb_t  on_owner_change;   /* optional */
    void                   *ctx;
} bsp_usb_msc_config_t;

/* Create the MSC storage (mounts the FAT volume at base_path for the app),
 * then install the TinyUSB device stack. Does NOT format the card. */
esp_err_t bsp_usb_msc_init(const bsp_usb_msc_config_t *cfg);

bool bsp_usb_msc_is_app_owner(void);

/* Block until the application owns the card. Returns false on timeout. */
bool bsp_usb_msc_wait_app_owner(TickType_t timeout);

/* Device-initiated eject: ask the owner task to give the
 * card back to the application, without unplugging the
 * cable. The original API had no such call: host eject
 * (Safely Remove) was the only way back. Sets
 * s_want_usb = false and wakes the owner task, which
 * then runs take_card_back() (300 ms write drain, then
 * APP mount). To upload again after this, unplug and
 * replug the cable: ATTACHED sets s_want_usb = true
 * again. Returns ESP_ERR_INVALID_STATE if not initialised. */
esp_err_t bsp_usb_msc_eject(void);

/* true once a USB host has configured the device. A plain charger never
 * enumerates, so this stays false and the player keeps running. */
bool bsp_usb_msc_is_host_connected(void);

/* true when the card has no FAT filesystem (reported by the driver through
 * TINYUSB_MSC_EVENT_FORMAT_REQUIRED). The card is never formatted for you:
 * format it as FAT32 on the PC. */
bool bsp_usb_msc_format_required(void);

/* Host write activity, measured from the MSC WRITE(10) path.
 *
 * MSC is a block protocol: the host never tells the device how large the
 * transfer will be, so there is no meaningful "percent complete". What can be
 * reported is how much has been written and how fast. */
typedef struct {
    uint64_t bytes;         /* total bytes written by the host since the last mount */
    uint32_t rate_bps;      /* bytes per second over the recent window, 0 when idle */
    bool     active;        /* a write was seen within the idle timeout */
    uint32_t idle_ms;       /* time since the last write, 0 while writing */
} bsp_usb_msc_write_stats_t;

void bsp_usb_msc_get_write_stats(bsp_usb_msc_write_stats_t *out);

#ifdef __cplusplus
}
#endif

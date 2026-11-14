#ifndef HW_PORT_USB_H
#define HW_PORT_USB_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB MSC adapter. Call hw_port_usb_init() once at boot,
 * after the low-level card init and before any VFS use.
 * It installs bsp_usb_msc, which then owns the FAT mount:
 * the player must NOT call bsp_sd_mount() after this.
 *
 * Ownership is exclusive: when the PC owns the card the
 * VFS path is NOT usable, and when the app owns it the
 * PC sees no medium. A plain charger never enumerates,
 * so it never takes the card and the player keeps
 * running. */

/* Init the card (low-level, no VFS) + MSC driver.
 * Returns ESP_OK even if the first APP mount failed
 * (e.g. unformatted card): the PC can still format it
 * over USB, and the owner task retries APP later. */
esp_err_t hw_port_usb_init(void);
/* True after hw_port_usb_init(), regardless of owner. */
bool hw_port_usb_is_enabled(void);
/* True only while the app owns the card (VFS usable). */
bool hw_port_usb_is_app_owner(void);
/* True while the PC owns the card (USB screen on). */
bool hw_port_usb_is_usb_owner(void);
bool hw_port_usb_is_host_connected(void);
bool hw_port_usb_format_required(void);
/* Host write stats, for a status label if the USB
 * screen has one. No percent exists in MSC: it is a
 * block protocol, so only bytes + rate are reported. */
void hw_port_usb_get_write_bytes(uint64_t *bytes, uint32_t *rate_bps, bool *active);
/* Device-initiated eject, for ui_eject_btn on the USB
 * screen. Refuses while a host write was seen in the
 * last 800 ms, so a mid-copy press does nothing
 * instead of truncating the file being copied. */
esp_err_t hw_port_usb_eject(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif

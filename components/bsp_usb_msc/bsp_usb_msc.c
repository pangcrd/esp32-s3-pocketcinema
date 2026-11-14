#include "bsp_usb_msc.h"
#include <string.h>
#include "tinyusb_default_config.h"
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_usb_msc";

#define EVT_APP_OWNS      BIT0   /* state: the firmware holds the FAT volume */
#define EVT_RECHECK       BIT1   /* request: reconcile ownership with s_want_usb */

#define OWNER_TASK_STACK  3072
#define OWNER_TASK_PRIO   5
#define OWNER_POLL_MS     1000   /* also retries a transition that failed */
#define WRITE_DRAIN_MS    300    /* deferred WRITE(10) callbacks need to drain */

/* A host that stops sending WRITE(10) for this long is treated as "not copying
 * right now". Windows flushes in bursts, so a short value makes the status
 * flicker between copying and idle. */
#define WRITE_IDLE_MS     800

static EventGroupHandle_t           s_evt;
static tinyusb_msc_storage_handle_t s_storage;
static bsp_usb_msc_owner_cb_t       s_cb;
static void                        *s_cb_ctx;
static sdmmc_card_t                *s_card;
static char                         s_base_path[24];

static volatile bool s_app_owner;
static volatile bool s_host_connected;
static volatile bool s_format_required;
/* Set by the storage events, which fire synchronously inside the mount call. */
static volatile bool s_mount_failed;
/* A failed APP mount can leave the managed driver's bookkeeping at APP even
 * though no VFS path was registered. Keep our own state authoritative so the
 * next retry can force a real APP mount instead of being treated as a no-op. */
static volatile bool s_driver_claims_app;
/* Who the card *should* belong to. Written by the USB task (attach/detach) and
 * by the eject hook, read by the owner task. */
static volatile bool s_want_usb;

/* WRITE(10) accounting. Written from the TinyUSB task, read from the UI task,
 * so the 64-bit counter needs the spinlock to be read consistently. */
static portMUX_TYPE s_wr_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t      s_wr_bytes;
static int64_t       s_wr_last_us;

static void notify_owner(bsp_usb_msc_owner_t owner)
{
    if (s_cb) s_cb(owner, s_cb_ctx);
}

/* ---------------------------------------------------------------------------
 * Linker-wrapped TinyUSB MSC callbacks
 *
 * esp_tinyusb defines these, so they cannot be overridden by redefining them.
 * --wrap lets us see the traffic without patching the component.
 * ------------------------------------------------------------------------- */

int32_t __real_tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
bool    __real_tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject);

int32_t __wrap_tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    int32_t n = __real_tud_msc_write10_cb(lun, lba, offset, buffer, bufsize);
    if (n > 0) {
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_wr_lock);
        s_wr_bytes += (uint32_t)n;
        s_wr_last_us = now;
        portEXIT_CRITICAL(&s_wr_lock);
    }
    return n;
}

bool __wrap_tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    bool ret = __real_tud_msc_start_stop_cb(lun, power_condition, start, load_eject);

    /* "Safely remove" on the PC: the cable usually stays in, so the driver
     * would keep the card. Hand it back to the player instead. */
    if (load_eject && !start && s_evt) {
        s_want_usb = false;
        xEventGroupSetBits(s_evt, EVT_RECHECK);
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------- */

/* Runs in the TinyUSB task: keep it short, never block here. */
static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        /* A charger never gets this far: it supplies VBUS but never enumerates,
         * so the player keeps running. */
        s_host_connected = true;
        s_want_usb = true;
        ESP_LOGI(TAG, "USB host connected");
        break;

    case TINYUSB_EVENT_DETACHED:
        s_host_connected = false;
        s_want_usb = false;
        ESP_LOGI(TAG, "USB host disconnected");
        break;

    default:
        return;
    }
    xEventGroupSetBits(s_evt, EVT_RECHECK);
}

/* Storage events. With auto_mount_off the mount/unmount is driven by the owner
 * task, so these arrive in that task's context, synchronously inside
 * tinyusb_msc_set_storage_mount_point(). That is the only way to find out
 * whether a mount actually worked: the setter reports ESP_OK regardless. */
static void storage_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;

    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        s_mount_failed = true;
        ESP_LOGE(TAG, "mount/unmount failed");
        break;

    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        s_format_required = true;
        ESP_LOGW(TAG, "no FAT filesystem on the card. It is NOT formatted automatically: "
                      "format it as FAT32 from the PC.");
        break;

    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        s_mount_failed = true;
        ESP_LOGE(TAG, "format failed");
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Ownership
 * ------------------------------------------------------------------------- */

static esp_err_t create_storage(tinyusb_msc_mount_point_t mp)
{
    /* base_path is stored by pointer, not copied, so it must outlive the
     * storage object: s_base_path is static for that reason. */
    const tinyusb_msc_storage_config_t cfg = {
        .medium.card = s_card,
        .fat_fs = {
            .base_path = s_base_path,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 4,
            },
            .do_not_format = true,   /* never silently wipe a card that has no filesystem */
        },
        .mount_point = mp,
    };
    return tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
}

/* Switches ownership and reports whether the FAT volume is really usable.
 *
 * tinyusb_msc_set_storage_mount_point() updates its own bookkeeping even when
 * the underlying mount fails, so its return value and the mount point getter
 * cannot be trusted here. The events fire synchronously inside the call, so
 * the flags they set are what actually tells us the outcome.
 *
 * The same quirk means a failed APP mount leaves the driver believing the card
 * belongs to the app. A later retry would then be a no-op that reports success
 * without mounting anything, so the bookkeeping is forced back to USB here. */
static bool switch_mount_point(tinyusb_msc_mount_point_t want)
{
    s_mount_failed = false;
    if (want == TINYUSB_MSC_STORAGE_MOUNT_APP) s_format_required = false;

    /* The managed setter returns early when its bookkeeping already equals
     * `want`. After a failed APP mount that bookkeeping can be stale, so first
     * force it through USB; this unmount is a no-op when the medium is already
     * exposed to USB and avoids the stale-APP retry trap. */
    if (want == TINYUSB_MSC_STORAGE_MOUNT_APP && s_driver_claims_app) {
        s_mount_failed = false;
        (void)tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
        /* A failed APP mount may have left the medium unregistered, in which
         * case the driver's USB unmount path reports INVALID_STATE. The setter
         * still updates its bookkeeping, which is exactly what is needed to
         * make the following APP request execute a real mount. */
        s_driver_claims_app = false;
    }

    esp_err_t err = tinyusb_msc_set_storage_mount_point(s_storage, want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set mount point failed: %s", esp_err_to_name(err));
        return false;
    }

    if (s_mount_failed || s_format_required) {
        if (want == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            /* The setter may still have changed its internal mount_point even
             * when the filesystem mount failed. Remember that for the next
             * retry; do not call delete_storage(), whose error path can panic. */
            s_driver_claims_app = true;
        }
        return false;
    }

    s_driver_claims_app = (want == TINYUSB_MSC_STORAGE_MOUNT_APP);
    return true;
}

static void give_card_to_usb(void)
{
    /* Tell the application first: it has to close every file before the FAT
     * volume goes away. The wait happens in this task, never in the TinyUSB
     * task, so a slow reader cannot stall USB enumeration. */
    notify_owner(BSP_USB_MSC_OWNER_USB);

    portENTER_CRITICAL(&s_wr_lock);
    s_wr_bytes = 0;
    s_wr_last_us = 0;
    portEXIT_CRITICAL(&s_wr_lock);

    /* An unmount failure still leaves the host able to reach the raw sectors
     * (reads and writes bypass FatFs), so the card is handed over either way.
     * This is also how a card with no filesystem reaches the PC to be
     * formatted. */
    if (!switch_mount_point(TINYUSB_MSC_STORAGE_MOUNT_USB)) {
        ESP_LOGW(TAG, "unmount reported a problem, exposing the card anyway");
    }

    s_app_owner = false;
    xEventGroupClearBits(s_evt, EVT_APP_OWNS);
    ESP_LOGI(TAG, "card handed to the USB host");
}

static void take_card_back(void)
{
    /* WRITE(10) is deferred by esp_tinyusb: the callback returns before the
     * actual SD write runs. Give the TinyUSB task time to drain its deferred
     * write queue before remounting the FAT volume. */
    vTaskDelay(pdMS_TO_TICKS(WRITE_DRAIN_MS));

    if (!switch_mount_point(TINYUSB_MSC_STORAGE_MOUNT_APP)) {
        /* Most often an unformatted card: the mount point now claims APP but
         * nothing is mounted, so the VFS path would fail on every open. Leave
         * ownership with USB and let the PC format it. */
        ESP_LOGE(TAG, "card not usable by the application%s",
                 s_format_required ? " (needs FAT32 formatting on a PC)" : "");
        return;
    }

    s_app_owner = true;
    xEventGroupSetBits(s_evt, EVT_APP_OWNS);
    ESP_LOGI(TAG, "card is back in the application at %s", s_base_path);
    notify_owner(BSP_USB_MSC_OWNER_APP);
}

static void owner_task(void *arg)
{
    (void)arg;

    /* An unformatted card fails every mount attempt, and the poll below would
     * retry it once a second forever. Back off instead, but always retry
     * immediately when the request itself changes (cable plugged or ejected). */
    int retry_backoff = 0;

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_RECHECK, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(OWNER_POLL_MS));
        if (bits & EVT_RECHECK) retry_backoff = 0;

        const bool want_usb = s_want_usb;
        if (want_usb && s_app_owner) {
            give_card_to_usb();
        } else if (!want_usb && !s_app_owner) {
            if (retry_backoff > 0) {
                retry_backoff--;
                continue;
            }
            take_card_back();
            /* ~15 s between retries while the card stays unusable. */
            if (!s_app_owner) retry_backoff = 15;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t bsp_usb_msc_init(const bsp_usb_msc_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->card, ESP_ERR_INVALID_ARG, TAG, "card is required");
    ESP_RETURN_ON_FALSE(!s_evt, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    strlcpy(s_base_path, cfg->base_path ? cfg->base_path : "/sdcard", sizeof(s_base_path));
    s_cb = cfg->on_owner_change;
    s_cb_ctx = cfg->ctx;
    s_card = cfg->card;

    s_evt = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_evt, ESP_ERR_NO_MEM, TAG, "event group alloc failed");

    /* auto_mount_off: the driver must not grab the card on its own. Ownership
     * changes go through owner_task, which can afford to wait for the player
     * to close its files. */
    const tinyusb_msc_driver_config_t msc_cfg = {
        .user_flags = { .auto_mount_off = 1 },
        .callback = storage_event_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&msc_cfg), TAG, "msc driver install failed");

    /* Created owned by USB so no FAT mount is attempted yet, then mounted for
     * the app below. Doing it in this order keeps a card with no filesystem
     * from failing the creation call outright. */
    ESP_RETURN_ON_ERROR(create_storage(TINYUSB_MSC_STORAGE_MOUNT_USB), TAG, "new storage failed");

    /* Synchronous first mount: callers expect the VFS path to work as soon as
     * this function returns. A card that cannot be mounted is not fatal, the
     * PC can still format it over USB. */
    if (switch_mount_point(TINYUSB_MSC_STORAGE_MOUNT_APP)) {
        s_app_owner = true;
        xEventGroupSetBits(s_evt, EVT_APP_OWNS);
    } else {
        ESP_LOGW(TAG, "card not usable by the application%s",
                 s_format_required ? " (needs FAT32 formatting on a PC)" : "");
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    /* Standalone MSC copied at ~6 MB/s; with LVGL running, the
     * default TinyUSB task (prio 5) can be starved by LVGL
     * refresh + player work, and the host then times out on
     * a 10 MB copy. Give USB prio 6 on core 0, away from
     * LVGL, so USB deadlines are met even while the UI
     * is up. Player is stopped during USB, so core 0 is
     * mostly free for USB at that point. */
    tusb_cfg.task = TINYUSB_TASK_CUSTOM(4096, 6, 0);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install failed");

    ESP_RETURN_ON_FALSE(xTaskCreate(owner_task, "usb_owner", OWNER_TASK_STACK, NULL,
                                    OWNER_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "owner task create failed");

    ESP_LOGI(TAG, "USB MSC ready, card mounted for the app at %s", s_base_path);
    return ESP_OK;
}

bool bsp_usb_msc_is_app_owner(void)
{
    return s_app_owner;
}

bool bsp_usb_msc_wait_app_owner(TickType_t timeout)
{
    if (!s_evt) return false;
    EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_APP_OWNS, pdFALSE, pdTRUE, timeout);
    return (bits & EVT_APP_OWNS) != 0;
}

esp_err_t bsp_usb_msc_eject(void)
{
    if (!s_evt) return ESP_ERR_INVALID_STATE;
    s_want_usb = false;
    xEventGroupSetBits(s_evt, EVT_RECHECK);
    ESP_LOGI(TAG, "device eject requested, card back to app");
    return ESP_OK;
}

bool bsp_usb_msc_is_host_connected(void)
{
    return s_host_connected;
}

bool bsp_usb_msc_format_required(void)
{
    return s_format_required;
}

void bsp_usb_msc_get_write_stats(bsp_usb_msc_write_stats_t *out)
{
    if (!out) return;

    /* Rate is measured between two calls, so it reflects whatever cadence the
     * caller polls at. Single caller assumed (the status screen). */
    static uint64_t prev_bytes;
    static int64_t  prev_us;
    static uint32_t rate_bps;

    uint64_t bytes;
    int64_t  last_us;

    portENTER_CRITICAL(&s_wr_lock);
    bytes = s_wr_bytes;
    last_us = s_wr_last_us;
    portEXIT_CRITICAL(&s_wr_lock);

    const int64_t now = esp_timer_get_time();

    if (bytes < prev_bytes) {       /* counter was reset on a new session */
        prev_bytes = 0;
        prev_us = 0;
        rate_bps = 0;
    }

    /* Needs a wide enough window, otherwise a burst-y host reads as 0 B/s. */
    if (prev_us == 0) {
        prev_us = now;
        prev_bytes = bytes;
    } else if (now - prev_us >= 500 * 1000) {
        rate_bps = (uint32_t)((bytes - prev_bytes) * 1000000ULL / (uint64_t)(now - prev_us));
        prev_us = now;
        prev_bytes = bytes;
    }

    const int64_t idle_us = last_us ? (now - last_us) : INT64_MAX;
    out->bytes = bytes;
    out->active = last_us && idle_us < (int64_t)WRITE_IDLE_MS * 1000;
    out->idle_ms = (idle_us == INT64_MAX) ? UINT32_MAX : (uint32_t)(idle_us / 1000);
    out->rate_bps = out->active ? rate_bps : 0;
}

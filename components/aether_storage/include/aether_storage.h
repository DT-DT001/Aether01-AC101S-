#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "aether_board.h"
#include "esp_err.h"
#include "aether_storage_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AETHER_STORAGE_MOUNT_POINT "/sdcard"

typedef struct aether_storage_dev_t *aether_storage_handle_t;

typedef enum {
    AETHER_STORAGE_NO_CARD = 0,
    AETHER_STORAGE_MOUNTING,
    AETHER_STORAGE_READY,
    AETHER_STORAGE_ERROR,
    AETHER_STORAGE_REMOVING,
    AETHER_STORAGE_USB_PREPARING,
    AETHER_STORAGE_USB_EXPORTED,
} aether_storage_state_t;

typedef struct {
    aether_storage_state_t state;
    bool card_inserted;
    bool mounted;
    bool usb_exported;
    uint32_t usb_clock_khz; /* Real host clock during raw export; 0 when unknown/off. */
    bool usb_high_speed_fallback; /* Initial 40MHz card init failed; lower-clock retry succeeded. */
    uint64_t capacity_bytes;
    esp_err_t last_error;
} aether_storage_status_t;

/**
 * Start the TF-card hot-plug manager.
 *
 * The card-detect switch is active-low. BLDO1 is enabled only while a card is
 * inserted. Removal first blocks new users, then unmounts FATFS, puts the SDMMC
 * pins into a non-driving state and finally removes power from the socket.
 */
esp_err_t aether_storage_new(aether_board_t *board,
                             aether_storage_handle_t *out_handle);

esp_err_t aether_storage_del(aether_storage_handle_t handle);
esp_err_t aether_storage_get_status(aether_storage_handle_t handle,
                                    aether_storage_status_t *status);

/** Cache-only local mount identity. Zero before the first mount (or NULL);
 * nonzero epochs change on observed removal of a local mount and each fresh
 * successful mount. Repeated absence, reinsert and cancelled transitions do
 * not restore an old epoch. No card/PMIC/filesystem I/O is performed. */
/* Cache-only filesystem/partition probe. Failure metadata does not imply a
 * successful mount. No formatting is ever performed. Explicit retry only
 * publishes a request to the hotplug worker; it never performs GUI-thread I/O. */
esp_err_t aether_storage_get_diagnostics(aether_storage_handle_t handle,
                                        aether_storage_diagnostics_t *diagnostics);
esp_err_t aether_storage_request_retry(aether_storage_handle_t handle);

uint32_t aether_storage_generation(aether_storage_handle_t handle);

/**
 * Acquire a nonblocking mount lease. Independent FILE/DIR users may coexist;
 * ESP-IDF FatFS serializes individual filesystem calls. timeout_ms is retained
 * for source compatibility and lease acquisition never waits. Keep the lease
 * until every FILE/DIR opened by this operation has been closed, then release
 * exactly once. Do not share an individual FILE/DIR between tasks without its
 * own lock. Hot-unplug rejects new leases and waits for all holders to leave;
 * long-running users must poll aether_storage_card_inserted() and close promptly.
 */
esp_err_t aether_storage_acquire(aether_storage_handle_t handle,
                                 uint32_t timeout_ms);
/** Block only new local leases; existing holders retain a valid mounted card
 * for flush/close. This does no I/O and does not change mounted/card_inserted.
 * Clearing this gate never bypasses USB, shutdown, removal or mount guards. */
esp_err_t aether_storage_gate_new_users(aether_storage_handle_t handle, bool blocked);
void aether_storage_release(aether_storage_handle_t handle);

bool aether_storage_card_inserted(aether_storage_handle_t handle);
bool aether_storage_is_mounted(aether_storage_handle_t handle);
const char *aether_storage_mount_point(void);

/* Exclusive raw-card ownership for USB. Both transitions run in the existing
 * hotplug worker. begin rejects new leases and cancels old media through the
 * card_inserted() predicate before waiting for every FILE/DIR holder to close.
 * The old FatFS SD host is fully destroyed; USB gets a freshly initialized card.
 * end is called only after USB has stopped all callbacks, then restores local
 * ownership. A failed FatFS mount with all resources safely released remains
 * visible as ERROR, but does not permanently trap USB mode or shutdown rollback.
 * A transition timeout leaves ownership blocked until end completes. */
/* Power-off barrier: no raw export. A timeout/error keeps leases blocked until
 * cancel_shutdown explicitly restores the local mount. USB ownership rejects it. */
esp_err_t aether_storage_prepare_shutdown(aether_storage_handle_t handle, uint32_t timeout_ms);
esp_err_t aether_storage_cancel_shutdown(aether_storage_handle_t handle, uint32_t timeout_ms);

esp_err_t aether_storage_usb_begin(aether_storage_handle_t handle, uint32_t timeout_ms);
esp_err_t aether_storage_usb_end(aether_storage_handle_t handle, uint32_t timeout_ms);
bool aether_storage_usb_owned(aether_storage_handle_t handle);
/* Synchronous byte access, serialized against removal/transition. Requests are
 * bounds checked; DMA-safe aligned buffers transfer directly in up to 16KiB
 * blocks, other buffers/partial sectors use the reserved 4KiB bounce buffer. */
esp_err_t aether_storage_usb_read(aether_storage_handle_t handle, uint64_t offset,
                                 void *buffer, size_t bytes);
esp_err_t aether_storage_usb_write(aether_storage_handle_t handle, uint64_t offset,
                                  const void *buffer, size_t bytes);
esp_err_t aether_storage_usb_sync(aether_storage_handle_t handle);

#ifdef __cplusplus
}
#endif

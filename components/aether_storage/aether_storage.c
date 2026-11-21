#include "aether_storage.h"
#include "aether_storage_probe.h"

#include <stdlib.h>
#include <string.h>
#include "aether_board_pins.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_vfs_fat.h"
#include "esp_vfs_fat_lifecycle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#define AETHER_STORAGE_POLL_MS          50
#define AETHER_STORAGE_DEBOUNCE_SAMPLES 4
#define AETHER_STORAGE_POWER_SETTLE_MS  20
#define AETHER_STORAGE_RETRY_MS         2000
#define AETHER_STORAGE_RETRY_MAX_MS     30000
#define AETHER_STORAGE_MOUNT_ATTEMPTS   3
#define AETHER_STORAGE_DRAIN_WARN_MS    2000
#define AETHER_STORAGE_DMA_BYTES        4096
#define AETHER_STORAGE_USB_DIRECT_BYTES (16 * 1024)

static const char *TAG = "aether_storage";

struct aether_storage_dev_t {
    aether_board_t *board;
    portMUX_TYPE state_lock;
    bool task_running;
    sdmmc_card_t *card;
    bool local_mount_invalidated; /* Sticky across reinsert/cancel until a fresh mount. */
    uint32_t local_generation;
    aether_storage_diagnostics_t diagnostics;
    bool mount_retry_requested;
    void *dma_buffer; /* Persistent, exclusively used under the FatFS volume lock. */
    bool stop_requested;
    SemaphoreHandle_t raw_lock;
    sdmmc_card_t *raw_card;
    bool raw_host_initialized;
    bool raw_high_speed_fallback;
    sdmmc_host_t raw_host;
    bool usb_requested;
    bool usb_transition;
    bool usb_active;
    bool usb_failed;
    esp_err_t usb_error;
    bool shutdown_requested;
    bool shutdown_transition;
    bool shutdown_complete;
    esp_err_t shutdown_error;
    uint32_t active_users;
    bool new_users_blocked;
    TickType_t drain_started;
    bool drain_warned;
    bool power_retry_pending;
    TickType_t power_retry_due;
    aether_storage_status_t status;
};

static esp_err_t diagnose_mount_failure(aether_storage_handle_t handle);
static uint32_t mount_retry_delay_ms(uint32_t failures)
{
    uint32_t delay=AETHER_STORAGE_RETRY_MS;
    while(failures>1 && delay<AETHER_STORAGE_RETRY_MAX_MS){
        delay*=2;--failures;
    }
    return delay>AETHER_STORAGE_RETRY_MAX_MS?AETHER_STORAGE_RETRY_MAX_MS:delay;
}
static void reset_mount_retry_locked(aether_storage_handle_t h)
{
    memset(&h->diagnostics,0,sizeof(h->diagnostics));
    h->mount_retry_requested=true;
}

static bool read_card_detect(void)
{
    /* R286 pulls DET high; the socket switch closes to ground on insertion. */
    return gpio_get_level(AETHER_GPIO_TF_DET) == 0;
}

static uint32_t next_local_generation(uint32_t generation)
{
    ++generation;
    return generation ? generation : 1U;
}

/* Called under state_lock by the debounced hotplug owner. Presence at the
 * moment of cancellation cannot establish that the old card is still mounted:
 * a different card may already occupy the socket while old FILE users drain. */
static void publish_card_presence_locked(aether_storage_handle_t h, bool present)
{
    if (present && !h->status.card_inserted) reset_mount_retry_locked(h);
    h->status.card_inserted = present;
    if (!present && h->card) {
        if (!h->local_mount_invalidated)
            h->local_generation = next_local_generation(h->local_generation);
        h->local_mount_invalidated = true;
        h->status.mounted = false;
    }
}

static void set_bus_pins_safe(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << AETHER_GPIO_TF_D0) |
                        (1ULL << AETHER_GPIO_TF_D1) |
                        (1ULL << AETHER_GPIO_TF_D2) |
                        (1ULL << AETHER_GPIO_TF_D3) |
                        (1ULL << AETHER_GPIO_TF_CMD) |
                        (1ULL << AETHER_GPIO_TF_CLK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to park SDMMC pins: %s", esp_err_to_name(err));
    }
}

static esp_err_t mount_card_locked(aether_storage_handle_t handle)
{
    portENTER_CRITICAL(&handle->state_lock);
    handle->status.state = AETHER_STORAGE_MOUNTING;
    handle->status.last_error = ESP_OK;
    portEXIT_CRITICAL(&handle->state_lock);

    esp_err_t err = aether_board_set_sd_power(handle->board, true);
    if (err != ESP_OK) {
        goto failed;
    }
    vTaskDelay(pdMS_TO_TICKS(AETHER_STORAGE_POWER_SETTLE_MS));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.unaligned_multi_block_rw_max_chunk_size = 8;
    /* IDF otherwise allocates/frees a DMA bounce buffer on each unaligned
     * read. Reserve it before media/radio task stacks can consume this pool.
     * The storage service owns it across remounts; IDF must not replace it. */
    host.flags &= ~SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.dma_aligned_buffer = handle->dma_buffer;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    /* Explicit schematic mapping; do not rely on the ESP32-S31 defaults. */
    slot.clk = AETHER_GPIO_TF_CLK;
    slot.cmd = AETHER_GPIO_TF_CMD;
    slot.d0 = AETHER_GPIO_TF_D0;
    slot.d1 = AETHER_GPIO_TF_D1;
    slot.d2 = AETHER_GPIO_TF_D2;
    slot.d3 = AETHER_GPIO_TF_D3;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    /* The board has external 10 kOhm pull-ups powered from VDD_SD. */
    slot.flags &= ~SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdmmc_mount(AETHER_STORAGE_MOUNT_POINT,
                                             &host, &slot, &mount_cfg,
                                             &handle->card);
    if (err != ESP_OK) {
        goto failed;
    }

    /* Mount initialization may block for seconds. Do not expose a card that
     * was removed (or a manager being deleted) during that operation. No
     * lease has been published yet, so this rollback cannot race a FILE. */
    const bool present = read_card_detect();
    portENTER_CRITICAL(&handle->state_lock);
    const bool stopping = handle->stop_requested || handle->usb_requested || handle->shutdown_requested;
    portEXIT_CRITICAL(&handle->state_lock);
    if (!present || stopping) {
        const esp_err_t cleanup = esp_vfs_fat_sdcard_unmount(AETHER_STORAGE_MOUNT_POINT, handle->card);
        if (cleanup != ESP_OK) {
            portENTER_CRITICAL(&handle->state_lock);
            handle->local_mount_invalidated = true;
            handle->status.mounted = false;
            handle->status.state = AETHER_STORAGE_REMOVING;
            handle->status.last_error = cleanup;
            portEXIT_CRITICAL(&handle->state_lock);
            return cleanup; /* Retain card/host until checked unmount succeeds. */
        }
        handle->card = NULL;
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }

    const uint64_t capacity = (uint64_t)handle->card->csd.capacity *
                              handle->card->csd.sector_size;
    portENTER_CRITICAL(&handle->state_lock);
    handle->status.capacity_bytes = capacity;
    handle->diagnostics.failure_count=0;
    handle->diagnostics.retry_delay_ms=0;
    handle->diagnostics.retry_suspended=false;
    handle->local_mount_invalidated = false;
    handle->local_generation = next_local_generation(handle->local_generation);
    handle->status.mounted = true;
    handle->status.state = AETHER_STORAGE_READY;
    portEXIT_CRITICAL(&handle->state_lock);
    ESP_LOGI(TAG, "TF mounted at %s, capacity=%llu MiB",
             AETHER_STORAGE_MOUNT_POINT,
             (unsigned long long)(capacity / (1024ULL * 1024ULL)));
    return ESP_OK;

failed:
    handle->card = NULL;
    portENTER_CRITICAL(&handle->state_lock);
    handle->status.mounted = false;
    handle->status.capacity_bytes = 0;
    handle->status.state = AETHER_STORAGE_ERROR;
    handle->status.last_error = err;
    portEXIT_CRITICAL(&handle->state_lock);
    /* Read-only diagnosis runs once per insertion/explicit retry. The SDK
     * convenience mount has already destroyed its failed host/card. A raw
     * probe owns a fresh descriptor and is closed before any normal retry. */
    esp_err_t probe_cleanup=ESP_OK;
    if(err==ESP_FAIL) probe_cleanup=diagnose_mount_failure(handle);
    portENTER_CRITICAL(&handle->state_lock);
    if(handle->diagnostics.failure_count<UINT32_MAX)++handle->diagnostics.failure_count;
    if(handle->diagnostics.failure_count>=AETHER_STORAGE_MOUNT_ATTEMPTS)
        handle->diagnostics.retry_suspended=true;
    handle->diagnostics.retry_delay_ms=handle->diagnostics.retry_suspended?0:
        mount_retry_delay_ms(handle->diagnostics.failure_count);
    portEXIT_CRITICAL(&handle->state_lock);
    if(probe_cleanup!=ESP_OK){
        portENTER_CRITICAL(&handle->state_lock);
        handle->status.state=AETHER_STORAGE_REMOVING;
        handle->status.last_error=probe_cleanup;
        portEXIT_CRITICAL(&handle->state_lock);
        return err; /* Keep live raw-host ownership until service_usb drains it. */
    }
    set_bus_pins_safe();
    const esp_err_t off_err = aether_board_set_sd_power(handle->board, false);
    if (off_err != ESP_OK) {
        handle->power_retry_pending = true;
        handle->power_retry_due = xTaskGetTickCount() + pdMS_TO_TICKS(AETHER_STORAGE_RETRY_MS);
        portENTER_CRITICAL(&handle->state_lock);
        handle->status.state = AETHER_STORAGE_REMOVING;
        handle->status.last_error = off_err;
        portEXIT_CRITICAL(&handle->state_lock);
        ESP_LOGW(TAG, "BLDO1 power-off cleanup pending: %s", esp_err_to_name(off_err));
    }
    ESP_LOGW(TAG, "TF mount failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t mount_card(aether_storage_handle_t handle)
{
    const esp_err_t admitted = esp_vfs_fat_lifecycle_acquire(200);
    if (admitted != ESP_OK) {
        portENTER_CRITICAL(&handle->state_lock);
        handle->status.last_error = admitted;
        handle->diagnostics.retry_delay_ms = 100;
        portEXIT_CRITICAL(&handle->state_lock);
        return admitted; /* Contention is not a bad-card attempt or probe. */
    }
    const esp_err_t result = mount_card_locked(handle);
    esp_vfs_fat_lifecycle_release();
    return result;
}

/* Only the hotplug task mounts/unmounts. Never tear down FATFS beneath an open
 * FILE/DIR. A stalled client leaves removal pending, rather than freeing the
 * driver's objects or cutting socket power while that client is still in use. */
static bool unmount_card_locked(aether_storage_handle_t handle)
{
    const TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&handle->state_lock);
    if (handle->status.state != AETHER_STORAGE_REMOVING) {
        handle->drain_started = now;
        handle->drain_warned = false;
    }
    handle->status.state = AETHER_STORAGE_REMOVING;
    handle->status.mounted = false;  /* Atomically reject new leases. */
    const uint32_t users = handle->active_users;
    const bool warn = users && !handle->drain_warned &&
                      (now - handle->drain_started >=
                       pdMS_TO_TICKS(AETHER_STORAGE_DRAIN_WARN_MS));
    if (warn) {
        handle->drain_warned = true;
        handle->status.last_error = ESP_ERR_TIMEOUT;
    }
    portEXIT_CRITICAL(&handle->state_lock);
    if (users) {
        if (warn) {
            ESP_LOGW(TAG, "TF removal waiting for %lu open users", (unsigned long)users);
        }
        return false;
    }
    if (handle->power_retry_pending &&
        (int32_t)(now - handle->power_retry_due) < 0) {
        return false;
    }
    esp_err_t unmount_err = ESP_OK;
    if (handle->card) {
        /* f_mount(NULL) may already detach the filesystem before a later
         * unregister/deinit failure. Cancellation must not revive that mount. */
        portENTER_CRITICAL(&handle->state_lock);
        if (!handle->local_mount_invalidated)
            handle->local_generation = next_local_generation(handle->local_generation);
        handle->local_mount_invalidated = true;
        portEXIT_CRITICAL(&handle->state_lock);
        unmount_err = esp_vfs_fat_sdcard_unmount(AETHER_STORAGE_MOUNT_POINT,
                                                handle->card);
        if (unmount_err != ESP_OK) {
            ESP_LOGW(TAG, "TF unmount returned %s", esp_err_to_name(unmount_err));
            portENTER_CRITICAL(&handle->state_lock);
            handle->status.last_error = unmount_err;
            portEXIT_CRITICAL(&handle->state_lock);
            handle->power_retry_pending = true;
            handle->power_retry_due = now + pdMS_TO_TICKS(100);
            return false; /* A failed unregister still owns its card and VFS. */
        }
        handle->card = NULL;
    }
    set_bus_pins_safe();
    esp_err_t power_err = aether_board_set_sd_power(handle->board, false);
    if (power_err != ESP_OK) {
        ESP_LOGW(TAG, "disable BLDO1 failed: %s", esp_err_to_name(power_err));
        handle->power_retry_pending = true;
        handle->power_retry_due = now + pdMS_TO_TICKS(AETHER_STORAGE_RETRY_MS);
        portENTER_CRITICAL(&handle->state_lock);
        handle->status.capacity_bytes = 0;
        handle->status.last_error = power_err;
        portEXIT_CRITICAL(&handle->state_lock);
        return false; /* REMOVING persists until socket power is actually off. */
    }
    handle->power_retry_pending = false;
    portENTER_CRITICAL(&handle->state_lock);
    handle->status.capacity_bytes = 0;
    handle->status.state = handle->status.card_inserted ? AETHER_STORAGE_ERROR
                                                       : AETHER_STORAGE_NO_CARD;
    handle->status.last_error = unmount_err != ESP_OK ? unmount_err : power_err;
    portEXIT_CRITICAL(&handle->state_lock);
    ESP_LOGI(TAG, "TF unmounted and powered off");
    return true;
}


static bool unmount_card(aether_storage_handle_t handle)
{
    /* Power-only cleanup does not touch shared drive/context registration. */
    if (!handle->card) return unmount_card_locked(handle);
    const esp_err_t admitted = esp_vfs_fat_lifecycle_acquire(20);
    if (admitted != ESP_OK) {
        portENTER_CRITICAL(&handle->state_lock);
        if (handle->status.state != AETHER_STORAGE_REMOVING) {
            handle->drain_started = xTaskGetTickCount();
            handle->drain_warned = false;
        }
        handle->status.state = AETHER_STORAGE_REMOVING;
        handle->status.mounted = false;
        handle->status.last_error = admitted;
        portEXIT_CRITICAL(&handle->state_lock);
        return false;
    }
    const bool result = unmount_card_locked(handle);
    esp_vfs_fat_lifecycle_release();
    return result;
}

/* USB never borrows the descriptor owned by esp_vfs_fat_sdmmc_mount(). That
 * descriptor is freed by sdcard_unmount, so export creates a separate raw host. */
static esp_err_t raw_close(aether_storage_handle_t h)
{
    if (h->raw_host_initialized) {
        esp_err_t err = (h->raw_host.flags & SDMMC_HOST_FLAG_DEINIT_ARG)
            ? h->raw_host.deinit_p(h->raw_host.slot) : h->raw_host.deinit();
        if (err != ESP_OK) return err;
        h->raw_host_initialized = false;
    }
    free(h->raw_card);
    h->raw_card = NULL;
    h->raw_high_speed_fallback = false;
    portENTER_CRITICAL(&h->state_lock);
    h->status.usb_clock_khz = 0;
    h->status.usb_high_speed_fallback = false;
    portEXIT_CRITICAL(&h->state_lock);
    set_bus_pins_safe();
    esp_err_t err = aether_board_set_sd_power(h->board, false);
    h->power_retry_pending = err != ESP_OK;
    if (h->power_retry_pending)
        h->power_retry_due = xTaskGetTickCount() + pdMS_TO_TICKS(AETHER_STORAGE_RETRY_MS);
    return err;
}

static esp_err_t raw_open_at(aether_storage_handle_t h, int clock_khz, bool *card_init_failed)
{
    *card_init_failed = false;
    esp_err_t err = aether_board_set_sd_power(h->board, true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(AETHER_STORAGE_POWER_SETTLE_MS));
    h->raw_card = calloc(1, sizeof(*h->raw_card));
    if (!h->raw_card) return ESP_ERR_NO_MEM;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = clock_khz;
    host.unaligned_multi_block_rw_max_chunk_size = 8;
    host.flags &= ~SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.dma_aligned_buffer = h->dma_buffer;
    h->raw_host = host;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = AETHER_GPIO_TF_CLK;
    slot.cmd = AETHER_GPIO_TF_CMD;
    slot.d0 = AETHER_GPIO_TF_D0;
    slot.d1 = AETHER_GPIO_TF_D1;
    slot.d2 = AETHER_GPIO_TF_D2;
    slot.d3 = AETHER_GPIO_TF_D3;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.flags &= ~SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    err = host.init();
    if (err != ESP_OK) return err;
    h->raw_host_initialized = true;
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) {
        err = sdmmc_card_init(&host, h->raw_card);
        *card_init_failed = err != ESP_OK;
    }
    if (err == ESP_OK && h->raw_card->csd.sector_size != 512)
        err = ESP_ERR_NOT_SUPPORTED;
    return err;
}

static int probe_read_sector(void *context,uint32_t lba,uint8_t *sector)
{
    aether_storage_handle_t h=context;
    if(!read_card_detect())return ESP_ERR_INVALID_STATE;
    esp_err_t error=sdmmc_read_sectors(h->raw_card,h->dma_buffer,lba,1);
    if(error==ESP_OK)memcpy(sector,h->dma_buffer,512);
    return error;
}
static esp_err_t diagnose_mount_failure(aether_storage_handle_t h)
{
    portENTER_CRITICAL(&h->state_lock);
    const bool allowed=!h->diagnostics.attempted&&!h->stop_requested&&!h->usb_requested&&
        !h->shutdown_requested&&h->status.card_inserted;
    portEXIT_CRITICAL(&h->state_lock);
    if(!allowed||!read_card_detect())return ESP_OK;
    if(xSemaphoreTake(h->raw_lock,0)!=pdTRUE)return ESP_OK;
    if(h->raw_card||h->raw_host_initialized){xSemaphoreGive(h->raw_lock);return ESP_ERR_INVALID_STATE;}
    /* Reinitialize at the existing local20MHz; this is not a speed experiment. */
    esp_err_t error=aether_board_set_sd_power(h->board,false);
    if(error!=ESP_OK){xSemaphoreGive(h->raw_lock);return error;}
    vTaskDelay(pdMS_TO_TICKS(AETHER_STORAGE_POWER_SETTLE_MS));
    bool init_failed=false;
    error=raw_open_at(h,SDMMC_FREQ_DEFAULT,&init_failed);
    aether_storage_diagnostics_t probe={.attempted=true,.read_error=error};
    if(error==ESP_OK)aether_storage_probe(probe_read_sector,h,h->raw_card->csd.capacity,&probe);
    const esp_err_t clean=raw_close(h);
    portENTER_CRITICAL(&h->state_lock);
    const uint32_t failures=h->diagnostics.failure_count;
    h->diagnostics=probe;h->diagnostics.failure_count=failures;
    portEXIT_CRITICAL(&h->state_lock);
    ESP_LOGW(TAG,"TF read-only probe: capacity=%llu MiB layout=%s fs=%s partition_lba=%lu partition_sectors=%lu sector=%u cluster_sectors=%u reserved=%u fats=%u fat_sectors=%lu total_sectors=%lu clusters=%lu bpb_valid=%u retry_suspended=%u read_error=%s cleanup=%s",
        (unsigned long long)(probe.capacity_bytes/(1024U*1024U)),aether_storage_layout_name(probe.layout),
        aether_storage_fs_name(probe.filesystem),(unsigned long)probe.partition_lba,(unsigned long)probe.partition_sectors,
        probe.sector_bytes,probe.sectors_per_cluster,probe.reserved_sectors,probe.fat_count,
        (unsigned long)probe.fat_sectors,(unsigned long)probe.total_sectors,(unsigned long)probe.clusters,
        probe.bpb_valid,probe.retry_suspended,esp_err_to_name(probe.read_error),esp_err_to_name(clean));
    (void)init_failed;
    xSemaphoreGive(h->raw_lock);
    return clean;
}

/* Only SD-card initialization failure can request a lower clock. Never retry
 * a write or change clocks underneath a live host/acknowledged USB transfer. */
static esp_err_t raw_open(aether_storage_handle_t h)
{
    if (h->raw_card || h->raw_host_initialized || h->power_retry_pending)
        return ESP_ERR_INVALID_STATE;
    /* All candidates retain 3.3V signalling. UHS modes require separate board
     * voltage-switch validation and are deliberately not selected here. */
    static const int clocks_khz[] = {SDMMC_FREQ_HIGHSPEED, 32000, 25000, SDMMC_FREQ_DEFAULT};
    esp_err_t err = ESP_FAIL;
    for (size_t attempt = 0; attempt < sizeof(clocks_khz) / sizeof(clocks_khz[0]); ++attempt) {
        bool card_init_failed = false;
        err = raw_open_at(h, clocks_khz[attempt], &card_init_failed);
        if (err == ESP_OK) {
            h->raw_high_speed_fallback = attempt != 0;
            return ESP_OK;
        }
        const esp_err_t clean = raw_close(h);
        if (clean != ESP_OK) return clean; /* Retain ownership until cleanup succeeds. */
        if (!card_init_failed || attempt + 1 == sizeof(clocks_khz) / sizeof(clocks_khz[0]))
            return err;
        /* Keep socket power off between attempts, allowing the card's supply
         * capacitors to discharge before restarting its initialization. */
        vTaskDelay(pdMS_TO_TICKS(AETHER_STORAGE_POWER_SETTLE_MS));
        portENTER_CRITICAL(&h->state_lock);
        const bool retry = h->usb_requested && !h->stop_requested && h->status.card_inserted;
        portEXIT_CRITICAL(&h->state_lock);
        if (!retry || !read_card_detect()) return err;
        ESP_LOGW(TAG, "USB SD initialization at %d kHz failed: %s; retrying at %d kHz",
                 clocks_khz[attempt], esp_err_to_name(err), clocks_khz[attempt + 1]);
    }
    return err;
}

/* The existing hotplug task is the only owner of SD construction/destruction.
 * Its raw mutex also waits for every synchronous host read/write to finish. */
static bool service_usb(aether_storage_handle_t h)
{
    portENTER_CRITICAL(&h->state_lock);
    const bool requested = h->usb_requested && !h->stop_requested;
    const bool active = h->usb_active;
    const bool transition = h->usb_transition;
    const bool failed = h->usb_failed;
    const bool present = h->status.card_inserted;
    const uint32_t users = h->active_users;
    if (requested) {
        h->status.mounted = false;
        h->status.state = active ? AETHER_STORAGE_USB_EXPORTED : AETHER_STORAGE_USB_PREPARING;
    }
    portEXIT_CRITICAL(&h->state_lock);
    if (!requested && !active && !transition && !h->raw_card && !h->raw_host_initialized)
        return false;
    if (requested && users) return true;
    if (!requested && !active && !h->raw_card && !h->raw_host_initialized && !h->power_retry_pending) {
        /* A timed-out begin may be cancelled while old FILE leases drain.
         * Restore that still-valid local mount; never cut its socket power. */
        portENTER_CRITICAL(&h->state_lock);
        h->usb_transition = false;
        h->usb_failed = false;
        h->usb_error = ESP_OK;
        h->status.usb_exported = false;
        h->status.mounted = h->card && present && !h->stop_requested && !h->local_mount_invalidated;
        h->status.state = h->status.mounted ? AETHER_STORAGE_READY :
            h->card ? AETHER_STORAGE_REMOVING : present ? AETHER_STORAGE_ERROR : AETHER_STORAGE_NO_CARD;
        h->status.last_error = ESP_OK;
        portEXIT_CRITICAL(&h->state_lock);
        return true;
    }
    if (requested && (h->card || (h->power_retry_pending && !h->raw_card && !h->raw_host_initialized))) {
        if (!unmount_card(h)) return true;
        if (h->status.last_error != ESP_OK) {
            portENTER_CRITICAL(&h->state_lock);
            h->usb_error = h->status.last_error;
            h->usb_failed = true;
            h->usb_transition = false;
            portEXIT_CRITICAL(&h->state_lock);
            return true;
        }
    }
    if (xSemaphoreTake(h->raw_lock, 0) != pdTRUE) return true;
    esp_err_t err = ESP_OK;
    if (!requested || !present || (failed && (h->raw_card || h->raw_host_initialized || h->power_retry_pending))) {
        if (h->power_retry_pending &&
            (int32_t)(xTaskGetTickCount() - h->power_retry_due) < 0) {
            xSemaphoreGive(h->raw_lock);
            return true;
        }
        err = raw_close(h);
        portENTER_CRITICAL(&h->state_lock);
        if (err == ESP_OK) {
            h->usb_active = false;
            h->status.usb_exported = false;
            h->status.capacity_bytes = 0;
            if (!requested) {
                if(active||transition||failed)reset_mount_retry_locked(h);
                h->usb_transition = false;
                h->usb_failed = false;
                h->usb_error = ESP_OK;
                h->status.state = present ? AETHER_STORAGE_ERROR : AETHER_STORAGE_NO_CARD;
                h->status.last_error = ESP_OK;
            } else {
                h->usb_failed = true;
                h->usb_transition = false;
                h->usb_error = ESP_ERR_INVALID_STATE;
                h->status.last_error = h->usb_error;
            }
        } else {
            h->status.last_error = err;
        }
        portEXIT_CRITICAL(&h->state_lock);
    } else if (requested && !active && !failed) {
        if (h->power_retry_pending) {
            xSemaphoreGive(h->raw_lock);
            return true;
        }
        err = raw_open(h);
        const bool opened = err == ESP_OK;
        portENTER_CRITICAL(&h->state_lock);
        const bool valid = h->usb_requested && !h->stop_requested && h->status.card_inserted;
        portEXIT_CRITICAL(&h->state_lock);
        if (err == ESP_OK && (!valid || !read_card_detect())) err = ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            /* raw_open already cleaned failed initialization. If its cleanup
             * failed, retain that host/card for the worker's later retry. */
            const esp_err_t clean = opened ? raw_close(h) : ESP_OK;
            portENTER_CRITICAL(&h->state_lock);
            h->usb_failed = true;
            h->usb_transition = false;
            h->usb_error = err;
            h->status.last_error = clean != ESP_OK ? clean : err;
            portEXIT_CRITICAL(&h->state_lock);
        } else {
            portENTER_CRITICAL(&h->state_lock);
            h->usb_active = true;
            h->usb_transition = false;
            h->usb_error = ESP_OK;
            h->status.usb_exported = true;
            h->status.state = AETHER_STORAGE_USB_EXPORTED;
            h->status.capacity_bytes = (uint64_t)h->raw_card->csd.capacity * 512;
            h->status.usb_clock_khz = h->raw_card->real_freq_khz > 0 ?
                (uint32_t)h->raw_card->real_freq_khz : 0;
            h->status.usb_high_speed_fallback = h->raw_high_speed_fallback;
            h->status.last_error = ESP_OK;
            portEXIT_CRITICAL(&h->state_lock);
            ESP_LOGI(TAG, "TF exclusively exported to USB, clock=%lu kHz fallback=%u; local FatFS unavailable",
                     (unsigned long)h->status.usb_clock_khz, h->raw_high_speed_fallback);
        }
    }
    xSemaphoreGive(h->raw_lock);
    return true;
}


static bool service_shutdown(aether_storage_handle_t h)
{
    portENTER_CRITICAL(&h->state_lock);
    const bool requested = h->shutdown_requested;
    const bool transition = h->shutdown_transition;
    const bool complete = h->shutdown_complete;
    const esp_err_t failure = h->shutdown_error;
    if (requested) h->status.mounted = false;
    portEXIT_CRITICAL(&h->state_lock);
    if (!requested && !transition) return false;
    if (!requested) {
        /* Serialized with mount/unmount in this worker, including cancelled
         * timeouts while old FILEs are closing. */
        portENTER_CRITICAL(&h->state_lock);
        h->shutdown_transition = false;
        h->shutdown_complete = false;
        h->shutdown_error = ESP_OK;
        if (h->card && h->status.card_inserted && !h->stop_requested && !h->local_mount_invalidated) {
            h->status.mounted = true;
            h->status.state = AETHER_STORAGE_READY;
            h->status.last_error = ESP_OK;
        } else if (h->card) {
            h->status.mounted = false;
            h->status.state = AETHER_STORAGE_REMOVING;
        }
        portEXIT_CRITICAL(&h->state_lock);
        return true;
    }
    if (complete || failure != ESP_OK) return true;
    /* A failed filesystem probe may retain its private raw host when deinit
     * fails. Shutdown has priority over service_usb(), so it must drain that
     * host itself before parking pins or cutting socket power. Keep ownership
     * and retry on the worker's next poll; the caller's existing timeout still
     * bounds the shutdown barrier. Cancelling shutdown hands cleanup back to
     * service_usb() without publishing a local mount over the retained host. */
    if (h->raw_card || h->raw_host_initialized) {
        if (xSemaphoreTake(h->raw_lock, 0) != pdTRUE) return true;
        const esp_err_t err = raw_close(h);
        xSemaphoreGive(h->raw_lock);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&h->state_lock);
            h->status.state = AETHER_STORAGE_REMOVING;
            h->status.last_error = err;
            portEXIT_CRITICAL(&h->state_lock);
            return true;
        }
    }
    if (!unmount_card(h)) return true;
    portENTER_CRITICAL(&h->state_lock);
    if (h->shutdown_requested) {
        h->shutdown_error = h->status.last_error;
        h->shutdown_complete = h->status.last_error == ESP_OK;
        h->shutdown_transition = false;
    }
    portEXIT_CRITICAL(&h->state_lock);
    return true;
}

static void storage_task(void *argument)
{
    aether_storage_handle_t handle = argument;
    bool candidate = read_card_detect();
    uint8_t stable_samples = 1;
    TickType_t next_mount_retry = 0;
    bool mount_retry_pending = false;

    set_bus_pins_safe();
    unmount_card(handle); /* Also retry an initial BLDO1-off failure. */

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(AETHER_STORAGE_POLL_MS));
        const bool sample = read_card_detect();
        if (sample == candidate) {
            if (stable_samples < AETHER_STORAGE_DEBOUNCE_SAMPLES) {
                ++stable_samples;
            }
        } else {
            candidate = sample;
            stable_samples = 1;
        }

        portENTER_CRITICAL(&handle->state_lock);
        const bool stopping = handle->stop_requested;
        const bool was_inserted = handle->status.card_inserted;
        if (stopping || stable_samples >= AETHER_STORAGE_DEBOUNCE_SAMPLES) {
            publish_card_presence_locked(handle, !stopping && candidate);
        }
        if (!handle->status.card_inserted && !handle->card &&
            handle->status.state != AETHER_STORAGE_REMOVING) {
            handle->status.state = AETHER_STORAGE_NO_CARD;
        }
        const aether_storage_status_t status = handle->status;
        const bool explicit_retry=handle->mount_retry_requested;
        handle->mount_retry_requested=false;
        const bool retry_suspended=handle->diagnostics.retry_suspended;
        portEXIT_CRITICAL(&handle->state_lock);
        if (service_shutdown(handle)) continue;
        if (service_usb(handle)) continue;
        if ((!was_inserted && status.card_inserted) || explicit_retry) {
            mount_retry_pending = false;
        }

        if (status.state == AETHER_STORAGE_REMOVING ||
            ((!status.card_inserted || stopping) && handle->card)) {
            if (unmount_card(handle)) {
                mount_retry_pending = false;
            }
        } else if (stopping) {
            break;
        } else if (status.card_inserted && !status.mounted && !retry_suspended &&
                   (!mount_retry_pending ||
                    (int32_t)(xTaskGetTickCount() - next_mount_retry) >= 0)) {
            if (mount_card(handle) != ESP_OK) {
                portENTER_CRITICAL(&handle->state_lock);
                const uint32_t delay_ms=handle->diagnostics.retry_delay_ms;
                portEXIT_CRITICAL(&handle->state_lock);
                next_mount_retry = xTaskGetTickCount()+pdMS_TO_TICKS(delay_ms);
                mount_retry_pending = true;
            } else {
                mount_retry_pending = false;
            }
        }
    }

    portENTER_CRITICAL(&handle->state_lock);
    handle->task_running = false; /* No handle access after this critical section. */
    portEXIT_CRITICAL(&handle->state_lock);
    vTaskDelete(NULL);
}

esp_err_t aether_storage_new(aether_board_t *board,
                             aether_storage_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(board && board->pmic && out_handle, ESP_ERR_INVALID_ARG,
                        TAG, "invalid argument");
    *out_handle = NULL;
    aether_storage_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");

    handle->board = board;
    handle->dma_buffer = heap_caps_aligned_alloc(64, AETHER_STORAGE_DMA_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!handle->dma_buffer) { free(handle); return ESP_ERR_NO_MEM; }
    handle->raw_lock = xSemaphoreCreateMutex();
    if (!handle->raw_lock) { free(handle->dma_buffer); free(handle); return ESP_ERR_NO_MEM; }
    handle->state_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    handle->status.state = AETHER_STORAGE_NO_CARD;
    handle->status.last_error = ESP_OK;
    handle->task_running = true;

    BaseType_t ok = xTaskCreate(storage_task, "tf_hotplug", 4096, handle, 5,
                                NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(handle->raw_lock);
        free(handle->dma_buffer);
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t aether_storage_get_diagnostics(aether_storage_handle_t h,aether_storage_diagnostics_t *out)
{
    if(!h||!out)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);*out=h->diagnostics;portEXIT_CRITICAL(&h->state_lock);
    return ESP_OK;
}
esp_err_t aether_storage_request_retry(aether_storage_handle_t h)
{
    if(!h)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    esp_err_t error=ESP_OK;
    if(h->stop_requested||h->usb_requested||h->usb_transition||h->usb_active||
        h->shutdown_requested||h->shutdown_transition||h->new_users_blocked)error=ESP_ERR_INVALID_STATE;
    else if(!h->status.card_inserted)error=ESP_ERR_NOT_FOUND;
    else if(!h->status.mounted)reset_mount_retry_locked(h);
    portEXIT_CRITICAL(&h->state_lock);
    return error;
}

esp_err_t aether_storage_del(aether_storage_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    portENTER_CRITICAL(&handle->state_lock);
    if (handle->usb_requested || handle->usb_transition || handle->usb_active ||
        handle->shutdown_requested || handle->shutdown_transition) {
        portEXIT_CRITICAL(&handle->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->stop_requested = true;
    handle->usb_requested = false;
    portEXIT_CRITICAL(&handle->state_lock);
    bool running = true;
    for (int i = 0; i < 100; ++i) {
        portENTER_CRITICAL(&handle->state_lock);
        running = handle->task_running;
        portEXIT_CRITICAL(&handle->state_lock);
        if (!running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_FALSE(!running, ESP_ERR_TIMEOUT, TAG,
                        "storage task did not stop");
    vSemaphoreDelete(handle->raw_lock);
    free(handle->dma_buffer);
    free(handle);
    return ESP_OK;
}

esp_err_t aether_storage_get_status(aether_storage_handle_t handle,
                                    aether_storage_status_t *status)
{
    ESP_RETURN_ON_FALSE(handle && status, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    portENTER_CRITICAL(&handle->state_lock);
    *status = handle->status;
    portEXIT_CRITICAL(&handle->state_lock);
    return ESP_OK;
}

uint32_t aether_storage_generation(aether_storage_handle_t handle)
{
    if (!handle) return 0;
    portENTER_CRITICAL(&handle->state_lock);
    const uint32_t generation = handle->local_generation;
    portEXIT_CRITICAL(&handle->state_lock);
    return generation;
}

esp_err_t aether_storage_acquire(aether_storage_handle_t handle,
                                 uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    (void)timeout_ms; /* Lease acquisition is nonblocking; FatFS serializes I/O. */
    portENTER_CRITICAL(&handle->state_lock);
    const bool available = handle->status.mounted && handle->status.card_inserted &&
                           !handle->stop_requested && !handle->local_mount_invalidated && !handle->new_users_blocked &&
                           !handle->usb_requested && !handle->usb_transition &&
                           !handle->usb_active && !handle->shutdown_requested && !handle->shutdown_transition &&
                           handle->active_users < UINT32_MAX;
    if (available) {
        ++handle->active_users;
    }
    portEXIT_CRITICAL(&handle->state_lock);
    return available ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t aether_storage_gate_new_users(aether_storage_handle_t handle, bool blocked)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    portENTER_CRITICAL(&handle->state_lock);
    handle->new_users_blocked = blocked;
    portEXIT_CRITICAL(&handle->state_lock);
    return ESP_OK;
}

void aether_storage_release(aether_storage_handle_t handle)
{
    if (handle) {
        portENTER_CRITICAL(&handle->state_lock);
        const bool held = handle->active_users > 0;
        if (held) {
            --handle->active_users;
        }
        portEXIT_CRITICAL(&handle->state_lock);
        if (!held) {
            ESP_LOGE(TAG, "unbalanced TF lease release");
        }
    }
}

bool aether_storage_card_inserted(aether_storage_handle_t handle)
{
    if (!handle) {
        return false;
    }
    portENTER_CRITICAL(&handle->state_lock);
    /* A rapid reinsert cannot revive old users while the old mount drains. */
    const bool inserted = handle->status.card_inserted && !handle->stop_requested && !handle->local_mount_invalidated &&
                          handle->status.state != AETHER_STORAGE_REMOVING && !handle->usb_requested &&
                          !handle->usb_transition && !handle->usb_active &&
                          !handle->shutdown_requested && !handle->shutdown_transition;
    portEXIT_CRITICAL(&handle->state_lock);
    return inserted;
}

bool aether_storage_is_mounted(aether_storage_handle_t handle)
{
    if (!handle) {
        return false;
    }
    portENTER_CRITICAL(&handle->state_lock);
    const bool mounted = handle->status.mounted && !handle->stop_requested && !handle->local_mount_invalidated &&
                         !handle->usb_requested && !handle->usb_transition && !handle->usb_active &&
                          !handle->shutdown_requested && !handle->shutdown_transition;
    portEXIT_CRITICAL(&handle->state_lock);
    return mounted;
}

const char *aether_storage_mount_point(void)
{
    return AETHER_STORAGE_MOUNT_POINT;
}


bool aether_storage_usb_owned(aether_storage_handle_t h)
{
    if (!h) return false;
    portENTER_CRITICAL(&h->state_lock);
    const bool value = h->usb_requested || h->usb_transition || h->usb_active;
    portEXIT_CRITICAL(&h->state_lock);
    return value;
}

esp_err_t aether_storage_usb_begin(aether_storage_handle_t h, uint32_t timeout_ms)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (h->stop_requested || h->shutdown_requested || h->shutdown_transition || !h->status.card_inserted) {
        portEXIT_CRITICAL(&h->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!h->usb_requested && !h->usb_transition && !h->usb_active) {
        h->usb_requested = true;
        h->usb_transition = true;
        h->usb_failed = false;
        h->usb_error = ESP_OK;
        h->status.mounted = false;
        h->status.state = AETHER_STORAGE_USB_PREPARING;
    }
    portEXIT_CRITICAL(&h->state_lock);
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&h->state_lock);
        const bool ready = h->usb_active;
        const bool failed = h->usb_failed;
        const esp_err_t error = h->usb_error;
        portEXIT_CRITICAL(&h->state_lock);
        if (ready) return ESP_OK;
        if (failed) return error != ESP_OK ? error : ESP_FAIL;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Restoring local ownership does not require a healthy filesystem. A card
 * can remain ERROR after a checked mount attempt, with no users/host/power
 * cleanup left. Keep that error visible, but do not trap USB exit or aborted
 * shutdown behind an impossible requirement to mount a damaged/unknown volume. */
static bool local_error_released_locked(aether_storage_handle_t h)
{
    return h->status.state==AETHER_STORAGE_ERROR && h->diagnostics.failure_count>0 &&
        !h->status.mounted&&!h->card&&!h->active_users&&!h->raw_card&&
        !h->raw_host_initialized&&!h->power_retry_pending;
}

esp_err_t aether_storage_usb_end(aether_storage_handle_t h, uint32_t timeout_ms)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (h->usb_requested || h->usb_transition || h->usb_active) {
        h->usb_requested = false;
        h->usb_transition = true;
    }
    portEXIT_CRITICAL(&h->state_lock);
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&h->state_lock);
        const bool released = !h->usb_transition && !h->usb_active && !h->usb_requested;
        const aether_storage_status_t status = h->status;
        const bool stopping = h->stop_requested;
        const bool local_error_released=local_error_released_locked(h);
        portEXIT_CRITICAL(&h->state_lock);
        if (released && (status.mounted || !status.card_inserted || stopping || local_error_released)) return ESP_OK;
        if (released && status.state == AETHER_STORAGE_ERROR && status.last_error != ESP_OK)
            return status.last_error;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t raw_transfer(aether_storage_handle_t h, uint64_t offset,
                               void *buffer, size_t bytes, bool writing)
{
    if (!h || (!buffer && bytes)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->raw_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&h->state_lock);
    const bool ready = h->usb_requested && h->usb_active && !h->stop_requested &&
                       h->status.card_inserted && !h->usb_failed;
    const uint64_t capacity = h->status.capacity_bytes;
    portEXIT_CRITICAL(&h->state_lock);
    esp_err_t err = ESP_OK;
    if (!ready || !h->raw_card || !read_card_detect()) err = ESP_ERR_INVALID_STATE;
    else if (offset > capacity || bytes > capacity - offset) err = ESP_ERR_INVALID_SIZE;
    uint8_t *data = buffer;
    while (err == ESP_OK && bytes) {
        if (!read_card_detect()) { err = ESP_ERR_INVALID_STATE; break; }
        const size_t lba = (size_t)(offset / 512);
        const size_t inside = (size_t)(offset % 512);
        size_t chunk;
        if (inside || bytes < 512) {
            chunk = 512 - inside;
            if (chunk > bytes) chunk = bytes;
            err = sdmmc_read_sectors(h->raw_card, h->dma_buffer, lba, 1);
            if (err == ESP_OK) {
                if (writing) {
                    memcpy((uint8_t *)h->dma_buffer + inside, data, chunk);
                    err = sdmmc_write_sectors(h->raw_card, h->dma_buffer, lba, 1);
                } else memcpy(data, (uint8_t *)h->dma_buffer + inside, chunk);
            }
        } else {
            size_t sectors = bytes / 512;
            if (sectors > AETHER_STORAGE_USB_DIRECT_BYTES / 512)
                sectors = AETHER_STORAGE_USB_DIRECT_BYTES / 512;
            chunk = sectors * 512;
            /* TinyUSB's aligned internal endpoint buffer can go straight to
             * SD DMA. Check both capability and the installed host's cache-line
             * requirements; all other buffers retain the reserved 4KiB bounce. */
            const bool direct = esp_ptr_dma_capable(data) &&
                esp_ptr_dma_capable(data + chunk - 1) &&
                h->raw_card->host.check_buffer_alignment &&
                h->raw_card->host.check_buffer_alignment(h->raw_card->host.slot, data, chunk);
            if (direct) {
                err = writing ? sdmmc_write_sectors(h->raw_card, data, lba, sectors) :
                                sdmmc_read_sectors(h->raw_card, data, lba, sectors);
            } else {
                if (sectors > AETHER_STORAGE_DMA_BYTES / 512)
                    sectors = AETHER_STORAGE_DMA_BYTES / 512;
                chunk = sectors * 512;
                if (writing) {
                    memcpy(h->dma_buffer, data, chunk);
                    err = sdmmc_write_sectors(h->raw_card, h->dma_buffer, lba, sectors);
                } else {
                    err = sdmmc_read_sectors(h->raw_card, h->dma_buffer, lba, sectors);
                    if (err == ESP_OK) memcpy(data, h->dma_buffer, chunk);
                }
            }
        }
        offset += chunk;
        data += chunk;
        bytes -= chunk;
    }
    if (err != ESP_OK) {
        portENTER_CRITICAL(&h->state_lock);
        h->status.last_error = err;
        portEXIT_CRITICAL(&h->state_lock);
    }
    xSemaphoreGive(h->raw_lock);
    return err;
}

esp_err_t aether_storage_usb_read(aether_storage_handle_t h, uint64_t offset,
                                   void *buffer, size_t bytes)
{ return raw_transfer(h, offset, buffer, bytes, false); }

esp_err_t aether_storage_usb_write(aether_storage_handle_t h, uint64_t offset,
                                    const void *buffer, size_t bytes)
{ return raw_transfer(h, offset, (void *)buffer, bytes, true); }

esp_err_t aether_storage_usb_sync(aether_storage_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(h->raw_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&h->state_lock);
    const bool ready = h->usb_requested && h->usb_active && !h->stop_requested && !h->usb_failed;
    portEXIT_CRITICAL(&h->state_lock);
    const esp_err_t error = ready && h->raw_card && read_card_detect()
        ? sdmmc_get_status(h->raw_card) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(h->raw_lock);
    return error;
}


esp_err_t aether_storage_prepare_shutdown(aether_storage_handle_t h, uint32_t timeout_ms)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (h->usb_requested || h->usb_transition || h->usb_active || h->stop_requested) {
        portEXIT_CRITICAL(&h->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!h->shutdown_requested) {
        h->shutdown_requested = true;
        h->shutdown_transition = true;
        h->shutdown_complete = false;
        h->shutdown_error = ESP_OK;
        h->status.mounted = false;
    }
    portEXIT_CRITICAL(&h->state_lock);
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&h->state_lock);
        const bool complete = h->shutdown_complete;
        const esp_err_t error = h->shutdown_error;
        portEXIT_CRITICAL(&h->state_lock);
        if (complete) return ESP_OK;
        if (error != ESP_OK) return error;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t aether_storage_cancel_shutdown(aether_storage_handle_t h, uint32_t timeout_ms)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->state_lock);
    if (h->shutdown_requested || h->shutdown_transition || h->shutdown_complete) {
        h->shutdown_requested = false;
        h->shutdown_transition = true;
    }
    portEXIT_CRITICAL(&h->state_lock);
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&h->state_lock);
        const bool restored = !h->shutdown_transition && !h->shutdown_requested;
        const aether_storage_status_t status = h->status;
        const bool local_error_released=local_error_released_locked(h);
        portEXIT_CRITICAL(&h->state_lock);
        if (restored && (status.mounted || !status.card_inserted || local_error_released)) return ESP_OK;
        if (restored && status.state == AETHER_STORAGE_ERROR && status.last_error != ESP_OK)
            return status.last_error;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

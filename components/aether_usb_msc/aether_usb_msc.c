#include "aether_usb_msc.h"
#include "aether_usb_bus.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "aether_board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_disk";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_control;
static aether_storage_handle_t s_storage;
static aether_usb_msc_status_t s_status;
static bool s_installed, s_accepting, s_prevent_removal, s_bus_reserved;
static uint32_t s_callbacks;
static bool s_host_seen;
static aether_usb_msc_vbus_query_t s_vbus_query;
static void *s_vbus_context;
static char s_serial[13] = "000000000000";

#define USB_DESC_LENGTH (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = 0, .bDeviceSubClass = 0,
    .bDeviceProtocol = 0, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303a, .idProduct = 0x40a1, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};
static const uint8_t s_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_DESC_LENGTH, 0, 100),
    TUD_MSC_DESCRIPTOR(0, 4, 0x01, 0x81, 64),
};
static const uint8_t s_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_DESC_LENGTH, 0, 100),
    TUD_MSC_DESCRIPTOR(0, 4, 0x01, 0x81, 512),
};
static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t), .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200, .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, .bNumConfigurations = 1,
};
static const char *s_strings[] = {(const char[]){0x09,0x04}, "Aether01", "Aether01 TF Card", s_serial, "TF Card"};

/* esp_tinyusb 2.3.0's driver calls these internal integration hooks on attach
 * and detach. This component owns all MSC callbacks and local mount policy.
 * Defining the hooks here keeps upstream tinyusb_msc.c's default deferred-write
 * storage driver out of the static link: it must never auto-mount this card. */
void msc_storage_mount_to_usb(void) { }
void msc_storage_mount_to_app(void) { }

static uint32_t remaining_ms(TickType_t start, uint32_t limit)
{
    const TickType_t elapsed = xTaskGetTickCount() - start;
    const uint64_t ms = (uint64_t)elapsed * portTICK_PERIOD_MS;
    return ms < limit ? limit - (uint32_t)ms : 0;
}
static void diagnostic_stage(aether_usb_msc_stage_t stage)
{
    portENTER_CRITICAL(&s_lock);
    s_status.diagnostic_stage = stage;
    portEXIT_CRITICAL(&s_lock);
}
static esp_err_t select_route(int level)
{
    const esp_err_t error = gpio_set_level(AETHER_GPIO_USB_SELECT, level);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.selected_route = (uint8_t)level;
        portEXIT_CRITICAL(&s_lock);
    }
    return error;
}
static void update_state(aether_usb_msc_state_t state, bool active, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.active = active;
    s_status.last_error = error;
    if (state == AETHER_USB_MSC_STARTING || !active) s_status.usb_speed_mbps = 0;
    portEXIT_CRITICAL(&s_lock);
}
static void usb_event(tinyusb_event_t *event, void *context)
{
    (void)context;
    /* ATTACHED is delivered after USB SetConfiguration. Query the negotiated
     * rate once on the USB task, outside our lock; GUI status stays cached. */
    uint16_t speed_mbps = 0;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        const tusb_speed_t speed = tud_speed_get();
        if (speed == TUSB_SPEED_HIGH) speed_mbps = 480;
        else if (speed == TUSB_SPEED_FULL) speed_mbps = 12;
    }
    portENTER_CRITICAL(&s_lock);
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_status.usb_speed_mbps = speed_mbps;
        s_status.host_connected = true;
        s_status.diagnostic_stage = AETHER_USB_MSC_STAGE_HOST;
        s_host_seen = true;
        s_status.host_ejected = false;
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_status.usb_speed_mbps = 0;
        s_status.host_connected = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t aether_usb_msc_init(aether_storage_handle_t storage)
{
    if (!storage) return ESP_ERR_INVALID_ARG;
    if (s_storage) return s_storage == storage ? ESP_OK : ESP_ERR_INVALID_STATE;
    SemaphoreHandle_t control = xSemaphoreCreateMutex();
    if (!control) return ESP_ERR_NO_MEM;
    uint8_t mac[6];
    /* The default-MAC API writes an 8-byte EUI-64 on S31 (802.15.4).
     * Factory MAC is explicitly the 6-byte identifier used by USB here. */
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK)
        snprintf(s_serial, sizeof(s_serial), "%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    portENTER_CRITICAL(&s_lock);
    s_control = control;
    s_storage = storage;
    s_status = (aether_usb_msc_status_t){.state=AETHER_USB_MSC_OFF};
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void aether_usb_msc_set_vbus_query(aether_usb_msc_vbus_query_t query, void *context)
{
    portENTER_CRITICAL(&s_lock);
    s_vbus_query = query;
    s_vbus_context = context;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t aether_usb_msc_get_status(aether_usb_msc_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    aether_storage_handle_t storage = s_storage;
    portEXIT_CRITICAL(&s_lock);
    if (!storage) return ESP_ERR_INVALID_STATE;
    aether_storage_status_t card;
    if (aether_storage_get_status(storage, &card) == ESP_OK) {
        status->media_present = card.card_inserted && card.usb_exported;
        status->capacity_bytes = card.capacity_bytes;
        status->usb_clock_khz = card.usb_clock_khz;
        status->usb_high_speed_fallback = card.usb_high_speed_fallback;
        if (status->active && status->last_error == ESP_OK && !status->media_present)
            status->last_error = card.last_error;
    }
    return ESP_OK;
}
bool aether_usb_msc_is_active(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool active = s_status.active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

/* Call after all host callbacks have ended. A failed uninstall leaves the card
 * exported/gated; retry may recover, but it must never publish a local mount. */
static esp_err_t leave_locked(uint32_t timeout_ms, bool confirmed)
{
    const TickType_t start = xTaskGetTickCount();
    aether_storage_status_t card;
    const bool medium_present = aether_storage_get_status(s_storage, &card) != ESP_OK || card.card_inserted;
    portENTER_CRITICAL(&s_lock);
    aether_usb_msc_vbus_query_t vbus_query = s_vbus_query;
    void *vbus_context = s_vbus_context;
    portEXIT_CRITICAL(&s_lock);
    bool vbus_present = true;
    const bool vbus_known = vbus_query && vbus_query(&vbus_present, vbus_context) == ESP_OK;
    /* A USB reset can temporarily report unmounted without unplugging the
     * host. Conversely a battery-powered device may only see suspend on cable
     * removal. A board VBUS query distinguishes both before releasing the disk. */
    portENTER_CRITICAL(&s_lock);
    /* The query may yield to USB ATTACHED or START_STOP(load) callbacks. Check
     * their latest ownership state in the same lock that closes admission. */
    const bool host_may_own = vbus_known ? vbus_present && s_host_seen :
        s_status.host_connected || s_host_seen;
    if (!confirmed && s_installed && host_may_own && !s_status.host_ejected && s_accepting && medium_present) {
        s_status.last_error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_accepting = false;
    s_status.state = AETHER_USB_MSC_STOPPING;
    s_status.usb_speed_mbps = 0;
    s_status.diagnostic_stage = AETHER_USB_MSC_STAGE_DRAIN;
    portEXIT_CRITICAL(&s_lock);
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const uint32_t callbacks = s_callbacks;
        portEXIT_CRITICAL(&s_lock);
        if (!callbacks) break;
        if (!remaining_ms(start, timeout_ms)) {
            update_state(AETHER_USB_MSC_ERROR, true, ESP_ERR_TIMEOUT);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (s_installed) {
        /* Every WRITE10 already completed synchronously. This checks final SD
         * readiness; a removed card is still safe to detach after callback drain. */
        const esp_err_t sync = aether_storage_usb_sync(s_storage);
        if (sync != ESP_OK) ESP_LOGW(TAG, "TF final status: %s", esp_err_to_name(sync));
        diagnostic_stage(AETHER_USB_MSC_STAGE_STACK_STOP);
        tud_disconnect();
        const esp_err_t error = tinyusb_driver_uninstall();
        if (error != ESP_OK) {
            update_state(AETHER_USB_MSC_ERROR, true, error);
            return error;
        }
        s_installed = false;
    }
    /* Switch the board mux back only after complete OTG teardown. LOW is the
     * measured working ROM/USJ download route; never auto-cycle a live host. */
    const esp_err_t route_error = select_route(AETHER_USB_DOWNLOAD_SELECT_LEVEL);
    portENTER_CRITICAL(&s_lock);
    s_status.host_connected = false;
    s_status.host_ejected = false;
    s_prevent_removal = false;
    s_host_seen = false;
    portEXIT_CRITICAL(&s_lock);
    diagnostic_stage(AETHER_USB_MSC_STAGE_RESTORE_TF);
    const esp_err_t restored = aether_storage_usb_end(s_storage, remaining_ms(start, timeout_ms));
    const bool owned = aether_storage_usb_owned(s_storage);
    esp_err_t error = restored != ESP_OK ? restored : route_error;
    if (!owned && !s_installed && route_error == ESP_OK && s_bus_reserved) {
        const esp_err_t released = aether_usb_bus_release(AETHER_USB_BUS_MSC);
        if (released == ESP_OK) s_bus_reserved = false;
        else if (error == ESP_OK) error = released;
    }
    update_state(error == ESP_OK ? AETHER_USB_MSC_OFF : AETHER_USB_MSC_ERROR,
                 owned || s_bus_reserved, error);
    if (error == ESP_OK) diagnostic_stage(AETHER_USB_MSC_STAGE_OFF);
    if (!owned) ESP_LOGI(TAG, "USB disk stopped; local TF/USB download route restored: %s", esp_err_to_name(error));
    return error;
}

esp_err_t aether_usb_msc_enter(uint32_t timeout_ms)
{
    if (!s_control || !s_storage) return ESP_ERR_INVALID_STATE;
    const TickType_t start = xTaskGetTickCount();
    if (xSemaphoreTake(s_control, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (aether_usb_msc_is_active()) {
        xSemaphoreGive(s_control);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t reserved = aether_usb_bus_claim(AETHER_USB_BUS_MSC);
    if (reserved != ESP_OK) {
        xSemaphoreGive(s_control);
        return reserved; /* OTG Host may reserve the port while waiting for CC. */
    }
    s_bus_reserved = true;
    update_state(AETHER_USB_MSC_STARTING, true, ESP_OK);
    diagnostic_stage(AETHER_USB_MSC_STAGE_STORAGE);
    esp_err_t error = aether_storage_usb_begin(s_storage, remaining_ms(start, timeout_ms));
    if (error == ESP_OK) {
        aether_storage_status_t card;
        error = aether_storage_get_status(s_storage, &card);
        if (error == ESP_OK && (!card.usb_exported || !card.capacity_bytes || card.capacity_bytes / 512 > UINT32_MAX))
            error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK && !remaining_ms(start, timeout_ms)) error = ESP_ERR_TIMEOUT;
    if (error == ESP_OK) {
        diagnostic_stage(AETHER_USB_MSC_STAGE_ROUTE);
        ESP_LOGI(TAG, "USB mux: download=%d -> MSC=%d (explicit settings request)",
                 AETHER_USB_DOWNLOAD_SELECT_LEVEL, AETHER_USB_MSC_SELECT_LEVEL);
        error = select_route(AETHER_USB_MSC_SELECT_LEVEL);
    }
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "starting USB TF disk; USB serial port will disconnect");
        tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event, NULL);
        config.descriptor.device = &s_device;
        config.descriptor.full_speed_config = s_fs;
        config.descriptor.high_speed_config = s_hs;
        config.descriptor.qualifier = &s_qualifier;
        config.descriptor.string = s_strings;
        config.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
        portENTER_CRITICAL(&s_lock);
        s_accepting = true;
        s_status.host_ejected = false;
        s_prevent_removal = false;
        s_host_seen = false;
        portEXIT_CRITICAL(&s_lock);
        diagnostic_stage(AETHER_USB_MSC_STAGE_STACK_START);
        error = tinyusb_driver_install(&config);
        if (error == ESP_OK) s_installed = true;
    }
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        const aether_usb_msc_stage_t failed_stage = s_status.diagnostic_stage;
        portEXIT_CRITICAL(&s_lock);
        const esp_err_t restored = leave_locked(remaining_ms(start, timeout_ms), false);
        update_state(AETHER_USB_MSC_ERROR, aether_storage_usb_owned(s_storage) || s_installed || s_bus_reserved, error);
        diagnostic_stage(failed_stage);
        ESP_LOGW(TAG, "USB start failed stage=%u: %s (rollback %s)", (unsigned)failed_stage, esp_err_to_name(error), esp_err_to_name(restored));
    } else {
        update_state(AETHER_USB_MSC_ACTIVE, true, ESP_OK);
        portENTER_CRITICAL(&s_lock);
        if (!s_status.host_connected) s_status.diagnostic_stage = AETHER_USB_MSC_STAGE_WAIT_HOST;
        portEXIT_CRITICAL(&s_lock);
    }
    xSemaphoreGive(s_control);
    return error;
}

static esp_err_t exit_disk(uint32_t timeout_ms, bool confirmed)
{
    if (!s_control || !s_storage) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_control, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t error = aether_usb_msc_is_active() ? leave_locked(timeout_ms, confirmed) : ESP_OK;
    xSemaphoreGive(s_control);
    return error;
}

esp_err_t aether_usb_msc_exit(uint32_t timeout_ms)
{
    return exit_disk(timeout_ms, false);
}

esp_err_t aether_usb_msc_exit_confirmed(uint32_t timeout_ms)
{
    return exit_disk(timeout_ms, true);
}

static bool claim_io(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool allowed = s_accepting && s_status.active && !s_status.host_ejected;
    if (allowed) ++s_callbacks;
    portEXIT_CRITICAL(&s_lock);
    return allowed;
}
static void release_io(void)
{
    portENTER_CRITICAL(&s_lock);
    --s_callbacks;
    portEXIT_CRITICAL(&s_lock);
}
static void sense_error(uint8_t lun, esp_err_t error, bool write)
{
    if (error == ESP_ERR_INVALID_SIZE) tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
    else if (error == ESP_ERR_INVALID_STATE) tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0);
    else tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, write ? 0x0c : 0x11, 0);
}
uint8_t tud_msc_get_maxlun_cb(void) { return 1; }
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4])
{
    (void)lun;
    memcpy(vendor, "AETHER01", 8); memcpy(product, "TF Card Reader  ", 16); memcpy(revision, "1.00", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    aether_usb_msc_status_t status;
    if (lun == 0 && aether_usb_msc_get_status(&status) == ESP_OK && status.active &&
        status.media_present && !status.host_ejected) return true;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0);
    return false;
}
void tud_msc_capacity_cb(uint8_t lun, uint32_t *blocks, uint16_t *size)
{
    aether_usb_msc_status_t status;
    *size = 512;
    *blocks = lun == 0 && aether_usb_msc_get_status(&status) == ESP_OK && status.media_present
        ? (uint32_t)(status.capacity_bytes / 512) : 0;
}
bool tud_msc_is_writable_cb(uint8_t lun) { return tud_msc_test_unit_ready_cb(lun); }
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bytes)
{
    if (lun != 0 || bytes > INT32_MAX || !claim_io()) { sense_error(lun, ESP_ERR_INVALID_STATE, false); return -1; }
    esp_err_t error = aether_storage_usb_read(s_storage, (uint64_t)lba * 512 + offset, buffer, bytes);
    release_io();
    if (error != ESP_OK) { sense_error(lun, error, false); return -1; }
    return (int32_t)bytes;
}
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bytes)
{
    if (lun != 0 || bytes > INT32_MAX || !claim_io()) { sense_error(lun, ESP_ERR_INVALID_STATE, true); return -1; }
    esp_err_t error = aether_storage_usb_write(s_storage, (uint64_t)lba * 512 + offset, buffer, bytes);
    release_io();
    if (error != ESP_OK) { sense_error(lun, error, true); return -1; }
    return (int32_t)bytes; /* ACK only after SD write completion, no deferred cache. */
}
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power, bool start, bool load_eject)
{
    (void)power;
    if (lun != 0) return false;
    if (!load_eject) return true;
    portENTER_CRITICAL(&s_lock);
    const bool prevent = s_prevent_removal;
    portEXIT_CRITICAL(&s_lock);
    if (!start && prevent) { tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x53, 0x02); return false; }
    if (!start && claim_io()) {
        const esp_err_t error = aether_storage_usb_sync(s_storage);
        release_io();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) { sense_error(lun,error,true); return false; }
    }
    portENTER_CRITICAL(&s_lock);
    s_status.host_ejected = !start;
    if (!start) s_status.diagnostic_stage = AETHER_USB_MSC_STAGE_EJECTED;
    portEXIT_CRITICAL(&s_lock);
    return true;
}
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control)
{
    (void)control;
    if (lun != 0) return false;
    /* TinyUSB dispatches this built-in SCSI command to this callback, not to
     * tud_msc_scsi_cb(). ALLOW alone does not mean that the host ejected. */
    portENTER_CRITICAL(&s_lock);
    s_prevent_removal = (prohibit_removal & 1) != 0;
    portEXIT_CRITICAL(&s_lock);
    return true;
}
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void *buffer, uint16_t bytes)
{
    (void)buffer; (void)bytes;
    if (lun != 0) return -1;
    if (command[0] == 0x35) { /* SYNCHRONIZE CACHE(10) */
        if (!claim_io()) { sense_error(lun,ESP_ERR_INVALID_STATE,true); return -1; }
        const esp_err_t error = aether_storage_usb_sync(s_storage);
        release_io();
        if (error != ESP_OK) { sense_error(lun,error,true); return -1; }
        return 0;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return -1;
}

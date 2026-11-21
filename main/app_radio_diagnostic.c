#include "app_radio_diagnostic.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "radio_budget";

static void snapshot(const char *phase, aether_bluetooth_handle_t bt, aether_wifi_handle_t wifi)
{
    aether_bluetooth_status_t b = {0};
    aether_wifi_status_t w = {0};
    const esp_err_t be = aether_bluetooth_get_status(bt, &b);
    const esp_err_t we = aether_wifi_get_status(wifi, &w);
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "%s BT read=%d state=%d enabled=%u err=%d found=%u WiFi read=%d state=%d enabled=%u err=%d APs=%u DMA=%u largest=%u",
        phase, be, b.state, b.enabled, b.last_error, b.device_count,
        we, w.state, w.enabled, w.last_error, w.ap_count,
        (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps));
}

void app_radio_diagnostic_run(aether_bluetooth_handle_t bt, aether_wifi_handle_t wifi, bool keep_enabled)
{
    if (!bt || !wifi) { ESP_LOGW(TAG, "missing radio service"); return; }
    bool qualified = false;
    snapshot("before", bt, wifi);
    esp_err_t error = aether_bluetooth_set_enabled(bt, true);
    ESP_LOGI(TAG, "BT first enable accepted=%d", error);
    int64_t deadline = esp_timer_get_time() + 10000000;
    aether_wifi_status_t w = {0};
    aether_bluetooth_status_t b = {0};
    while (error == ESP_OK && esp_timer_get_time() < deadline) {
        aether_bluetooth_get_status(bt, &b);
        if (b.enabled || b.state == AETHER_BT_ERROR) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    snapshot("BT initialized", bt, wifi);
    if (b.enabled) {
        const esp_err_t bt_scan = aether_bluetooth_scan(bt);
        bool bt_scan_seen = false;
        deadline = esp_timer_get_time() + 1000000;
        while (bt_scan == ESP_OK && esp_timer_get_time() < deadline) {
            aether_bluetooth_get_status(bt, &b);
            if (b.scanning) { bt_scan_seen = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        snapshot("BT scan before WiFi", bt, wifi);
        error = aether_wifi_set_enabled(wifi, true);
        ESP_LOGI(TAG, "WiFi second enable accepted=%d", error);
        deadline = esp_timer_get_time() + 10000000;
        while (error == ESP_OK && esp_timer_get_time() < deadline) {
            aether_wifi_get_status(wifi, &w);
            if (w.enabled || w.state == AETHER_WIFI_ERROR) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        snapshot("both initialized", bt, wifi);
        if (w.enabled) {
            const uint32_t scan_generation = w.scan_generation;
            const esp_err_t wifi_scan = aether_wifi_scan(wifi);
            ESP_LOGI(TAG, "scan accepted BT=%d WiFi=%d", bt_scan, wifi_scan);
            deadline = esp_timer_get_time() + 20000000;
            while (esp_timer_get_time() < deadline) {
                aether_bluetooth_get_status(bt, &b);
                aether_wifi_get_status(wifi, &w);
                bt_scan_seen |= b.scanning;
                if (bt_scan_seen && !b.scanning && w.scan_generation != scan_generation && !w.scanning) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            const bool scans_ok = bt_scan == ESP_OK && wifi_scan == ESP_OK &&
                bt_scan_seen && !b.scanning && w.scan_generation != scan_generation && !w.scanning &&
                b.last_error == ESP_OK && w.last_error == ESP_OK;
            ESP_LOGI(TAG, "scan completion success=%u BT_started=%u BT_done=%u WiFi_generation_advanced=%u",
                scans_ok, bt_scan_seen, !b.scanning, w.scan_generation != scan_generation);
            snapshot("scan result", bt, wifi);
            qualified = scans_ok;
        }
    }
    if (keep_enabled && qualified) {
        snapshot("retained for media coexistence", bt, wifi);
        return;
    }
    const esp_err_t wifi_off = aether_wifi_set_enabled(wifi, false);
    const esp_err_t bt_off = aether_bluetooth_set_enabled(bt, false);
    deadline = esp_timer_get_time() + 10000000;
    while (esp_timer_get_time() < deadline) {
        aether_wifi_get_status(wifi, &w); aether_bluetooth_get_status(bt, &b);
        if (!b.enabled && !w.enabled && b.state == AETHER_BT_OFF && w.state == AETHER_WIFI_OFF) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "cleanup success=%u BT_accepted=%d WiFi_accepted=%d",
        bt_off == ESP_OK && wifi_off == ESP_OK && !b.enabled && !w.enabled &&
        b.state == AETHER_BT_OFF && w.state == AETHER_WIFI_OFF, bt_off, wifi_off);
    snapshot("cleanup result", bt, wifi);
}

static struct { aether_bluetooth_handle_t bt; aether_wifi_handle_t wifi; } s_diagnostic;
static StaticTask_t s_diagnostic_tcb __attribute__((aligned(16)));
static StackType_t *s_diagnostic_stack;
static atomic_bool s_diagnostic_claimed;
/* A diagnostic must not consume the HP/DMA budget it measures. The worker only
 * submits asynchronous radio commands and reads cached status; actual controller,
 * host and NVS operations run on the services' own internal owner tasks.
 * Application-owned static-task stack is retained until reboot after self-delete.
 * Normal builds have no references to these opt-in diagnostic entry points. */
static void diagnostic_task(void *context)
{
    (void)context;
    /* Let startup rendering, TF mounting and main-task cleanup finish first. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    app_radio_diagnostic_run(s_diagnostic.bt, s_diagnostic.wifi, false);
    ESP_LOGI(TAG, "diagnostic PSRAM stack minimum_free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t app_radio_diagnostic_start(aether_bluetooth_handle_t bt, aether_wifi_handle_t wifi)
{
    if (!bt || !wifi) return ESP_ERR_INVALID_ARG;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_diagnostic_claimed, &expected, true)) return ESP_ERR_INVALID_STATE;
    s_diagnostic.bt = bt; s_diagnostic.wifi = wifi;
    s_diagnostic_stack = heap_caps_aligned_alloc(16, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_diagnostic_stack) {
        atomic_store(&s_diagnostic_claimed, false); return ESP_ERR_NO_MEM;
    }
    if (!xTaskCreateStaticPinnedToCore(diagnostic_task, "radio_check", 4096, NULL, 2,
            s_diagnostic_stack, &s_diagnostic_tcb, tskNO_AFFINITY)) {
        heap_caps_free(s_diagnostic_stack); s_diagnostic_stack = NULL;
        atomic_store(&s_diagnostic_claimed, false); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

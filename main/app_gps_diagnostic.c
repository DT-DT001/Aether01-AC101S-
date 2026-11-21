#include "app_gps_diagnostic.h"
#include "app_gps.h"
#include "app_otg_control.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define GPS_PROBE_TIMEOUT_US INT64_C(300000000)
#define GPS_PROBE_STABLE_FIX_US INT64_C(10000000)
#define GPS_PROBE_LOG_INTERVAL_US INT64_C(5000000)
#define GPS_PROBE_PPS_STALE_MS 2500U

static atomic_bool s_manual_tests_enabled = ATOMIC_VAR_INIT(false);

bool app_gps_diagnostic_manual_tests_enabled(void)
{
    return atomic_load_explicit(&s_manual_tests_enabled, memory_order_acquire);
}

#ifndef AETHER_GPS_DIAGNOSTIC_PASSIVE
static void log_gps_status(const app_gps_status_t *status, uint32_t elapsed_ms,
                           int64_t first_fix_ms, uint32_t fix_run_ms,
                           uint32_t fix_updates, uint32_t pps_run,
                           uint32_t pps_total)
{
    const gnss_nmea_snapshot_t *nav = &status->navigation;
    unsigned cn0_max = 0, cn0_sum = 0, cn0_count = 0;
    const unsigned count = nav->satellite_count < GNSS_MAX_SATELLITES ?
        nav->satellite_count : GNSS_MAX_SATELLITES;
    for (unsigned i = 0; i < count; ++i) {
        if (!nav->satellites[i].signal_valid) continue;
        const unsigned cn0 = nav->satellites[i].snr_dbhz;
        if (cn0 > cn0_max) cn0_max = cn0;
        cn0_sum += cn0;
        ++cn0_count;
    }
    const unsigned cn0_average_x10 = cn0_count ? cn0_sum * 10U / cn0_count : 0;
    ESP_LOGI("gps_probe", "t=%lums state=%u requested=%u active=%u source=%s voltage=%umV(valid=%u) UART=%u NMEA=%lu bad=%lu malformed=%lu sats=%u used=%u PPS=%lu fresh=%u age=%lums",
        (unsigned long)elapsed_ms, (unsigned)status->state, status->requested_enabled,
        status->active, status->external_power ? "USB-input" : status->active ? "OTG-output" : "pending",
        status->supply_mv, status->supply_valid, status->uart_receiving,
        (unsigned long)nav->accepted_sentences, (unsigned long)nav->bad_checksums,
        (unsigned long)nav->malformed_sentences, nav->satellite_count,
        nav->satellites_used, (unsigned long)status->pps_count,
        status->pps_seen && status->pps_age_ms < GPS_PROBE_PPS_STALE_MS,
        (unsigned long)status->pps_age_ms);
    ESP_LOGI("gps_probe", "position=%u lat=%.6f lon=%.6f time=%u date=%u UTC=%04u-%02u-%02uT%02u:%02u:%02u quality=%u dimension=%u HDOP=%.2f(valid=%u) CN0_max=%u avg=%u.%u count=%u first_fix_ms=%lld fix_run_ms=%lu fix_updates=%lu PPS_run=%lu PPS_total=%lu error=%s",
        nav->position_valid, nav->latitude_deg, nav->longitude_deg,
        nav->time_valid, nav->date_valid, nav->year, nav->month, nav->day,
        nav->hour, nav->minute, nav->second, nav->fix_quality, nav->fix_dimension,
        (double)nav->hdop, nav->hdop_valid, cn0_max,
        cn0_average_x10 / 10U, cn0_average_x10 % 10U, cn0_count,
        (long long)first_fix_ms, (unsigned long)fix_run_ms,
        (unsigned long)fix_updates, (unsigned long)pps_run,
        (unsigned long)pps_total, esp_err_to_name(status->last_error));
}
#endif

/* These getters only copy retained worker snapshots; this task must not take
 * an external-input lease, issue an ADC/I2C read, or request source power. */
static void log_otg_cache(void)
{
    aether_usb_host_status_t host = {0};
    app_otg_power_status_t power = {0};
    const esp_err_t host_read = app_otg_get_status(&host);
    const esp_err_t power_read = app_otg_get_power_status(&power);
    ESP_LOGI("otg_watch", "cache-only host_read=%s state=%u enabled=%u active=%u VBUS_command=%u cleanup_pending=%u host_error=%s power_read=%s manual_requested=%u manual_failed=%u history=%u age=%lums valid=%u output_verified=%u",
        esp_err_to_name(host_read), (unsigned)host.state, host.enabled,
        host.active, host.vbus_enabled, host.cleanup_pending,
        esp_err_to_name(host.last_error), esp_err_to_name(power_read),
        power.manual_source_requested, power.manual_source_failed,
        power.failure_snapshot, (unsigned long)power.age_ms,
        power.valid, power.output_verified);
    ESP_LOGI("otg_watch", "cache-only VBUS=%umV(valid=%u trusted=%u) raw=%u TS=%umV(valid=%u) diag_valid=%u E7_CC=0x%02X R0B_CC_DETECT=0x%02X E3_CC_MODE=0x%02X R19_BOOST=0x%02X R13_RBFET=0x%02X C0_ADC=0x%02X TS_config=0x%02X work_faults=0x%02X boost=%u rbfet=%u last=%s verify=%s cleanup=%s diagnostic=%s",
        power.vbus_mv, power.vbus_valid, power.vbus_adc_trusted,
        power.vbus_raw, power.ts_mv, power.ts_valid, power.diagnostics_valid,
        power.cc_raw, power.cc_detection_raw, power.cc_mode_raw,
        power.control_raw, power.rbfet_control_raw,
        power.adc_enable_raw, power.ts_config, power.work_faults,
        power.boost_enabled, power.rbfet_enabled,
        esp_err_to_name(power.last_error), esp_err_to_name(power.verify_error),
        esp_err_to_name(power.cleanup_error), esp_err_to_name(power.diagnostic_error));
}

static void log_gps_cache(const app_gps_status_t *current,
                          const app_gps_status_t *last_active, bool last_active_valid,
                          int64_t last_active_at_us, int64_t now)
{
    const gnss_nmea_snapshot_t *nav = &current->navigation;
    ESP_LOGI("gps_watch", "cache-only current state=%u requested=%u active=%u source=%s NMEA=%lu bad=%lu malformed=%lu sats=%u used=%u fix=%u time=%u PPS=%lu PPS_age=%lums error=%s manual_tests_allowed=%u",
        (unsigned)current->state, current->requested_enabled, current->active,
        current->external_power ? "USB-input" : current->active ? "OTG-output" : "pending",
        (unsigned long)nav->accepted_sentences, (unsigned long)nav->bad_checksums,
        (unsigned long)nav->malformed_sentences, nav->satellite_count,
        nav->satellites_used, nav->position_valid, nav->time_valid,
        (unsigned long)current->pps_count, (unsigned long)current->pps_age_ms,
        esp_err_to_name(current->last_error), app_gps_diagnostic_manual_tests_enabled());
    const gnss_nmea_snapshot_t *last = &last_active->navigation;
    ESP_LOGI("gps_watch", "historical last_active_valid=%u age_ms=%lld source=%s state=%u NMEA=%lu bad=%lu malformed=%lu sats=%u used=%u fix=%u time=%u PPS=%lu error=%s",
        last_active_valid, (long long)(last_active_valid ? (now - last_active_at_us) / 1000 : -1),
        !last_active_valid ? "none" : last_active->external_power ? "USB-input" : "OTG-output",
        (unsigned)last_active->state, (unsigned long)last->accepted_sentences,
        (unsigned long)last->bad_checksums, (unsigned long)last->malformed_sentences,
        last->satellite_count, last->satellites_used, last->position_valid,
        last->time_valid, (unsigned long)last_active->pps_count,
        esp_err_to_name(last_active->last_error));
}

static void probe_task(void *unused)
{
    (void)unused;
    app_gps_status_t status = {0};
    app_gps_status_t last_active = {0};
    bool last_active_valid = false;
    int64_t last_active_at_us = 0;
    int64_t next_log;
#ifdef AETHER_GPS_DIAGNOSTIC_PASSIVE
    ESP_LOGI("gps_probe", "passive-only diagnostics; no GPS start/stop, no power commands; user manual tests enabled");
#else
    ESP_LOGI("gps_probe", "one-shot USB-input-only acceptance up to300s; OTG fallback is disabled during automatic acceptance");
    const esp_err_t started = app_gps_set_enabled(true);
    const int64_t began = esp_timer_get_time();
    next_log = began;
    int64_t first_fix_ms = -1, fix_began = 0;
    uint64_t last_fix_message = UINT64_MAX;
    uint32_t fix_updates = 0, pps_run = 0, pps_total = 0, previous_pps = 0;
    uint32_t nmea_total = 0, bad_total = 0, malformed_total = 0;
    bool fixing = false;
    const char *reason = started == ESP_OK ? "timeout" : "start-failed";
    if (started == ESP_OK) {
        for (;;) {
            const int64_t now = esp_timer_get_time();
            app_gps_get_status(&status);
            if (status.active && status.state != APP_GPS_STOPPING && status.state != APP_GPS_ERROR) {
                last_active = status;
                last_active_valid = true;
                last_active_at_us = now;
            }
            const gnss_nmea_snapshot_t *nav = &status.navigation;
            if (nav->accepted_sentences > nmea_total) nmea_total = nav->accepted_sentences;
            if (nav->bad_checksums > bad_total) bad_total = nav->bad_checksums;
            if (nav->malformed_sentences > malformed_total) malformed_total = nav->malformed_sentences;
            if (status.pps_count > pps_total) pps_total = status.pps_count;
            const bool valid_fix = status.active && status.external_power &&
                status.state == APP_GPS_FIXED && nav->position_valid;
            if (valid_fix) {
                if (!fixing) {
                    fixing = true;
                    fix_began = now;
                    fix_updates = 0;
                    last_fix_message = UINT64_MAX;
                }
                if (nav->last_fix_ms != last_fix_message) {
                    ++fix_updates;
                    last_fix_message = nav->last_fix_ms;
                }
                if (first_fix_ms < 0) {
                    first_fix_ms = (now - began) / 1000;
                    ESP_LOGI("gps_probe", "first valid fix after %lld.%03llds",
                        (long long)(first_fix_ms / 1000), (long long)(first_fix_ms % 1000));
                }
            } else {
                fixing = false;
                fix_updates = 0;
            }
            if (status.active && status.pps_seen && status.pps_age_ms < GPS_PROBE_PPS_STALE_MS) {
                if (status.pps_count >= previous_pps) pps_run += status.pps_count - previous_pps;
                else pps_run = 0;
            } else pps_run = 0;
            previous_pps = status.pps_count;
            const uint32_t fix_run_ms = fixing ? (uint32_t)((now - fix_began) / 1000) : 0;
            if (now >= next_log) {
                log_gps_status(&status, (uint32_t)((now - began) / 1000),
                    first_fix_ms, fix_run_ms, fix_updates, pps_run, pps_total);
                next_log = now + GPS_PROBE_LOG_INTERVAL_US;
            }
            if (status.state == APP_GPS_ERROR) { reason = "GPS-error"; break; }
            if (!status.requested_enabled || status.state == APP_GPS_STOPPING) {
                reason = "user-or-system-stop"; break;
            }
            if (status.active && !status.external_power) { reason = "non-USB-source-refused"; break; }
            if (fixing && now - fix_began >= GPS_PROBE_STABLE_FIX_US && pps_run >= 3 &&
                status.pps_seen && status.pps_age_ms < GPS_PROBE_PPS_STALE_MS) {
                reason = "stable-fix-and-PPS"; break;
            }
            if (now - began >= GPS_PROBE_TIMEOUT_US) break;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    app_gps_get_status(&status);
    const esp_err_t stopped = app_gps_stop_wait(3000);
    if (stopped == ESP_OK)
        atomic_store_explicit(&s_manual_tests_enabled, true, memory_order_release);
    ESP_LOGI("gps_probe", "acceptance done: reason=%s start=%s elapsed_ms=%lld first_fix_ms=%lld NMEA=%lu bad=%lu malformed=%lu sats=%u used=%u fix=%u PPS_total=%lu stop=%s; no RTC/NVS write",
        reason, esp_err_to_name(started), (long long)((esp_timer_get_time() - began) / 1000),
        (long long)first_fix_ms, (unsigned long)nmea_total,
        (unsigned long)bad_total, (unsigned long)malformed_total, status.navigation.satellite_count,
        status.navigation.satellites_used, status.navigation.position_valid,
        (unsigned long)pps_total, esp_err_to_name(stopped));
#endif
    ESP_LOGI("otg_watch", "passive cache monitor every5s; no automatic GPS/OTG restart, no I2C; user manual tests enabled=%u",
        app_gps_diagnostic_manual_tests_enabled());
    uint32_t verified_runs = 0;
    bool was_verified = false, last_verified_valid = false;
    app_otg_power_status_t last_verified = {0};
    int64_t last_verified_at_us = 0;
    next_log = esp_timer_get_time();
    for (;;) {
        const int64_t now = esp_timer_get_time();
        app_gps_get_status(&status);
        /* Cached getters only: neither call starts hardware or performs I2C. */
        aether_usb_host_status_t host = {0};
        app_otg_power_status_t power = {0};
        const esp_err_t host_read = app_otg_get_status(&host);
        const esp_err_t power_read = app_otg_get_power_status(&power);
        const bool verified = host_read == ESP_OK && power_read == ESP_OK &&
            host.active && host.enabled && host.vbus_enabled &&
            power.valid && power.output_verified && power.vbus_valid && !power.failure_snapshot;
        if (verified) {
            if (!was_verified && verified_runs < UINT32_MAX) ++verified_runs;
            last_verified = power;
            last_verified_valid = true;
            last_verified_at_us = now;
        }
        was_verified = verified;
        if (status.active && status.state != APP_GPS_STOPPING && status.state != APP_GPS_ERROR) {
            last_active = status;
            last_active_valid = true;
            last_active_at_us = now;
        }
        if (now >= next_log) {
            log_gps_cache(&status, &last_active, last_active_valid, last_active_at_us, now);
            log_otg_cache();
            ESP_LOGI("otg_watch", "historical last_verified_valid=%u verified_runs=%lu current_verified=%u age_ms=%lld VBUS=%umV(valid=%u) raw=%u E7_CC=0x%02X R0B_CC_DETECT=0x%02X E3_CC_MODE=0x%02X; counter=verified-rising-edges, cache-only",
                last_verified_valid, (unsigned long)verified_runs, verified,
                (long long)(last_verified_valid ? last_verified.age_ms + (now - last_verified_at_us) / 1000 : -1),
                last_verified.vbus_mv, last_verified.vbus_valid, last_verified.vbus_raw,
                last_verified.cc_raw, last_verified.cc_detection_raw, last_verified.cc_mode_raw);
            next_log = now + GPS_PROBE_LOG_INTERVAL_US;
        }
        /* Cache-only sampling catches short manual sessions; output stays5s. */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t app_gps_diagnostic_start(void)
{
#ifdef AETHER_GPS_DIAGNOSTIC_PASSIVE
    atomic_store_explicit(&s_manual_tests_enabled, true, memory_order_release);
#else
    atomic_store_explicit(&s_manual_tests_enabled, false, memory_order_release);
#endif
    return xTaskCreate(probe_task, "gps_probe", 6144, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

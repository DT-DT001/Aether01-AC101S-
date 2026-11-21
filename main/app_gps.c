#include "app_gps.h"
#ifdef AETHER_GPS_DIAGNOSTIC
#include "app_gps_diagnostic.h"
#endif
#include "app_otg_control.h"
#include "aether_board_pins.h"

#include <limits.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GPS_UART UART_NUM_1
#define GPS_POWER_TIMEOUT_MS 6500U
#define GPS_POLL_MS 50U

static const char *const TAG = "gps";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR portMUX_TYPE s_pps_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint32_t s_pps_count;
static DRAM_ATTR TickType_t s_pps_tick;
static app_gps_status_t s_status;
static app_power_t *s_power;
static TaskHandle_t s_task;
static gnss_nmea_parser_t *s_parser;
static bool s_requested, s_busy;
static uint32_t s_generation;

static uint64_t gps_now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void IRAM_ATTR pps_isr(void *unused)
{
    (void)unused;
    portENTER_CRITICAL_ISR(&s_pps_lock);
    ++s_pps_count;
    s_pps_tick = xTaskGetTickCountFromISR();
    portEXIT_CRITICAL_ISR(&s_pps_lock);
}

static bool power_allows_gps(bool starting)
{
    app_power_status_t power = {0};
    return s_power && app_power_get_status(s_power, &power) == ESP_OK &&
        (!starting || !power.sleeping) && !power.shutdown_recovering &&
        power.shutdown_state != APP_POWER_SHUTDOWN_PREPARING &&
        power.shutdown_state != APP_POWER_SHUTDOWN_REQUESTED;
}

static bool session_requested(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    const bool requested = s_requested && generation == s_generation;
    portEXIT_CRITICAL(&s_lock);
    return requested && power_allows_gps(false);
}

static void set_phase(app_gps_state_t phase, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = phase;
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_lock);
}

static bool output_ready(uint32_t lease, app_otg_power_status_t *power)
{
    memset(power, 0, sizeof(*power));
    aether_usb_host_status_t host = {0};
    return app_otg_manual_lease_valid(lease) &&
        app_otg_get_status(&host) == ESP_OK && host.enabled && host.vbus_enabled &&
        !host.cleanup_pending && app_otg_get_power_status(power) == ESP_OK &&
        power->manual_source_requested && !power->manual_source_failed &&
        power->valid && !power->failure_snapshot && power->vbus_valid &&
        power->output_verified && power->vbus_mv >= 4750 && power->vbus_mv <= 5500;
}

static esp_err_t read_supply(bool external, uint32_t lease, uint32_t input_lease,
                              app_otg_power_status_t *power, bool *ready)
{
    *ready = false;
    if (!external) {
        if (!app_otg_manual_lease_valid(lease)) {
            /* Preserve the source worker's real failure instead of hiding a
             * measured-output timeout behind a generic revoked-lease error. */
            memset(power, 0, sizeof(*power));
            if (app_otg_get_power_status(power) == ESP_OK &&
                power->manual_source_failed && power->last_error != ESP_OK)
                return power->last_error;
            return ESP_ERR_INVALID_STATE;
        }
        *ready = output_ready(lease, power);
        return ESP_OK;
    }
    memset(power, 0, sizeof(*power));
    app_otg_input_status_t input = {0};
    const esp_err_t error = app_otg_external_input_read(input_lease, &input);
    if (error != ESP_OK) return error;
    if (!input.valid || !input.present) return ESP_ERR_INVALID_STATE;
    if (input.voltage_valid && (input.voltage_mv < 4750 || input.voltage_mv > 5500))
        return ESP_ERR_INVALID_RESPONSE;
    /* A verified input is NOT an OTG output: never set output_verified or
     * manual_source_requested on this path. Only the common voltage fields. */
    power->valid = true;
    power->vbus_valid = input.voltage_valid;
    power->vbus_mv = input.voltage_mv;
    *ready = input.voltage_valid;
    return ESP_OK;
}

#ifdef AETHER_GPS_DIAGNOSTIC
/* A bounded raw trace distinguishes receiver no-fix reports from a parser
 * rejection. Only the temporary diagnostic build logs these local sentences. */
static void trace_nmea(const uint8_t *bytes, size_t length)
{
    static char line[192];
    static size_t used;
    static uint64_t last[3];
    static bool logged[3];
    for (size_t i = 0; i < length; ++i) {
        const char c = (char)bytes[i];
        if (c == '$') used = 0;
        if (c == '\n') {
            if (used && line[used - 1] == '\r') --used;
            line[used] = 0;
            int type = -1;
            if (used > 7 && line[0] == '$') {
                if (!memcmp(line + 3, "RMC", 3)) type = 0;
                else if (!memcmp(line + 3, "GGA", 3)) type = 1;
                else if (!memcmp(line + 3, "GSA", 3)) type = 2;
            }
            if (type >= 0) {
                const uint64_t now = gps_now_ms();
                if (!logged[type] || now - last[type] >= 5000) {
                    ESP_LOGI("gps_nmea", "%s", line);
                    logged[type] = true;
                    last[type] = now;
                }
            }
            used = 0;
        } else if (used < sizeof(line) - 1) line[used++] = c;
        else used = 0;
    }
}
#endif

static esp_err_t start_uart(bool *installed)
{
    /* User wiring is named at the GPS: GPIO39 <- GPS_TXD, GPIO40 -> GPS_RXD.
     * This application is receive-only. Leave GPIO40 high-Z, so an unpowered
     * module can never be back-fed by UART TX. No PCAS/reset/Flash commands. */
    const uart_config_t config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t error = uart_driver_install(GPS_UART, 4096, 0, 0, NULL, 0);
    if (error != ESP_OK) return error;
    *installed = true;
    error = uart_param_config(GPS_UART, &config);
    if (error == ESP_OK) error = uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE,
        AETHER_GPIO_GPS_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error == ESP_OK) error = uart_flush_input(GPS_UART);
    if (error == ESP_OK) error = gpio_set_pull_mode(AETHER_GPIO_GPS_UART_RX, GPIO_FLOATING);
    return error;
}

static void publish_navigation(uint32_t generation, uint64_t started, const app_otg_power_status_t *power)
{
    const uint64_t now = gps_now_ms();
    gnss_nmea_snapshot_t navigation;
    gnss_nmea_get_snapshot(s_parser, now, &navigation);
    portENTER_CRITICAL(&s_pps_lock);
    const uint32_t pulses = s_pps_count;
    const TickType_t pulse_tick = s_pps_tick;
    portEXIT_CRITICAL(&s_pps_lock);
    const uint64_t pulse_age = pulses ?
        (uint64_t)(TickType_t)(xTaskGetTickCount() - pulse_tick) * portTICK_PERIOD_MS : UINT32_MAX;
    portENTER_CRITICAL(&s_lock);
    if (!s_requested || generation != s_generation) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_status.navigation = navigation;
    s_status.active = true;
    s_status.state = navigation.position_valid ? APP_GPS_FIXED : APP_GPS_SEARCHING;
    s_status.uart_receiving = navigation.accepted_sentences && now >= navigation.last_sentence_ms &&
        now - navigation.last_sentence_ms <= 3000;
    s_status.session_ms = now - started < UINT32_MAX ? (uint32_t)(now - started) : UINT32_MAX;
    s_status.pps_count = pulses;
    s_status.pps_age_ms = pulse_age < UINT32_MAX ? (uint32_t)pulse_age : UINT32_MAX;
    s_status.pps_seen = pulses && pulse_age < 2500;
    s_status.supply_valid = power->vbus_valid;
    s_status.supply_mv = power->vbus_mv;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t cleanup_session(bool *uart_installed, uint32_t *lease, uint32_t *input_lease)
{
    /* ON_OFF low enters sleep. Open-drain control never drives a high level
     * into a powered-down module, even if the user forces OTG off first. */
    esp_err_t error = gpio_set_level(AETHER_GPIO_GPS_ON_OFF, 0);
    const esp_err_t irq_error = gpio_intr_disable(AETHER_GPIO_GPS_PPS);
    if (error == ESP_OK) error = irq_error;
    if (*uart_installed) {
        const esp_err_t uart_error = uart_driver_delete(GPS_UART);
        if (uart_error == ESP_OK) *uart_installed = false;
        if (error == ESP_OK) error = uart_error;
    }
    if (!*uart_installed) {
        esp_err_t pin_error = gpio_reset_pin(AETHER_GPIO_GPS_UART_RX);
        /* IDF gpio_reset_pin enables a pull-up by default. Explicitly remove
         * it so the receiver cannot back-feed an unpowered GPS TXD. */
        if (pin_error == ESP_OK)
            pin_error = gpio_set_pull_mode(AETHER_GPIO_GPS_UART_RX, GPIO_FLOATING);
        if (error == ESP_OK) error = pin_error;
    }
    if (error != ESP_OK) return error;
    if (*input_lease) {
        const esp_err_t release = app_otg_external_input_release(*input_lease);
        if (release != ESP_OK) return release;
        *input_lease = 0;
    }
    if (*lease) {
        const esp_err_t release = app_otg_manual_lease_release(*lease);
        if (release != ESP_OK && release != ESP_ERR_INVALID_STATE) return release;
        *lease = 0;
    }
    return ESP_OK;
}

static void gps_worker(void *unused)
{
    (void)unused;
    uint8_t bytes[256];
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const bool requested = s_requested;
        const uint32_t generation = s_generation;
        if (requested) {
            s_busy = true;
            s_status = (app_gps_status_t){.available = true, .requested_enabled = true,
                .state = APP_GPS_POWERING, .pps_age_ms = UINT32_MAX};
        }
        portEXIT_CRITICAL(&s_lock);
        if (!requested) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        bool uart_installed = false;
        uint32_t lease = 0, input_lease = 0;
        bool external = false;
        const uint64_t started = gps_now_ms();
        esp_err_t error;
        do {
            error = app_otg_external_input_acquire(&input_lease);
            if (error != ESP_ERR_NOT_FINISHED) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        } while (session_requested(generation) && gps_now_ms() - started < GPS_POWER_TIMEOUT_MS);
        if (error == ESP_OK) external = true;
        else if (error == ESP_ERR_NOT_FOUND && session_requested(generation)) {
#ifdef AETHER_GPS_DIAGNOSTIC
            /* Automatic boot acceptance is strictly input-only. Once it has
             * stopped cleanly, explicit UI starts may test the normal battery
             * path, still subject to every driver's input/voltage interlock. */
            if (!app_gps_diagnostic_manual_tests_enabled()) error = ESP_ERR_NOT_SUPPORTED;
            else
#endif
            {
            do {
                error = app_otg_manual_lease_acquire(&lease);
                if (error != ESP_ERR_NOT_FINISHED) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            } while (session_requested(generation) && gps_now_ms() - started < GPS_POWER_TIMEOUT_MS);
            }
        }
        portENTER_CRITICAL(&s_lock);
        s_status.external_power = external;
        portEXIT_CRITICAL(&s_lock);
        app_otg_power_status_t power = {0};
        bool ready = false;
        const uint64_t acquired_at = gps_now_ms();
        unsigned stable_samples = 0;
        if (error == ESP_OK) {
            while (session_requested(generation)) {
                bool measured = false;
                error = read_supply(external, lease, input_lease, &power, &measured);
                if (error != ESP_OK) break;
                if (external) {
                    /* Same conservative ADC settling policy as OTG: wait at
                     * least1s and two distinct20ms-spaced valid input samples. */
                    if (measured && gps_now_ms() - acquired_at >= 1000) ++stable_samples;
                    else stable_samples = 0;
                    if (stable_samples >= 2) { ready = true; break; }
                } else if (measured) { ready = true; break; }
                if (gps_now_ms() - started >= GPS_POWER_TIMEOUT_MS) { error = ESP_ERR_TIMEOUT; break; }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (ready && session_requested(generation)) error = start_uart(&uart_installed);
        if (error == ESP_OK && ready && session_requested(generation)) {
            bool measured = false;
            error = read_supply(external, lease, input_lease, &power, &measured);
            if (error == ESP_OK && !measured) error = ESP_ERR_INVALID_STATE;
        }
        if (error == ESP_OK && ready && session_requested(generation)) {
            gnss_nmea_reset(s_parser);
            portENTER_CRITICAL(&s_pps_lock);
            s_pps_count = 0;
            s_pps_tick = 0;
            portEXIT_CRITICAL(&s_pps_lock);
            error = gpio_intr_enable(AETHER_GPIO_GPS_PPS);
            if (error == ESP_OK) error = gpio_set_level(AETHER_GPIO_GPS_ON_OFF, 1);
            if (error == ESP_OK) ESP_LOGI(TAG, "GPS started: RX=%d TX=%d(high-Z), 9600 8N1, %s %umV",
                AETHER_GPIO_GPS_UART_RX, AETHER_GPIO_GPS_UART_TX,
                external ? "USB input" : "OTG output", power.vbus_mv);
        }
        bool was_fixed = false;
        while (error == ESP_OK && ready && session_requested(generation)) {
            bool measured = false;
            error = read_supply(external, lease, input_lease, &power, &measured);
            if (error != ESP_OK || !measured) { if (error == ESP_OK) error = ESP_ERR_INVALID_STATE; break; }
            const int received = uart_read_bytes(GPS_UART, bytes, sizeof(bytes), pdMS_TO_TICKS(GPS_POLL_MS));
            if (received < 0) { error = ESP_FAIL; break; }
            if (!session_requested(generation)) break;
            error = read_supply(external, lease, input_lease, &power, &measured);
            if (error != ESP_OK || !measured) { if (error == ESP_OK) error = ESP_ERR_INVALID_STATE; break; }
            if (received) {
#ifdef AETHER_GPS_DIAGNOSTIC
                trace_nmea(bytes, (size_t)received);
#endif
                gnss_nmea_feed(s_parser, (const char *)bytes, (size_t)received, gps_now_ms());
            }
            publish_navigation(generation, started, &power);
            portENTER_CRITICAL(&s_lock);
            const bool fixed = s_status.navigation.position_valid;
            portEXIT_CRITICAL(&s_lock);
            if (fixed != was_fixed) ESP_LOGI(TAG, "GPS %s", fixed ? "position acquired" : "position lost");
            was_fixed = fixed;
        }
        const bool failure = error != ESP_OK && session_requested(generation);
        set_phase(APP_GPS_STOPPING, failure ? error : ESP_OK);
        for (;;) {
            const esp_err_t cleanup = cleanup_session(&uart_installed, &lease, &input_lease);
            if (cleanup == ESP_OK) break;
            set_phase(APP_GPS_STOPPING, cleanup);
            vTaskDelay(pdMS_TO_TICKS(GPS_POLL_MS));
        }
        gnss_nmea_reset(s_parser);
        portENTER_CRITICAL(&s_lock);
        s_requested = false;
        s_busy = false;
        s_status = (app_gps_status_t){.available = true, .state = failure ? APP_GPS_ERROR : APP_GPS_OFF,
            .last_error = failure ? error : ESP_OK, .pps_age_ms = UINT32_MAX};
        portEXIT_CRITICAL(&s_lock);
        if (failure) ESP_LOGW(TAG, "GPS stopped safely: %s", esp_err_to_name(error));
    }
}

esp_err_t app_gps_init(app_power_t *power)
{
    if (!power || s_task) return ESP_ERR_INVALID_ARG;
    s_power = power;
    /* Module TXD is connected to MCU RX39, not MCU TX39. All inputs and the
     * reserved TX40 have no internal pulls. ON_OFF uses the module's pull-up. */
    const gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << AETHER_GPIO_GPS_PPS) | (1ULL << AETHER_GPIO_GPS_UART_RX) |
                        (1ULL << AETHER_GPIO_GPS_UART_TX),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&inputs);
    if (error == ESP_OK) error = gpio_set_level(AETHER_GPIO_GPS_ON_OFF, 0);
    const gpio_config_t onoff = {
        .pin_bit_mask = 1ULL << AETHER_GPIO_GPS_ON_OFF, .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (error == ESP_OK) error = gpio_config(&onoff);
    if (error == ESP_OK) error = gpio_set_intr_type(AETHER_GPIO_GPS_PPS, GPIO_INTR_POSEDGE);
    bool handler_installed = false;
    if (error == ESP_OK) {
        error = gpio_isr_handler_add(AETHER_GPIO_GPS_PPS, pps_isr, NULL);
        if (error == ESP_ERR_INVALID_STATE) {
            error = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
            if (error == ESP_ERR_INVALID_STATE) error = ESP_OK;
            if (error == ESP_OK) error = gpio_isr_handler_add(AETHER_GPIO_GPS_PPS, pps_isr, NULL);
        }
        handler_installed = error == ESP_OK;
    }
    if (error == ESP_OK) error = gpio_intr_disable(AETHER_GPIO_GPS_PPS);
    if (error == ESP_OK) {
        s_parser = gnss_nmea_new();
        if (!s_parser) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) {
        s_status = (app_gps_status_t){.available = true, .state = APP_GPS_OFF, .pps_age_ms = UINT32_MAX};
        if (xTaskCreate(gps_worker, "gps", 6144, NULL, 4, &s_task) != pdPASS) {
            s_task = NULL;
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error != ESP_OK) {
        if (handler_installed) (void)gpio_isr_handler_remove(AETHER_GPIO_GPS_PPS);
        gnss_nmea_free(s_parser);
        s_parser = NULL;
        s_status = (app_gps_status_t){.state = APP_GPS_ERROR, .last_error = error};
    } else ESP_LOGI(TAG, "GPS ready (off): ON_OFF=%d PPS=%d RX=%d TX=%d(high-Z)",
        AETHER_GPIO_GPS_ON_OFF, AETHER_GPIO_GPS_PPS, AETHER_GPIO_GPS_UART_RX, AETHER_GPIO_GPS_UART_TX);
    return error;
}

esp_err_t app_gps_set_enabled(bool enabled)
{
    if (!s_task) return enabled ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (enabled && !power_allows_gps(true)) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (enabled && s_busy && !s_requested) error = ESP_ERR_INVALID_STATE;
    else if (enabled != s_requested) {
        s_requested = enabled;
        ++s_generation;
        s_status.requested_enabled = enabled;
        if (!enabled) {
            if (s_busy) s_status.state = APP_GPS_STOPPING;
            s_status.navigation.position_valid = false;
            s_status.navigation.time_valid = false;
            s_status.navigation.date_valid = false;
            s_status.navigation.altitude_valid = false;
            s_status.navigation.speed_valid = false;
            s_status.navigation.course_valid = false;
            s_status.navigation.hdop_valid = false;
            s_status.navigation.satellite_count = 0;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (error == ESP_OK) xTaskNotifyGive(s_task);
    return error;
}

void app_gps_get_status(app_gps_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void app_gps_cancel(void)
{
    (void)app_gps_set_enabled(false);
}

esp_err_t app_gps_stop_wait(uint32_t timeout_ms)
{
    app_gps_cancel();
    const uint64_t start = gps_now_ms();
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const bool busy = s_busy || s_requested;
        portEXIT_CRITICAL(&s_lock);
        if (!busy) return ESP_OK;
        if (gps_now_ms() - start >= timeout_ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

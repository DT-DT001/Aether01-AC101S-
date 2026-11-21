#include "app_telemetry.h"
#include "app_display_metrics.h"
#include "app_display_sync.h"

#include <math.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The QMI8658A generates a new six-axis frame about every 4.46 ms. */
#define MEASUREMENT_IMU_PERIOD_MS 5U
#define MEASUREMENT_IMU_RETRY_MS 2U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_telemetry_snapshot_t s_snapshot = {
    .imu_error = ESP_ERR_NOT_FINISHED,
    .barometer_error = ESP_ERR_NOT_FINISHED,
    .rtc_error = ESP_ERR_NOT_FINISHED,
};
static uint32_t s_interest;
static uint32_t s_interest_generation;
static bool s_started;
static bool s_warm_imu;
static bool s_quiesce_requested;
static bool s_quiesced;
static void (*s_media_probe)(void *);
static void *s_media_probe_context;
static void (*s_consumer)(const app_telemetry_snapshot_t *);
static bool (*s_measurement_requested)(void);
static void (*s_measurement_consumer)(const app_telemetry_snapshot_t *);

void app_telemetry_warm_imu(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_started) s_warm_imu = true;
    portEXIT_CRITICAL(&s_lock);
}

void app_telemetry_set_consumer(void (*consumer)(const app_telemetry_snapshot_t *))
{
    portENTER_CRITICAL(&s_lock);
    s_consumer = consumer;
    portEXIT_CRITICAL(&s_lock);
}

void app_telemetry_set_imu_measurement(bool (*requested)(void),
                                      void (*consume)(const app_telemetry_snapshot_t *))
{
    portENTER_CRITICAL(&s_lock);
    s_measurement_requested = requested;
    s_measurement_consumer = consume;
    portEXIT_CRITICAL(&s_lock);
}

void app_telemetry_set_media_probe(void (*probe)(void *), void *context)
{
    portENTER_CRITICAL(&s_lock);
    s_media_probe = probe;
    s_media_probe_context = context;
    portEXIT_CRITICAL(&s_lock);
}

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void app_telemetry_get(app_telemetry_snapshot_t *snapshot)
{
    if (snapshot) {
        portENTER_CRITICAL(&s_lock);
        *snapshot = s_snapshot;
        const uint32_t interest = s_interest;
        const bool paused = s_quiesce_requested;
        portEXIT_CRITICAL(&s_lock);
        const uint32_t now = uptime_ms();
        if (paused) {
            snapshot->imu_error = snapshot->barometer_error = snapshot->rtc_error = ESP_ERR_NOT_FINISHED;
            return;
        }
        /* A cached sample must never look live after leaving/reopening a page
         * or when the worker is delayed by a peripheral error. */
        if (!(interest & APP_TELEMETRY_IMU)) snapshot->imu_error = ESP_ERR_NOT_FINISHED;
        else if (snapshot->imu_error == ESP_OK && now - snapshot->imu_updated_ms > 2000) {
            snapshot->imu_error = ESP_ERR_TIMEOUT;
        }
        if (!(interest & APP_TELEMETRY_BAROMETER)) snapshot->barometer_error = ESP_ERR_NOT_FINISHED;
        else if (snapshot->barometer_error == ESP_OK && now - snapshot->barometer_updated_ms > 2500) {
            snapshot->barometer_error = ESP_ERR_TIMEOUT;
        }
        if (snapshot->rtc_error == ESP_OK && now - snapshot->rtc_updated_ms > 3000) {
            snapshot->rtc_error = ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t app_telemetry_quiesce(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    portENTER_CRITICAL(&s_lock);
    s_quiesce_requested = true;
    ++s_interest_generation;
    if (!s_started) s_quiesced = true;
    portEXIT_CRITICAL(&s_lock);
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const bool done = s_quiesced;
        portEXIT_CRITICAL(&s_lock);
        if (done) return ESP_OK;
        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_telemetry_resume(void)
{
    portENTER_CRITICAL(&s_lock);
    s_quiesce_requested = false;
    s_quiesced = false;
    ++s_interest_generation;
    s_snapshot.imu_error = s_snapshot.barometer_error = s_snapshot.rtc_error = ESP_ERR_NOT_FINISHED;
    portEXIT_CRITICAL(&s_lock);
}

void app_telemetry_set_interest(uint32_t mask)
{
    mask &= APP_TELEMETRY_IMU | APP_TELEMETRY_BAROMETER;
    portENTER_CRITICAL(&s_lock);
    if (s_interest != mask) {
        const uint32_t activated = mask & ~s_interest;
        if (activated & APP_TELEMETRY_IMU) s_snapshot.imu_error = ESP_ERR_NOT_FINISHED;
        if (activated & APP_TELEMETRY_BAROMETER) s_snapshot.barometer_error = ESP_ERR_NOT_FINISHED;
        s_interest = mask;
        ++s_interest_generation;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void telemetry_task(void *argument)
{
    aether_board_t *board = argument;
    uint32_t due_imu = 0, due_baro = 0, due_rtc = 0;
    uint32_t previous_generation = 0;
    uint32_t health_due = uptime_ms() + 60000;
    uint32_t media_due = uptime_ms() + 5000;
    const uint32_t warm_until = uptime_ms() + 12000;
    bool warm_imu = s_warm_imu && board->imu_ready;
    bool previous_measurement = false;
    unsigned imu_reads = 0, still_reads = 0;
    float raw_squared = 0, corrected_squared = 0;
    for (;;) {
        const uint32_t now = uptime_ms();
        portENTER_CRITICAL(&s_lock);
        /* This acknowledgement is published only between complete read batches.
         * Once observed by quiesce(), no sensor or RTC transaction remains live. */
        const bool paused = s_quiesce_requested;
        s_quiesced = paused;
        const uint32_t interest = s_interest;
        const uint32_t generation = s_interest_generation;
        void (*consumer)(const app_telemetry_snapshot_t *) = s_consumer;
        bool (*measurement_requested)(void) = s_measurement_requested;
        void (*measurement_consumer)(const app_telemetry_snapshot_t *) = s_measurement_consumer;
        app_telemetry_snapshot_t next = s_snapshot;
        portEXIT_CRITICAL(&s_lock);
        if ((int32_t)(now - media_due) >= 0) {
            portENTER_CRITICAL(&s_lock);
            void (*probe)(void *) = s_media_probe;
            void *context = s_media_probe_context;
            portEXIT_CRITICAL(&s_lock);
            if (!paused && probe) probe(context);
            media_due = now + 5000;
        }
        if (paused) {
            if (consumer) consumer(NULL);
            if (measurement_consumer) measurement_consumer(NULL);
            previous_measurement = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        app_display_sync_poll_diagnostics(board->display_io);
        const bool measurement = measurement_consumer && measurement_requested && measurement_requested();
        if (measurement != previous_measurement) due_imu = now;
        previous_measurement = measurement;
        const uint32_t activated = generation != previous_generation ? interest : 0;
        if (activated & APP_TELEMETRY_IMU) due_imu = now;
        if (activated & APP_TELEMETRY_BAROMETER) due_baro = now;
        previous_generation = generation;

        if (warm_imu && (int32_t)(now - warm_until) >= 0) {
            warm_imu = false;
            if (still_reads) {
                ESP_LOGI("imu", "stationary samples=%u/%u raw_rms_mdps=%d corrected_rms_mdps=%d",
                         still_reads, imu_reads, (int)(1000 * sqrtf(raw_squared / still_reads)),
                         (int)(1000 * sqrtf(corrected_squared / still_reads)));
            } else ESP_LOGI("imu", "startup calibration: no qualified stationary samples (%u reads)", imu_reads);
        }
        const bool sample_imu = measurement || warm_imu || (interest & APP_TELEMETRY_IMU);
        const bool read_imu = sample_imu && (int32_t)(now - due_imu) >= 0;
        bool waiting_for_frame = false;
        if (read_imu) {
            const esp_err_t imu_error = !board->imu_ready ? ESP_ERR_NOT_FOUND :
                (measurement ? qmi8658a_read_fresh(board->imu, &next.imu) :
                               qmi8658a_read(board->imu, &next.imu));
            waiting_for_frame = measurement && imu_error == ESP_ERR_NOT_FINISHED;
            if (!waiting_for_frame) next.imu_error = imu_error;
            if (imu_error == ESP_OK) {
                next.imu_updated_ms = uptime_ms();
                if (warm_imu) {
                    ++imu_reads;
                    if (next.imu.gyro_bias_valid && next.imu.gyro_stationary) {
                        ++still_reads;
                        for (unsigned axis = 0; axis < 3; ++axis) {
                            const float raw = next.imu.gyro_raw_dps[axis];
                            const float corrected = next.imu.gyro_dps[axis];
                            raw_squared += raw * raw;
                            corrected_squared += corrected * corrected;
                        }
                    }
                }
            }
            /* Anchor the fast cadence to the start of the transaction. A
             * late read produces one sample, with its actual timestamp. */
            if (waiting_for_frame) due_imu = uptime_ms() + MEASUREMENT_IMU_RETRY_MS;
            else if (imu_error == ESP_OK && measurement) due_imu = now + MEASUREMENT_IMU_PERIOD_MS;
            else due_imu = uptime_ms() + (imu_error == ESP_OK ? 20U : 2000U);
        }
        if ((interest & APP_TELEMETRY_BAROMETER) && (int32_t)(now - due_baro) >= 0) {
            next.barometer_error = board->barometer_ready
                ? dps368_read(board->barometer, &next.barometer) : ESP_ERR_NOT_FOUND;
            if (next.barometer_error == ESP_OK) next.barometer_updated_ms = uptime_ms();
            due_baro = uptime_ms() + (next.barometer_error == ESP_OK ? 700 : 2000);
        }
        if ((int32_t)(now - due_rtc) >= 0) {
            next.rtc_error = board->rtc_ready
                ? pcf85063_get_time(board->rtc, &next.rtc) : ESP_ERR_NOT_FOUND;
            if (next.rtc_error == ESP_OK) next.rtc_updated_ms = uptime_ms();
            due_rtc = uptime_ms() + (next.rtc_error == ESP_OK ? 1000 : 2000);
        }
        portENTER_CRITICAL(&s_lock);
        if (s_interest_generation == generation) {
            s_snapshot = next;
        } else {
            /* RTC sampling is independent of page subscriptions. Repeated
             * navigation must not discard every successful clock update. */
            s_snapshot.rtc = next.rtc;
            s_snapshot.rtc_error = next.rtc_error;
            s_snapshot.rtc_updated_ms = next.rtc_updated_ms;
        }
        portEXIT_CRITICAL(&s_lock);
        if (consumer) consumer(&next);
        if (measurement && read_imu) {
            if (waiting_for_frame) next.imu_error = ESP_ERR_NOT_FINISHED;
            measurement_consumer(&next);
        }
        if ((int32_t)(now - health_due) >= 0) {
            const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            ESP_LOGI("health", "heap internal=%u min=%u largest=%u psram=%u telemetry_stack_min=%u dma=%u dma_min=%u dma_largest=%u",
                     (unsigned)heap_caps_get_free_size(internal_caps),
                     (unsigned)heap_caps_get_minimum_free_size(internal_caps),
                     (unsigned)heap_caps_get_largest_free_block(internal_caps),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            health_due = now + 60000;
            app_display_metrics_t display;
            app_display_metrics_get(&display);
            if (display.flush_wait_available) {
                ESP_LOGI("health", "display DMA submitted=%llu completed=%llu waiting=%u stalled=%u timeouts=%lu max_wait_us=%lu",
                    (unsigned long long)display.submitted_sequence,
                    (unsigned long long)display.completed_sequence,
                    (unsigned)display.flush_waiting, (unsigned)display.flush_stalled,
                    (unsigned long)display.flush_wait_timeouts, (unsigned long)display.max_flush_wait_us);
            }
            if (display.staging_available) {
                ESP_LOGI("health", "display staging capacity=%u copied=%llu dropped=%lu error=%d",
                    (unsigned)display.staging_capacity,
                    (unsigned long long)display.staging_copied_bytes,
                    (unsigned long)display.dropped_regions, display.staging_error);
            }
        }
        unsigned sleep_ms = sample_imu ? 10U : 50U;
        if (measurement) {
            const int32_t until_imu = (int32_t)(due_imu - uptime_ms());
            if (until_imu <= 0) sleep_ms = 1U;
            else if (until_imu < (int32_t)MEASUREMENT_IMU_PERIOD_MS) sleep_ms = (unsigned)until_imu;
            else sleep_ms = MEASUREMENT_IMU_PERIOD_MS;
        }
        const TickType_t delay = pdMS_TO_TICKS(sleep_ms);
        vTaskDelay(delay ? delay : 1);
    }
}

esp_err_t app_telemetry_start(aether_board_t *board)
{
    if (!board) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_started) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_started = true;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(telemetry_task, "telemetry", 4096, board, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_started = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

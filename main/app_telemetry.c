#include "app_telemetry.h"
#include "app_service_task.h"
#include "app_display_metrics.h"
#include "app_display_sync.h"
#include "app_gyro_calibration.h"

#include <math.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

/* The QMI8658A generates a new six-axis frame about every 4.46 ms. */
#define MEASUREMENT_IMU_PERIOD_MS 5U
#define MEASUREMENT_IMU_RETRY_MS 2U
#define MEASUREMENT_RTC_TIMEOUT_MS 8
enum {
    GYRO_COUNTDOWN_MS = 2000U,
    GYRO_SAMPLE_MS = 5000U,
    GYRO_SAMPLE_GAP_MS = 100U,
    GYRO_MIN_SAMPLES = 100U,
    GYRO_CAL_VERSION = 1U,
};

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
static bool s_screen_suspended;
static bool s_imu_available;
static app_gyro_calibration_status_t s_gyro_calibration;
static uint32_t s_gyro_generation, s_gyro_deadline_ms;
static void (*s_media_probe)(void *);
static void *s_media_probe_context;
static bool (*s_gyro_persistence_busy)(void *);
static void *s_gyro_persistence_context;
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

void app_telemetry_set_gyro_persistence_guard(bool (*busy)(void *), void *context)
{
    portENTER_CRITICAL(&s_lock);
    s_gyro_persistence_busy = busy;
    s_gyro_persistence_context = context;
    portEXIT_CRITICAL(&s_lock);
}

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool gyro_calibration_cancellable(app_gyro_calibration_phase_t phase)
{
    return phase == APP_GYRO_CAL_COUNTDOWN || phase == APP_GYRO_CAL_SAMPLING;
}

static bool gyro_calibration_active(app_gyro_calibration_phase_t phase)
{
    return gyro_calibration_cancellable(phase) || phase == APP_GYRO_CAL_SAVING;
}

static void finish_gyro_calibration_locked(app_gyro_calibration_phase_t phase, esp_err_t error)
{
    s_gyro_calibration.phase = phase;
    s_gyro_calibration.error = error;
    s_gyro_deadline_ms = 0;
}

esp_err_t app_telemetry_gyro_calibration_begin(void)
{
    const uint32_t deadline = uptime_ms() + GYRO_COUNTDOWN_MS;
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = ESP_OK;
    if (!s_started || s_quiesce_requested || gyro_calibration_active(s_gyro_calibration.phase))
        error = ESP_ERR_INVALID_STATE;
    else if (!s_imu_available)
        error = ESP_ERR_NOT_SUPPORTED;
    else {
        s_gyro_calibration = (app_gyro_calibration_status_t){.phase = APP_GYRO_CAL_COUNTDOWN};
        s_gyro_deadline_ms = deadline;
        ++s_gyro_generation;
    }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

esp_err_t app_telemetry_gyro_calibration_cancel(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool cancel = gyro_calibration_cancellable(s_gyro_calibration.phase);
    if (cancel) finish_gyro_calibration_locked(APP_GYRO_CAL_CANCELLED, ESP_OK);
    portEXIT_CRITICAL(&s_lock);
    return cancel ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void app_telemetry_gyro_calibration_get(app_gyro_calibration_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_gyro_calibration;
    const uint32_t deadline = s_gyro_deadline_ms;
    portEXIT_CRITICAL(&s_lock);
    const int32_t remaining = (int32_t)(deadline - uptime_ms());
    if (gyro_calibration_cancellable(status->phase) && remaining > 0)
        status->remaining_ms = (uint32_t)remaining;
}

static float median_pressure(const float samples[3])
{
    const float low = fminf(samples[0], samples[1]);
    const float high = fmaxf(samples[0], samples[1]);
    return fmaxf(low, fminf(high, samples[2]));
}

void app_telemetry_get(app_telemetry_snapshot_t *snapshot)
{
    if (snapshot) {
        portENTER_CRITICAL(&s_lock);
        *snapshot = s_snapshot;
        snapshot->gyro_calibrating = gyro_calibration_active(s_gyro_calibration.phase);
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
    if (gyro_calibration_cancellable(s_gyro_calibration.phase))
        finish_gyro_calibration_locked(APP_GYRO_CAL_CANCELLED, ESP_OK);
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

void app_telemetry_set_screen_suspended(bool suspended)
{
    portENTER_CRITICAL(&s_lock);
    if (s_screen_suspended != suspended) {
        s_screen_suspended = suspended;
        ++s_interest_generation;
        s_snapshot.imu_error = ESP_ERR_NOT_FINISHED;
        s_snapshot.barometer_error = ESP_ERR_NOT_FINISHED;
    }
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

typedef struct {
    uint32_t version;
    float bias_dps[3];
} gyro_calibration_record_t;

typedef struct {
    qmi8658a_gyro_zero_t window;
    uint32_t generation, sampling_since_ms, first_sample_ms, last_sample_ms;
    bool auto_paused, sampling;
} gyro_calibration_job_t;

static const char gyro_namespace[] = "gyro_cal";
static const char gyro_key[] = "bias_v1";

static void restore_gyro_bias(qmi8658a_handle_t imu)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(gyro_namespace, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        gyro_calibration_record_t record;
        size_t length = sizeof(record);
        error = nvs_get_blob(nvs, gyro_key, &record, &length);
        nvs_close(nvs);
        if (error == ESP_OK) {
            if (length != sizeof(record) || record.version != GYRO_CAL_VERSION)
                error = ESP_ERR_INVALID_RESPONSE;
            else
                error = qmi8658a_set_gyro_bias(imu, record.bias_dps);
        }
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW("imu", "Stored gyro calibration unavailable: %s", esp_err_to_name(error));
}

static esp_err_t save_gyro_bias(const float bias_dps[3])
{
    portENTER_CRITICAL(&s_lock);
    bool (*busy)(void *) = s_gyro_persistence_busy;
    void *context = s_gyro_persistence_context;
    portEXIT_CRITICAL(&s_lock);
    if (busy && busy(context)) return ESP_ERR_INVALID_STATE;

    const gyro_calibration_record_t record = {
        .version = GYRO_CAL_VERSION,
        .bias_dps = {bias_dps[0], bias_dps[1], bias_dps[2]},
    };
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(gyro_namespace, NVS_READWRITE, &nvs);
    if (error == ESP_OK) {
        error = nvs_set_blob(nvs, gyro_key, &record, sizeof(record));
        if (error == ESP_OK) error = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return error;
}

static bool prepare_gyro_calibration(qmi8658a_handle_t imu, gyro_calibration_job_t *job,
                                     uint32_t now, bool measurement, bool *sampling_started)
{
    portENTER_CRITICAL(&s_lock);
    if (measurement && gyro_calibration_cancellable(s_gyro_calibration.phase))
        finish_gyro_calibration_locked(APP_GYRO_CAL_FAILED, ESP_ERR_INVALID_STATE);
    if (s_gyro_calibration.phase == APP_GYRO_CAL_COUNTDOWN &&
        (int32_t)(now - s_gyro_deadline_ms) >= 0) {
        s_gyro_calibration.phase = APP_GYRO_CAL_SAMPLING;
        s_gyro_deadline_ms = now + GYRO_SAMPLE_MS;
    }
    const app_gyro_calibration_phase_t phase = s_gyro_calibration.phase;
    const uint32_t generation = s_gyro_generation;
    portEXIT_CRITICAL(&s_lock);

    if (job->generation != generation) {
        *job = (gyro_calibration_job_t){.generation = generation, .auto_paused = job->auto_paused};
    }
    const bool active = gyro_calibration_active(phase);
    if (job->auto_paused != active) {
        qmi8658a_set_gyro_auto_bias(imu, !active);
        job->auto_paused = active;
    }
    *sampling_started = phase == APP_GYRO_CAL_SAMPLING && !job->sampling;
    if (*sampling_started) job->sampling_since_ms = now;
    job->sampling = phase == APP_GYRO_CAL_SAMPLING;
    return job->sampling;
}

static bool collect_gyro_sample(qmi8658a_handle_t imu, gyro_calibration_job_t *job,
                                esp_err_t error, const qmi8658a_sample_t *sample, uint32_t now)
{
    const bool first = job->window.samples == 0;
    const uint32_t previous = first ? job->sampling_since_ms : job->last_sample_ms;
    if (now - previous > GYRO_SAMPLE_GAP_MS) error = ESP_ERR_TIMEOUT;
    if (error == ESP_ERR_NOT_FINISHED) return false;
    if (error == ESP_OK) {
        if (!qmi8658a_gyro_zero_add(&job->window, sample)) error = ESP_ERR_INVALID_RESPONSE;
        else {
            if (first) job->first_sample_ms = now;
            job->last_sample_ms = now;
        }
    }
    const bool finished = error == ESP_OK && now - job->first_sample_ms >= GYRO_SAMPLE_MS;
    if (finished && job->window.samples < GYRO_MIN_SAMPLES) error = ESP_ERR_TIMEOUT;

    portENTER_CRITICAL(&s_lock);
    const bool current = !s_quiesce_requested && s_gyro_generation == job->generation &&
                         s_gyro_calibration.phase == APP_GYRO_CAL_SAMPLING;
    if (current) {
        s_gyro_calibration.samples = job->window.samples;
        if (error != ESP_OK) finish_gyro_calibration_locked(APP_GYRO_CAL_FAILED, error);
        else if (finished) finish_gyro_calibration_locked(APP_GYRO_CAL_SAVING, ESP_OK);
        else if (first) s_gyro_deadline_ms = now + GYRO_SAMPLE_MS;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!current || !finished || error != ESP_OK) return false;

    /* Saving reserves the commit. A failed commit never changes live bias;
     * shutdown waits for this worker to acknowledge quiescence afterwards. */
    error = save_gyro_bias(job->window.bias_dps);
    if (error == ESP_OK) error = qmi8658a_set_gyro_bias(imu, job->window.bias_dps);
    portENTER_CRITICAL(&s_lock);
    finish_gyro_calibration_locked(error == ESP_OK ? APP_GYRO_CAL_COMPLETE : APP_GYRO_CAL_FAILED,
                                   error);
    if (error == ESP_OK) s_snapshot.imu_error = ESP_ERR_NOT_FINISHED;
    portEXIT_CRITICAL(&s_lock);
    return error == ESP_OK;
}

static void update_sensor_sleep(aether_board_t *board, bool suspended,
                                uint32_t *sleeping, uint32_t *uncertain)
{
    const uint32_t target = suspended ? APP_TELEMETRY_IMU | APP_TELEMETRY_BAROMETER : 0;
    const uint32_t changed = (*sleeping ^ target) | *uncertain;
    uint32_t verified = 0;
    if (board->imu_ready && (changed & APP_TELEMETRY_IMU) &&
        qmi8658a_set_sleep(board->imu, suspended) == ESP_OK)
        verified |= APP_TELEMETRY_IMU;
    if (board->barometer_ready && (changed & APP_TELEMETRY_BAROMETER) &&
        dps368_set_sleep(board->barometer, suspended) == ESP_OK)
        verified |= APP_TELEMETRY_BAROMETER;
    *sleeping = (*sleeping & ~verified) | (target & verified);
    /* A failed readback can follow a successful write. Retry on the next
     * transition even if the recorded mode already matches its destination. */
    *uncertain = changed & ~verified;
    if (verified) ESP_LOGI("telemetry", "screen sensor power mode=%s verified=0x%x",
                          suspended ? "standby" : "running", (unsigned)verified);
}

static void telemetry_task(void *argument)
{
    aether_board_t *board = argument;
    if (board->imu_ready && board->imu) restore_gyro_bias(board->imu);
    gyro_calibration_job_t gyro_job = {0};
    uint32_t due_imu = 0, due_baro = 0, due_rtc = 0;
    uint32_t previous_generation = 0;
    uint32_t health_due = uptime_ms() + 60000;
    uint32_t media_due = uptime_ms() + 5000;
    const uint32_t warm_until = uptime_ms() + 12000;
    bool warm_imu = s_warm_imu && board->imu_ready;
    bool previous_measurement = false;
    bool previous_suspend = false;
    uint32_t sleeping_sensors = 0, uncertain_sensors = 0, sensor_retry_due = 0;
    float pressures[3] = {0};
    unsigned pressure_count = 0, pressure_index = 0;
    unsigned imu_reads = 0, still_reads = 0;
    float raw_squared = 0, corrected_squared = 0;
    for (;;) {
        const uint32_t now = uptime_ms();
        portENTER_CRITICAL(&s_lock);
        /* This acknowledgement is published only between complete read batches.
         * Once observed by quiesce(), no sensor or RTC transaction remains live. */
        const bool paused = s_quiesce_requested;
        const bool suspended = s_screen_suspended;
        s_quiesced = paused;
        const uint32_t interest = s_interest;
        const uint32_t generation = s_interest_generation;
        void (*consumer)(const app_telemetry_snapshot_t *) = s_consumer;
        bool (*measurement_requested)(void) = s_measurement_requested;
        void (*measurement_consumer)(const app_telemetry_snapshot_t *) = s_measurement_consumer;
        app_telemetry_snapshot_t next = s_snapshot;
        portEXIT_CRITICAL(&s_lock);
        if (paused) {
            /* The shutdown coordinator now owns and verifies the sensors. */
            sleeping_sensors = 0;
            uncertain_sensors = 0;
            sensor_retry_due = 0;
            pressure_count = pressure_index = 0;
            if (consumer) consumer(NULL);
            if (measurement_consumer) measurement_consumer(NULL);
            previous_measurement = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const bool suspension_changed = suspended != previous_suspend;
        if (suspension_changed || (int32_t)(now - sensor_retry_due) >= 0) {
            update_sensor_sleep(board, suspended, &sleeping_sensors, &uncertain_sensors);
            sensor_retry_due = now + 250;
        }
        if (suspension_changed) {
            if (suspended && measurement_consumer) measurement_consumer(NULL);
            pressure_count = pressure_index = 0;
            due_imu = due_baro = now;
            previous_suspend = suspended;
        }
        const uint32_t parked_sensors = sleeping_sensors | uncertain_sensors;
        const bool can_sample_imu = !suspended && !(parked_sensors & APP_TELEMETRY_IMU);
        const bool measurement = can_sample_imu && measurement_consumer &&
            measurement_requested && measurement_requested();
        /* Console writes and auxiliary application work can block longer
         * than a valid motion sample interval. Run them between sessions. */
        if (!measurement && !suspended) {
            if ((int32_t)(now - media_due) >= 0) {
                portENTER_CRITICAL(&s_lock);
                void (*probe)(void *) = s_media_probe;
                void *context = s_media_probe_context;
                portEXIT_CRITICAL(&s_lock);
                if (probe) probe(context);
                media_due = now + 5000;
            }
            app_display_sync_poll_diagnostics(board->display_io);
        }
        bool sampling_started = false;
        const bool gyro_sampling = can_sample_imu && prepare_gyro_calibration(
            board->imu, &gyro_job, now, measurement, &sampling_started);
        if (sampling_started) due_imu = now;
        if (measurement != previous_measurement) due_imu = now;
        previous_measurement = measurement;
        const uint32_t activated = generation != previous_generation ? interest : 0;
        if (activated & APP_TELEMETRY_IMU) due_imu = now;
        if (activated & APP_TELEMETRY_BAROMETER) {
            due_baro = now;
            pressure_count = pressure_index = 0;
        }
        previous_generation = generation;

        if (warm_imu && !measurement && (int32_t)(now - warm_until) >= 0) {
            warm_imu = false;
            if (still_reads) {
                ESP_LOGI("imu", "stationary samples=%u/%u raw_rms_mdps=%d corrected_rms_mdps=%d",
                         still_reads, imu_reads, (int)(1000 * sqrtf(raw_squared / still_reads)),
                         (int)(1000 * sqrtf(corrected_squared / still_reads)));
            } else ESP_LOGI("imu", "startup calibration: no qualified stationary samples (%u reads)", imu_reads);
        }
        const bool sample_imu = can_sample_imu &&
            (measurement || warm_imu || gyro_sampling || (interest & APP_TELEMETRY_IMU));
        const bool read_imu = sample_imu && (int32_t)(now - due_imu) >= 0;
        esp_err_t imu_error = ESP_ERR_NOT_FINISHED;
        if (read_imu) {
            imu_error = board->imu_ready
                ? qmi8658a_read_fresh(board->imu, &next.imu) : ESP_ERR_NOT_FOUND;
            const bool waiting_for_frame = imu_error == ESP_ERR_NOT_FINISHED;
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
            if (waiting_for_frame)
                due_imu = uptime_ms() + (measurement ? MEASUREMENT_IMU_RETRY_MS : 5U);
            else if (imu_error == ESP_OK && measurement) due_imu = now + MEASUREMENT_IMU_PERIOD_MS;
            else due_imu = uptime_ms() + (imu_error == ESP_OK ? 20U : 2000U);
            if (measurement) {
                const esp_err_t cached_error = next.imu_error;
                next.imu_error = imu_error;
                measurement_consumer(&next);
                next.imu_error = cached_error;
            }
        }
        if (gyro_sampling &&
            collect_gyro_sample(board->imu, &gyro_job, imu_error, &next.imu, uptime_ms())) {
            /* The just-published sample was corrected with the old bias. */
            next.imu_error = ESP_ERR_NOT_FINISHED;
            due_imu = uptime_ms();
        }
        if (!measurement && !suspended && !(parked_sensors & APP_TELEMETRY_BAROMETER) &&
            (interest & APP_TELEMETRY_BAROMETER) && (int32_t)(now - due_baro) >= 0) {
            dps368_sample_t reading;
            const esp_err_t error = board->barometer_ready
                ? dps368_read(board->barometer, &reading) : ESP_ERR_NOT_FOUND;
            if (error == ESP_OK) {
                pressures[pressure_index] = reading.pressure_pa;
                pressure_index = (pressure_index + 1) % 3;
                if (pressure_count < 3) ++pressure_count;
                if (pressure_count == 3) reading.pressure_pa = median_pressure(pressures);
                next.barometer = reading;
                next.barometer_updated_ms = uptime_ms();
                next.barometer_error = ESP_OK;
                due_baro = now + 250;
            } else if (error == ESP_ERR_NOT_FINISHED) {
                due_baro = uptime_ms() + 70;
            } else {
                next.barometer_error = error;
                pressure_count = pressure_index = 0;
                due_baro = uptime_ms() + 2000;
            }
        }
        if ((int32_t)(now - due_rtc) >= 0) {
            esp_err_t rtc_error = ESP_ERR_NOT_FOUND;
            if (board->rtc_ready) {
                rtc_error = measurement
                    ? pcf85063_get_time_with_timeout(board->rtc, &next.rtc, MEASUREMENT_RTC_TIMEOUT_MS)
                    : pcf85063_get_time(board->rtc, &next.rtc);
            }
            if (rtc_error == ESP_OK) next.rtc_updated_ms = uptime_ms();
            /* Keep a recent valid clock through transient bus contention.
             * Readers already expire RTC data older than three seconds. */
            if (!measurement || rtc_error != ESP_ERR_TIMEOUT) next.rtc_error = rtc_error;
            const unsigned retry_ms = measurement ? 250U : 2000U;
            due_rtc = uptime_ms() + (rtc_error == ESP_OK ? 1000U : retry_ms);
        }
        portENTER_CRITICAL(&s_lock);
        next.gyro_calibrating = gyro_calibration_active(s_gyro_calibration.phase);
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
        if (consumer && !measurement) consumer(suspended ? NULL : &next);
        if (!measurement && (int32_t)(now - health_due) >= 0) {
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
        unsigned sleep_ms = suspended ? 100U : sample_imu ? 10U : 50U;
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
    s_imu_available = board->imu_ready && board->imu;
    portEXIT_CRITICAL(&s_lock);
    if (app_create_service_task(telemetry_task, "telemetry", 4096, board, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_started = false;
        s_imu_available = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

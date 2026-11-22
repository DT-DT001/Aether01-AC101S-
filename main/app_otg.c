#include "app_nes_controls.h"
#include "app_otg.h"
#include "aether_usb_audio.h"
#include "aether_usb_camera.h"
#include "aether_usb_storage.h"
#include "app_usb_input.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define OTG_DIAGNOSTIC_INTERVAL_US INT64_C(1000000)
#define OTG_DIAGNOSTIC_STALE_MS 2000U
#define OTG_MANUAL_ROLE_TIMEOUT_US INT64_C(1000000)
/* Bounded conversion-ready budget; the manual does not specify ADC latency. */
#define OTG_VERIFY_TIMEOUT_MS 2000U
#define OTG_CONTROL_SETTLE_MS 100U
#define OTG_RESULT_FORMAT "manual phase=%s result=%s snapshot=%u R0B=%02X RE3=%02X CC=%02X R19=%02X R13=%02X C0=%02X VBUS_raw=%04X VBUS_mv=%u voltage_valid=%u VBUSGOOD=%u diag_attempted=%u diag_valid=%u R50=%02X TS_half_mv=%u TS_valid=%u work_fault=%02X verify=%X cleanup=%X diag=%X"

static const char *const TAG = "app_otg";

typedef enum {
    OTG_SOURCE_OFF,
    OTG_SOURCE_AUTO,
    OTG_SOURCE_MANUAL,
} otg_source_mode_t;

typedef struct {
    aether_board_t *board;
    aether_nes_handle_t nes;
    app_power_t *power;
    aether_usb_host_handle_t host;
    bool vbus_adc_trusted; /* Immutable after initialization. */
    /* Only the host worker mutates these fields. */
    bool cc_configured;
    uint32_t configured_source_epoch;
    int64_t manual_role_deadline;
    bool boost_expected;
    bool failure_evidence;
    bool diagnostics_started;
    int64_t diagnostics_at;
    atomic_int source_mode;
    atomic_bool command_busy;
    atomic_bool stop_pending;
    atomic_uint cancelling;
    /* Session/lease bookkeeping is protected by s_cache_lock. */
    uint32_t source_epoch, lease_sequence, lease_token, lease_epoch;
    uint32_t input_token; /* Separate external ADC lease, not a source epoch. */
    bool lease_owns_session;
    /* The cache and timestamp are protected by s_cache_lock. No I2C while held. */
    app_otg_power_status_t cache;
    bool sampled;
    int64_t sampled_at;
    bool ts_sampled;
    int64_t ts_sampled_at;
    esp_err_t init_error;
} app_otg_context_t;
static app_otg_context_t s_otg = {
    .source_mode = ATOMIC_VAR_INIT(OTG_SOURCE_OFF), .init_error = ESP_ERR_INVALID_STATE,
};
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;

/* Serialise short admission/Host publications, never the worker's PMIC IO.
 * A caller never waits for the command gate while holding the LVGL lock. */
static bool command_begin(app_otg_context_t *state)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&state->command_busy, &expected, true);
}

static esp_err_t command_end(app_otg_context_t *state)
{
    esp_err_t error = ESP_OK;
    for (;;) {
        if (atomic_exchange(&state->stop_pending, false) && state->host) {
            const esp_err_t stopped = aether_usb_host_set_enabled(state->host, false);
            if (error == ESP_OK) error = stopped;
        }
        atomic_store(&state->command_busy, false);
        /* OFF may arrive between the exchange and unlock. Either it acquired
         * the gate, or this/new command flushes its retained stop request. */
        if (!atomic_load(&state->stop_pending) || !command_begin(state)) break;
    }
    return error;
}

static void invalidate_session_locked(app_otg_context_t *state)
{
    ++state->source_epoch;
    if (!state->source_epoch) ++state->source_epoch;
    state->lease_token = 0;
    state->lease_owns_session = false;
    state->cache.output_verified = false;
    atomic_store(&state->source_mode, OTG_SOURCE_OFF);
}

/* OFF must revoke permission immediately even when an ON publisher is in its
 * cache-only admission checks. The publisher drains stop_pending before unlock.
 * cancelling blocks a new ON while an older OFF is still revoking permission. */
static esp_err_t request_source_off(app_otg_context_t *state)
{
    atomic_fetch_add(&state->cancelling, 1);
    portENTER_CRITICAL(&s_cache_lock);
    invalidate_session_locked(state);
    atomic_store(&state->stop_pending, true);
    portEXIT_CRITICAL(&s_cache_lock);
    if (state->board && state->board->pmic)
        tg29_revoke_otg_source_permission(state->board->pmic);
    atomic_fetch_sub(&state->cancelling, 1);
    return command_begin(state) ? command_end(state) : ESP_OK;
}

static bool session_current(app_otg_context_t *state, otg_source_mode_t mode, uint32_t epoch)
{
    portENTER_CRITICAL(&s_cache_lock);
    const bool current = state->source_epoch == epoch &&
        atomic_load(&state->source_mode) == (int)mode && !atomic_load(&state->cancelling) &&
        !atomic_load(&state->stop_pending);
    portEXIT_CRITICAL(&s_cache_lock);
    return current;
}

static bool voltage_in_range(const tg29_usb_power_state_t *power)
{
    return power->vbus_voltage_valid && power->vbus_mv >= 4750 && power->vbus_mv <= 5500;
}

static bool requires_measured_vbus(const app_otg_context_t *state, otg_source_mode_t mode)
{
    return state->vbus_adc_trusted || mode == OTG_SOURCE_MANUAL;
}

static bool source_role_ready(const tg29_usb_power_state_t *power, otg_source_mode_t mode)
{
    return power->source_allowed && !power->attached_as_sink &&
           (mode == OTG_SOURCE_MANUAL ? !power->cc_detection_enabled && power->otg_role_ready :
            power->cc_detection_enabled && power->attached_as_source);
}

/* Every cache publication copies values, never pointers into driver storage. */
static void copy_power_locked(app_otg_context_t *state, const tg29_usb_power_state_t *power)
{
    state->cache.vbus_adc_trusted = requires_measured_vbus(state, atomic_load(&state->source_mode));
    state->cache.valid = power->snapshot_valid;
    state->cache.vbus_valid = power->snapshot_valid && state->cache.vbus_adc_trusted && power->vbus_voltage_valid;
    if (!power->snapshot_valid) return;
    state->cache.boost_enabled = power->boost_enabled;
    state->cache.rbfet_enabled = power->rbfet_enabled;
    state->cache.rbfet_control_raw = power->rbfet_control_raw;
    state->cache.cc_raw = power->cc_raw;
    state->cache.cc_detection_raw = power->cc_detection_raw;
    state->cache.cc_mode_raw = power->cc_mode_raw;
    state->cache.control_raw = power->module_enable_raw;
    state->cache.adc_enable_raw = power->adc_enable_raw;
    state->cache.vbus_raw = power->vbus_raw;
    if (state->cache.vbus_valid) state->cache.vbus_mv = power->vbus_mv;
}

static void copy_diagnostics_locked(app_otg_context_t *state,
                                     const tg29_otg_diagnostics_t *diagnostics, int64_t now)
{
    state->cache.diagnostics_valid = true;
    state->cache.ts_config = diagnostics->ts_config_raw;
    state->cache.work_faults = diagnostics->fault_raw & 0x03U;
    state->cache.ts_valid = diagnostics->ts_voltage_valid;
    if (diagnostics->ts_voltage_valid) state->cache.ts_mv = diagnostics->ts_half_mv / 2U;
    state->ts_sampled = true;
    state->ts_sampled_at = now;
}

static void cache_error(app_otg_context_t *state, esp_err_t error)
{
    state->failure_evidence = true;
    portENTER_CRITICAL(&s_cache_lock);
    state->cache.failure_snapshot = true;
    state->cache.valid = false;
    state->cache.diagnostics_valid = false;
    state->cache.output_verified = false;
    state->cache.vbus_valid = false;
    state->cache.ts_valid = false;
    state->cache.last_error = error;
    portEXIT_CRITICAL(&s_cache_lock);
}

static bool shutdown_allows_source(const app_power_status_t *power)
{
    return !power->shutdown_recovering && power->shutdown_state != APP_POWER_SHUTDOWN_PREPARING &&
           power->shutdown_state != APP_POWER_SHUTDOWN_REQUESTED;
}

/* A deliberate manual ON authorizes one session. Do not let the host's
 * automatic reconnect policy energize the connector again after a fault. */
static esp_err_t stop_manual_source(app_otg_context_t *state, esp_err_t error)
{
    portENTER_CRITICAL(&s_cache_lock);
    const bool manual = atomic_load(&state->source_mode) == OTG_SOURCE_MANUAL;
    if (manual) {
        invalidate_session_locked(state);
        state->cache.manual_source_failed = true;
        state->cache.last_error = error;
        atomic_store(&state->stop_pending, true);
        atomic_fetch_add(&state->cancelling, 1);
    }
    portEXIT_CRITICAL(&s_cache_lock);
    if (manual) {
        tg29_revoke_otg_source_permission(state->board->pmic);
        atomic_fetch_sub(&state->cancelling, 1);
        if (command_begin(state)) (void)command_end(state);
    }
    return error;
}

/* Repeat the cheap shutdown gate in the worker: an ON publisher may be
 * preempted between publishing the command and its post-publication check. */
static esp_err_t source_guard(app_otg_context_t *state, otg_source_mode_t mode)
{
    if (mode == OTG_SOURCE_OFF || atomic_load(&state->source_mode) != (int)mode)
        return ESP_ERR_INVALID_STATE;
    app_power_status_t power;
    if (app_power_get_status(state->power, &power) == ESP_OK && shutdown_allows_source(&power))
        return ESP_OK;
    cache_error(state, ESP_ERR_INVALID_STATE);
    if (mode == OTG_SOURCE_MANUAL) return stop_manual_source(state, ESP_ERR_INVALID_STATE);
    (void)request_source_off(state);
    return ESP_ERR_INVALID_STATE;
}

static void cache_power(app_otg_context_t *state, const tg29_usb_power_state_t *power,
                        bool verified, esp_err_t error)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_cache_lock);
    copy_power_locked(state, power);
    if (state->cache.failure_snapshot) {
        state->cache.diagnostics_valid = false;
        state->cache.ts_valid = false;
    }
    state->cache.failure_snapshot = false;
    state->cache.output_verified = state->cache.vbus_adc_trusted && verified &&
                                   power->snapshot_valid && power->boost_enabled && power->rbfet_enabled;
    state->cache.last_error = error;
    state->cache.verify_error = error;
    state->cache.cleanup_error = ESP_OK;
    state->cache.diagnostic_error = ESP_OK;
    state->sampled = true;
    state->sampled_at = now;
    portEXIT_CRITICAL(&s_cache_lock);
}

/* The complete pre-cleanup probe is a single historical publication. Its
 * power, R50, TS and fault fields always come from the same driver probe.
 * A missing probe explicitly invalidates the configuration/TS fields; reading
 * them after checked OFF would describe restored idle settings instead. */
static void cache_failed_verification(app_otg_context_t *state,
                                       const tg29_otg_verify_result_t *result)
{
    const int64_t now = esp_timer_get_time();
    const bool full_probe = result->pre_cleanup_diag_valid &&
                            result->pre_cleanup_diag.power.snapshot_valid;
    const tg29_usb_power_state_t *power = full_probe ? &result->pre_cleanup_diag.power : &result->power;
    state->failure_evidence = true;
    portENTER_CRITICAL(&s_cache_lock);
    copy_power_locked(state, power);
    state->cache.failure_snapshot = true;
    state->cache.output_verified = false;
    state->cache.diagnostics_valid = false;
    state->cache.ts_valid = false;
    state->ts_sampled = false;
    if (full_probe) copy_diagnostics_locked(state, &result->pre_cleanup_diag, now);
    state->cache.verify_error = result->verify_error;
    state->cache.cleanup_error = result->cleanup_error;
    state->cache.diagnostic_error = result->diag_error;
    state->cache.last_error = result->verify_error != ESP_OK ? result->verify_error : result->cleanup_error;
    state->sampled = true;
    state->sampled_at = now;
    portEXIT_CRITICAL(&s_cache_lock);
}

/* Log only the result already returned by this attempt, before cleanup may
 * replace live PMIC state. A missing probe stays explicitly invalid; never
 * perform extra I2C or combine historical cached TS with a current power read. */
static void log_manual_result(const char *phase, const tg29_otg_verify_result_t *result)
{
    const bool diagnostic = result->pre_cleanup_diag_valid &&
                            result->pre_cleanup_diag.power.snapshot_valid;
    const tg29_usb_power_state_t *power = diagnostic ? &result->pre_cleanup_diag.power : &result->power;
    const tg29_otg_diagnostics_t *diag = &result->pre_cleanup_diag;
    const bool success = result->verify_error == ESP_OK;
    if (success)
        ESP_LOGI(TAG, OTG_RESULT_FORMAT, phase, esp_err_to_name(result->verify_error),
            (unsigned)power->snapshot_valid, (unsigned)power->cc_detection_raw,
            (unsigned)power->cc_mode_raw, (unsigned)power->cc_raw,
            (unsigned)power->module_enable_raw, (unsigned)power->rbfet_control_raw,
            (unsigned)power->adc_enable_raw, (unsigned)power->vbus_raw, (unsigned)power->vbus_mv,
            (unsigned)power->vbus_voltage_valid, (unsigned)power->vbus_good,
            (unsigned)result->pre_cleanup_diag_attempted, (unsigned)diagnostic,
            diagnostic ? (unsigned)diag->ts_config_raw : 0U,
            diagnostic && diag->ts_voltage_valid ? (unsigned)diag->ts_half_mv : 0U,
            (unsigned)(diagnostic && diag->ts_voltage_valid),
            diagnostic ? (unsigned)(diag->fault_raw & 0x03U) : 0U,
            (unsigned)result->verify_error, (unsigned)result->cleanup_error, (unsigned)result->diag_error);
    else
        ESP_LOGW(TAG, OTG_RESULT_FORMAT, phase, esp_err_to_name(result->verify_error),
            (unsigned)power->snapshot_valid, (unsigned)power->cc_detection_raw,
            (unsigned)power->cc_mode_raw, (unsigned)power->cc_raw,
            (unsigned)power->module_enable_raw, (unsigned)power->rbfet_control_raw,
            (unsigned)power->adc_enable_raw, (unsigned)power->vbus_raw, (unsigned)power->vbus_mv,
            (unsigned)power->vbus_voltage_valid, (unsigned)power->vbus_good,
            (unsigned)result->pre_cleanup_diag_attempted, (unsigned)diagnostic,
            diagnostic ? (unsigned)diag->ts_config_raw : 0U,
            diagnostic && diag->ts_voltage_valid ? (unsigned)diag->ts_half_mv : 0U,
            (unsigned)(diagnostic && diag->ts_voltage_valid),
            diagnostic ? (unsigned)(diag->fault_raw & 0x03U) : 0U,
            (unsigned)result->verify_error, (unsigned)result->cleanup_error, (unsigned)result->diag_error);
}

/* Slow live diagnostics are worker-only and bounded to one read per second.
 * Failed attempts are captured inside the PMIC verifier before checked OFF,
 * and must never be replaced with a post-cleanup slow read. */
static esp_err_t sample_diagnostics(app_otg_context_t *state)
{
    const int64_t now = esp_timer_get_time();
    if (state->diagnostics_started && now - state->diagnostics_at < OTG_DIAGNOSTIC_INTERVAL_US)
        return ESP_OK;
    state->diagnostics_started = true;
    state->diagnostics_at = now;
    tg29_otg_diagnostics_t diagnostics;
    const esp_err_t error = tg29_get_otg_diagnostics(state->board->pmic, &diagnostics);
    if (error != ESP_OK || !diagnostics.power.snapshot_valid) {
        const esp_err_t failure = error != ESP_OK ? error : ESP_ERR_INVALID_RESPONSE;
        cache_error(state, failure);
        portENTER_CRITICAL(&s_cache_lock);
        state->cache.diagnostic_error = failure;
        portEXIT_CRITICAL(&s_cache_lock);
        return failure;
    }
    portENTER_CRITICAL(&s_cache_lock);
    copy_power_locked(state, &diagnostics.power);
    copy_diagnostics_locked(state, &diagnostics, now);
    state->cache.output_verified = state->cache.vbus_adc_trusted && state->boost_expected &&
                                  diagnostics.power.boost_enabled && diagnostics.power.rbfet_enabled &&
                                  voltage_in_range(&diagnostics.power) &&
                                  atomic_load(&state->source_mode) != OTG_SOURCE_OFF;
    state->sampled = true;
    state->sampled_at = now;
    portEXIT_CRITICAL(&s_cache_lock);
    return ESP_OK;
}

static esp_err_t query_sink(void *context, bool *attached)
{
    app_otg_context_t *state = context;
    *attached = false;
    portENTER_CRITICAL(&s_cache_lock);
    const otg_source_mode_t mode = atomic_load(&state->source_mode);
    const uint32_t epoch = state->source_epoch;
    portEXIT_CRITICAL(&s_cache_lock);
    if (mode == OTG_SOURCE_OFF) return ESP_OK;
    const bool manual = mode == OTG_SOURCE_MANUAL;
    const esp_err_t guard_error = source_guard(state, mode);
    if (guard_error != ESP_OK) return guard_error;
    if (!state->cc_configured || state->configured_source_epoch != epoch) {
        const esp_err_t error = manual ? tg29_configure_manual_otg(state->board->pmic) :
                                        tg29_configure_cc_detection(state->board->pmic, true);
        if (manual) {
            const tg29_otg_verify_result_t configured = {.verify_error = error};
            log_manual_result("configure_cc", &configured);
        }
        if (error != ESP_OK) {
            cache_error(state, error);
            return manual ? stop_manual_source(state, error) : error;
        }
        /* OFF can arrive while CC configuration owns the I2C mutex. */
        if (!session_current(state, mode, epoch)) {
            tg29_revoke_otg_source_permission(state->board->pmic);
            return ESP_ERR_INVALID_STATE;
        }
        state->cc_configured = true;
        state->configured_source_epoch = epoch;
        if (manual) state->manual_role_deadline = esp_timer_get_time() + OTG_MANUAL_ROLE_TIMEOUT_US;
    }
    tg29_usb_power_state_t power;
    esp_err_t error = tg29_get_usb_power_state(state->board->pmic, &power);
    if (error != ESP_OK || !power.snapshot_valid) {
        if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
        cache_error(state, error);
        return manual ? stop_manual_source(state, error) : error;
    }
    *attached = source_role_ready(&power, mode) && atomic_load(&state->source_mode) == (int)mode;
    /* VBUSGOOD detects an external input source, not our output power-good. */
    if (state->boost_expected && (!power.boost_enabled || !power.rbfet_enabled))
        error = ESP_ERR_INVALID_STATE;
    else if (state->boost_expected && requires_measured_vbus(state, mode) && !voltage_in_range(&power))
        error = ESP_ERR_INVALID_RESPONSE;
    else if (manual && (power.attached_as_sink || (!state->boost_expected && power.vbus_good) ||
                        (state->boost_expected && !*attached)))
        error = ESP_ERR_INVALID_STATE;
    else if (manual && !*attached && esp_timer_get_time() >= state->manual_role_deadline)
        error = ESP_ERR_TIMEOUT;
    if (error != ESP_OK) {
        const tg29_otg_verify_result_t failure = {.power = power, .verify_error = error};
        cache_failed_verification(state, &failure);
        if (manual) log_manual_result(state->boost_expected ? "monitor_output" : "role_wait", &failure);
        *attached = false;
        return manual ? stop_manual_source(state, error) : error;
    }
    /* Keep a failed attempt through fresh CC retries; replace it only when a
     * subsequent physical verification finishes. Role decisions remain live. */
    if (!state->failure_evidence) {
        cache_power(state, &power, state->boost_expected && *attached, ESP_OK);
        error = sample_diagnostics(state);
        if (error != ESP_OK) {
            *attached = false;
            if (manual) return stop_manual_source(state, error);
        }
    }
    return error;
}

static esp_err_t set_vbus(void *context, bool enabled)
{
    app_otg_context_t *state = context;
    const otg_source_mode_t mode = atomic_load(&state->source_mode);
    const bool manual = mode == OTG_SOURCE_MANUAL;
    state->boost_expected = false;
    if (enabled) {
        const esp_err_t guard_error = source_guard(state, mode);
        if (guard_error != ESP_OK) return guard_error;
        portENTER_CRITICAL(&s_cache_lock);
        state->cache.output_verified = false;
        /* An idle-period TS snapshot cannot describe the upcoming bias phase.
         * A previous explicitly historical failure may stay visible meanwhile. */
        if (!state->cache.failure_snapshot) {
            state->cache.diagnostics_valid = false;
            state->cache.ts_valid = false;
        }
        portEXIT_CRITICAL(&s_cache_lock);
        esp_err_t error = tg29_set_otg_enabled(state->board->pmic, true);
        if (error != ESP_OK) {
            const tg29_otg_verify_result_t failure = {.verify_error = error};
            cache_failed_verification(state, &failure);
            if (manual) log_manual_result("enable_output", &failure);
            return manual ? stop_manual_source(state, error) : error;
        }
        tg29_otg_verify_result_t result;
        const bool measured = requires_measured_vbus(state, mode);
        if (measured)
            error = tg29_verify_otg_output_ex(state->board->pmic, OTG_VERIFY_TIMEOUT_MS, &result);
        else
            /* Untrusted ADC cannot prove physical voltage. Retain all role,
             * permission, boost and hardware-protection checks before Host. */
            error = tg29_settle_otg_control(state->board->pmic, OTG_CONTROL_SETTLE_MS, &result);
        if (error == ESP_OK && source_guard(state, mode) != ESP_OK) {
            tg29_revoke_otg_source_permission(state->board->pmic);
            result.cleanup_error = tg29_set_otg_enabled(state->board->pmic, false);
            error = ESP_ERR_INVALID_STATE;
        }
        if (error == ESP_OK && result.power.snapshot_valid && !result.power.rbfet_enabled)
            error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK && (!result.power.snapshot_valid || !source_role_ready(&result.power, mode) ||
                                !result.power.boost_enabled || (measured && !voltage_in_range(&result.power))))
            error = ESP_ERR_INVALID_RESPONSE;
        if (error != ESP_OK) {
            result.verify_error = error;
            cache_failed_verification(state, &result);
            if (manual) log_manual_result("verify_output", &result);
            return manual ? stop_manual_source(state, error) : error;
        }
        state->failure_evidence = false;
        state->boost_expected = true;
        cache_power(state, &result.power, true, ESP_OK);
        if (manual) log_manual_result("verify_output", &result);
        error = sample_diagnostics(state);
        return manual && error != ESP_OK ? stop_manual_source(state, error) : error;
    }
    if (manual) {
        /* Backend startup or teardown can fail before a power callback.
         * Revoke this session before cleanup so it cannot restart itself. */
        aether_usb_host_status_t status;
        const esp_err_t error = aether_usb_host_get_status(state->host, &status);
        stop_manual_source(state, error != ESP_OK ? error :
                           status.last_error != ESP_OK ? status.last_error : ESP_ERR_INVALID_STATE);
    }
    /* Revoke policy even when a bus failure prevents the checked OFF write. */
    state->cc_configured = false;
    portENTER_CRITICAL(&s_cache_lock);
    state->cache.output_verified = false;
    portEXIT_CRITICAL(&s_cache_lock);
    const esp_err_t error = tg29_configure_cc_detection(state->board->pmic, false);
    if (error != ESP_OK) {
        state->failure_evidence = true;
        portENTER_CRITICAL(&s_cache_lock);
        state->cache.failure_snapshot = true;
        state->cache.output_verified = false;
        state->cache.cleanup_error = error;
        if (state->cache.last_error == ESP_OK) state->cache.last_error = error;
        portEXIT_CRITICAL(&s_cache_lock);
    } else {
        portENTER_CRITICAL(&s_cache_lock);
        if (!state->cache.failure_snapshot) {
            state->sampled = false;
            state->cache.boost_enabled = false;
            state->cache.rbfet_enabled = false;
            state->cache.diagnostics_valid = false;
            state->cache.ts_valid = false;
            state->ts_sampled = false;
        }
        portEXIT_CRITICAL(&s_cache_lock);
    }
    return error;
}

static void input(void *context, uint16_t buttons)
{
    app_otg_context_t *state = context;
    if (state->nes) (void)aether_nes_set_external_input(state->nes, buttons);
}

enum { OTG_CAMERA_CLASS = 2 };
static const aether_usb_host_class_ops_t s_classes[] = {
    {.start=aether_usb_storage_class_start, .request_stop=aether_usb_storage_class_request_stop,
     .poll_stopped=aether_usb_storage_class_poll_stopped},
    {.start=aether_usb_audio_class_start, .request_stop=aether_usb_audio_class_request_stop,
     .poll_stopped=aether_usb_audio_class_poll_stopped},
    [OTG_CAMERA_CLASS] = {.start=aether_usb_camera_class_start, .request_stop=aether_usb_camera_class_request_stop,
     .poll_stopped=aether_usb_camera_class_poll_stopped},
};

esp_err_t app_otg_init(aether_board_t *board, aether_nes_handle_t nes, app_power_t *power)
{
    if (!board || !board->pmic || !power || s_otg.host) return ESP_ERR_INVALID_ARG;
    s_otg.board = board; s_otg.nes = nes; s_otg.power = power;
    s_otg.vbus_adc_trusted = board->otg_vbus_adc_trusted;
    portENTER_CRITICAL(&s_cache_lock);
    s_otg.cache.vbus_adc_trusted = s_otg.vbus_adc_trusted;
    portEXIT_CRITICAL(&s_cache_lock);
    atomic_store(&s_otg.source_mode, OTG_SOURCE_OFF);
    const aether_usb_host_config_t config = {
        .context = &s_otg, .query_sink = query_sink, .set_vbus = set_vbus, .input = input,
        .hid_input = app_usb_input_publish, .keyboard_map=app_nes_controls_map, .classes=s_classes, .class_count=sizeof(s_classes)/sizeof(s_classes[0]),
        .camera_class_mask=1U<<OTG_CAMERA_CLASS,
    };
    s_otg.init_error = aether_usb_host_new(&config, &s_otg.host);
    return s_otg.init_error;
}

/* Caller owns command_busy. Slow CC/ADC/PHY work remains on Host's worker. */
static esp_err_t set_source_mode_held(otg_source_mode_t mode)
{
    if (!s_otg.host) return s_otg.init_error;
    portENTER_CRITICAL(&s_cache_lock);
    const bool input_owned = s_otg.input_token != 0;
    portEXIT_CRITICAL(&s_cache_lock);
    if (input_owned) return ESP_ERR_INVALID_STATE;
    if (atomic_load(&s_otg.cancelling) || atomic_load(&s_otg.stop_pending))
        return ESP_ERR_INVALID_STATE;
    app_power_status_t power;
    if (app_power_get_status(s_otg.power, &power) != ESP_OK ||
        !shutdown_allows_source(&power)) return ESP_ERR_INVALID_STATE;
    uint32_t epoch;
    {
        const int previous = atomic_load(&s_otg.source_mode);
        if (previous != (int)mode) {
            if (previous != OTG_SOURCE_OFF) return ESP_ERR_INVALID_STATE;
            aether_usb_host_status_t status;
            const esp_err_t status_error = aether_usb_host_get_status(s_otg.host, &status);
            if (status_error != ESP_OK) return status_error;
            if (status.enabled || status.active || status.vbus_enabled || status.cleanup_pending ||
                status.state != AETHER_OTG_OFF) return ESP_ERR_INVALID_STATE;
            portENTER_CRITICAL(&s_cache_lock);
            if (atomic_load(&s_otg.cancelling) || atomic_load(&s_otg.stop_pending) ||
                atomic_load(&s_otg.source_mode) != previous) {
                portEXIT_CRITICAL(&s_cache_lock);
                return ESP_ERR_INVALID_STATE;
            }
            ++s_otg.source_epoch;
            if (!s_otg.source_epoch) ++s_otg.source_epoch;
            atomic_store(&s_otg.source_mode, mode);
            s_otg.cache.manual_source_failed = false;
            s_otg.cache.output_verified = false;
            portEXIT_CRITICAL(&s_cache_lock);
        }
    }
    portENTER_CRITICAL(&s_cache_lock);
    epoch = s_otg.source_epoch;
    portEXIT_CRITICAL(&s_cache_lock);
    if (!session_current(&s_otg, mode, epoch)) return ESP_ERR_INVALID_STATE;
    const esp_err_t error = aether_usb_host_set_enabled(s_otg.host, true);
    if (error != ESP_OK) {
        (void)request_source_off(&s_otg);
        return error;
    }
    if (app_power_get_status(s_otg.power, &power) != ESP_OK || !shutdown_allows_source(&power) ||
        !session_current(&s_otg, mode, epoch)) {
        /* Shutdown BEGIN may have published OFF after our first check. */
        (void)request_source_off(&s_otg);
        return ESP_ERR_INVALID_STATE;
    }
    return error;
}

static esp_err_t set_source_mode(otg_source_mode_t mode)
{
    if (mode == OTG_SOURCE_OFF) {
        const esp_err_t error = request_source_off(&s_otg);
        return s_otg.host ? error : s_otg.init_error;
    }
    if (!command_begin(&s_otg)) return ESP_ERR_NOT_FINISHED;
    const esp_err_t error = set_source_mode_held(mode);
    if (error == ESP_OK && mode == OTG_SOURCE_MANUAL) {
        /* An explicit user ON adopts a session previously accessory-owned. */
        portENTER_CRITICAL(&s_cache_lock);
        s_otg.lease_owns_session = false;
        portEXIT_CRITICAL(&s_cache_lock);
    }
    const esp_err_t cleanup = command_end(&s_otg);
    return error != ESP_OK ? error : cleanup;
}

esp_err_t app_otg_set_enabled(bool enabled)
{
    return set_source_mode(enabled ? OTG_SOURCE_AUTO : OTG_SOURCE_OFF);
}

esp_err_t app_otg_set_camera_profile(bool enabled)
{
    if (!s_otg.host) return s_otg.init_error;
    if (enabled && atomic_load(&s_otg.source_mode) == OTG_SOURCE_MANUAL) return ESP_ERR_INVALID_STATE;
    return aether_usb_host_set_camera_profile(s_otg.host, enabled);
}

esp_err_t app_otg_set_manual_enabled(bool enabled)
{
    return set_source_mode(enabled ? OTG_SOURCE_MANUAL : OTG_SOURCE_OFF);
}

esp_err_t app_otg_manual_lease_acquire(uint32_t *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    *token = 0;
    if (!s_otg.host) return s_otg.init_error;
    if (!command_begin(&s_otg)) return ESP_ERR_NOT_FINISHED;
    esp_err_t error = ESP_OK;
    app_power_status_t power;
    aether_usb_host_status_t host;
    portENTER_CRITICAL(&s_cache_lock);
    const bool leased = s_otg.lease_token != 0 || s_otg.input_token != 0;
    portEXIT_CRITICAL(&s_cache_lock);
    const otg_source_mode_t mode = atomic_load(&s_otg.source_mode);
    if (leased || atomic_load(&s_otg.cancelling) || atomic_load(&s_otg.stop_pending) ||
        mode == OTG_SOURCE_AUTO || app_power_get_status(s_otg.power, &power) != ESP_OK ||
        !shutdown_allows_source(&power)) error = ESP_ERR_INVALID_STATE;
    else error = aether_usb_host_get_status(s_otg.host, &host);
    if (error == ESP_OK && (host.cleanup_pending || host.state == AETHER_OTG_STOPPING ||
        host.state == AETHER_OTG_ERROR || (mode == OTG_SOURCE_MANUAL && !host.enabled)))
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && mode == OTG_SOURCE_OFF)
        error = set_source_mode_held(OTG_SOURCE_MANUAL);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_cache_lock);
        if (atomic_load(&s_otg.source_mode) != OTG_SOURCE_MANUAL ||
            atomic_load(&s_otg.cancelling) || atomic_load(&s_otg.stop_pending)) error = ESP_ERR_INVALID_STATE;
        else {
            ++s_otg.lease_sequence;
            if (!s_otg.lease_sequence) ++s_otg.lease_sequence;
            s_otg.lease_token = s_otg.lease_sequence;
            s_otg.lease_epoch = s_otg.source_epoch;
            s_otg.lease_owns_session = mode == OTG_SOURCE_OFF;
            *token = s_otg.lease_token;
        }
        portEXIT_CRITICAL(&s_cache_lock);
    }
    const esp_err_t cleanup = command_end(&s_otg);
    return error != ESP_OK ? error : cleanup;
}

esp_err_t app_otg_manual_lease_release(uint32_t token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    if (!command_begin(&s_otg)) return ESP_ERR_NOT_FINISHED;
    portENTER_CRITICAL(&s_cache_lock);
    const bool valid = s_otg.lease_token == token && s_otg.lease_epoch == s_otg.source_epoch &&
        atomic_load(&s_otg.source_mode) == OTG_SOURCE_MANUAL;
    const bool owned = valid && s_otg.lease_owns_session;
    if (valid) { s_otg.lease_token = 0; s_otg.lease_owns_session = false; }
    portEXIT_CRITICAL(&s_cache_lock);
    if (owned) (void)request_source_off(&s_otg);
    const esp_err_t cleanup = command_end(&s_otg);
    return !valid ? ESP_ERR_INVALID_STATE : cleanup;
}

bool app_otg_manual_lease_valid(uint32_t token)
{
    if (!token) return false;
    portENTER_CRITICAL(&s_cache_lock);
    const bool valid = s_otg.lease_token == token && s_otg.lease_epoch == s_otg.source_epoch &&
        atomic_load(&s_otg.source_mode) == OTG_SOURCE_MANUAL && !atomic_load(&s_otg.cancelling) &&
        !atomic_load(&s_otg.stop_pending);
    portEXIT_CRITICAL(&s_cache_lock);
    return valid;
}

esp_err_t app_otg_external_input_acquire(uint32_t *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    *token = 0;
    if (!s_otg.host) return s_otg.init_error;
    if (!command_begin(&s_otg)) return ESP_ERR_NOT_FINISHED;
    app_power_status_t power;
    aether_usb_host_status_t host;
    esp_err_t error = ESP_OK;
    portENTER_CRITICAL(&s_cache_lock);
    const bool leased = s_otg.input_token != 0 || s_otg.lease_token != 0;
    portEXIT_CRITICAL(&s_cache_lock);
    const otg_source_mode_t mode = atomic_load(&s_otg.source_mode);
    if (leased || mode == OTG_SOURCE_AUTO || atomic_load(&s_otg.cancelling) ||
        atomic_load(&s_otg.stop_pending)) error = ESP_ERR_INVALID_STATE;
    else error = app_power_get_status(s_otg.power, &power);
    if (error == ESP_OK && !shutdown_allows_source(&power)) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) error = aether_usb_host_get_status(s_otg.host, &host);
    if (error == ESP_OK && (host.cleanup_pending || host.state == AETHER_OTG_STOPPING ||
        host.state == AETHER_OTG_ERROR)) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && mode == OTG_SOURCE_MANUAL)
        error = host.enabled ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && (host.enabled || host.active || host.vbus_enabled ||
                            host.state != AETHER_OTG_OFF)) error = ESP_ERR_INVALID_STATE;
    uint32_t input = 0;
    if (error == ESP_OK) error = tg29_vbus_input_acquire(s_otg.board->pmic, &input);
    if (input) {
        /* Failed ADC restoration still owns the token and must block source ON. */
        portENTER_CRITICAL(&s_cache_lock);
        s_otg.input_token = input;
        *token = input;
        portEXIT_CRITICAL(&s_cache_lock);
        if (error == ESP_OK) {
            error = app_power_get_status(s_otg.power, &power);
            if (error == ESP_OK && !shutdown_allows_source(&power)) error = ESP_ERR_INVALID_STATE;
        }
    }
    const esp_err_t cleanup = command_end(&s_otg);
    return error != ESP_OK ? error : cleanup;
}

esp_err_t app_otg_external_input_read(uint32_t token, app_otg_input_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    if (!token) return ESP_ERR_INVALID_ARG;
    if (!command_begin(&s_otg)) return ESP_ERR_NOT_FINISHED;
    portENTER_CRITICAL(&s_cache_lock);
    const bool owned = s_otg.input_token == token;
    portEXIT_CRITICAL(&s_cache_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    app_power_status_t power;
    if (owned && atomic_load(&s_otg.source_mode) == OTG_SOURCE_OFF) {
        error = app_power_get_status(s_otg.power, &power);
        if (error == ESP_OK && !shutdown_allows_source(&power)) error = ESP_ERR_INVALID_STATE;
        tg29_usb_power_state_t input = {0};
        if (error == ESP_OK) error = tg29_vbus_input_read(s_otg.board->pmic, token, &input);
        if (error == ESP_OK) *status = (app_otg_input_status_t){
            .valid = input.snapshot_valid, .present = input.vbus_good,
            .voltage_valid = input.snapshot_valid && input.vbus_voltage_valid,
            .voltage_mv = input.vbus_voltage_valid ? input.vbus_mv : 0,
        };
    }
    const esp_err_t cleanup = command_end(&s_otg);
    if (error == ESP_OK && cleanup != ESP_OK) memset(status, 0, sizeof(*status));
    return error != ESP_OK ? error : cleanup;
}

esp_err_t app_otg_external_input_release(uint32_t token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    if (!command_begin(&s_otg)) return ESP_ERR_NOT_FINISHED;
    portENTER_CRITICAL(&s_cache_lock);
    const bool owned = s_otg.input_token == token;
    portEXIT_CRITICAL(&s_cache_lock);
    const esp_err_t error = owned ? tg29_vbus_input_release(s_otg.board->pmic, token) : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_cache_lock);
        s_otg.input_token = 0;
        portEXIT_CRITICAL(&s_cache_lock);
    }
    /* A separately requested Host OFF may fail, but it no longer owns this
     * released input token. Do not report a retryable lease failure after
     * discarding its owner. Host retains its own cleanup/error state. */
    (void)command_end(&s_otg);
    return error;
}

esp_err_t app_otg_get_status(aether_usb_host_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    if (!s_otg.host) return s_otg.init_error;
    return aether_usb_host_get_status(s_otg.host, status);
}

esp_err_t app_otg_get_power_status(app_otg_power_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cache_lock);
    *status = s_otg.cache;
    const bool sampled = s_otg.sampled;
    const int64_t sampled_at = s_otg.sampled_at;
    const bool ts_sampled = s_otg.ts_sampled;
    const int64_t ts_sampled_at = s_otg.ts_sampled_at;
    portEXIT_CRITICAL(&s_cache_lock);
    const int64_t now = esp_timer_get_time();
    const uint64_t age = sampled && now >= sampled_at ? (uint64_t)(now - sampled_at) / 1000U : UINT32_MAX;
    status->age_ms = age < UINT32_MAX ? (uint32_t)age : UINT32_MAX;
    if (!sampled || (!status->failure_snapshot && status->age_ms > OTG_DIAGNOSTIC_STALE_MS)) {
        status->valid = false;
        status->vbus_valid = false;
        status->diagnostics_valid = false;
        status->ts_valid = false;
        status->output_verified = false;
    }
    if (!ts_sampled || now < ts_sampled_at || (!status->failure_snapshot &&
        (uint64_t)(now - ts_sampled_at) / 1000U > OTG_DIAGNOSTIC_STALE_MS)) {
        status->diagnostics_valid = false;
        status->ts_valid = false;
    }
    const otg_source_mode_t mode = atomic_load(&s_otg.source_mode);
    status->manual_source_requested = mode == OTG_SOURCE_MANUAL;
    if (status->failure_snapshot || mode == OTG_SOURCE_OFF) status->output_verified = false;
    return s_otg.host ? ESP_OK : s_otg.init_error;
}

esp_err_t app_otg_disable_wait(uint32_t timeout_ms)
{
    /* Publish OFF and revoke source permission before the blocking join. The
     * PMIC verifier notices cancellation between its 10ms-spaced samples. */
    (void)app_otg_set_enabled(false);
    if (!s_otg.host) return ESP_OK;
    return aether_usb_host_disable_wait(s_otg.host, timeout_ms);
}

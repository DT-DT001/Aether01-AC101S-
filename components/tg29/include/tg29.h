#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TG29_I2C_ADDRESS_7BIT 0x34
#define TG29_KEY_EVENT_RELEASE     (1U << 0)
#define TG29_KEY_EVENT_PRESS       (1U << 1)
#define TG29_KEY_EVENT_LONG_PRESS  (1U << 2)
#define TG29_KEY_EVENT_SHORT_PRESS (1U << 3)

typedef struct tg29_dev_t *tg29_handle_t;

typedef enum {
    TG29_RAIL_DCDC1,
    TG29_RAIL_DCDC2,
    TG29_RAIL_DCDC3,
    TG29_RAIL_ALDO1,
    TG29_RAIL_ALDO2,
    TG29_RAIL_ALDO3,
    TG29_RAIL_ALDO4,
    TG29_RAIL_BLDO1,
    TG29_RAIL_BLDO2,
    TG29_RAIL_BLDO3,
    TG29_RAIL_BLDO4,
    TG29_RAIL_CLDO1,
    TG29_RAIL_CLDO2,
    TG29_RAIL_CLDO3,
    TG29_RAIL_CLDO4,
    TG29_RAIL_CPUSLDO,
} tg29_rail_t;

typedef struct {
    uint16_t aldo2_mv; /* Board shared I2C supply: must be 3300 mV. */
    uint16_t aldo4_mv;
    uint16_t bldo1_mv;
    uint16_t cldo1_mv;
    uint16_t cldo3_mv;
    bool enable_sensor_rail; /* Must be true: attached sensor I/O shares PMIC I2C. */
    bool enable_codec_rail;
    bool enable_sd_rail;
    bool enable_touch_rail;
    bool enable_display_rail;
} tg29_power_profile_t;

typedef struct {
    uint8_t dcdc_enable;
    uint8_t ldo_enable_0;
    uint8_t ldo_enable_1;
    uint16_t dcdc2_mv;
} tg29_power_state_t;

typedef struct {
    bool presence_valid;
    bool battery_present; /* PMIC-reported bit; trust only with presence_valid. */
    bool vbus_present; /* VBUSGOOD: usable input, not merely physical insertion. */
    bool charging;
    bool charge_done;
    bool charge_state_valid;
    bool percentage_valid;
    bool voltage_valid;
    uint8_t percentage; /* Full REG A4, not a seven-bit field; >100 is invalid. */
    uint8_t charge_state;
    uint8_t current_direction;
    uint16_t voltage_mv; /* Raw 14-bit reading, 1 mV/LSB when sampled. */
} tg29_battery_state_t;

typedef enum {
    TG29_CC_DISABLED = 0,
    TG29_CC_SINK = 1,
    TG29_CC_SOURCE = 2,
    TG29_CC_DRP = 3,
} tg29_cc_mode_t;

typedef enum {
    TG29_CC_STATE_DISABLED = 0,
    TG29_CC_UNATTACH_SNK = 1,
    TG29_CC_ATTACHWAIT_SNK = 2,
    TG29_CC_ATTACH_SNK = 3,
    TG29_CC_UNATTACH_SRC = 4,
    TG29_CC_ATTACHWAIT_SRC = 5,
    TG29_CC_ATTACH_SRC = 6,
    TG29_CC_AUDIO_ACCESSORY = 7,
    TG29_CC_TRY_SRC = 9,
    TG29_CC_TRYWAIT_SNK = 10,
    TG29_CC_TRY_SNK = 11,
    TG29_CC_TRYWAIT_SRC = 12,
    TG29_CC_ERROR_RECOVERY = 14,
} tg29_cc_state_t;

typedef struct {
    bool snapshot_valid; /* All reads of this snapshot succeeded. */
    tg29_cc_mode_t cc_mode;
    tg29_cc_state_t cc_state;
    uint8_t cc_raw;
    uint8_t cc_detection_raw; /* REG0B, same snapshot as the CC state. */
    uint8_t cc_mode_raw; /* REGE3, includes port preference/current policy. */
    bool cc_detection_enabled;
    bool source_allowed;    /* Verified software policy; resets false at creation. */
    bool attached_as_source; /* Local SOURCE / remote SINK (Rd); stable state6 only. */
    bool attached_as_sink;   /* Local SINK / remote SOURCE; never source VBUS. */
    bool otg_role_ready;     /* AUTO: stable SOURCE. MANUAL: detector and port OFF; E7 diagnostic only. */
    bool boost_enabled;      /* Verified control bit, not a measured VBUS voltage. */
    bool vbus_good; /* Input-source detection only; never OTG output PG. */
    uint8_t module_enable_raw;
    uint8_t rbfet_control_raw;
    bool rbfet_enabled; /* REG13[0] source-path gate; not physical voltage proof. */
    uint8_t adc_enable_raw;
    bool vbus_voltage_valid; /* Enabled, nonzero, within documented0..0x1FFF; not trust/proof. */
    uint16_t vbus_mv;
    uint16_t vbus_raw; /* Unscaled14-bit ADC value, retained even at endpoints. */
    uint8_t vbus_adc_high_raw, vbus_adc_low_raw;
    uint16_t boost_mv;
    uint16_t boost_limit_ma; /* 0 means current limiting disabled, not 0mA. */
} tg29_usb_power_state_t;

typedef struct {
    tg29_usb_power_state_t power;
    uint8_t ts_config_raw, ts_work_low_raw, ts_work_high_raw;
    uint8_t fault_raw, irq0_raw, irq3_raw, adc_select_raw;
    bool ts_voltage_valid; /* Only if channel already enabled and selector is TS. */
    uint16_t ts_half_mv; /* 0.5mV/LSB; no floating point. */
} tg29_otg_diagnostics_t;

typedef struct {
    tg29_usb_power_state_t power; /* Last complete verification sample, before cleanup. */
    tg29_otg_diagnostics_t pre_cleanup_diag;
    bool pre_cleanup_diag_attempted;
    bool pre_cleanup_diag_valid; /* Whole probe completed before checked OFF. */
    esp_err_t diag_error; /* Separate from the original verification failure. */
    esp_err_t verify_error;
    esp_err_t cleanup_error;
} tg29_otg_verify_result_t;

/** Extended verification: returns original verify_error, with cleanup_error
 * independently retained. Only a current eligible-policy voltage deadline permits one full
 * pre-cleanup probe. Cancellation, role loss and read/lock errors skip it.
 * Probe reads check cancellation between transactions; no fault ACK or ADC mux
 * writes. Invalid/incomplete probe is entirely cleared. No probe on success. */
esp_err_t tg29_verify_otg_output_ex(tg29_handle_t handle, uint32_t timeout_ms,
                                    tg29_otg_verify_result_t *result);

/** Worker-only control settle for an automatic session whose VBUS ADC is untrusted.
 * settle_ms100..300 (normally100). Continuously checks fresh CC6/detection,
 * source permission/cancel epoch, RBFET gate and BOOST control; never enables ADC or proves
 * voltage. Success permits enumeration only, never output_verified. Returns
 * verify_error and retains independent cleanup_error after a failed checked
 * OFF. Does not collect a failure probe or change protection/charger settings. */
esp_err_t tg29_settle_otg_control(tg29_handle_t handle, uint32_t settle_ms,
                                  tg29_otg_verify_result_t *result);

#define TG29_VBUS_INPUT_TRACE_MAX 5

typedef struct {
    uint32_t elapsed_ms;
    uint8_t adc_enable_raw, vbus_adc_high_raw, vbus_adc_low_raw;
    uint16_t vbus_raw, vbus_mv;
    bool vbus_voltage_valid;
} tg29_vbus_input_trace_t;

typedef struct {
    tg29_usb_power_state_t power; /* Latest complete read, not first100ms sample. */
    bool attempted;
    esp_err_t probe_error;
    esp_err_t cleanup_error;
    tg29_vbus_input_trace_t trace[TG29_VBUS_INPUT_TRACE_MAX];
    uint8_t trace_count;
    uint32_t first_valid_elapsed_ms; /* First range-valid sample >=100ms;0=none. */
} tg29_vbus_input_probe_result_t;

/** Board-bringup-only input ADC baseline, before GUI/Host can acquire source.
 * Requires no source permission, BOOST OFF and external VBUSGOOD throughout.
 * timeout100..2500ms. Keeps an owned C0[2] enabled for the whole observation,
 * recording up to5 milestones100/250/500/1000/2000ms at actual elapsed times.
 * The final power sample is latest; raw endpoints never imply5V. The probe
 * always attempts to restore only that owned bit. No CC/BOOST/TS/charger/mux/
 * protection writes. No delay holds a mutex. Returns probe_error; cleanup_error
 * is separate and retained ownership allows checked-OFF cleanup retry. */
esp_err_t tg29_probe_vbus_input(tg29_handle_t handle, uint32_t timeout_ms,
                                tg29_vbus_input_probe_result_t *result);

/** Explicit board declaration: TS is a fixed10k resistor to GND, idle low
 * REG50 nibble=0x2. Default false. No hardware I/O. Set before CC setup;
 * do not opt in an NTC, external-voltage or unknown TS circuit. */
esp_err_t tg29_set_otg_fixed_ts_10k_policy(tg29_handle_t handle, bool enabled);
/** Immediate cancellation gate only, safe from GUI/task context: atomic store,
 * no I2C or mutex. Worker must still perform checked OFF and cleanup. */
void tg29_revoke_otg_source_permission(tg29_handle_t handle);
/** Read-only slower diagnostics. Does not ACK faults or change ADC selector.
 * Disabled/unselected/endpoint samples are invalid, never a measured zero. */
esp_err_t tg29_get_otg_diagnostics(tg29_handle_t handle, tg29_otg_diagnostics_t *state);
/** Worker-only bounded physical-output proof, after set_otg_enabled(true).
 * Temporarily owns a previously disabled VBUS ADC channel until checked OFF.
 * Requires fresh eligible role/policy/RBFET/boost throughout, at least1000ms settling and two
 * consecutive10ms-spaced samples in4.75..5.50V. The settling budget is empirical
 * policy, not a documented ADC-ready guarantee. No mutex is held over delays.
 * timeout1..2000ms; errors attempt checked OFF. Output is the last complete
 * sample (possibly before cleanup); read/lock failures clear it. Non-OK never
 * proves output. Never enables
 * boost. Caller may power the USB root port only after ESP_OK. */
esp_err_t tg29_verify_otg_output(tg29_handle_t handle, uint32_t timeout_ms,
                                 tg29_usb_power_state_t *state);

typedef struct {
    uint16_t fast_charge_ma;
    uint16_t precharge_ma;
    uint8_t fast_charge_code;
    uint8_t precharge_code;
    bool fast_charge_valid;
    bool precharge_valid;
} tg29_charge_current_state_t;

/** Configure default-current CC detection: allowed=DRP with sink preference;
 * false=SINK and verified boost OFF. Explicitly enables CC detection. A role
 * or detection change first switches boost off.
 * Does not enable boost or charging. Call from a task, never from an ISR. */
esp_err_t tg29_configure_cc_detection(tg29_handle_t handle, bool allow_source);
/** Authorize explicit CC-less manual output. Checked OFF precedes verified
 * detector disable and port DISABLED; E7 can retain a stale state and is
 * diagnostic only. Starting still requires settled unpowered VBUS preflight,
 * TS/undervoltage protection, and measured output verification before Host.
 * configure_cc_detection(false) restores enabled SINK detection. Turn manual output OFF before attaching
 * a charger/computer: a disabled CC FSM cannot report attachment roles. */
esp_err_t tg29_configure_manual_otg(tg29_handle_t handle);
/** Read/ACK only CC detection-finished/removal IRQs in REG4C[6:5]. Poll from
 * the USB control task so shared PMIC IRQ cannot remain low; key/fault IRQs
 * are untouched. On ACK failure events still contains the read snapshot. */
esp_err_t tg29_get_cc_events(tg29_handle_t handle, uint8_t *events);
/** Pure register snapshot. No interrupt acknowledgment, ADC or charger writes.
 * Automatic sourcing rejects ATTACHWAIT/TRY/reserved states. Explicit manual
 * sourcing instead verifies detector/port OFF and treats E7 as diagnostic. */
esp_err_t tg29_get_usb_power_state(tg29_handle_t handle, tg29_usb_power_state_t *state);
/** Worker-only external-input ADC lease. Does not change CC, BOOST, TS or
 * charger settings, and excludes source configuration/enable until released.
 * NOT_FOUND means absent VBUSGOOD with no owner/source/BOOST/RBFET conflict;
 * stale CC state is irrelevant to absence. Only the separate manual policy
 * and physical preflight may subsequently authorize output.
 * A nonzero token on a failed acquire retains cleanup ownership: release it
 * before trying anything else. Read is fresh, with no settling guarantee;
 * callers must wait for settled, consecutive valid in-range samples. Missing
 * input or a conflicting source role invalidates the read, not the owner.
 * Release restores only an ADC bit enabled by this lease. Failure retains
 * ownership for retry; stale tokens never touch a newer owner's hardware. */
esp_err_t tg29_vbus_input_acquire(tg29_handle_t handle, uint32_t *token);
esp_err_t tg29_vbus_input_read(tg29_handle_t handle, uint32_t token,
                               tg29_usb_power_state_t *state);
esp_err_t tg29_vbus_input_release(tg29_handle_t handle, uint32_t token);
/** Synchronous checked control. Automatic mode requires fresh ATTACH_SRC in
 * SOURCE/DRP. Explicit manual mode requires detector OFF/port DISABLED and settled
 * VBUS ADC readings <=800mV with VBUSGOOD low before opening the source path.
 * Both modes require verified source policy and reject external VBUSGOOD.
 * Programs 5.126V / 500mA with the existing VBAT cutoff retained. Disabling
 * always independently clears/verifies RBFET and boost regardless of CC. TS/ADC
 * restoration waits until both are confirmed OFF. This API does not own
 * the UI policy: its caller must also enforce the user's OFF preference. */
esp_err_t tg29_set_otg_enabled(tg29_handle_t handle, bool enabled);
/** Read programmed limits (not instantaneous measured current). Reserved ICC
 * encodings are returned invalid. I2C/mutex failures clear the output. */
esp_err_t tg29_get_charge_current(tg29_handle_t handle, tg29_charge_current_state_t *state);
/** Only lower programmed fast/precharge limits, never enable charging. Each
 * target is floored to a 64mA step; an800mA ceiling therefore means768mA.
 * Already lower limits are left untouched. Unsupported high ICC codes are
 * repaired to the safe ceiling. On error state is cleared; changed reports
 * whether any limit was successfully verified before a later failure. */
esp_err_t tg29_limit_charge_current(tg29_handle_t handle, uint16_t maximum_ma,
                                    tg29_charge_current_state_t *state, bool *changed);
/** Failure protection only: clear and verify REG19[1], retaining boost and all
 * other settings. Never enables charging or changes configured current. */
esp_err_t tg29_disable_charging(tg29_handle_t handle);

esp_err_t tg29_new(i2c_master_bus_handle_t bus, tg29_handle_t *out_handle);
esp_err_t tg29_del(tg29_handle_t handle);

/** Apply the Aether01 rail policy. ALDO2 is verified at 3.3 V and enabled first,
 * then retained while optional loads are isolated/configured. DCDC2 voltage is
 * verified but never reprogrammed. Failure cleanup preserves ALDO2's existing
 * enable state and never enables an unverified supply. */
esp_err_t tg29_apply_power_profile(tg29_handle_t handle, const tg29_power_profile_t *profile);

/** Enable or disable one rail. Disabling DCDC2 or ALDO2 is rejected.
 * ALDO2 can only be enabled after its 3.3 V setting is verified. */
esp_err_t tg29_set_rail_enabled(tg29_handle_t handle, tg29_rail_t rail, bool enabled);

/** Configure and verify an ALDO/BLDO/CLDO rail in 100 mV steps from 0.5 V
 * to 3.5 V. ALDO2 is fixed at 3.3 V on this board. DCDC and CPUSLDO use different
 * encodings and are rejected. */
esp_err_t tg29_set_ldo_voltage(tg29_handle_t handle, tg29_rail_t rail, uint16_t millivolts);

esp_err_t tg29_get_power_state(tg29_handle_t handle, tg29_power_state_t *state);
/** Pure register reads under the PMIC recursive mutex; never enables ADC,
 * gauge, detection or charging. An I2C/mutex/argument failure clears *state.
 * Reported presence requires detection enabled; the TG29 does not expose a
 * documented detection-complete flag. Its detection runs with usable input.
 * Measurements require valid reported presence and their respective enable.
 * Voltage endpoints 0/0x3FFF fail a conservative plausibility check, not a
 * documented hardware validity flag. Other raw values are not chemistry-clamped.
 * Reserved direction/state codes invalidate charge_state_valid. */
esp_err_t tg29_get_battery_state(tg29_handle_t handle, tg29_battery_state_t *state);
/** Enable PWRON press/release/short/long events on the PMIC IRQ output.
 * Clear pre-existing key latches only; preserve unrelated IRQ enables/status.
 * Does not change hardware power-on/off timings or long-press shutdown. */
esp_err_t tg29_configure_key_wakeup(tg29_handle_t handle);
/** Read and acknowledge PWRON key events. Call in a task, never an ISR.
 * Event bits match REG49[3:0]. PRESS is the negative edge, so waking need not
 * wait for release. Only observed key bits are acknowledged (RW1C); unrelated
 * VBUS/BAT/fault status remains latched. On acknowledgement failure, events
 * still contains the successfully read snapshot; on read failure it is zero. */
esp_err_t tg29_get_key_events(tg29_handle_t handle, uint8_t *events);
/** Last step of a separately completed shutdown preparation. Request software
 * power-off using REG27.bit0 (RWAC). Preserve bits3:2/reserved settings; never
 * change PWRON timers, hard-off enable or the independent hardware long press.
 * No readback is possible after power removal. ESP_OK means command ACK only. */
esp_err_t tg29_request_power_off(tg29_handle_t handle);
const char *tg29_rail_name(tg29_rail_t rail);

#ifdef __cplusplus
}
#endif

/* Real application bridge / real TG29 and host public types. The fakes only
 * replace hardware, host scheduling and time. No LVGL or panel-wake symbols. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "stubs/app_usb_input.h"
#include "../../main/app_otg.c"

static app_power_t manager;
static aether_board_t board;
static aether_usb_host_config_t callbacks;
static aether_usb_host_status_t host_state;
static tg29_usb_power_state_t cc_state, verify_state;
static tg29_otg_diagnostics_t diagnostic_state, failure_diagnostic_state;
static bool failure_probe_enabled, probe_power_override;
static esp_err_t verify_diagnostic_error, verify_cleanup_error;
static unsigned new_calls, commands, status_calls, waits, power_reads;
static unsigned keyboard_map_calls;
static void *keyboard_map_context;
static uint8_t keyboard_map_keys[32];
static unsigned configure_calls, cc_reads, vbus_calls, input_calls, verify_calls, settle_calls, diagnostic_calls, revokes;
static unsigned manual_configure_calls;
static bool last_command, last_vbus, last_configure, cancel_in_verify, cancel_in_configure;
static uint16_t last_buttons;
static uint32_t last_timeout;
static int host_token, pmic_token, nes_token;
static esp_err_t new_error, command_error, status_error, wait_error, power_error;
static esp_err_t configure_error, cc_error, vbus_error, input_error, verify_error, diagnostic_error;
static int64_t now_us;
static bool shutdown_between_check_and_publish;
static _Thread_local unsigned critical_depth;
#define HOST ((aether_usb_host_handle_t)&host_token)
#define PMIC ((tg29_handle_t)&pmic_token)
#define NES ((aether_nes_handle_t)&nes_token)

void test_enter(portMUX_TYPE *lock) { assert(!critical_depth); assert(!pthread_mutex_lock(lock)); ++critical_depth; }
void test_exit(portMUX_TYPE *lock) { assert(critical_depth==1); --critical_depth; assert(!pthread_mutex_unlock(lock)); }
int64_t esp_timer_get_time(void) { return now_us; }
static void outside_cache_lock(void) { assert(!critical_depth); }

/* Cache-only mapping double: exercise the registered callback and full HID
 * bitmap without bringing the production NVS/settings worker into this suite. */
uint16_t app_nes_controls_map(void *context, const uint8_t keys[32])
{
    outside_cache_lock(); assert(keys);
    ++keyboard_map_calls; keyboard_map_context=context;
    memcpy(keyboard_map_keys,keys,sizeof(keyboard_map_keys));
    uint16_t result=0;
    if(keys[0x52U/8U] & (1U<<(0x52U%8U)))result|=AETHER_HID_UP;
    if(keys[0x1DU/8U] & (1U<<(0x1DU%8U)))result|=AETHER_HID_A;
    return result;
}

esp_err_t aether_usb_host_new(const aether_usb_host_config_t *c, aether_usb_host_handle_t *out)
{
    outside_cache_lock(); assert(c && out && c->query_sink && c->set_vbus && c->input && c->context);
    assert(c->keyboard_map==app_nes_controls_map);
    assert(!configure_calls && !cc_reads && !vbus_calls && !input_calls && !diagnostic_calls && !revokes);
    callbacks=*c; ++new_calls; *out=new_error ? NULL : HOST; return new_error;
}
esp_err_t aether_usb_host_set_enabled(aether_usb_host_handle_t h, bool enabled)
{
    outside_cache_lock(); assert(h==HOST); ++commands; last_command=enabled;
    if(command_error==ESP_OK)host_state.enabled=enabled;
    return command_error;
}
esp_err_t aether_usb_host_get_status(aether_usb_host_handle_t h, aether_usb_host_status_t *s)
{ outside_cache_lock(); assert(h==HOST && s); ++status_calls; if(status_error)return status_error; *s=host_state; return ESP_OK; }
esp_err_t aether_usb_host_disable_wait(aether_usb_host_handle_t h, uint32_t timeout)
{
    outside_cache_lock(); assert(h==HOST && atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF);
    assert(!cc_state.source_allowed); ++waits; last_timeout=timeout; return wait_error;
}
esp_err_t app_power_get_status(const app_power_t *p, app_power_status_t *s)
{
    outside_cache_lock(); assert(p==&manager && s); ++power_reads;
    if(power_error)return power_error;
    *s=(app_power_status_t){.shutdown_state=manager.shutdown_state,.sleeping=manager.sleeping,.shutdown_recovering=manager.shutdown_cancel_error!=ESP_OK};
    if(shutdown_between_check_and_publish) {
        shutdown_between_check_and_publish=false;
        manager.shutdown_state=APP_POWER_SHUTDOWN_PREPARING;
        assert(app_otg_set_enabled(false)==ESP_OK); /* Shutdown BEGIN interleaves. */
    }
    return ESP_OK;
}
void tg29_revoke_otg_source_permission(tg29_handle_t h)
{ outside_cache_lock(); assert(h==PMIC); ++revokes; cc_state.source_allowed=false; }
esp_err_t tg29_configure_cc_detection(tg29_handle_t h, bool enabled)
{
    outside_cache_lock(); assert(h==PMIC); ++configure_calls; last_configure=enabled;
    if(!enabled)cc_state.source_allowed=false;
    if(configure_error)return configure_error;
    if(enabled && cancel_in_configure) { cancel_in_configure=false; assert(app_otg_set_enabled(false)==ESP_OK); }
    cc_state.source_allowed=enabled;
    if(!enabled) {
        cc_state.boost_enabled=false; cc_state.module_enable_raw&=~0x10U; cc_state.vbus_voltage_valid=false;
        cc_state.rbfet_enabled=false;cc_state.rbfet_control_raw&=~1U;
        diagnostic_state.ts_config_raw=0x12;diagnostic_state.ts_voltage_valid=false;
    }
    return ESP_OK;
}
esp_err_t tg29_configure_manual_otg(tg29_handle_t h)
{
    ++manual_configure_calls;
    return tg29_configure_cc_detection(h, true);
}
esp_err_t tg29_get_usb_power_state(tg29_handle_t h, tg29_usb_power_state_t *s)
{ outside_cache_lock(); assert(h==PMIC && s); ++cc_reads; if(cc_error){memset(s,0,sizeof(*s));return cc_error;} *s=cc_state;return ESP_OK; }
esp_err_t tg29_set_otg_enabled(tg29_handle_t h, bool enabled)
{
    outside_cache_lock(); assert(h==PMIC); ++vbus_calls; last_vbus=enabled;
    if(vbus_error)return vbus_error;
    cc_state.boost_enabled=enabled; cc_state.module_enable_raw=enabled?0x11:1;
    cc_state.rbfet_enabled=enabled;cc_state.rbfet_control_raw=enabled?0xA1:0xA0;
    if(enabled) {diagnostic_state.ts_config_raw=0x1E;diagnostic_state.ts_voltage_valid=true;diagnostic_state.ts_half_mv=1000;}
    return ESP_OK;
}
esp_err_t tg29_verify_otg_output_ex(tg29_handle_t h, uint32_t timeout, tg29_otg_verify_result_t *s)
{
    outside_cache_lock(); assert(h==PMIC && s && timeout==2000 && last_vbus); ++verify_calls;
    *s=(tg29_otg_verify_result_t){.power=verify_state};
    if(cancel_in_verify) {
        cancel_in_verify=false; assert(app_otg_set_enabled(false)==ESP_OK);
        assert(!cc_state.source_allowed && atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF);
        s->verify_error=ESP_ERR_INVALID_STATE;cc_state.boost_enabled=false;cc_state.rbfet_enabled=false; return s->verify_error;
    }
    now_us+=120000;s->verify_error=verify_error;s->cleanup_error=verify_cleanup_error;
    if(verify_error!=ESP_OK) {
        if(failure_probe_enabled && verify_error==ESP_ERR_TIMEOUT) {
            s->pre_cleanup_diag_attempted=true;s->diag_error=verify_diagnostic_error;
            if(verify_diagnostic_error==ESP_OK) {
                s->pre_cleanup_diag_valid=true;s->pre_cleanup_diag=failure_diagnostic_state;
                if(!probe_power_override)s->pre_cleanup_diag.power=verify_state;
            }
        }
        if(verify_cleanup_error==ESP_OK) {cc_state.boost_enabled=false;cc_state.rbfet_enabled=false;cc_state.vbus_voltage_valid=false;}
        /* Simulate the driver's TS restore before returning, so an erroneous
         * post-OFF diagnostic read would expose0x12 instead of pre-OFF0x1E. */
        diagnostic_state.ts_config_raw=0x12;diagnostic_state.ts_voltage_valid=false;
    } else cc_state=verify_state;
    return verify_error; /* Cleanup errors are independent, never mask cause. */
}
esp_err_t tg29_settle_otg_control(tg29_handle_t h, uint32_t settle, tg29_otg_verify_result_t *s)
{
    outside_cache_lock();assert(h==PMIC && s && settle==100 && last_vbus);++settle_calls;
    *s=(tg29_otg_verify_result_t){.power=verify_state,.verify_error=verify_error,.cleanup_error=verify_cleanup_error};
    if(cancel_in_verify) {
        cancel_in_verify=false;assert(app_otg_set_enabled(false)==ESP_OK);
        s->verify_error=ESP_ERR_INVALID_STATE;cc_state.boost_enabled=false;cc_state.rbfet_enabled=false;return s->verify_error;
    }
    now_us+=100000;
    if(verify_error==ESP_OK)cc_state=verify_state;
    else if(verify_cleanup_error==ESP_OK){cc_state.boost_enabled=false;cc_state.rbfet_enabled=false;}
    return s->verify_error;
}
esp_err_t tg29_get_otg_diagnostics(tg29_handle_t h, tg29_otg_diagnostics_t *s)
{
    outside_cache_lock(); assert(h==PMIC && s); ++diagnostic_calls;
    if(diagnostic_error) { memset(s,0,sizeof(*s)); return diagnostic_error; }
    *s=diagnostic_state; s->power=cc_state; return ESP_OK;
}
esp_err_t aether_nes_set_external_input(aether_nes_handle_t n, uint16_t buttons)
{ outside_cache_lock(); assert(n==NES); ++input_calls; last_buttons=buttons; return input_error; }

static void fresh(void)
{
    assert(!critical_depth);
    s_otg=(app_otg_context_t){.source_mode=ATOMIC_VAR_INIT(OTG_SOURCE_OFF),.init_error=ESP_ERR_INVALID_STATE};
    board=(aether_board_t){.pmic=PMIC,.otg_vbus_adc_trusted=true}; manager=(app_power_t){0};
    callbacks=(aether_usb_host_config_t){0}; host_state=(aether_usb_host_status_t){0};
    cc_state=(tg29_usb_power_state_t){.snapshot_valid=true,.cc_detection_enabled=true,
        .attached_as_source=true,.cc_state=TG29_CC_ATTACH_SRC,.cc_raw=6,.module_enable_raw=1,.rbfet_control_raw=0xA0};
    verify_state=(tg29_usb_power_state_t){.snapshot_valid=true,.source_allowed=true,.cc_detection_enabled=true,
        .attached_as_source=true,.cc_state=TG29_CC_ATTACH_SRC,.cc_raw=6,.module_enable_raw=0x11,
        .boost_enabled=true,.rbfet_enabled=true,.rbfet_control_raw=0xA1,.adc_enable_raw=0x04,.vbus_voltage_valid=true,.vbus_mv=5120,.vbus_raw=5120};
    diagnostic_state=(tg29_otg_diagnostics_t){.ts_config_raw=0x12,.fault_raw=0x42,
        .ts_voltage_valid=true,.ts_half_mv=450};
    failure_diagnostic_state=(tg29_otg_diagnostics_t){.ts_config_raw=0x1E,.fault_raw=0x03,
        .ts_voltage_valid=true,.ts_half_mv=1000};
    failure_probe_enabled=true;probe_power_override=false;verify_diagnostic_error=verify_cleanup_error=ESP_OK;
    new_calls=commands=status_calls=waits=power_reads=0;
    keyboard_map_calls=0; keyboard_map_context=NULL;
    memset(keyboard_map_keys,0,sizeof(keyboard_map_keys));
    configure_calls=cc_reads=vbus_calls=input_calls=verify_calls=settle_calls=diagnostic_calls=revokes=0;
    manual_configure_calls=0;
    last_command=last_vbus=last_configure=cancel_in_verify=cancel_in_configure=false;
    last_buttons=0; last_timeout=0; now_us=0; shutdown_between_check_and_publish=false;
    new_error=command_error=status_error=wait_error=power_error=ESP_OK;
    configure_error=cc_error=vbus_error=input_error=verify_error=diagnostic_error=ESP_OK;
}
static void init(void)
{ const unsigned before=new_calls; assert(app_otg_init(&board,NES,&manager)==ESP_OK && new_calls==before+1); }
static void enable(void) { assert(app_otg_set_enabled(true)==ESP_OK); }
static unsigned hardware_calls(void) { return configure_calls+cc_reads+vbus_calls+verify_calls+settle_calls+diagnostic_calls; }
static app_otg_power_status_t power_snapshot(void)
{ app_otg_power_status_t s; assert(app_otg_get_power_status(&s)==ESP_OK); return s; }
static void start_output(void)
{
    bool sink=false; enable(); assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK && s_otg.boost_expected);
}

static void request_manual(void)
{
    board.otg_vbus_adc_trusted=false;
    init();
    cc_state.attached_as_source=false;
    cc_state.cc_state=TG29_CC_UNATTACH_SRC;
    cc_state.otg_role_ready=true;
    verify_state.attached_as_source=false;
    verify_state.cc_state=TG29_CC_UNATTACH_SRC;
    verify_state.otg_role_ready=true;
    assert(app_otg_set_manual_enabled(true)==ESP_OK && host_state.enabled && !hardware_calls());
    assert(power_snapshot().manual_source_requested);
}

/* Exercise cache locking against a concurrent GUI reader without any worker
 * I2C, lock nesting, or use of pointers into mutable driver storage. */
static void *cache_reader(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<10000;++i) {
        app_otg_power_status_t p; assert(app_otg_get_power_status(&p)==ESP_OK);
        if(p.valid)assert(p.vbus_mv==5120 || p.vbus_mv==5000);
    }
    return NULL;
}

static void *failure_cache_reader(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<10000;++i) {
        app_otg_power_status_t p=power_snapshot();
        assert(p.failure_snapshot && p.valid && p.diagnostics_valid && !p.output_verified);
        assert((p.vbus_mv==2100 && p.ts_mv==500 && p.ts_config==0x1E && p.cleanup_error==ESP_FAIL) ||
               (p.vbus_mv==1800 && p.ts_mv==400 && p.ts_config==0x1D && p.cleanup_error==ESP_OK));
    }
    return NULL;
}

int main(void)
{
    fresh();
    aether_usb_host_status_t snapshot; memset(&snapshot,0xA5,sizeof(snapshot));
    app_otg_power_status_t p; memset(&p,0xA5,sizeof(p));
    assert(app_otg_get_status(&snapshot)==ESP_ERR_INVALID_STATE && !snapshot.active);
    assert(app_otg_get_status(NULL)==ESP_ERR_INVALID_ARG);
    assert(app_otg_get_power_status(NULL)==ESP_ERR_INVALID_ARG);
    assert(app_otg_get_power_status(&p)==ESP_ERR_INVALID_STATE && !p.valid && !p.vbus_valid && p.age_ms==UINT32_MAX);
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(app_otg_disable_wait(1)==ESP_OK && !waits && !hardware_calls());
    assert(app_otg_init(NULL,NES,&manager)==ESP_ERR_INVALID_ARG);
    assert(app_otg_init(&board,NES,NULL)==ESP_ERR_INVALID_ARG);
    board.pmic=NULL; assert(app_otg_init(&board,NES,&manager)==ESP_ERR_INVALID_ARG); board.pmic=PMIC;
    init(); assert(!hardware_calls() && !input_calls && !commands && !power_reads && !revokes);
    p=power_snapshot();assert(p.vbus_adc_trusted && !p.valid && p.age_ms==UINT32_MAX && !hardware_calls());
    assert(app_otg_init(&board,NES,&manager)==ESP_ERR_INVALID_ARG && new_calls==1);
    enable(); assert(commands==1 && last_command && power_reads==2 && !hardware_calls());
    assert(app_otg_set_enabled(false)==ESP_OK && commands==2 && !last_command && revokes==1 && !hardware_calls());
    command_error=ESP_ERR_NO_MEM; assert(app_otg_set_enabled(true)==ESP_ERR_NO_MEM && commands==3 && !hardware_calls());
    assert(atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF && revokes==2);
    puts("PASS lazy init/default OFF performs no hardware work; command failures and OFF revoke atomically without I2C");

    fresh(); init(); enable(); bool sink=false;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink && configure_calls==1 && cc_reads==1 && diagnostic_calls==1);
    for(unsigned i=0;i<10;++i)assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    assert(configure_calls==1 && cc_reads==11 && diagnostic_calls==1 && !vbus_calls);
    cc_state.attached_as_source=false;cc_state.attached_as_sink=true;cc_state.cc_state=TG29_CC_ATTACH_SNK;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_state.attached_as_sink=false;cc_state.cc_state=(tg29_cc_state_t)15;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_state.attached_as_source=true;cc_state.cc_detection_enabled=false;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_state.cc_detection_enabled=true;cc_state.source_allowed=false;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_state.source_allowed=true;cc_state.attached_as_sink=true;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_error=ESP_FAIL;sink=true;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_FAIL && !sink && configure_calls==1);
    p=power_snapshot();assert(!p.valid && p.last_error==ESP_FAIL);
    puts("PASS fresh CC/policy checks reject charger/unknown/disabled/read errors; slow diagnostics are throttled separately");

    fresh(); init(); start_output();
    assert(verify_calls==1 && vbus_calls==1 && !verify_state.vbus_good); /* Input GOOD must not gate output proof. */
    p=power_snapshot();assert(p.valid && p.output_verified && p.boost_enabled && p.vbus_valid && p.vbus_mv==5120);
    assert(!p.diagnostics_valid && !p.ts_valid && p.control_raw==0x11);
    assert(p.adc_enable_raw==4 && p.vbus_raw==5120); /* Idle TS phase is hidden after startup. */
    const unsigned before_cache=hardware_calls(), before_status=status_calls;
    for(unsigned i=0;i<1000;++i) { p=power_snapshot();assert(p.output_verified); }
    assert(hardware_calls()==before_cache && status_calls==before_status);
    now_us+=2001000;p=power_snapshot();assert(!p.failure_snapshot && !p.valid && !p.vbus_valid && !p.output_verified && p.vbus_raw==5120);
    cc_state.boost_enabled=false;cc_state.module_enable_raw=1;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink);
    p=power_snapshot();assert(!p.output_verified && !p.boost_enabled && p.last_error==ESP_ERR_INVALID_STATE && p.control_raw==1);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    p=power_snapshot();assert(p.last_error==ESP_ERR_INVALID_STATE && !p.output_verified && p.vbus_mv==5120);
    puts("PASS startup requires measured VBUS; VBUSGOOD input bit is irrelevant; cached GUI reads perform zero I2C; boost-bit loss cannot remain active");

    fresh(); init(); enable(); verify_error=ESP_ERR_TIMEOUT; verify_state.vbus_mv=1200;verify_state.vbus_raw=1200;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT && !s_otg.boost_expected && verify_calls==1 && !settle_calls);
    p=power_snapshot();assert(p.valid && p.vbus_valid && p.vbus_mv==1200 && !p.output_verified && p.last_error==ESP_ERR_TIMEOUT);
    assert(p.failure_snapshot && p.diagnostics_valid && p.ts_config==0x1E && p.ts_valid && p.ts_mv==500);
    assert(!diagnostic_calls && p.verify_error==ESP_ERR_TIMEOUT && p.cleanup_error==ESP_OK && p.diagnostic_error==ESP_OK);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    p=power_snapshot();assert(p.vbus_mv==1200 && p.last_error==ESP_ERR_TIMEOUT && !p.output_verified);
    const unsigned retained_diagnostics=diagnostic_calls;
    for(unsigned i=0;i<50;++i) {
        now_us+=20000;assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
        p=power_snapshot();assert(p.vbus_mv==1200 && p.last_error==ESP_ERR_TIMEOUT && !p.output_verified && p.control_raw==0x11);
    }
    assert(diagnostic_calls==retained_diagnostics && p.age_ms==1000);
    now_us+=1001000;p=power_snapshot();assert(p.valid && p.vbus_valid && p.ts_valid && p.diagnostics_valid && p.failure_snapshot && !p.output_verified && p.vbus_mv==1200 && p.control_raw==0x11 && p.age_ms==2001);
    verify_error=ESP_OK;verify_state.vbus_mv=5120;verify_state.vbus_raw=5120;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK);
    p=power_snapshot();assert(p.output_verified && p.vbus_mv==5120 && p.last_error==ESP_OK);
    puts("PASS pre-cleanup power/R50/TS/fault stay coherent and historical after OFF/CC retries/TTL, until a new physical verification");

    fresh();init();enable();verify_error=ESP_ERR_TIMEOUT;verify_cleanup_error=ESP_FAIL;
    verify_state.vbus_mv=900;verify_state.vbus_raw=900;
    probe_power_override=true;failure_diagnostic_state.power=verify_state;
    failure_diagnostic_state.power.vbus_mv=2100;failure_diagnostic_state.power.vbus_raw=2100;
    failure_diagnostic_state.power.cc_raw=0x16;failure_diagnostic_state.power.module_enable_raw=0x31;
    failure_diagnostic_state.power.adc_enable_raw=0x06;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    p=power_snapshot();assert(p.failure_snapshot && p.diagnostics_valid && p.valid && p.vbus_mv==2100 && p.vbus_raw==2100);
    assert(p.cc_raw==0x16 && p.control_raw==0x31 && p.adc_enable_raw==0x06 && p.ts_config==0x1E && p.ts_mv==500);
    assert(p.verify_error==ESP_ERR_TIMEOUT && p.cleanup_error==ESP_FAIL && p.last_error==ESP_ERR_TIMEOUT);
    configure_error=ESP_ERR_TIMEOUT;assert(callbacks.set_vbus(callbacks.context,false)==ESP_ERR_TIMEOUT);
    p=power_snapshot();assert(p.valid && p.diagnostics_valid && p.verify_error==ESP_ERR_TIMEOUT && p.cleanup_error==ESP_ERR_TIMEOUT && p.vbus_mv==2100 && !p.output_verified);
    puts("PASS full probe publishes its own power/R50/TS atomically; original verify and cleanup failures stay separate even during OFF retries");

    fresh();init();enable();verify_error=ESP_ERR_TIMEOUT;verify_state.vbus_voltage_valid=false;verify_state.vbus_mv=0;verify_state.vbus_raw=0;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    p=power_snapshot();assert(p.valid && p.failure_snapshot && p.adc_enable_raw==4 && p.vbus_raw==0 && !p.vbus_valid && !p.output_verified);
    now_us+=9000000;p=power_snapshot();assert(p.valid && p.failure_snapshot && p.adc_enable_raw==4 && p.vbus_raw==0 && !p.vbus_valid);
    verify_state.adc_enable_raw=0;assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    p=power_snapshot();assert(p.valid && p.adc_enable_raw==0 && p.vbus_raw==0 && !p.vbus_valid);
    verify_state.adc_enable_raw=4;verify_state.vbus_raw=0x3FFF;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    p=power_snapshot();assert(p.valid && p.adc_enable_raw==4 && p.vbus_raw==0x3FFF && !p.vbus_valid);
    puts("PASS raw ADC zero/fullscale and disabled channel are distinguishable without inventing measured0V; historical flags survive live TTL");

    fresh();init();enable();assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    const unsigned before_no_probe=diagnostic_calls;
    verify_error=ESP_ERR_INVALID_STATE;failure_probe_enabled=false;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_INVALID_STATE);
    p=power_snapshot();assert(p.failure_snapshot && !p.diagnostics_valid && !p.ts_valid && diagnostic_calls==before_no_probe);
    assert(p.verify_error==ESP_ERR_INVALID_STATE && p.diagnostic_error==ESP_OK);
    puts("PASS cancel/role/error results without a full pre-cleanup probe never recycle idleR50 or trigger post-OFF diagnostics");

    fresh(); init(); start_output();
    cc_state.vbus_voltage_valid=false;cc_state.vbus_mv=0;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_RESPONSE && !sink);
    p=power_snapshot();assert(!p.vbus_valid && p.vbus_mv==5120 && !p.output_verified && p.last_error==ESP_ERR_INVALID_RESPONSE);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    assert(app_otg_set_enabled(false)==ESP_OK);enable();
    verify_error=ESP_FAIL;verify_state=(tg29_usb_power_state_t){0};
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_FAIL);
    p=power_snapshot();assert(!p.valid && !p.vbus_valid && p.vbus_mv==5120 && p.last_error==ESP_FAIL && !p.output_verified);
    puts("PASS disabled ADC/final-read failure is invalid, not zero; last real voltage is retained separately from validity");

    fresh(); init(); enable(); cancel_in_verify=true;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_INVALID_STATE && revokes==1 && !s_otg.boost_expected);
    p=power_snapshot();assert(!p.output_verified && !host_state.enabled);
    const unsigned before_cancel=configure_calls;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink && configure_calls==before_cancel);
    enable();cancel_in_configure=true;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink && !cc_state.source_allowed);
    assert(!host_state.enabled && revokes==3);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK && !s_otg.cc_configured);
    puts("PASS OFF cancels voltage verification promptly and prevents in-flight CC setup from reauthorizing source policy");

    fresh(); init(); enable();configure_error=ESP_FAIL;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_FAIL && !sink && !s_otg.cc_configured && !cc_reads);
    configure_error=ESP_OK;assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    vbus_error=ESP_ERR_INVALID_STATE;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_INVALID_STATE && !verify_calls);
    configure_error=ESP_FAIL;
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_FAIL && !last_configure && !s_otg.cc_configured && !cc_state.source_allowed);
    puts("PASS PMIC control errors skip physical verification; OFF always revokes CC policy and configuration latch even if checked bus operation fails");

    fresh(); init(); start_output();
    const unsigned before_diagnostics=diagnostic_calls;
    now_us=999999;assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink && diagnostic_calls==before_diagnostics);
    now_us=1000000;assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink && diagnostic_calls==before_diagnostics+1);
    diagnostic_error=ESP_FAIL;now_us=2000000;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_FAIL && !sink);
    p=power_snapshot();assert(!p.valid && !p.output_verified && p.last_error==ESP_FAIL && p.vbus_mv==5120);
    puts("PASS TS/fault reads run at most once per second, outside spinlock; active diagnostic bus failure clears verified status and propagates");

    fresh();init();start_output();now_us=1000000;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    p=power_snapshot();assert(p.diagnostics_valid && p.ts_valid && p.ts_config==0x1E);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    p=power_snapshot();assert(!p.failure_snapshot && !p.diagnostics_valid && !p.ts_valid);
    const unsigned before_phase_retry=diagnostic_calls;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    p=power_snapshot();assert(!p.diagnostics_valid && !p.ts_valid && p.control_raw==1);
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK);
    p=power_snapshot();assert(p.output_verified && !p.diagnostics_valid && !p.ts_valid && diagnostic_calls==before_phase_retry);
    puts("PASS rapid normal OFF/ON never attaches pre-restore TS/R50 to a new power phase when slow diagnostics are throttled");

    fresh(); init();start_output();
    now_us=2001000;cache_power(&s_otg,&verify_state,true,ESP_OK);
    p=power_snapshot();assert(p.valid && p.vbus_valid && !p.ts_valid && p.ts_mv==225);
    diagnostic_error=ESP_FAIL;verify_diagnostic_error=ESP_FAIL;verify_error=ESP_ERR_TIMEOUT;verify_state.vbus_mv=1500;verify_state.vbus_raw=1500;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    p=power_snapshot();assert(p.vbus_valid && p.vbus_mv==1500 && !p.ts_valid && !p.output_verified);
    assert(p.failure_snapshot && !p.diagnostics_valid && p.diagnostic_error==ESP_FAIL);
    puts("PASS failed/incomplete pre-cleanup probe hides R50/TS instead of mixing restored idle data with the last verification power sample");

    fresh(); init();shutdown_between_check_and_publish=true;
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(!host_state.enabled && atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF && revokes==2 && commands==3 && !hardware_calls());
    manager.shutdown_state=APP_POWER_SHUTDOWN_IDLE;enable();manager.shutdown_state=APP_POWER_SHUTDOWN_PREPARING;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink && !hardware_calls());
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_INVALID_STATE && !hardware_calls());
    puts("PASS shutdown BEGIN interleaving cannot be overwritten by ON: publication rolls back and worker rechecks before CC/boost");

    fresh();board.otg_vbus_adc_trusted=false;init();
    p=power_snapshot();assert(!p.vbus_adc_trusted && !p.valid && p.age_ms==UINT32_MAX && !hardware_calls());
    verify_state.adc_enable_raw=7;verify_state.vbus_raw=0x3FFC;verify_state.vbus_mv=16380;verify_state.vbus_voltage_valid=true;
    start_output();
    assert(settle_calls==1 && !verify_calls && s_otg.boost_expected);
    p=power_snapshot();assert(!p.vbus_adc_trusted && p.valid && p.boost_enabled && p.adc_enable_raw==7 && p.vbus_raw==0x3FFC);
    assert(!p.vbus_valid && !p.output_verified && p.vbus_mv==0 && !p.failure_snapshot);
    board.otg_vbus_adc_trusted=true; /* Runtime board mutation cannot change init policy. */
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    now_us=1000000;assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    p=power_snapshot();assert(!p.vbus_adc_trusted && p.diagnostics_valid && !p.vbus_valid && !p.output_verified && p.vbus_mv==0);
    assert(p.vbus_raw==0x3FFC && !verify_calls && settle_calls==1);
    cc_state.adc_enable_raw=0;cc_state.vbus_raw=0;cc_state.vbus_voltage_valid=false;cc_state.vbus_mv=0;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    p=power_snapshot();assert(p.valid && !p.vbus_valid && !p.output_verified && p.adc_enable_raw==0 && p.vbus_raw==0);
    puts("PASS immutable untrusted-ADC policy permits control-settled Host without ADC and never reports saturated raw3FFC as16V or verified5V");

    cc_state.source_allowed=false;assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_state.source_allowed=true;cc_state.cc_detection_enabled=false;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_state.cc_detection_enabled=true;cc_state.attached_as_source=false;cc_state.attached_as_sink=true;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    cc_state.attached_as_source=true;cc_state.attached_as_sink=false;cc_state.boost_enabled=false;cc_state.module_enable_raw=1;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink);
    p=power_snapshot();assert(p.failure_snapshot && !p.vbus_adc_trusted && !p.output_verified && !p.vbus_valid && p.verify_error==ESP_ERR_INVALID_STATE);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK && !s_otg.boost_expected && !cc_state.source_allowed);
    puts("PASS untrusted ADC bypasses only voltage gating; current policy/detection/CC and boost-control loss still stop sourcing");

    fresh();board.otg_vbus_adc_trusted=false;init();enable();cancel_in_verify=true;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_INVALID_STATE && settle_calls==1 && !verify_calls);
    p=power_snapshot();assert(!p.vbus_adc_trusted && !p.output_verified && !p.vbus_valid && p.failure_snapshot && !host_state.enabled);
    assert(!cc_state.source_allowed && atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    manager.shutdown_state=APP_POWER_SHUTDOWN_PREPARING;
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE && !host_state.enabled);
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_INVALID_STATE && settle_calls==1);
    puts("PASS OFF during untrusted control settle and shutdown retain immediate atomic cancellation and reject re-enable");

    fresh();board.otg_vbus_adc_trusted=false;init();enable();verify_error=ESP_FAIL;verify_cleanup_error=ESP_ERR_TIMEOUT;
    verify_state.vbus_mv=16380;verify_state.vbus_raw=0x3FFC;verify_state.adc_enable_raw=7;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_FAIL && settle_calls==1 && !verify_calls);
    p=power_snapshot();assert(p.failure_snapshot && p.verify_error==ESP_FAIL && p.cleanup_error==ESP_ERR_TIMEOUT);
    assert(!p.vbus_adc_trusted && !p.vbus_valid && !p.output_verified && p.vbus_mv==0 && p.vbus_raw==0x3FFC);
    puts("PASS untrusted control-settle failures preserve primary/cleanup errors and raw evidence without claiming measured voltage");

    for(unsigned trusted=0;trusted<2;++trusted) {
        fresh();board.otg_vbus_adc_trusted=trusted!=0;init();start_output();
        p=power_snapshot();assert(p.rbfet_enabled && p.rbfet_control_raw==0xA1 && p.boost_enabled);
        cc_state.rbfet_enabled=false;cc_state.rbfet_control_raw=0xA0;
        assert(cc_state.boost_enabled); /* BOOST alone cannot reach connector VBUS. */
        assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink);
        p=power_snapshot();assert(p.valid && p.failure_snapshot && !p.rbfet_enabled && p.rbfet_control_raw==0xA0);
        assert(p.boost_enabled && !p.output_verified && p.verify_error==ESP_ERR_INVALID_STATE && p.vbus_valid==(trusted!=0));
        assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK && !cc_state.rbfet_enabled && !cc_state.boost_enabled);
        now_us+=3000000;p=power_snapshot();assert(p.failure_snapshot && p.rbfet_control_raw==0xA0 && !p.rbfet_enabled && !p.output_verified);
    }
    puts("PASS trusted and untrusted ongoing monitors reject lost REG13 gate even while BOOST and voltage still look healthy; historical R13 survives OFF");

    for(unsigned trusted=0;trusted<2;++trusted) {
        fresh();board.otg_vbus_adc_trusted=trusted!=0;init();enable();
        verify_state.rbfet_enabled=false;verify_state.rbfet_control_raw=0xA0;
        assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_INVALID_STATE && !s_otg.boost_expected);
        assert(verify_calls==trusted && settle_calls==!trusted);
        p=power_snapshot();assert(p.valid && p.failure_snapshot && !p.rbfet_enabled && p.rbfet_control_raw==0xA0 && !p.output_verified);
        assert(p.verify_error==ESP_ERR_INVALID_STATE);
        assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    }
    puts("PASS both startup paths require fresh REG13 gate proof; an inconsistent helper success cannot enable the Host with gate OFF");

    fresh();init();enable();
    tg29_otg_verify_result_t gate_probe={.power=verify_state,.pre_cleanup_diag_valid=true,.verify_error=ESP_ERR_TIMEOUT};
    gate_probe.pre_cleanup_diag=failure_diagnostic_state;gate_probe.pre_cleanup_diag.power=verify_state;
    gate_probe.pre_cleanup_diag.power.rbfet_control_raw=0xB1;
    cache_failed_verification(&s_otg,&gate_probe);
    p=power_snapshot();assert(p.rbfet_enabled && p.rbfet_control_raw==0xB1 && p.diagnostics_valid && !p.output_verified);
    puts("PASS failure cache carries R13 from the same full pre-cleanup probe as R19/ADC/TS, preserving unrelated R13 bits");

    fresh(); init();
    uint8_t keys[32]={0};
    keys[0x52U/8U]|=1U<<(0x52U%8U); /* HID Up */
    keys[0x1DU/8U]|=1U<<(0x1DU%8U); /* HID Z */
    keys[31]=0xA5; /* Preserve bytes beyond the two mapped test usages. */
    const uint16_t mapped=callbacks.keyboard_map(callbacks.context,keys);
    assert(mapped==(AETHER_HID_UP|AETHER_HID_A) && keyboard_map_calls==1);
    assert(keyboard_map_context==callbacks.context && !memcmp(keyboard_map_keys,keys,sizeof(keys)));
    callbacks.input(callbacks.context,mapped);
    assert(input_calls==1 && last_buttons==mapped);
    memset(keys,0,sizeof(keys));
    assert(callbacks.keyboard_map(callbacks.context,keys)==0 && keyboard_map_calls==2);
    assert(!memcmp(keyboard_map_keys,keys,sizeof(keys)));
    assert(!hardware_calls() && !commands && !power_reads);
    puts("PASS Host config registers keyboard mapping and forwards context/full HID bitmap/mapped NES bits without NVS or hardware work");

    fresh(); init();
    const uint16_t inputs[]={0,AETHER_HID_UP|AETHER_HID_A,AETHER_HID_SELECT|AETHER_HID_START,AETHER_HID_EXIT,0};
    for(size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i){callbacks.input(callbacks.context,inputs[i]);assert(last_buttons==inputs[i] && input_calls==i+1);}
    assert(!hardware_calls() && !commands && !power_reads);
    input_error=ESP_FAIL;callbacks.input(callbacks.context,AETHER_HID_B);assert(last_buttons==AETHER_HID_B);
    s_otg.nes=NULL;const unsigned before_input=input_calls;callbacks.input(callbacks.context,0);assert(input_calls==before_input);
    puts("PASS HID masks/disconnect release forward only to NES, without panel wake or LVGL");

    fresh(); init();
    const app_power_shutdown_state_t states[]={APP_POWER_SHUTDOWN_IDLE,APP_POWER_SHUTDOWN_PREPARING,APP_POWER_SHUTDOWN_FAILED,APP_POWER_SHUTDOWN_REQUESTED};
    for(size_t i=0;i<sizeof(states)/sizeof(states[0]);++i){
        manager.shutdown_state=states[i];const unsigned old=commands;
        bool denied=states[i]==APP_POWER_SHUTDOWN_PREPARING||states[i]==APP_POWER_SHUTDOWN_REQUESTED;
        assert(app_otg_set_enabled(true)==(denied?ESP_ERR_INVALID_STATE:ESP_OK));assert(commands==old+!denied);
        assert(app_otg_set_enabled(false)==ESP_OK && !last_command && atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF);
    }
    power_error=ESP_FAIL;const unsigned before_unknown=commands;
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE && commands==before_unknown);
    assert(app_otg_set_enabled(false)==ESP_OK && commands==before_unknown+1);
    wait_error=ESP_ERR_TIMEOUT;assert(app_otg_disable_wait(123)==ESP_ERR_TIMEOUT && waits==1 && last_timeout==123);
    wait_error=ESP_OK;assert(app_otg_disable_wait(456)==ESP_OK && waits==2 && last_timeout==456);
    manager.shutdown_state=APP_POWER_SHUTDOWN_FAILED;manager.shutdown_cancel_error=ESP_ERR_TIMEOUT;
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(app_otg_set_enabled(false)==ESP_OK);
    manager.shutdown_cancel_error=ESP_OK;
    puts("PASS shutdown rejects ON, always accepts OFF; disable_wait publishes/revokes before waiting and propagates timeout");

    fresh(); init(); enable();cache_power(&s_otg,&verify_state,true,ESP_OK);
    pthread_t reader;assert(!pthread_create(&reader,NULL,cache_reader,NULL));
    for(unsigned i=0;i<10000;++i){tg29_usb_power_state_t sample=verify_state;sample.vbus_mv=(i&1)?5000:5120;cache_power(&s_otg,&sample,true,ESP_OK);}
    assert(!pthread_join(reader,NULL) && !hardware_calls());
    puts("PASS concurrent GUI reads and worker snapshots are copied under a short cache lock, with no hardware calls");

    fresh();init();enable();
    tg29_otg_verify_result_t reports[2]={
        {.pre_cleanup_diag_valid=true,.verify_error=ESP_ERR_TIMEOUT,.cleanup_error=ESP_FAIL},
        {.pre_cleanup_diag_valid=true,.verify_error=ESP_ERR_TIMEOUT},
    };
    for(unsigned i=0;i<2;++i) {
        reports[i].pre_cleanup_diag=failure_diagnostic_state;
        reports[i].pre_cleanup_diag.power=verify_state;
    }
    reports[0].pre_cleanup_diag.power.vbus_mv=2100;reports[0].pre_cleanup_diag.ts_half_mv=1000;
    reports[1].pre_cleanup_diag.power.vbus_mv=1800;reports[1].pre_cleanup_diag.ts_half_mv=800;reports[1].pre_cleanup_diag.ts_config_raw=0x1D;
    cache_failed_verification(&s_otg,&reports[0]);
    assert(!pthread_create(&reader,NULL,failure_cache_reader,NULL));
    for(unsigned i=0;i<10000;++i)cache_failed_verification(&s_otg,&reports[i&1]);
    assert(!pthread_join(reader,NULL) && !hardware_calls());
    puts("PASS concurrent GUI reads see whole pre-cleanup failure epochs, never power/TS/error combinations from different probes");

    fresh();new_error=ESP_ERR_NO_MEM;
    assert(app_otg_init(&board,NES,&manager)==ESP_ERR_NO_MEM && !hardware_calls());
    assert(app_otg_set_enabled(false)==ESP_ERR_NO_MEM);
    assert(app_otg_disable_wait(0)==ESP_OK && !waits);
    new_error=ESP_OK; revokes=0; init();
    puts("PASS failed lazy allocation is safe and retryable without PMIC/PHY hardware side effects");

    fresh(); request_manual();
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE && commands==1);
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink && manual_configure_calls==1);
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK && verify_calls==1 && !settle_calls);
    p=power_snapshot();
    assert(p.manual_source_requested && p.output_verified && p.vbus_valid && p.vbus_mv==5120);
    assert(app_otg_set_manual_enabled(false)==ESP_OK && !power_snapshot().manual_source_requested);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK && !cc_state.boost_enabled && !cc_state.rbfet_enabled);
    p=power_snapshot(); assert(!p.valid && !p.vbus_valid && !p.boost_enabled && !p.rbfet_enabled);
    puts("PASS explicit CC-less session requires measured VBUS even after a battery-only boot; OFF clears both gates");

    for(unsigned sink_role=0;sink_role<2;++sink_role) {
        fresh(); request_manual();
        cc_state.vbus_good=!sink_role;
        cc_state.attached_as_sink=sink_role;
        assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink);
        p=power_snapshot();
        assert(!host_state.enabled && !p.manual_source_requested && p.manual_source_failed && !vbus_calls);
        assert(p.last_error==ESP_ERR_INVALID_STATE);
        cc_state.vbus_good=false; cc_state.attached_as_sink=false;
        assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink && !vbus_calls);
    }
    puts("PASS external input or CC sink role cancels manual ON without boosting or retry after input removal");

    fresh(); request_manual();
    cc_state.otg_role_ready=false;
    cc_state.cc_state=TG29_CC_ERROR_RECOVERY;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink && host_state.enabled);
    now_us+=OTG_MANUAL_ROLE_TIMEOUT_US;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_TIMEOUT && !sink && !host_state.enabled);
    assert(power_snapshot().manual_source_failed && !vbus_calls);
    puts("PASS manual request releases the USB host reservation after a persistent unsuitable CC state");

    fresh(); request_manual();
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    verify_error=ESP_ERR_TIMEOUT;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT && !host_state.enabled && !settle_calls);
    p=power_snapshot();
    assert(p.manual_source_failed && !p.manual_source_requested && p.verify_error==ESP_ERR_TIMEOUT && !p.output_verified);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    assert(app_otg_set_manual_enabled(true)==ESP_OK && !power_snapshot().manual_source_failed);
    puts("PASS manual voltage failure releases the request and requires a fresh user ON; no control-only fallback");

    fresh(); request_manual();
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK);
    cc_state.attached_as_sink=true;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink && !host_state.enabled);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK && !cc_state.boost_enabled);
    assert(!power_snapshot().output_verified && power_snapshot().manual_source_failed);
    puts("PASS later CC sink detection terminates and cleans up an active manual session");

    fresh(); request_manual();
    host_state.last_error=ESP_ERR_NO_MEM;
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK && !host_state.enabled);
    p=power_snapshot(); assert(p.manual_source_failed && p.last_error==ESP_ERR_NO_MEM && !p.manual_source_requested);
    host_state.state=AETHER_OTG_STOPPING; host_state.cleanup_pending=true;
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_INVALID_STATE && app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE);
    puts("PASS failed USB backend cancels manual session; either mode waits for physical teardown before starting");
    return 0;
}

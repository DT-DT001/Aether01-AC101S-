/* Real application bridge / real TG29 and host public types. The fakes only
 * replace hardware, host scheduling and time. No LVGL or panel-wake symbols. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "stubs/app_usb_input.h"
#define TEST_OTG_CAPTURE_LOGS 1
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
static bool off_during_host_on;
static unsigned external_acquires, external_reads, external_releases;
static uint32_t external_sequence, external_owner;
static esp_err_t external_acquire_error, external_read_error, external_release_error;
static bool external_failed_owner, off_during_external_acquire;
static char phase_logs[8192];
static unsigned phase_log_count;
static bool recover_cc_on_manual_configure;
static _Thread_local unsigned critical_depth;
#define HOST ((aether_usb_host_handle_t)&host_token)
#define PMIC ((tg29_handle_t)&pmic_token)
#define NES ((aether_nes_handle_t)&nes_token)

void test_enter(portMUX_TYPE *lock) { assert(!critical_depth); assert(!pthread_mutex_lock(lock)); ++critical_depth; }
void test_exit(portMUX_TYPE *lock) { assert(critical_depth==1); --critical_depth; assert(!pthread_mutex_unlock(lock)); }
int64_t esp_timer_get_time(void) { return now_us; }
static void outside_cache_lock(void) { assert(!critical_depth); }
void test_otg_log(const char *level,const char *tag,const char *format,...)
{
    outside_cache_lock();assert(level && tag && format);++phase_log_count;
    const size_t used=strlen(phase_logs);assert(used<sizeof(phase_logs)-1);
    va_list args;va_start(args,format);
    const int count=vsnprintf(phase_logs+used,sizeof(phase_logs)-used,format,args);
    va_end(args);assert(count>=0 && (size_t)count<sizeof(phase_logs)-used);
    const size_t end=strlen(phase_logs);assert(end+1<sizeof(phase_logs));phase_logs[end]='\n';phase_logs[end+1]=0;
}

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
    if(enabled && off_during_host_on) {
        off_during_host_on=false;
        assert(app_otg_set_enabled(false)==ESP_OK);
    }
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
    cc_state.cc_detection_enabled=true;
    cc_state.cc_detection_raw=0x18;cc_state.cc_mode_raw=enabled ? 0x13 : 0x11;
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
    const esp_err_t error=tg29_configure_cc_detection(h, true);
    if(error==ESP_OK) {
        cc_state.cc_detection_enabled=false;cc_state.cc_detection_raw&=~8U;
        cc_state.cc_mode=TG29_CC_DISABLED;cc_state.cc_mode_raw=0x10;
    }
    if(error==ESP_OK && recover_cc_on_manual_configure) {
        recover_cc_on_manual_configure=false;cc_state.cc_raw=0x40;
        cc_state.cc_state=TG29_CC_STATE_DISABLED;cc_state.otg_role_ready=true;
    }
    return error;
}
esp_err_t tg29_get_usb_power_state(tg29_handle_t h, tg29_usb_power_state_t *s)
{ outside_cache_lock(); assert(h==PMIC && s); ++cc_reads; if(cc_error){memset(s,0,sizeof(*s));return cc_error;} *s=cc_state;return ESP_OK; }
esp_err_t tg29_vbus_input_acquire(tg29_handle_t h,uint32_t *token)
{
    outside_cache_lock();assert(h==PMIC && token);++external_acquires;*token=0;
    if(external_owner)return ESP_ERR_INVALID_STATE;
    if(external_acquire_error==ESP_OK || external_failed_owner) {
        external_owner=++external_sequence;*token=external_owner;
    }
    if(off_during_external_acquire) {off_during_external_acquire=false;assert(app_otg_set_enabled(false)==ESP_OK);}
    return external_acquire_error;
}
esp_err_t tg29_vbus_input_read(tg29_handle_t h,uint32_t token,tg29_usb_power_state_t *s)
{
    outside_cache_lock();assert(h==PMIC && s);++external_reads;memset(s,0,sizeof(*s));
    if(!token || token!=external_owner)return ESP_ERR_INVALID_STATE;
    if(external_read_error)return external_read_error;
    *s=cc_state;return ESP_OK;
}
esp_err_t tg29_vbus_input_release(tg29_handle_t h,uint32_t token)
{
    outside_cache_lock();assert(h==PMIC);++external_releases;
    if(!token || token!=external_owner)return ESP_ERR_INVALID_STATE;
    if(external_release_error)return external_release_error;
    external_owner=0;return ESP_OK;
}
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
    off_during_host_on=false;
    external_acquires=external_reads=external_releases=0;external_sequence=external_owner=0;
    external_acquire_error=external_read_error=external_release_error=ESP_OK;
    external_failed_owner=off_during_external_acquire=false;
    phase_logs[0]=0;phase_log_count=0;
    recover_cc_on_manual_configure=false;
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
    verify_state.cc_detection_enabled=false;
    verify_state.cc_detection_raw=0x10;verify_state.cc_mode_raw=0x10;verify_state.cc_mode=TG29_CC_DISABLED;
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
    command_error=ESP_ERR_NO_MEM; assert(app_otg_set_enabled(true)==ESP_ERR_NO_MEM && commands==4 && !hardware_calls());
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
    assert(!host_state.enabled && atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF && revokes==1 && commands==1 && !hardware_calls());
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
    puts("PASS external input or an inconsistent sink snapshot cancels manual ON without boosting or retry after input removal");

    fresh(); request_manual();
    cc_state.otg_role_ready=false;
    cc_state.cc_state=TG29_CC_ERROR_RECOVERY;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink && host_state.enabled);
    now_us+=OTG_MANUAL_ROLE_TIMEOUT_US;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_TIMEOUT && !sink && !host_state.enabled);
    assert(power_snapshot().manual_source_failed && !vbus_calls);
    puts("PASS manual request releases the USB host reservation after a persistent unverified manual policy");

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
    puts("PASS an inconsistent sink snapshot terminates and cleans up an active manual session");

    fresh(); request_manual();
    host_state.last_error=ESP_ERR_NO_MEM;
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK && !host_state.enabled);
    p=power_snapshot(); assert(p.manual_source_failed && p.last_error==ESP_ERR_NO_MEM && !p.manual_source_requested);
    host_state.state=AETHER_OTG_STOPPING; host_state.cleanup_pending=true;
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_INVALID_STATE && app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE);
    puts("PASS failed USB backend cancels manual session; either mode waits for physical teardown before starting");

    fresh();
    uint32_t lease=99, other=99;
    assert(app_otg_manual_lease_acquire(NULL)==ESP_ERR_INVALID_ARG);
    assert(app_otg_manual_lease_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease);
    assert(!app_otg_manual_lease_valid(0));
    assert(app_otg_manual_lease_release(0)==ESP_ERR_INVALID_ARG);
    init();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK && lease && host_state.enabled);
    assert(app_otg_manual_lease_valid(lease) && !hardware_calls() && commands==1);
    assert(!power_snapshot().output_verified); /* Admission is not 5V proof. */
    assert(app_otg_manual_lease_acquire(&other)==ESP_ERR_INVALID_STATE && !other);
    assert(app_otg_manual_lease_release(lease)==ESP_OK && !host_state.enabled);
    assert(!app_otg_manual_lease_valid(lease) && commands==2);
    assert(app_otg_manual_lease_release(lease)==ESP_ERR_INVALID_STATE && commands==2);
    puts("PASS accessory lease starts manual power asynchronously and releases only its owned session; admission never claims verified5V");

    fresh();request_manual();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK && lease && commands==1);
    assert(app_otg_manual_lease_valid(lease));
    assert(app_otg_manual_lease_release(lease)==ESP_OK && host_state.enabled && commands==1);
    assert(power_snapshot().manual_source_requested && !app_otg_manual_lease_valid(lease));
    assert(app_otg_set_manual_enabled(false)==ESP_OK);
    puts("PASS borrowed manual session remains user-owned and ON after accessory release");

    fresh();init();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK);
    assert(app_otg_set_enabled(false)==ESP_OK && !app_otg_manual_lease_valid(lease));
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    assert(app_otg_set_manual_enabled(true)==ESP_OK && host_state.enabled);
    const unsigned new_session_commands=commands;
    assert(app_otg_manual_lease_release(lease)==ESP_ERR_INVALID_STATE && host_state.enabled && commands==new_session_commands);
    assert(app_otg_manual_lease_acquire(&other)==ESP_OK && other && other!=lease);
    assert(app_otg_manual_lease_release(lease)==ESP_ERR_INVALID_STATE && app_otg_manual_lease_valid(other));
    assert(app_otg_manual_lease_release(other)==ESP_OK && host_state.enabled && commands==new_session_commands);
    puts("PASS forced OFF invalidates old tokens; stale release cannot stop a restarted user session or invalidate its newer lease");

    fresh();init();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK);
    assert(app_otg_set_manual_enabled(true)==ESP_OK); /* Deliberate user adoption. */
    const unsigned adopted_commands=commands;
    assert(app_otg_manual_lease_release(lease)==ESP_OK && host_state.enabled && commands==adopted_commands);
    puts("PASS explicit user manual ON adopts accessory-owned power so later accessory release does not turn it off");

    fresh();init();enable();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease && commands==1);
    assert(atomic_load(&s_otg.source_mode)==OTG_SOURCE_AUTO);
    assert(app_otg_set_enabled(false)==ESP_OK);
    host_state.cleanup_pending=true;host_state.state=AETHER_OTG_STOPPING;
    assert(app_otg_manual_lease_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease);
    assert(!power_snapshot().manual_source_requested);
    puts("PASS accessory never takes over normal AUTO OTG or physical cleanup");

    fresh();init();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK);
    cc_state.attached_as_sink=true;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink);
    assert(!app_otg_manual_lease_valid(lease) && !host_state.enabled);
    const unsigned fault_commands=commands;
    assert(app_otg_manual_lease_release(lease)==ESP_ERR_INVALID_STATE && commands==fault_commands);
    puts("PASS manual worker faults immediately invalidate the accessory lease and never trigger automatic re-enable");

    fresh();init();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK);
    assert(command_begin(&s_otg));
    assert(app_otg_manual_lease_acquire(&other)==ESP_ERR_NOT_FINISHED && !other);
    assert(app_otg_manual_lease_release(lease)==ESP_ERR_NOT_FINISHED && app_otg_manual_lease_valid(lease));
    assert(app_otg_set_manual_enabled(false)==ESP_OK && !app_otg_manual_lease_valid(lease));
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_NOT_FINISHED);
    assert(command_end(&s_otg)==ESP_OK && !host_state.enabled && !atomic_load(&s_otg.stop_pending));
    assert(!hardware_calls());
    puts("PASS command contention never blocks GUI; OFF revokes immediately and retained Host OFF drains before a new command");

    fresh();init();off_during_host_on=true;
    assert(app_otg_manual_lease_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease);
    assert(!host_state.enabled && !atomic_load(&s_otg.command_busy) && !atomic_load(&s_otg.stop_pending));
    assert(atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF && !hardware_calls());
    assert(app_otg_set_manual_enabled(true)==ESP_OK && host_state.enabled);
    puts("PASS OFF interleaved during Host ON publication cancels the command and cannot resurrect power; later explicit user ON remains possible");

    fresh();init();shutdown_between_check_and_publish=true;
    assert(app_otg_manual_lease_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease);
    assert(!host_state.enabled && !app_otg_manual_lease_valid(lease));
    puts("PASS shutdown BEGIN during lease admission cannot publish an accessory token or manual source");

    fresh();init();
    app_otg_input_status_t input;
    cc_state=(tg29_usb_power_state_t){.snapshot_valid=true,.vbus_good=true,
        .vbus_voltage_valid=true,.vbus_mv=5100};
    assert(app_otg_external_input_acquire(&lease)==ESP_OK && lease && external_acquires==1);
    assert(!commands && !hardware_calls() && !host_state.enabled && !cc_state.source_allowed);
    assert(app_otg_external_input_read(lease,&input)==ESP_OK && input.valid && input.present && input.voltage_valid && input.voltage_mv==5100);
    cc_state.vbus_voltage_valid=false;cc_state.vbus_mv=0x3FFC;
    assert(app_otg_external_input_read(lease,&input)==ESP_OK && input.valid && input.present && !input.voltage_valid && !input.voltage_mv);
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE && !commands);
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_INVALID_STATE && !commands);
    assert(app_otg_manual_lease_acquire(&other)==ESP_ERR_INVALID_STATE && !other && !commands);
    assert(app_otg_external_input_acquire(&other)==ESP_ERR_INVALID_STATE && !other && external_acquires==1);
    assert(app_otg_external_input_release(lease)==ESP_OK && !commands && !hardware_calls());
    assert(app_otg_set_manual_enabled(true)==ESP_OK && host_state.enabled);
    puts("PASS external input lease performs only worker ADC operations, exposes readiness separately, blocks both source modes and leaves Host/mux untouched");

    fresh();init();
    assert(app_otg_external_input_acquire(&lease)==ESP_OK);
    assert(app_otg_set_enabled(false)==ESP_OK && s_otg.input_token==lease && external_owner==lease);
    assert(app_otg_external_input_read(lease,&input)==ESP_OK && external_reads==1);
    assert(app_otg_external_input_release(lease)==ESP_OK && !external_owner);
    assert(app_otg_external_input_acquire(&other)==ESP_OK && other!=lease);
    unsigned old_release_calls=external_releases;
    assert(app_otg_external_input_release(lease)==ESP_ERR_INVALID_STATE && external_releases==old_release_calls && external_owner==other);
    assert(app_otg_external_input_read(lease,&input)==ESP_ERR_INVALID_STATE && !input.valid);
    assert(app_otg_external_input_release(other)==ESP_OK);
    puts("PASS ordinary source OFF preserves external measurement owner; stale external tokens cannot read or release a replacement lease");

    fresh();request_manual();
    assert(app_otg_external_input_acquire(&lease)==ESP_ERR_NOT_FOUND && !lease && !external_acquires);
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK && app_otg_manual_lease_valid(lease));
    assert(app_otg_manual_lease_release(lease)==ESP_OK && host_state.enabled);
    fresh();init();enable();
    assert(app_otg_external_input_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease && !external_acquires);
    fresh();init();host_state.cleanup_pending=true;host_state.state=AETHER_OTG_STOPPING;
    assert(app_otg_external_input_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease && !external_acquires);
    puts("PASS external admission explicitly permits manual-user borrowing fallback but never takes over AUTO or incomplete Host cleanup");

    const esp_err_t input_errors[]={ESP_ERR_NOT_FOUND,ESP_FAIL,ESP_ERR_TIMEOUT,ESP_ERR_INVALID_RESPONSE,ESP_ERR_INVALID_STATE};
    for(unsigned i=0;i<sizeof(input_errors)/sizeof(input_errors[0]);++i) {
        fresh();init();external_acquire_error=input_errors[i];
        assert(app_otg_external_input_acquire(&lease)==input_errors[i] && !lease && external_acquires==1 && !commands && !hardware_calls());
    }
    fresh();init();status_error=ESP_FAIL;
    assert(app_otg_external_input_acquire(&lease)==ESP_FAIL && !lease && !external_acquires);
    fresh();init();power_error=ESP_FAIL;
    assert(app_otg_external_input_acquire(&lease)==ESP_FAIL && !lease && !external_acquires);
    puts("PASS only explicit NOT_FOUND authorizes external-not-applicable handling; PMIC/status/power errors propagate without starting source or Host");

    fresh();init();external_acquire_error=ESP_FAIL;external_failed_owner=true;
    assert(app_otg_external_input_acquire(&lease)==ESP_FAIL && lease && s_otg.input_token==lease);
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_INVALID_STATE && !commands);
    external_release_error=ESP_FAIL;
    assert(app_otg_external_input_release(lease)==ESP_FAIL && s_otg.input_token==lease && external_owner==lease);
    assert(app_otg_set_enabled(true)==ESP_ERR_INVALID_STATE && !commands);
    external_release_error=ESP_OK;
    assert(app_otg_external_input_release(lease)==ESP_OK && !s_otg.input_token && !external_owner);
    assert(app_otg_set_manual_enabled(true)==ESP_OK && host_state.enabled);
    puts("PASS failed external acquire publishes retained cleanup token; failed release blocks source until a successful retry without losing owner");

    fresh();init();assert(app_otg_external_input_acquire(&lease)==ESP_OK);
    external_read_error=ESP_FAIL;memset(&input,0xA5,sizeof(input));
    assert(app_otg_external_input_read(lease,&input)==ESP_FAIL && !input.valid && !input.present && !input.voltage_valid && !input.voltage_mv);
    assert(external_owner==lease && !commands);
    assert(app_otg_external_input_release(lease)==ESP_OK);
    fresh();init();off_during_external_acquire=true;
    assert(app_otg_external_input_acquire(&lease)==ESP_OK && lease && s_otg.input_token==lease && !host_state.enabled);
    assert(!atomic_load(&s_otg.command_busy) && !atomic_load(&s_otg.stop_pending));
    assert(app_otg_external_input_release(lease)==ESP_OK);
    puts("PASS external read faults clear status but retain cleanup ownership; a concurrent source OFF neither steals nor clears the independent ADC lease");

    fresh();init();assert(command_begin(&s_otg));
    assert(app_otg_external_input_acquire(&lease)==ESP_ERR_NOT_FINISHED && !lease);
    assert(app_otg_external_input_read(1,&input)==ESP_ERR_NOT_FINISHED && !input.valid);
    assert(app_otg_external_input_release(1)==ESP_ERR_NOT_FINISHED);
    assert(command_end(&s_otg)==ESP_OK);
    assert(app_otg_external_input_acquire(NULL)==ESP_ERR_INVALID_ARG);
    assert(app_otg_external_input_read(0,&input)==ESP_ERR_INVALID_ARG);
    assert(app_otg_external_input_read(1,NULL)==ESP_ERR_INVALID_ARG);
    assert(app_otg_external_input_release(0)==ESP_ERR_INVALID_ARG);
    fresh();init();manager.shutdown_state=APP_POWER_SHUTDOWN_PREPARING;
    assert(app_otg_external_input_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease && !external_acquires);
    puts("PASS external worker commands share nonblocking admission, reject invalid arguments and cannot acquire power during shutdown");

    fresh();request_manual();
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    assert(phase_log_count==1 && strstr(phase_logs,"phase=configure_cc"));
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK);
    assert(phase_log_count==2 && strstr(phase_logs,"phase=verify_output"));
    const unsigned normal_diagnostics=diagnostic_calls;
    for(unsigned i=0;i<5;++i)assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    assert(phase_log_count==2 && diagnostic_calls==normal_diagnostics && verify_calls==1 && vbus_calls==1);
    puts("PASS manual configuration and output proof log once per attempt; ordinary role polling adds no phase logs or diagnostic reads");

    fresh();request_manual();cc_state.otg_role_ready=false;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink);
    now_us=1000001;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_TIMEOUT && !sink);
    assert(phase_log_count==2 && strstr(phase_logs,"phase=role_wait") && strstr(phase_logs,"verify=107"));
    assert(!verify_calls && !vbus_calls);
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink && phase_log_count==2);
    fresh();request_manual();configure_error=ESP_FAIL;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_FAIL);
    assert(phase_log_count==1 && strstr(phase_logs,"phase=configure_cc") && strstr(phase_logs,"snapshot=0"));
    assert(!cc_reads && !diagnostic_calls && !verify_calls && !vbus_calls);
    puts("PASS manual role timeout is distinguished from CC configuration failure without adding a PMIC snapshot or repeating after cancellation");

    fresh();request_manual();
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    const unsigned enable_hardware=hardware_calls();
    vbus_error=ESP_ERR_TIMEOUT;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    assert(phase_log_count==2 && strstr(phase_logs,"phase=enable_output") && !strstr(phase_logs,"phase=verify_output"));
    assert(strstr(phase_logs,"diag_valid=0") && !verify_calls && hardware_calls()==enable_hardware+1);
    puts("PASS failed checked enable has its own phase and explicitly unavailable snapshot; logging performs no extra PMIC I/O");

    fresh();request_manual();
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    probe_power_override=true;verify_error=ESP_ERR_TIMEOUT;verify_cleanup_error=ESP_FAIL;
    failure_diagnostic_state.power=(tg29_usb_power_state_t){.snapshot_valid=true,
        .cc_raw=0xA6,.cc_detection_raw=0xB8,.cc_mode_raw=0xD3,.module_enable_raw=0x71,.rbfet_control_raw=0xB1,
        .adc_enable_raw=0x97,.vbus_raw=0x3FFC,.vbus_voltage_valid=false};
    const unsigned verify_hardware=hardware_calls();
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    const char *failed_log=strstr(phase_logs,"phase=verify_output");assert(failed_log);
    assert(strstr(failed_log,"CC=A6 R19=71 R13=B1 C0=97 VBUS_raw=3FFC VBUS_mv=0 voltage_valid=0"));
    assert(strstr(failed_log,"R0B=B8 RE3=D3 CC=A6"));
    assert(power_snapshot().cc_detection_raw==0xB8 && power_snapshot().cc_mode_raw==0xD3);
    assert(strstr(failed_log,"diag_attempted=1 diag_valid=1 R50=1E TS_half_mv=1000 TS_valid=1 work_fault=03"));
    assert(strstr(failed_log,"verify=107 cleanup=FFFFFFFF diag=0"));
    assert(phase_log_count==2 && hardware_calls()==verify_hardware+2);
    assert(diagnostic_state.ts_config_raw==0x12); /* Actual cleanup changed live state. */
    puts("PASS verification failure logs its complete pre-cleanup phase and independent errors, never restored TS or an invented endpoint voltage");

    fresh();request_manual();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK);
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    verify_error=ESP_ERR_TIMEOUT;
    assert(callbacks.set_vbus(callbacks.context,true)==ESP_ERR_TIMEOUT);
    assert(!host_state.enabled && !app_otg_manual_lease_valid(lease));
    assert(atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF && !s_otg.lease_token && !s_otg.input_token);
    host_state.state=AETHER_OTG_STOPPING;host_state.cleanup_pending=true;
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(app_otg_manual_lease_acquire(&other)==ESP_ERR_INVALID_STATE && !other);
    assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
    /* The bridge fake does not run Host's worker. Publish its completed drain. */
    host_state=(aether_usb_host_status_t){.state=AETHER_OTG_OFF};
    assert(app_otg_manual_lease_acquire(&other)==ESP_OK && other && other!=lease);
    const unsigned restarted_commands=commands;
    assert(app_otg_manual_lease_release(lease)==ESP_ERR_INVALID_STATE && commands==restarted_commands);
    assert(host_state.enabled && app_otg_manual_lease_valid(other));
    assert(!power_snapshot().manual_source_failed);
    puts("PASS manual physical-output fault retains no bridge token; incomplete teardown rejects restart, completed Host OFF admits it, and stale failed-session release cannot stop the replacement");

    fresh();init();
    host_state=(aether_usb_host_status_t){.state=AETHER_OTG_ERROR,.last_error=ESP_ERR_TIMEOUT};
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_INVALID_STATE && !commands);
    assert(app_otg_manual_lease_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease);
    assert(app_otg_external_input_acquire(&lease)==ESP_ERR_INVALID_STATE && !lease);
    /* Characterization only: production Host never assigns ERROR. Its worker
     * normalizes actual failure/cleanup to OFF, which all admissions accept. */
    host_state=(aether_usb_host_status_t){.state=AETHER_OTG_OFF};
    assert(app_otg_set_manual_enabled(true)==ESP_OK && host_state.enabled);
    puts("PASS synthetic drained ERROR is rejected, while physical completion published as OFF is restartable; this fake ERROR is not a production Host failure transition");

    fresh();init();
    assert(app_otg_manual_lease_acquire(&lease)==ESP_OK);
    command_error=ESP_FAIL;
    assert(app_otg_manual_lease_release(lease)==ESP_FAIL && !app_otg_manual_lease_valid(lease));
    assert(atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF && !s_otg.lease_token && !cc_state.source_allowed);
    const unsigned failed_stop_commands=commands;
    assert(app_otg_manual_lease_release(lease)==ESP_ERR_INVALID_STATE && commands==failed_stop_commands);
    assert(app_otg_set_manual_enabled(true)==ESP_ERR_INVALID_STATE && host_state.enabled);
    command_error=ESP_OK;
    assert(app_otg_set_manual_enabled(false)==ESP_OK && !host_state.enabled);
    assert(app_otg_set_manual_enabled(true)==ESP_OK && host_state.enabled);
    puts("PASS failed Host OFF publication revokes the manual lease permanently; stale release does not retry or stop a new session, while an explicit successful OFF permits restart");

    fresh();request_manual();
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    const uint32_t configured_epoch=s_otg.configured_source_epoch;
    const unsigned first_configures=manual_configure_calls;
    for(unsigned i=0;i<4;++i)assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    assert(manual_configure_calls==first_configures);
    cc_state.cc_raw=0x40;cc_state.cc_state=TG29_CC_STATE_DISABLED;cc_state.otg_role_ready=false;
    now_us=1000001;
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_TIMEOUT && !sink);
    assert(atomic_load(&s_otg.source_mode)==OTG_SOURCE_OFF && !host_state.enabled);
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && !sink && manual_configure_calls==first_configures);
    /* Preserve an old worker cache across a newly admitted session. The
     * epoch, rather than this stale bool, must force the new configure. */
    assert(s_otg.cc_configured && s_otg.configured_source_epoch==configured_epoch);
    host_state=(aether_usb_host_status_t){.state=AETHER_OTG_OFF};
    now_us=5000000;recover_cc_on_manual_configure=true;
    assert(app_otg_set_manual_enabled(true)==ESP_OK);
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
    assert(manual_configure_calls==first_configures+1 && s_otg.configured_source_epoch!=configured_epoch);
    assert(s_otg.manual_role_deadline==now_us+OTG_MANUAL_ROLE_TIMEOUT_US);
    assert(cc_state.cc_raw==0x40 && power_snapshot().cc_detection_raw==0x10 && power_snapshot().cc_mode_raw==0x10);
    assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink && manual_configure_calls==first_configures+1);
    puts("PASS each new source epoch reinstalls manual CC policy and a fresh role deadline despite a stale configured flag; faults never restart automatically and same-epoch polls never reconfigure");

    fresh();request_manual();
    verify_state.vbus_mv=verify_state.vbus_raw=5134;
    const uint8_t stale_cc[]={0,1,4,6,3};
    for(unsigned cycle=0;cycle<3;++cycle) {
        if(cycle)assert(app_otg_set_manual_enabled(true)==ESP_OK);
        cc_state.otg_role_ready=true;cc_state.attached_as_sink=false;
        assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
        assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK);
        for(unsigned second=0;second<61;++second) {
            cc_state.cc_raw=0x40U|stale_cc[second%sizeof(stale_cc)];
            cc_state.cc_state=(tg29_cc_state_t)stale_cc[second%sizeof(stale_cc)];
            now_us+=1000000;
            assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
            assert(host_state.enabled && power_snapshot().output_verified && power_snapshot().vbus_mv==5134);
        }
        assert(app_otg_set_manual_enabled(false)==ESP_OK);
        assert(callbacks.set_vbus(callbacks.context,false)==ESP_OK);
        assert(cc_state.cc_detection_enabled && cc_state.cc_mode_raw==0x11);
        host_state=(aether_usb_host_status_t){.state=AETHER_OTG_OFF};
    }
    puts("PASS three explicit manual sessions each run beyond60s at5.134V with stale E7 states and no CC-triggered shutdown; every OFF restores sink detection");

    for(unsigned fault=0;fault<2;++fault) {
        fresh();request_manual();
        assert(callbacks.query_sink(callbacks.context,&sink)==ESP_OK && sink);
        assert(callbacks.set_vbus(callbacks.context,true)==ESP_OK);
        if(!fault)cc_state.cc_detection_enabled=true;
        else{cc_state.cc_mode=TG29_CC_SINK;cc_state.otg_role_ready=false;}
        assert(callbacks.query_sink(callbacks.context,&sink)==ESP_ERR_INVALID_STATE && !sink && !host_state.enabled);
        assert(!power_snapshot().output_verified);
    }
    puts("PASS manual output is revoked if detection is unexpectedly re-enabled or the disabled-port policy changes, while E7 history alone never revokes it");
    return 0;
}

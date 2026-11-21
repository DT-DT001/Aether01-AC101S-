/* Production GPS worker and parser with deterministic virtual FreeRTOS time.
 * Only an idle, completely cleaned worker may jump back to the test harness. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/app_gps.c"

static app_power_t manager;
static app_otg_power_status_t source;
static app_otg_input_status_t input;
static aether_usb_host_status_t host;
static jmp_buf idle_return;
static void (*worker_task)(void *), (*pulse_handler)(void *);
static void (*delay_hook)(void), (*read_hook)(void);
static void *pulse_context;
static int64_t clock_us;
static unsigned critical_depth, task_creates, notifications, hardware_calls;
static unsigned acquire_calls, release_calls, uart_installs, uart_deletes, reads, delays, level_high_calls;
static unsigned irq_removes, lease_release_failures, uart_delete_failures, off_failures;
static unsigned lease_acquire_failures;
static unsigned reset_floating_failures;
static unsigned input_acquires, input_reads, input_releases, input_release_failures;
static unsigned valid_input_samples, external_cleanup_retries;
static unsigned settled_input_samples;
static int64_t input_acquired_us, last_valid_input_us;
static int64_t last_settled_input_us;
static bool input_live;
static uint32_t current_input_token;
static esp_err_t input_acquire_error, input_read_error;
static unsigned pin_mode[64], pin_pull[64], pin_down[64], pin_level[64];
static bool irq_enabled, driver_installed, lease_live, borrowed, user_source;
static bool fail_task_create, fail_uart_install, fail_uart_config, fail_irq_enable, off_during_uart_install;
static bool fail_uart_read;
static uint32_t current_lease, lease_sequence;
static unsigned recovery_delay_base, recovery_read_base;

void test_enter(portMUX_TYPE *lock)
{ assert(!critical_depth); assert(!pthread_mutex_lock(lock)); ++critical_depth; }
void test_exit(portMUX_TYPE *lock)
{ assert(critical_depth==1); --critical_depth; assert(!pthread_mutex_unlock(lock)); }
static void outside_lock(void) { assert(!critical_depth); }
int64_t esp_timer_get_time(void) { return clock_us; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(clock_us/1000); }
TickType_t xTaskGetTickCountFromISR(void) { return xTaskGetTickCount(); }
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                      void *context, unsigned priority, TaskHandle_t *handle)
{
    outside_lock(); assert(task && !strcmp(name,"gps") && stack>=4096 && !context && priority==4);
    ++task_creates;
    if(fail_task_create) return pdFALSE;
    worker_task=task; *handle=(TaskHandle_t)&worker_task; return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task)
{ outside_lock(); assert(task==(TaskHandle_t)&worker_task); ++notifications; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    outside_lock(); assert(clear==pdTRUE && timeout==portMAX_DELAY);
    assert(!s_busy && !s_requested && !driver_installed && !lease_live && !input_live && !pin_level[37]);
    longjmp(idle_return,1);
}
void vTaskDelay(TickType_t ticks)
{
    outside_lock(); assert(ticks && ++delays<1000);
    clock_us+=(int64_t)ticks*1000;
    if(delay_hook) delay_hook();
}
esp_err_t app_power_get_status(const app_power_t *power, app_power_status_t *out)
{
    outside_lock(); assert(power==&manager && out);
    *out=(app_power_status_t){.sleeping=manager.sleeping,.shutdown_state=manager.shutdown_state,
        .shutdown_recovering=manager.shutdown_cancel_error!=ESP_OK}; return ESP_OK;
}
esp_err_t app_otg_manual_lease_acquire(uint32_t *token)
{
    outside_lock(); ++acquire_calls; assert(token && !lease_live && !input_live);
    if(lease_acquire_failures) {--lease_acquire_failures;return ESP_ERR_NOT_FINISHED;}
    if(host.cleanup_pending || (host.enabled && !user_source))return ESP_ERR_INVALID_STATE;
    /* The real bridge clears the prior manual failure when an explicit fresh
     * source epoch is admitted. Physical voltage verification remains supplied
     * by the mocked Host worker/sample, not by merely owning the lease. */
    if(!host.enabled)source.manual_source_failed=false;
    lease_live=true; borrowed=user_source; current_lease=++lease_sequence; *token=current_lease;
    host.enabled=true;source.manual_source_requested=true; return ESP_OK;
}
esp_err_t app_otg_manual_lease_release(uint32_t token)
{
    outside_lock(); ++release_calls;
    assert(!driver_installed && !pin_level[37] && !irq_enabled);
    /* IDF gpio_reset_pin enables a pull-up, so production must undo it. */
    assert(pin_pull[39]==GPIO_PULLUP_DISABLE && pin_down[39]==GPIO_PULLDOWN_DISABLE);
    if(lease_release_failures) { --lease_release_failures; return ESP_ERR_NOT_FINISHED; }
    if(!lease_live || token!=current_lease)return ESP_ERR_INVALID_STATE;
    lease_live=false;
    if(!borrowed) {host.enabled=false;host.vbus_enabled=false;source.output_verified=false;source.manual_source_requested=false;}
    return ESP_OK;
}
bool app_otg_manual_lease_valid(uint32_t token)
{ outside_lock(); return token && lease_live && token==current_lease; }
esp_err_t app_otg_get_status(aether_usb_host_status_t *out)
{ outside_lock(); *out=host; return ESP_OK; }
esp_err_t app_otg_get_power_status(app_otg_power_status_t *out)
{ outside_lock(); *out=source; return ESP_OK; }

esp_err_t app_otg_external_input_acquire(uint32_t *token)
{
    outside_lock(); ++input_acquires;
    assert(token && !input_live && !lease_live);
    if(input_acquire_error!=ESP_OK)return input_acquire_error;
    input_live=true;current_input_token=0x10000U+ ++lease_sequence;
    *token=current_input_token;input_acquired_us=clock_us;
    valid_input_samples=settled_input_samples=0;
    last_valid_input_us=last_settled_input_us=0;
    return ESP_OK;
}
esp_err_t app_otg_external_input_read(uint32_t token, app_otg_input_status_t *out)
{
    outside_lock(); ++input_reads;
    assert(token && token==current_input_token && input_live && !lease_live && out);
    if(input_read_error!=ESP_OK)return input_read_error;
    *out=input;
    if(input.valid && input.present && input.voltage_valid && input.voltage_mv>=4750 && input.voltage_mv<=5500 &&
        (!valid_input_samples || clock_us-last_valid_input_us>=20000)) {
        ++valid_input_samples;last_valid_input_us=clock_us;
    }
    if(input.valid && input.present && input.voltage_valid && input.voltage_mv>=4750 && input.voltage_mv<=5500 &&
        clock_us-input_acquired_us>=1000000 && (!settled_input_samples || clock_us-last_settled_input_us>=20000)) {
        ++settled_input_samples;last_settled_input_us=clock_us;
    }
    return ESP_OK;
}
esp_err_t app_otg_external_input_release(uint32_t token)
{
    outside_lock(); ++input_releases;
    assert(token==current_input_token && input_live && !lease_live);
    assert(!driver_installed && !pin_level[37] && !irq_enabled);
    assert(pin_pull[39]==GPIO_PULLUP_DISABLE && pin_down[39]==GPIO_PULLDOWN_DISABLE);
    if(input_release_failures) {--input_release_failures;return ESP_ERR_NOT_FINISHED;}
    input_live=false;current_input_token=0;
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    outside_lock(); ++hardware_calls;
    for(unsigned i=0;i<64;++i)if(config->pin_bit_mask&(UINT64_C(1)<<i)) {
        pin_mode[i]=config->mode;pin_pull[i]=config->pull_up_en;pin_down[i]=config->pull_down_en;
    }
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    outside_lock(); ++hardware_calls; assert(pin==37);
    if(level) {
        if(input_live) {
            assert(!lease_live && !acquire_calls && !release_calls);
            assert(input.valid && input.present && input.voltage_valid && input.voltage_mv>=4750 && input.voltage_mv<=5500);
            assert(clock_us-input_acquired_us>=1000000 && settled_input_samples>=2);
        } else {
            assert(lease_live && source.valid && source.output_verified && source.vbus_valid &&
                source.manual_source_requested && !source.failure_snapshot && host.vbus_enabled);
        }
        assert(pin_mode[37]==GPIO_MODE_OUTPUT_OD && !pin_pull[37] && !pin_down[37]);
        assert(driver_installed && pin_mode[40]==GPIO_MODE_INPUT && !pin_pull[40] && !pin_down[40]);
        ++level_high_calls;
    } else if(off_failures && pin_level[37]) {--off_failures;return ESP_FAIL;}
    pin_level[pin]=level;return ESP_OK;
}
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode)
{
    outside_lock();++hardware_calls;assert(pin==39);
    if(reset_floating_failures && !driver_installed) {--reset_floating_failures;return ESP_FAIL;}
    pin_pull[pin]=mode==GPIO_FLOATING?GPIO_PULLUP_DISABLE:GPIO_PULLUP_ENABLE;
    pin_down[pin]=GPIO_PULLDOWN_DISABLE;return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, int type)
{ outside_lock();++hardware_calls;assert(pin==38 && type==GPIO_INTR_POSEDGE);return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags)
{ outside_lock();++hardware_calls;assert(flags==ESP_INTR_FLAG_IRAM);return ESP_ERR_INVALID_STATE; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *context)
{ outside_lock();++hardware_calls;assert(pin==38 && handler);pulse_handler=handler;pulse_context=context;return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{ outside_lock();++hardware_calls;assert(pin==38);++irq_removes;pulse_handler=NULL;return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin)
{ outside_lock();++hardware_calls;assert(pin==38);irq_enabled=false;return ESP_OK; }
esp_err_t gpio_intr_enable(gpio_num_t pin)
{ outside_lock();++hardware_calls;assert(pin==38);if(fail_irq_enable)return ESP_FAIL;irq_enabled=true;return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin)
{ outside_lock();++hardware_calls;assert(pin==39);pin_mode[pin]=GPIO_MODE_DISABLE;pin_pull[pin]=GPIO_PULLUP_ENABLE;pin_down[pin]=GPIO_PULLDOWN_DISABLE;return ESP_OK; }
esp_err_t uart_driver_install(int port, int rx_buffer, int tx_buffer, int queue_size, void *queue, int flags)
{
    outside_lock();++hardware_calls;++uart_installs;
    assert(port==GPS_UART && rx_buffer==4096 && !tx_buffer && !queue_size && !queue && !flags && !driver_installed);
    if(fail_uart_install)return ESP_ERR_NO_MEM;
    driver_installed=true;
    if(off_during_uart_install) {
        if(input_live)input.present=false;
        else {lease_live=false;source.output_verified=false;source.vbus_valid=false;host.enabled=false;host.vbus_enabled=false;}
    }
    return ESP_OK;
}
esp_err_t uart_param_config(int port, const uart_config_t *config)
{ outside_lock();++hardware_calls;assert(port==GPS_UART && config->baud_rate==9600 && config->data_bits==8 && config->stop_bits==1 && !config->parity && !config->flow_ctrl);return fail_uart_config?ESP_FAIL:ESP_OK; }
esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts)
{ outside_lock();++hardware_calls;assert(port==GPS_UART && tx==UART_PIN_NO_CHANGE && rx==39 && rts==UART_PIN_NO_CHANGE && cts==UART_PIN_NO_CHANGE);return ESP_OK; }
esp_err_t uart_flush_input(int port)
{ outside_lock();++hardware_calls;assert(port==GPS_UART);return ESP_OK; }
esp_err_t uart_driver_delete(int port)
{
    outside_lock();++hardware_calls;++uart_deletes;assert(port==GPS_UART && driver_installed);
    if(uart_delete_failures) {--uart_delete_failures;return ESP_FAIL;}
    driver_installed=false;return ESP_OK;
}
static void make_sentence(char *out, size_t cap, const char *body)
{
    unsigned checksum=0;for(const char *p=body;*p;++p)checksum^=(unsigned char)*p;
    snprintf(out,cap,"$%s*%02X\r\n",body,checksum);
}
int uart_read_bytes(int port, void *bytes, uint32_t size, TickType_t timeout)
{
    outside_lock();++hardware_calls;assert(port==GPS_UART && driver_installed && timeout==50 && size>=192);
    clock_us+=50000;assert(++reads<1000);
    if(fail_uart_read)return -1;
    if(read_hook) read_hook();
    char sentence[192];
    make_sentence(sentence,sizeof(sentence),"GNRMC,123456.00,A,3112.00000,N,12130.00000,E,1.0,90.0,211126,,,A");
    const size_t len=strlen(sentence);memcpy(bytes,sentence,len);return (int)len;
}

static void fresh(void)
{
    assert(!critical_depth && !driver_installed && !s_busy && !input_live);
    gnss_nmea_free(s_parser);s_parser=NULL;s_task=NULL;s_power=NULL;
    s_requested=false;s_busy=false;s_generation=0;s_pps_count=0;s_pps_tick=0;
    s_status=(app_gps_status_t){0};manager=(app_power_t){0};
    source=(app_otg_power_status_t){.valid=true,.vbus_valid=true,.output_verified=true,.vbus_mv=5100};
    host=(aether_usb_host_status_t){.vbus_enabled=true};
    worker_task=NULL;pulse_handler=NULL;pulse_context=NULL;delay_hook=read_hook=NULL;
    clock_us=0;task_creates=notifications=hardware_calls=acquire_calls=release_calls=0;
    uart_installs=uart_deletes=reads=delays=level_high_calls=irq_removes=0;
    lease_release_failures=uart_delete_failures=off_failures=0;
    lease_acquire_failures=0;
    reset_floating_failures=0;
    input=(app_otg_input_status_t){.valid=true,.present=true,.voltage_valid=true,.voltage_mv=5000};
    input_live=false;current_input_token=0;
    input_acquires=input_reads=input_releases=input_release_failures=0;
    valid_input_samples=external_cleanup_retries=0;
    settled_input_samples=0;last_settled_input_us=0;
    input_acquired_us=last_valid_input_us=0;
    input_acquire_error=ESP_ERR_NOT_FOUND;input_read_error=ESP_OK;
    memset(pin_mode,0,sizeof(pin_mode));memset(pin_pull,0,sizeof(pin_pull));
    memset(pin_down,0,sizeof(pin_down));memset(pin_level,0,sizeof(pin_level));
    irq_enabled=driver_installed=lease_live=borrowed=user_source=false;
    fail_task_create=fail_uart_install=fail_uart_config=fail_irq_enable=off_during_uart_install=false;
    fail_uart_read=false;
    current_lease=lease_sequence=0;
    recovery_delay_base=recovery_read_base=0;
}
static void initialize(void) {assert(app_gps_init(&manager)==ESP_OK && s_task && worker_task);}
static void run_worker(void)
{ assert(worker_task);if(!setjmp(idle_return))worker_task(NULL);assert(!s_busy && !s_requested && !lease_live && !input_live && !driver_installed); }
static app_gps_status_t status(void) {app_gps_status_t result;app_gps_get_status(&result);return result;}
static void stop_on_first_read(void) {app_gps_cancel();}
static void delayed_power(void)
{
    assert(!level_high_calls && !driver_installed && lease_live && s_busy);
    if(delays==3) {source.output_verified=true;host.vbus_enabled=true;}
}
static void cancel_power_wait(void)
{ assert(!driver_installed && !level_high_calls);if(delays==3)app_gps_cancel(); }
static void navigation_then_stop(void)
{
    if(reads==1) {assert(pulse_handler && irq_enabled);pulse_handler(pulse_context);}
    if(reads==2) {
        const app_gps_status_t result=status();
        assert(result.active && result.state==APP_GPS_FIXED && result.uart_receiving && result.navigation.position_valid);
        assert(result.pps_seen && result.pps_count==1 && result.supply_valid && result.supply_mv==5100);
        app_gps_cancel();
    }
}
static void retain_failed_cleanup(void)
{
    assert(s_busy && lease_live && status().state==APP_GPS_STOPPING);
    assert(app_gps_set_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(app_gps_stop_wait(0)==ESP_ERR_TIMEOUT);
}
static void fault_source(void)
{lease_live=false;source.output_verified=false;source.vbus_valid=false;host.vbus_enabled=false;host.enabled=false;}
static void timeout_source(void)
{fault_source();source.manual_source_failed=true;source.last_error=ESP_ERR_TIMEOUT;}
static void shutdown_while_powering(void)
{
    assert(!level_high_calls && !driver_installed);
    manager.shutdown_state=APP_POWER_SHUTDOWN_PREPARING;
    assert(app_gps_set_enabled(true)==ESP_ERR_INVALID_STATE);
}
static void cancel_then_stale_publish(void)
{
    if(reads!=2)return;
    assert(status().navigation.position_valid);
    const uint32_t old_generation=s_generation;
    app_gps_cancel();
    assert(status().state==APP_GPS_STOPPING && !status().navigation.position_valid);
    publish_navigation(old_generation,0,&source);
    const app_gps_status_t after=status();
    assert(after.state==APP_GPS_STOPPING && !after.navigation.position_valid && !after.navigation.time_valid &&
        !after.navigation.date_valid && !after.navigation.altitude_valid && !after.navigation.satellite_count);
    assert(app_gps_set_enabled(true)==ESP_ERR_INVALID_STATE);
}

static void prepare_external(void)
{
    input_acquire_error=ESP_OK;
    source=(app_otg_power_status_t){0};
    host=(aether_usb_host_status_t){0};
}
static void external_navigation_then_stop(void)
{
    assert(input_live && !lease_live && !acquire_calls && !release_calls);
    assert(!host.enabled && !host.vbus_enabled && !source.boost_enabled && !source.manual_source_requested);
    if(reads==1) {assert(pulse_handler && irq_enabled);pulse_handler(pulse_context);}
    if(reads==2) {
        const app_gps_status_t result=status();
        assert(result.active && result.external_power && result.state==APP_GPS_FIXED);
        assert(result.navigation.position_valid && result.supply_valid && result.supply_mv==input.voltage_mv);
        app_gps_cancel();
    }
}
static void unplug_external(void)
{assert(input_live);input.present=false;}
static void fault_external_boost(void)
{assert(input_live);source.boost_enabled=true;input_read_error=ESP_ERR_INVALID_STATE;}
static void lose_external_voltage(void)
{assert(input_live);input.voltage_valid=false;}
static void retain_external_cleanup(void)
{
    if(status().state!=APP_GPS_STOPPING) return;
    ++external_cleanup_retries;
    assert(s_busy && input_live && !lease_live && !acquire_calls && !release_calls);
    assert(app_gps_set_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(app_gps_stop_wait(0)==ESP_ERR_TIMEOUT);
}
static void settle_external_voltage(void)
{
    assert(!driver_installed && !level_high_calls && input_live);
    if(clock_us>=1020000)input.voltage_valid=true;
}
static void unplug_external_during_wait(void)
{
    assert(!driver_installed && !level_high_calls && input_live);
    if(clock_us>=400000)input.present=false;
}
static void timeout_during_source_start(void)
{
    assert(!driver_installed && !level_high_calls && lease_live && s_busy);
    if(delays==3) {timeout_source();source.failure_snapshot=true;}
}
static void verify_restarted_source(void)
{
    assert(!driver_installed && !level_high_calls && lease_live && s_busy);
    assert(!source.manual_source_failed);
    if(delays==recovery_delay_base+3) {
        source.valid=true;source.failure_snapshot=false;source.vbus_valid=true;
        source.output_verified=true;source.vbus_mv=5100;source.last_error=ESP_OK;
        host.vbus_enabled=true;
    }
}
static void resumed_external_navigation_then_stop(void)
{
    assert(input_live && !lease_live && !acquire_calls && !release_calls);
    unsigned relative=reads-recovery_read_base;
    if(relative==1) {assert(pulse_handler && irq_enabled);pulse_handler(pulse_context);}
    if(relative==2) {
        const app_gps_status_t result=status();
        assert(result.active && result.external_power && result.state==APP_GPS_FIXED);
        assert(result.navigation.position_valid && result.navigation.accepted_sentences==1);
        assert(result.pps_seen && result.pps_count==1 && result.supply_valid);
        app_gps_cancel();
    }
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    fresh();assert(app_gps_set_enabled(true)==ESP_ERR_INVALID_STATE);
    assert(app_gps_set_enabled(false)==ESP_OK && app_gps_stop_wait(0)==ESP_OK);
    assert(app_gps_init(NULL)==ESP_ERR_INVALID_ARG);initialize();
    assert(status().available && status().state==APP_GPS_OFF && !acquire_calls && !uart_installs);
    assert(pin_mode[37]==GPIO_MODE_OUTPUT_OD && !pin_level[37] && !pin_pull[37] && !pin_down[37]);
    for(unsigned pin=38;pin<=40;++pin)assert(pin_mode[pin]==GPIO_MODE_INPUT && !pin_pull[pin] && !pin_down[pin]);
    const unsigned cold_hw=hardware_calls;
    assert(app_gps_set_enabled(true)==ESP_OK && hardware_calls==cold_hw);
    assert(app_gps_set_enabled(false)==ESP_OK && hardware_calls==cold_hw);
    run_worker();assert(!acquire_calls);
    puts("PASS cold/default OFF and GUI start/stop publish only intent; GPS pins are no-pull input or open-drain LOW");

    fresh();initialize();source.output_verified=false;host.vbus_enabled=false;
    delay_hook=cancel_power_wait;assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(!uart_installs && !level_high_calls && release_calls==1 && status().state==APP_GPS_OFF);
    puts("PASS canceled unverified supply never starts UART or releases ON_OFF; owned supply lease is cleaned");

    fresh();initialize();source.output_verified=false;host.vbus_enabled=false;
    delay_hook=delayed_power;read_hook=navigation_then_stop;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(delays==3 && reads==2 && level_high_calls==1 && uart_deletes==1 && release_calls==1);
    assert(status().state==APP_GPS_OFF && !status().navigation.position_valid);
    puts("PASS delayed measured power precedes UART/ON_OFF; real RMC parser and PPS reach FIXED; stop clears live navigation");

    fresh();initialize();source.output_verified=false;host.vbus_enabled=false;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && status().last_error==ESP_ERR_TIMEOUT);
    assert(!level_high_calls && !uart_installs && release_calls==1 && delays>=325);
    puts("PASS supply verification timeout is bounded and cleans its lease without enabling UART or GPS");

    fresh();initialize();lease_acquire_failures=2;read_hook=stop_on_first_read;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(acquire_calls==3 && delays==2 && uart_installs==1 && release_calls==1);
    puts("PASS nonblocking OTG admission contention retries inside the startup budget instead of failing or busy-spinning");

    fresh();initialize();read_hook=stop_on_first_read;user_source=true;host.enabled=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(borrowed && user_source && host.enabled && source.manual_source_requested && release_calls==1);
    puts("PASS stopping GPS releases its borrowed lease without turning off preexisting user power");

    fresh();initialize();read_hook=fault_source;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && !status().active && !status().supply_valid);
    const unsigned fault_acquires=acquire_calls;run_worker();assert(acquire_calls==fault_acquires);
    puts("PASS forced user OFF/manual failure stops UART safely and stays stopped until an explicit new start");

    fresh();initialize();read_hook=timeout_source;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && status().last_error==ESP_ERR_TIMEOUT && uart_deletes==1);
    puts("PASS revoked source preserves original OTG timeout in GPS status instead of hiding it as INVALID_STATE");

    fresh();initialize();read_hook=stop_on_first_read;uart_delete_failures=2;delay_hook=retain_failed_cleanup;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(uart_deletes==3 && delays==2 && release_calls==1);
    puts("PASS UART teardown failures retain session ownership and reject restart; bounded stop_wait reports timeout until cleanup succeeds");

    fresh();initialize();read_hook=stop_on_first_read;lease_release_failures=2;delay_hook=retain_failed_cleanup;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(release_calls==3 && uart_deletes==1 && delays==2);
    puts("PASS accessory lease release contention retains cleanup ownership and retries without reinstalling UART");

    fresh();initialize();read_hook=stop_on_first_read;off_failures=1;delay_hook=retain_failed_cleanup;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(delays==1 && release_calls==1 && !pin_level[37]);
    puts("PASS failed ON_OFF sleep command retains lease until a successful retry; never releases power ownership prematurely");

    fresh();initialize();read_hook=stop_on_first_read;reset_floating_failures=1;delay_hook=retain_failed_cleanup;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(delays==1 && release_calls==1 && !pin_pull[39]);
    puts("PASS failed floating-RX restoration retains supply ownership until retry removes the IDF reset pull-up");

    fresh();initialize();read_hook=cancel_then_stale_publish;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(reads==2 && status().state==APP_GPS_OFF);
    puts("PASS late navigation publication cannot overwrite STOPPING or restore position/date/satellite validity after cancellation");

    fresh();initialize();fail_uart_read=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && status().last_error==ESP_FAIL && uart_deletes==1 && release_calls==1);
    puts("PASS UART read failure terminates the active session and tears down driver before releasing the lease");

    fresh();initialize();fail_uart_install=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && uart_installs==1 && !uart_deletes && !level_high_calls && release_calls==1);
    puts("PASS UART allocation failure never deletes an unowned driver or enables GPS and releases its supply lease");

    fresh();initialize();fail_uart_config=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && uart_deletes==1 && !level_high_calls && release_calls==1);
    puts("PASS partial UART setup rolls back the installed driver before releasing supply");

    fresh();initialize();fail_irq_enable=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && uart_deletes==1 && !level_high_calls && release_calls==1);
    puts("PASS PPS IRQ failure cannot release ON_OFF or leak UART/power ownership");

    fresh();initialize();source.output_verified=false;host.vbus_enabled=false;delay_hook=shutdown_while_powering;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    manager.shutdown_state=APP_POWER_SHUTDOWN_IDLE;
    const unsigned shutdown_acquires=acquire_calls;run_worker();
    assert(acquire_calls==shutdown_acquires && !level_high_calls && !uart_installs && status().state==APP_GPS_OFF);
    puts("PASS shutdown during power wait cancels GPS and cancellation recovery never silently restarts source power");

    fresh();initialize();read_hook=stop_on_first_read;
    assert(app_gps_set_enabled(true)==ESP_OK && app_gps_set_enabled(false)==ESP_OK && app_gps_set_enabled(true)==ESP_OK);
    run_worker();assert(acquire_calls==1 && uart_installs==1 && status().state==APP_GPS_OFF);
    source.output_verified=true;host.vbus_enabled=true; /* New mocked boost verification. */
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();assert(acquire_calls==2 && uart_installs==2);
    puts("PASS rapid pre-worker start/stop/start admits only newest request and fully cleaned sessions can explicitly restart");

    fresh();fail_task_create=true;
    assert(app_gps_init(&manager)==ESP_ERR_NO_MEM && !s_task && !s_parser && irq_removes==1 && !level_high_calls);
    fail_task_create=false;initialize();assert(status().available && !acquire_calls);
    puts("PASS worker creation failure frees parser and removes only GPS ISR; constructor can retry without energizing output");

    fresh();initialize();off_during_uart_install=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(!level_high_calls && uart_deletes==1 && !status().active);
    puts("PASS OTG OFF interleaved with UART setup is rechecked before releasing GPS ON_OFF");

    /* External input is independent of charging phase. Both a charging input
     * and a charge-complete input must avoid the manual source/Host path. */
    static const uint16_t external_voltages[]={5000,5250,4750,5500};
    for(size_t i=0;i<sizeof(external_voltages)/sizeof(external_voltages[0]);++i) {
        fresh();initialize();prepare_external();input.voltage_mv=external_voltages[i];
        read_hook=external_navigation_then_stop;
        assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
        assert(input_acquires==1 && input_reads>=2 && input_releases==1 && !acquire_calls && !release_calls);
        assert(settled_input_samples>=2 && clock_us>=1020000 && uart_installs==1 && uart_deletes==1);
        assert(!host.enabled && !host.vbus_enabled && !source.manual_source_requested && !source.boost_enabled);
        printf("PASS external/charge-complete input %umV uses only ADC lease, waits for stable power, publishes FIXED/PPS, and never opens source/Host\n",(unsigned)external_voltages[i]);
    }

    fresh();initialize();prepare_external();input.voltage_valid=false;
    delay_hook=settle_external_voltage;read_hook=stop_on_first_read;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(clock_us>=1040000 && valid_input_samples>=2 && level_high_calls==1 && input_releases==1);
    assert(!acquire_calls && !release_calls);
    puts("PASS external ADC becoming valid after warmup still requires two distinct valid samples before ON_OFF");

    fresh();initialize();prepare_external();read_hook=unplug_external;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && input_releases==1 && uart_deletes==1 && !acquire_calls && !release_calls);
    unsigned prior_input_acquires=input_acquires;
    run_worker();assert(input_acquires==prior_input_acquires && !acquire_calls);
    puts("PASS unplugging external power stops GPS, releases only ADC lease, and never silently switches to OTG");
    input_acquire_error=ESP_ERR_NOT_FOUND;
    source=(app_otg_power_status_t){.valid=true,.vbus_valid=true,.output_verified=true,.vbus_mv=5100};
    host=(aether_usb_host_status_t){.vbus_enabled=true};
    read_hook=stop_on_first_read;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(acquire_calls==1 && release_calls==1 && input_acquires==prior_input_acquires+1);
    puts("PASS only a new explicit user start after unplugging may re-evaluate and acquire a source lease");

    fresh();initialize();prepare_external();read_hook=fault_external_boost;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && status().last_error==ESP_ERR_INVALID_STATE && input_releases==1);
    prior_input_acquires=input_acquires;run_worker();
    assert(input_acquires==prior_input_acquires && !acquire_calls && !release_calls && uart_deletes==1);
    puts("PASS bridge-reported BOOST/source anomaly cancels an external session without source fallback or restart");

    static const uint16_t invalid_external_voltages[]={0,4749,5501,65535};
    for(size_t i=0;i<sizeof(invalid_external_voltages)/sizeof(invalid_external_voltages[0]);++i) {
        fresh();initialize();prepare_external();input.voltage_mv=invalid_external_voltages[i];
        assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
        assert(status().state==APP_GPS_ERROR && !uart_installs && !level_high_calls && input_releases==1);
        assert(!acquire_calls && !release_calls);
        printf("PASS out-of-range external input %umV rejects startup without OTG fallback\n",(unsigned)invalid_external_voltages[i]);
    }

    fresh();initialize();prepare_external();input.voltage_valid=false;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && status().last_error==ESP_ERR_TIMEOUT && delays>=325);
    assert(!uart_installs && !level_high_calls && input_releases==1 && !acquire_calls && !release_calls);
    puts("PASS absent external ADC voltage validity times out within startup budget, retaining no UART or source ownership");

    fresh();initialize();prepare_external();input.valid=false;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && !uart_installs && input_releases==1 && !acquire_calls);
    puts("PASS invalid external-input status is not treated as a measured or usable supply");

    fresh();initialize();prepare_external();input_read_error=ESP_FAIL;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && status().last_error==ESP_FAIL && !uart_installs && input_releases==1 && !acquire_calls);
    puts("PASS external-input measurement error refuses startup and cleans only its ADC lease");

    fresh();initialize();prepare_external();input_acquire_error=ESP_FAIL;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && !uart_installs && !input_releases && !acquire_calls);
    puts("PASS external-input admission error other than NOT_FOUND can never fall through to enabling OTG");

    fresh();initialize();prepare_external();delay_hook=unplug_external_during_wait;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && !uart_installs && !level_high_calls && input_releases==1 && !acquire_calls);
    puts("PASS unplugging during external ADC warmup aborts before UART/ON_OFF and cannot turn on BOOST");

    fresh();initialize();prepare_external();input_release_failures=2;
    read_hook=stop_on_first_read;delay_hook=retain_external_cleanup;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(input_releases==3 && external_cleanup_retries==2 && uart_deletes==1 && !acquire_calls && !release_calls);
    puts("PASS ADC cleanup failure retains external session ownership, rejects restart, and retries release without touching manual power");

    fresh();initialize();prepare_external();fail_uart_config=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && uart_deletes==1 && !level_high_calls && input_releases==1 && !acquire_calls && !release_calls);
    puts("PASS partial UART setup on external power rolls back UART and ADC lease without invoking source release");

    fresh();initialize();prepare_external();off_during_uart_install=true;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && uart_deletes==1 && !level_high_calls && input_releases==1 && !acquire_calls && !release_calls);
    puts("PASS external input lost during UART allocation is rechecked before ON_OFF and never triggers source fallback");

    fresh();initialize();prepare_external();read_hook=lose_external_voltage;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && uart_deletes==1 && input_releases==1 && !acquire_calls && !release_calls);
    puts("PASS external ADC validity lost while receiving stops live GPS instead of treating stale voltage as usable");

    fresh();initialize();prepare_external();input_acquire_error=ESP_ERR_NOT_FINISHED;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && delays>=325 && !uart_installs && !level_high_calls && !input_releases && !acquire_calls);
    puts("PASS sustained external-admission contention is bounded and never bypasses source-role protection");

    fresh();initialize();source.output_verified=false;host.vbus_enabled=false;
    delay_hook=timeout_during_source_start;
    TaskHandle_t same_task=s_task;gnss_nmea_parser_t *same_parser=s_parser;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && status().last_error==ESP_ERR_TIMEOUT);
    assert(!s_busy && !s_requested && !lease_live && !input_live && !driver_installed);
    assert(acquire_calls==1 && release_calls==1 && !uart_installs);
    assert(source.manual_source_failed && source.failure_snapshot && source.last_error==ESP_ERR_TIMEOUT);
    uint32_t failed_lease=current_lease;
    /* No fresh()/initialize(), no status or owner reset: only an explicit new
     * command and new physical verification stimulus are introduced here. */
    recovery_delay_base=delays;delay_hook=verify_restarted_source;read_hook=navigation_then_stop;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(s_task==same_task && s_parser==same_parser && task_creates==1);
    assert(current_lease!=failed_lease && acquire_calls==2 && release_calls==2);
    assert(uart_installs==1 && uart_deletes==1 && reads==2 && level_high_calls==1);
    assert(!source.manual_source_failed && !source.failure_snapshot);
    assert(status().state==APP_GPS_OFF && !s_busy && !lease_live && !input_live && !driver_installed);
    puts("PASS same GPS worker survives source TIMEOUT, releases every owner, explicitly starts a fresh verified source epoch, receives real FIXED/PPS, and stops without fixture reset");

    fresh();initialize();prepare_external();read_hook=unplug_external;
    same_task=s_task;same_parser=s_parser;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(status().state==APP_GPS_ERROR && !s_busy && !s_requested && !input_live);
    assert(!current_input_token && input_acquires==1 && input_releases==1 && uart_deletes==1);
    uint32_t failed_input_sequence=lease_sequence;
    /* Reinsert measured external power, preserving task/parser/counters and
     * all cleanup results from the failed session. */
    input.present=true;recovery_read_base=reads;read_hook=resumed_external_navigation_then_stop;
    assert(app_gps_set_enabled(true)==ESP_OK);run_worker();
    assert(s_task==same_task && s_parser==same_parser && task_creates==1);
    assert(lease_sequence>failed_input_sequence && !current_input_token);
    assert(input_acquires==2 && input_releases==2 && uart_installs==2 && uart_deletes==2 && reads==3);
    assert(settled_input_samples>=2 && !acquire_calls && !release_calls && !host.enabled && !host.vbus_enabled);
    assert(status().state==APP_GPS_OFF && !s_busy && !input_live && !lease_live && !driver_installed);
    puts("PASS same GPS worker survives external unplug ERROR, explicitly restarts after replug with a fresh ADC token/settling window, receives real FIXED/PPS, and stops without fixture reset");

    fresh();
    puts("PASS all worker tests clean parser/driver/lease ownership; hardware/RTOS are mocked, no device or USB IO executed");
    return 0;
}

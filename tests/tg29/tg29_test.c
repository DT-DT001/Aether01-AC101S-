/* The fake bank models the TG29 register contract, not an ESP-IDF device.
 * Include the production driver so bus-failure cleanup is exercised directly. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../../components/tg29/tg29.c"

struct test_mutex { unsigned depth; };
struct test_i2c { uint8_t reg[256]; };
static struct test_i2c chip;
static unsigned reads[256], writes[256], write_count;
static uint8_t last_write[256], key_arrive_on_ack;
static int fail_write_reg = -1, drop_write_reg = -1, fail_read_reg = -1;
static unsigned fail_write_occurrence = 1, drop_write_occurrence = 1;
static unsigned fail_read_occurrence = 1;
static bool require_off_before_voltage;
static bool emulate_clamp, lost_bus;
static bool battery_read_only, fail_mutex_take;
static tg29_handle_t battery_handle;
static tg29_battery_state_t *battery_output;
static uint8_t battery_read_order[16];
static unsigned battery_read_count;
static unsigned cc_change_read;
static unsigned vbus_good_on_read;
static uint8_t cc_change_to, cc_after_boost, cc_arrive_on_ack;
static bool change_cc_after_boost;
static TickType_t fake_ticks;
static void (*delay_hook)(void);
static tg29_handle_t verification_handle;
static bool revoke_on_mode_write;
static int cancel_on_read_reg = -1;
static bool lose_gate_after_boost, good_after_gate, detach_after_gate;
static uint8_t write_order[256];
static unsigned write_order_count;
static bool manual_disable_guard;
TickType_t xTaskGetTickCount(void) { return fake_ticks; }
void vTaskDelay(TickType_t ticks)
{
    assert(!verification_handle || !verification_handle->mutex->depth);
    fake_ticks += ticks;
    if (delay_hook) delay_hook();
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return calloc(1, sizeof(struct test_mutex));
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t timeout)
{
    (void)timeout;
    assert(mutex);
    if (fail_mutex_take) { fail_mutex_take = false; return pdFALSE; }
    ++mutex->depth;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex)
{
    assert(mutex && mutex->depth);
    --mutex->depth;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(mutex && !mutex->depth);
    free(mutex);
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                   const i2c_device_config_t *config,
                                   i2c_master_dev_handle_t *device)
{
    assert(bus && config && device);
    assert(config->device_address == 0x34 && config->scl_speed_hz <= 400000);
    *device = &chip;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(device == &chip);
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                      const uint8_t *command, size_t command_size,
                                      uint8_t *data, size_t size, int timeout)
{
    (void)timeout;
    assert(device == &chip && command && command_size == 1 && data && size == 1);
    if (lost_bus) return ESP_FAIL;
    const uint8_t reg = command[0];
    if (battery_read_only) {
        assert(battery_handle && battery_handle->mutex->depth);
        assert(battery_read_count < sizeof(battery_read_order));
        battery_read_order[battery_read_count++] = reg;
        if (battery_output) {
            const unsigned char *bytes = (const unsigned char *)battery_output;
            for (size_t i = 0; i < sizeof(*battery_output); ++i) assert(bytes[i] == 0);
        }
    }
    ++reads[reg];
    if (cancel_on_read_reg == reg) { tg29_revoke_otg_source_permission(verification_handle); cancel_on_read_reg = -1; }
    if (fail_read_reg == reg && reads[reg] == fail_read_occurrence) {
        fail_read_reg = -1;
        return ESP_FAIL;
    }
    if (reg == 0xE7 && cc_change_read && reads[reg] == cc_change_read) chip.reg[reg] = cc_change_to;
    if (reg == 0x00 && vbus_good_on_read == reads[reg]) chip.reg[reg] |= 0x20U;
    *data = chip.reg[reg];
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data,
                              size_t size, int timeout)
{
    (void)timeout;
    assert(device == &chip && data && size == 2);
    assert(!battery_read_only); /* Battery queries cannot write even once. */
    if (lost_bus) return ESP_FAIL;
    const uint8_t reg = data[0], value = data[1];
    /* Key acknowledgement is limited to REG49 low bits. Charger, boost,
     * protection, DCDC voltage and other fault-clear writes stay forbidden. */
    assert(reg == 0x13 || reg == 0x50 || reg == 0xC0 || reg == 0x0B || reg == 0x19 || reg == 0x1E || reg == 0x4C || reg == 0x61 || reg == 0x62 || reg == 0xE3 ||
           reg == 0x27 || reg == 0x41 || reg == 0x49 || reg == 0x80 || reg == 0x90 ||
           reg == 0x91 || (reg >= 0x93 && reg <= 0x9E));
    if (reg == 0x49) assert((value & 0xF0) == 0);
    if (reg == 0x4C) assert((value & ~0x60U) == 0);
    if (reg == 0x80) assert(value & 0x02);
    if (require_off_before_voltage && reg >= 0x93 && reg <= 0x9E && reg != 0x94) {
        assert((chip.reg[0x80] & 0x07) == 0x02);
        assert(chip.reg[0x90] == 0x02 && (chip.reg[0x91] & 0x1F) == 0);
    }
    if (reg == 0xE3 && revoke_on_mode_write) { tg29_revoke_otg_source_permission(verification_handle); revoke_on_mode_write = false; }
    if (write_order_count < sizeof(write_order)) write_order[write_order_count++] = reg;
    ++write_count;
    ++writes[reg];
    last_write[reg] = value;
    if (fail_write_reg == reg && writes[reg] == fail_write_occurrence) {
        fail_write_reg = -1;
        return ESP_FAIL;
    }
    if (drop_write_reg == reg && writes[reg] == drop_write_occurrence) {
        drop_write_reg = -1;
        return ESP_OK; /* ACK without committing: readback must detect this. */
    }
    if (reg == 0x90 && !(chip.reg[0x90] & 0x02) && (value & 0x02))
        assert((chip.reg[0x94] & 0x1F) == 0x1C); /* Do not enable an unknown voltage. */
    if (emulate_clamp && reg == 0x90 && (chip.reg[0x90] & 0x02) && !(value & 0x02))
        lost_bus = true;
    if (reg == 0x49) {
        chip.reg[reg] &= (uint8_t)~value; /* Real RW1C semantics. */
        chip.reg[reg] |= key_arrive_on_ack;
        key_arrive_on_ack = 0;
    } else if (reg == 0x4C) {
        chip.reg[reg] &= (uint8_t)~value; chip.reg[reg] |= cc_arrive_on_ack; cc_arrive_on_ack = 0;
    } else chip.reg[reg] = value;
    if (manual_disable_guard && reg == 0x0B && !(value & 0x08U)) {
        assert(!(chip.reg[0x13] & 1U) && !(chip.reg[0x19] & 0x10U));
        assert(!(value & 1U)); /* Never replay RWAC watchdog. */
    }
    if (reg == 0x13 && (value & 1U) && good_after_gate) chip.reg[0x00] |= 0x20U;
    if (reg == 0x13 && (value & 1U) && detach_after_gate) chip.reg[0xE7] = 3;
    if (reg == 0x19 && (value & 0x10U) && change_cc_after_boost) chip.reg[0xE7] = cc_after_boost;
    if (reg == 0x19 && (value & 0x10U) && lose_gate_after_boost) chip.reg[0x13] &= (uint8_t)~1U;
    return ESP_OK;
}

static tg29_power_profile_t board_profile(void)
{
    return (tg29_power_profile_t) {
        .aldo2_mv = 3300, .aldo4_mv = 3300, .bldo1_mv = 3300,
        .cldo1_mv = 3300, .cldo3_mv = 3300,
        .enable_sensor_rail = true, .enable_codec_rail = true,
        .enable_touch_rail = true, .enable_display_rail = true,
    };
}
static void reset_chip(void)
{
    memset(&chip, 0, sizeof(chip));
    fake_ticks = 0; delay_hook = NULL; revoke_on_mode_write = false; cancel_on_read_reg = -1;
    manual_disable_guard=false;
    memset(reads, 0, sizeof(reads));
    memset(writes, 0, sizeof(writes));
    memset(last_write, 0, sizeof(last_write));
    key_arrive_on_ack = 0;
    cc_change_read = vbus_good_on_read = 0;
    change_cc_after_boost = false; cc_change_to = cc_after_boost = cc_arrive_on_ack = 0;
    write_count = 0; write_order_count = 0; lose_gate_after_boost = good_after_gate = detach_after_gate = false;
    fail_write_reg = drop_write_reg = fail_read_reg = -1;
    fail_write_occurrence = drop_write_occurrence = fail_read_occurrence = 1;
    require_off_before_voltage = false;
    emulate_clamp = lost_bus = false;
    battery_read_only = fail_mutex_take = false;
    battery_output = NULL; battery_read_count = 0;
    chip.reg[0x80] = 0xE7; /* All DCDCs initially enabled; preserve reserved bits. */
    chip.reg[0x84] = 0xE9; /* DVM plus the unchanged board's 3.3V code. */
    chip.reg[0x90] = 0xFF;
    chip.reg[0x91] = 0xFF;
    for (unsigned reg = 0x93; reg <= 0x9F; ++reg) chip.reg[reg] = 0xE3;
    chip.reg[0x08] = 0x31;
    chip.reg[0x4A] = 0x64;
    chip.reg[0x19] = 0x06;
    chip.reg[0x62] = 0x10;
    chip.reg[0x64] = 0x02;
    chip.reg[0x65] = 0x02;
}
static void assert_unrelated_preserved(void)
{
    assert(chip.reg[0x84] == 0xE9 && !writes[0x84]);
    assert(chip.reg[0x08] == 0x31 && chip.reg[0x4A] == 0x64);
    assert(chip.reg[0x19] == 0x06 && chip.reg[0x62] == 0x10);
    assert(chip.reg[0x64] == 0x02 && chip.reg[0x65] == 0x02);
    assert(!writes[0x9F]);
}
static void assert_safe_off(void)
{
    assert((chip.reg[0x80] & 0x07) == 0x02);
    assert(chip.reg[0x90] == 0x02 && (chip.reg[0x91] & 0x1F) == 0);
    assert(!lost_bus);
    assert_unrelated_preserved();
}

static void battery_fixture(tg29_handle_t h)
{
    reset_chip(); battery_handle = h; battery_read_only = true;
    chip.reg[0x00] = 0x28; /* Battery reported present, usable VBUS. */
    chip.reg[0x01] = 0xA2; /* Ignore high bit 7: charging direction, CC. */
    chip.reg[0x68] = 1; chip.reg[0x0B] = 4; chip.reg[0xC0] = 1;
    chip.reg[0xA4] = 55; chip.reg[0xC4] = 0xD0; chip.reg[0xC5] = 0x02;
}
static tg29_battery_state_t battery_read(tg29_handle_t h)
{
    tg29_battery_state_t out;
    memset(&out, 0xA5, sizeof(out)); battery_output = &out;
    assert(tg29_get_battery_state(h, &out) == ESP_OK);
    battery_output = NULL;
    assert(!write_count && !h->mutex->depth);
    return out;
}
static void test_battery(tg29_handle_t h)
{
    const uint8_t order[] = {0x00,0x01,0x68,0x0B,0xC0,0xA4,0xC4,0xC5};
    battery_fixture(h); tg29_battery_state_t s = battery_read(h);
    assert(s.presence_valid && s.battery_present && s.vbus_present && s.charging && !s.charge_done);
    assert(s.charge_state_valid && s.charge_state == 2 && s.current_direction == 1);
    assert(s.percentage_valid && s.percentage == 55 && s.voltage_valid && s.voltage_mv == 4098);
    assert(battery_read_count == sizeof(order) && !memcmp(battery_read_order, order, sizeof(order)));
    puts("PASS battery query is purely read-only, publishes once after complete reads, and reads high VBAT byte before low");

    const uint8_t soc[] = {0,100,101,128,255};
    for (size_t i = 0; i < sizeof(soc); ++i) {
        battery_fixture(h); chip.reg[0xA4] = soc[i]; s = battery_read(h);
        assert(s.percentage == soc[i] && s.percentage_valid == (soc[i] <= 100));
    }
    const uint16_t volts[] = {0,1,4098,5001,8191,8192,16382,16383};
    for (size_t i = 0; i < sizeof(volts)/sizeof(volts[0]); ++i) {
        battery_fixture(h); chip.reg[0xC4] = (uint8_t)((volts[i] >> 8) | 0xC0); chip.reg[0xC5] = (uint8_t)volts[i];
        s = battery_read(h); assert(s.voltage_mv == volts[i]);
        assert(s.voltage_valid == (volts[i] != 0 && volts[i] != 0x3FFF));
    }
    puts("PASS full-byte SOC 0/100/101/128/255 and raw 14-bit voltage endpoints without invented cell-voltage clamping");

    for (unsigned direction = 0; direction < 4; ++direction) {
        for (unsigned charge = 0; charge < 8; ++charge) {
            battery_fixture(h); chip.reg[0x01] = (uint8_t)(0x98 | (direction << 5) | charge);
            s = battery_read(h); const bool valid = direction < 3 && charge < 6;
            assert(s.current_direction == direction && s.charge_state == charge && s.charge_state_valid == valid);
            assert(s.charging == (valid && direction == 1 && charge <= 3));
            assert(s.charge_done == (valid && charge == 4));
        }
    }
    battery_fixture(h); chip.reg[0x01] = 0x94; s = battery_read(h);
    assert(s.charge_done && !s.charging && s.current_direction == 0 && s.charge_state == 4);
    battery_fixture(h); chip.reg[0x00] = 0x08; chip.reg[0x01] = 0x45; s = battery_read(h);
    assert(!s.vbus_present && !s.charging && !s.charge_done && s.current_direction == 2);
    assert(s.percentage_valid && s.voltage_valid);
    battery_fixture(h); chip.reg[0x00] = 0x20; s = battery_read(h);
    assert(s.presence_valid && !s.battery_present && s.vbus_present && !s.charging && !s.charge_done);
    assert(!s.percentage_valid && !s.voltage_valid && battery_read_count == 5);
    puts("PASS all charge/direction codes, real 0x94 done state, battery-only discharge and USB without battery");

    battery_fixture(h); chip.reg[0x68] = 0xFE; s = battery_read(h);
    assert(!s.presence_valid && s.battery_present && !s.charging && !s.charge_done);
    assert(!s.percentage_valid && !s.voltage_valid && !s.percentage && !s.voltage_mv && battery_read_count == 5);
    battery_fixture(h); chip.reg[0x0B] = 0xFB; s = battery_read(h);
    assert(!s.percentage_valid && !s.percentage && s.voltage_valid && !reads[0xA4]);
    battery_fixture(h); chip.reg[0xC0] = 0xFE; s = battery_read(h);
    assert(s.percentage_valid && !s.voltage_valid && !s.voltage_mv && !reads[0xC4] && !reads[0xC5]);
    puts("PASS disabled detection never trusts forced presence; disabled gauge/ADC remain disabled and are not sampled");

    for (size_t i = 0; i < sizeof(order); ++i) {
        battery_fixture(h); fail_read_reg = order[i];
        memset(&s, 0xA5, sizeof(s)); battery_output = &s;
        assert(tg29_get_battery_state(h, &s) == ESP_FAIL);
        battery_output = NULL;
        const uint8_t *bytes = (const uint8_t *)&s;
        for (size_t j = 0; j < sizeof(s); ++j) assert(bytes[j] == 0);
        assert(!write_count && !h->mutex->depth && battery_read_count == i + 1);
    }
    battery_fixture(h); fail_mutex_take = true; memset(&s, 0xA5, sizeof(s));
    assert(tg29_get_battery_state(h, &s) == ESP_ERR_TIMEOUT && !battery_read_count && !write_count && !h->mutex->depth);
    assert(!s.presence_valid && !s.voltage_valid && !s.percentage_valid && !s.voltage_mv);
    assert(tg29_get_battery_state(NULL, &s) == ESP_ERR_INVALID_ARG && !s.presence_valid);
    assert(tg29_get_battery_state(h, NULL) == ESP_ERR_INVALID_ARG);
    battery_fixture(h); assert(xSemaphoreTakeRecursive(h->mutex, 0) == pdTRUE);
    assert(tg29_get_battery_state(h, &s) == ESP_OK && h->mutex->depth == 1);
    assert(xSemaphoreGiveRecursive(h->mutex) == pdTRUE && !write_count);
    battery_read_only = false;
    puts("PASS every battery I2C failure clears the snapshot, mutex failure/NULL guards clear validity, recursive caller retains its lock");
}

static void usb_fixture(tg29_handle_t h)
{
    reset_chip();
    atomic_store(&h->source_allowed, false);
    chip.reg[0xE3] = 0xD1; /* Reserved high bits, default-current sink preference. */
    chip.reg[0xE7] = TG29_CC_UNATTACH_SNK;
    chip.reg[0x19] = 0xAF; /* All unrelated bits set, BOOST off. */
    chip.reg[0x1E] = 0x47; /* Wrong voltage/current; retain VBAT cutoff bits[3:2]. */
    chip.reg[0x00] = 0x08;
}
static bool all_zero(const void *p, size_t size)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < size; ++i) if (b[i]) return false;
    return true;
}
static void test_usb_and_charge(tg29_handle_t h)
{
    tg29_usb_power_state_t u;
    usb_fixture(h);
    assert(tg29_configure_cc_detection(h, true) == ESP_OK);
    assert(chip.reg[0xE3] == 0xD3 && !(chip.reg[0x19] & 0x10));
    assert(atomic_load(&h->source_allowed) && !writes[0x19]);
    chip.reg[0xE7] = TG29_CC_ATTACH_SRC;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    assert(chip.reg[0x1E] == 0x94 && chip.reg[0x19] == 0xBF);
    assert(tg29_get_usb_power_state(h, &u) == ESP_OK && u.source_allowed &&
           u.attached_as_source && !u.attached_as_sink && u.boost_enabled &&
           u.boost_mv == 5126 && u.boost_limit_ma == 500 && !u.vbus_good);
    const unsigned before_idempotent = write_count;
    chip.reg[0x00] |= 0x20; /* Our own valid boost may make VBUSGOOD high. */
    assert(tg29_set_otg_enabled(h, true) == ESP_OK && write_count == before_idempotent);
    assert(tg29_configure_cc_detection(h, false) == ESP_OK);
    assert(!atomic_load(&h->source_allowed) && chip.reg[0xE3] == 0xD1 && chip.reg[0x19] == 0xAF);
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE);
    assert(!writes[0x62] && !writes[0x61] && chip.reg[0x80] == 0xE7 && !writes[0x80]);
    puts("PASS CC policy starts OFF, verified DRP never boosts; source-only5.126V/500mA readback, own-VBUS idempotence and OFF retain unrelated settings");

    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned cc = 0; cc < 16; ++cc) {
        usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK);
        chip.reg[0xE3] = 0xD0 | mode; chip.reg[0xE7] = 0xC0 | cc; chip.reg[0x1E] = 0x94;
        const bool allowed = cc == 6 && (mode == TG29_CC_SOURCE || mode == TG29_CC_DRP);
        assert(tg29_set_otg_enabled(h, true) == (allowed ? ESP_OK : ESP_ERR_INVALID_STATE));
        assert(((chip.reg[0x19] & 0x10) != 0) == allowed);
        assert(tg29_get_usb_power_state(h, &u) == ESP_OK && u.cc_state == (tg29_cc_state_t)cc);
        assert(u.attached_as_source == allowed);
        assert(u.attached_as_sink == (cc == 3 && (mode == 1 || mode == 3)));
    }
    usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK);
    chip.reg[0xE7] = 6; chip.reg[0x00] = 0x28;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
    usb_fixture(h); chip.reg[0xE3] = 0xD3; chip.reg[0xE7] = 6;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
    puts("PASS all64 CC state/mode combinations: only stable local source with verified policy permits boost; external supply is refused");

    const uint8_t usb_reads[] = {0xE3, 0xE7, 0x19, 0x1E, 0x00, 0x0B};
    for (size_t i = 0; i < sizeof(usb_reads); ++i) {
        usb_fixture(h); memset(&u, 0xA5, sizeof(u));
        fail_read_reg = usb_reads[i];
        assert(tg29_get_usb_power_state(h, &u) == ESP_FAIL && all_zero(&u, sizeof(u)) && !write_count);
    }
    usb_fixture(h); memset(&u, 0xA5, sizeof(u)); fail_mutex_take = true;
    assert(tg29_get_usb_power_state(h, &u) == ESP_ERR_TIMEOUT && all_zero(&u, sizeof(u)));
    assert(tg29_get_usb_power_state(NULL, &u) == ESP_ERR_INVALID_ARG && all_zero(&u, sizeof(u)));
    assert(tg29_get_usb_power_state(h, NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS USB snapshot is read-only and cleared on I2C/mutex/argument failures");
    usb_fixture(h); chip.reg[0x0B] = 0xF5;
    assert(tg29_configure_cc_detection(h, true) == ESP_OK && chip.reg[0x0B] == 0xFC);
    assert((last_write[0x0B] & 1U) == 0 && (chip.reg[0x0B] & 0xF6U) == (0xF5U & 0xF6U));
    chip.reg[0xE7] = 6; chip.reg[0x0B] &= (uint8_t)~0x08U;
    assert(tg29_get_usb_power_state(h, &u) == ESP_OK && !u.cc_detection_enabled && !u.attached_as_source);
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
    for (unsigned drop = 0; drop < 2; ++drop) {
        usb_fixture(h);
        if (drop) drop_write_reg = 0x0B; else fail_write_reg = 0x0B;
        assert(tg29_configure_cc_detection(h, true) != ESP_OK && !atomic_load(&h->source_allowed));
        chip.reg[0xE7] = 6; chip.reg[0xE3] = 0xD3;
        assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
    }
    uint8_t events = 0;
    usb_fixture(h); chip.reg[0x4C] = 0xA7; chip.reg[0x49] = 0xF2; cc_arrive_on_ack = 0x40;
    assert(tg29_get_cc_events(h, &events) == ESP_OK && events == 0x20 && last_write[0x4C] == 0x20);
    assert(chip.reg[0x4C] == 0xC7 && chip.reg[0x49] == 0xF2 && !writes[0x49]);
    assert(tg29_get_cc_events(h, &events) == ESP_OK && events == 0x40 && chip.reg[0x4C] == 0x87);
    assert(tg29_get_cc_events(h, &events) == ESP_OK && events == 0 && writes[0x4C] == 2);
    fail_read_reg = 0x4C; fail_read_occurrence = reads[0x4C] + 1;
    assert(tg29_get_cc_events(h, &events) == ESP_FAIL && events == 0);
    chip.reg[0x4C] = 0x60; fail_write_reg = 0x4C; fail_write_occurrence = writes[0x4C] + 1;
    assert(tg29_get_cc_events(h, &events) == ESP_FAIL && events == 0x60 && chip.reg[0x4C] == 0x60);
    assert(tg29_get_cc_events(NULL, &events) == ESP_ERR_INVALID_ARG && !events);
    assert(tg29_get_cc_events(h, NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS CC detection explicitly enabled/verified without watchdog replay; stale disabled state refused; only observed CC IRQ bits acknowledged preserving key/fault latches");


    for (unsigned kind = 0; kind < 3; ++kind) {
        usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK); chip.reg[0xE7] = 6;
        if (kind == 0) fail_write_reg = 0x1E;
        if (kind == 1) drop_write_reg = 0x1E;
        if (kind == 2) { fail_read_reg = 0x1E; fail_read_occurrence = reads[0x1E] + 3; }
        assert(tg29_set_otg_enabled(h, true) != ESP_OK && !(chip.reg[0x19] & 0x10));
    }
    for (unsigned kind = 0; kind < 3; ++kind) {
        usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK);
        chip.reg[0xE7] = 6; chip.reg[0x1E] = 0x94;
        if (kind == 0) fail_write_reg = 0x19;
        if (kind == 1) drop_write_reg = 0x19;
        if (kind == 2) { fail_read_reg = 0x19; fail_read_occurrence = reads[0x19] + 3; }
        assert(tg29_set_otg_enabled(h, true) != ESP_OK && !(chip.reg[0x19] & 0x10));
    }
    for (unsigned cc = 0; cc < 16; ++cc) if (cc != 6) {
        usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK); chip.reg[0xE7] = 6;
        cc_change_read = 2; cc_change_to = (uint8_t)cc;
        assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
        usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK); chip.reg[0xE7] = 6;
        change_cc_after_boost = true; cc_after_boost = (uint8_t)cc;
        assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
    }
    usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK);
    chip.reg[0x19] |= 0x10; fail_write_reg = 0x19;
    assert(tg29_configure_cc_detection(h, false) == ESP_FAIL && !atomic_load(&h->source_allowed));
    /* Even stale DRP/attached state cannot bypass a failed OFF transaction. */
    chip.reg[0xE7] = 6;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
    usb_fixture(h); assert(tg29_configure_cc_detection(h, true) == ESP_OK); chip.reg[0x19] |= 0x10;
    fail_mutex_take = true;
    assert(tg29_configure_cc_detection(h, false) == ESP_ERR_TIMEOUT && !atomic_load(&h->source_allowed));
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10));
    assert(!h->mutex->depth);
    puts("PASS boost write/drop/readback faults, detach during enable and software-OFF despite stale CC or failed lock/bus transaction");

    tg29_charge_current_state_t c; bool changed;
    for (unsigned f = 0; f < 64; ++f) for (unsigned pre = 0; pre < 16; ++pre) {
        reset_chip(); chip.reg[0x62] = 0x80U | f; chip.reg[0x61] = 0xE0U | pre;
        const uint8_t old_enable = chip.reg[0x19];
        assert(tg29_limit_charge_current(h, 800, &c, &changed) == ESP_OK);
        assert(c.fast_charge_valid && c.precharge_valid);
        assert(c.fast_charge_ma == (f > 12 ? 768 : f * 64) && c.precharge_ma == (pre > 12 ? 768 : pre * 64));
        assert(changed == (f > 12 || pre > 12));
        assert(chip.reg[0x62] == (0x80U | (f > 12 ? 12 : f)));
        assert(chip.reg[0x61] == (0xE0U | (pre > 12 ? 12 : pre)));
        assert(writes[0x62] == (f > 12) && writes[0x61] == (pre > 12));
        assert(chip.reg[0x19] == old_enable && !writes[0x19] && !writes[0x64] && !writes[0x65]);
    }
    reset_chip(); chip.reg[0x61] = 2;
    assert(tg29_get_charge_current(h, &c) == ESP_OK && c.fast_charge_ma == 1024 && c.precharge_ma == 128 && !write_count);
    assert(tg29_limit_charge_current(h, 800, &c, &changed) == ESP_OK && c.fast_charge_ma == 768 && c.precharge_ma == 128 && changed);
    assert(tg29_limit_charge_current(h, 800, &c, &changed) == ESP_OK && !changed);
    reset_chip(); chip.reg[0x62] = 63;
    assert(tg29_get_charge_current(h, &c) == ESP_ERR_INVALID_RESPONSE && !c.fast_charge_valid && c.fast_charge_code == 63);
    puts("PASS1024 fast/precharge code pairs:800mA ceiling floors to768mA, lower values unchanged, reserved ICC repaired; charger enable/voltage/protection untouched");

    for (unsigned reg = 0x61; reg <= 0x62; ++reg) {
        reset_chip(); memset(&c, 0xA5, sizeof(c)); fail_read_reg = (int)reg;
        assert(tg29_get_charge_current(h, &c) == ESP_FAIL && all_zero(&c, sizeof(c)) && !write_count);
        reset_chip(); memset(&c, 0xA5, sizeof(c)); fail_read_reg = (int)reg; changed = true;
        assert(tg29_limit_charge_current(h, 800, &c, &changed) == ESP_FAIL && all_zero(&c, sizeof(c)) && !changed && !write_count);
        for (unsigned drop = 0; drop < 2; ++drop) {
            reset_chip(); chip.reg[0x61] = 15;
            if (drop) drop_write_reg = (int)reg; else fail_write_reg = (int)reg;
            assert(tg29_limit_charge_current(h, 800, &c, &changed) != ESP_OK && all_zero(&c, sizeof(c)));
            assert(changed == (reg == 0x61)); /* Fast charge commits first. */
        }
    }
    reset_chip(); memset(&c, 0xA5, sizeof(c)); fail_mutex_take = true; changed = true;
    assert(tg29_limit_charge_current(h, 800, &c, &changed) == ESP_ERR_TIMEOUT && !changed && all_zero(&c, sizeof(c)));
    assert(tg29_limit_charge_current(h, 63, &c, &changed) == ESP_ERR_INVALID_ARG);
    assert(tg29_limit_charge_current(h, 3009, &c, &changed) == ESP_ERR_INVALID_ARG);
    assert(tg29_configure_cc_detection(NULL, true) == ESP_ERR_INVALID_ARG);
    assert(tg29_set_otg_enabled(NULL, true) == ESP_ERR_INVALID_ARG);
    assert(!h->mutex->depth);
    puts("PASS charger I2C/readback/partial-commit/mutex/argument errors retain error and never claim unsafe limit success");
    for (unsigned v = 0; v < 256; ++v) {
        reset_chip(); chip.reg[0x19] = (uint8_t)v;
        assert(tg29_disable_charging(h) == ESP_OK && chip.reg[0x19] == (v & ~0x02U));
        assert(writes[0x19] == ((v & 0x02U) != 0) && !writes[0x1E] && !writes[0xE3]);
    }
    for (unsigned kind = 0; kind < 3; ++kind) {
        reset_chip(); chip.reg[0x19] = 0xFF;
        if (kind == 0) fail_read_reg = 0x19;
        if (kind == 1) fail_write_reg = 0x19;
        if (kind == 2) drop_write_reg = 0x19;
        assert(tg29_disable_charging(h) != ESP_OK && chip.reg[0x19] == 0xFF);
    }
    reset_chip(); chip.reg[0x19] = 0xFF;
    fail_read_reg = 0x19; fail_read_occurrence = 2;
    assert(tg29_disable_charging(h) == ESP_FAIL && chip.reg[0x19] == 0xFD);
    reset_chip(); fail_mutex_take = true;
    assert(tg29_disable_charging(h) == ESP_ERR_TIMEOUT && !write_count);
    assert(tg29_disable_charging(NULL) == ESP_ERR_INVALID_ARG);
    assert(!h->mutex->depth);
    puts("PASS256 charger-disable values preserve boost/other bits; failed bus/write/readback/lock cannot be reported as safe charging OFF");

}

static void verified_fixture(tg29_handle_t h, bool board_policy)
{
    usb_fixture(h); verification_handle = h;
    h->fixed_ts_10k = h->ts_bias_owned = h->vbus_adc_owned = false;
    assert(tg29_set_otg_fixed_ts_10k_policy(h, board_policy) == ESP_OK);
    chip.reg[0x50] = 0xD2; /* Reserved high bits + explicit fixed input, bias OFF. */
    chip.reg[0x56] = 0x3E; chip.reg[0x57] = 0x4C;
    chip.reg[0xC0] = 0x93; chip.reg[0xCD] = 0;
    chip.reg[0xCE] = 3; chip.reg[0xCF] = 0xE8; /* TS 500mV. */
    chip.reg[0xC6] = 0x14; chip.reg[0xC7] = 0x06; /* 5126mV. */
    assert(tg29_configure_cc_detection(h, true) == ESP_OK);
    chip.reg[0xE7] = 6;
}
static void revoke_during_settle(void)
{
    if (fake_ticks >= 40) tg29_revoke_otg_source_permission(verification_handle);
}
static void detach_during_settle(void)
{
    if (fake_ticks >= 40) chip.reg[0xE7] = 3;
}
static void disable_adc_during_settle(void)
{
    chip.reg[0xC0] &= (uint8_t)~4U;
}
static void read_error_during_settle(void)
{
    fail_read_reg = 0xC6;
    fail_read_occurrence = reads[0xC6] + 1;
    delay_hook = NULL;
}
static void epoch_cancel_during_settle(void)
{
    tg29_revoke_otg_source_permission(verification_handle);
    atomic_store(&verification_handle->source_allowed, true); /* A later ON cannot revive this older verification. */
    delay_hook = NULL;
}
static void hardware_boost_off_during_settle(void)
{
    chip.reg[0x19] &= (uint8_t)~0x10U;
}
static void test_verified_otg(tg29_handle_t h)
{
    tg29_usb_power_state_t p;
    tg29_otg_diagnostics_t d;
    verified_fixture(h, true);
    assert(tg29_set_otg_enabled(h, true) == ESP_OK && chip.reg[0x50] == 0xDE && h->ts_bias_owned);
    assert(chip.reg[0x56] == 0x3E && chip.reg[0x57] == 0x4C && !writes[0x56] && !writes[0x57]);
    const uint8_t old_charge = chip.reg[0x19] & ~0x10U;
    assert(tg29_verify_otg_output(h, 2000, &p) == ESP_OK && p.vbus_voltage_valid && p.vbus_mv == 5126);
    assert(p.snapshot_valid && fake_ticks == 1010 && h->vbus_adc_owned && chip.reg[0xC0] == 0x97);
    assert(tg29_set_otg_enabled(h, false) == ESP_OK && chip.reg[0x50] == 0xD2 && chip.reg[0xC0] == 0x93);
    assert(!h->ts_bias_owned && !h->vbus_adc_owned && chip.reg[0x19] == old_charge);
    puts("PASS opted fixed10k bias is50uA while boosting,1000ms+two-sample VBUS proof precedes success; OFF restores owned TS/ADC and preserves charger/protection");

    for (unsigned policy = 0; policy < 2; ++policy) {
        verified_fixture(h, policy != 0);
        if (policy) chip.reg[0x50] &= (uint8_t)~0x10U; /* Actual NTC mode. */
        const uint8_t original = chip.reg[0x50];
        assert(tg29_set_otg_enabled(h, true) == ESP_OK && chip.reg[0x50] == original && !writes[0x50]);
        chip.reg[0xC0] |= 4U; /* Existing ADC owner must survive OFF. */
        assert(tg29_verify_otg_output(h, 2000, &p) == ESP_OK && !h->vbus_adc_owned);
        assert(tg29_set_otg_enabled(h, false) == ESP_OK && chip.reg[0x50] == original && (chip.reg[0xC0] & 4U));
    }
    verified_fixture(h, true); chip.reg[0x50] = 0xDE;
    assert(tg29_configure_cc_detection(h, false) == ESP_OK && chip.reg[0x50] == 0xD2);
    verified_fixture(h, false); chip.reg[0x50] = 0xDE;
    assert(tg29_configure_cc_detection(h, false) == ESP_OK && chip.reg[0x50] == 0xDE);
    puts("PASS opt-out and actual NTC remain unchanged; only explicit fixed10k board recognizes known warm-reset bias pattern");

    for (unsigned drop = 0; drop < 2; ++drop) {
        verified_fixture(h, true);
        if (drop) drop_write_reg = 0x50; else fail_write_reg = 0x50;
        assert(tg29_set_otg_enabled(h, true) != ESP_OK && !(chip.reg[0x19] & 0x10U));
        assert(chip.reg[0x50] == 0xD2 && !h->ts_bias_owned);
        verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        if (drop) drop_write_reg = 0xC0; else fail_write_reg = 0xC0;
        assert(tg29_verify_otg_output(h, 2000, &p) != ESP_OK && !(chip.reg[0x19] & 0x10U));
        assert(chip.reg[0x50] == 0xD2 && chip.reg[0xC0] == 0x93 && !h->vbus_adc_owned);
    }
    verified_fixture(h, true); chip.reg[0x57] = 251; /* 502mV minimum excludes expected500mV. */
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !writes[0x50] && !writes[0x57]);
    puts("PASS dropped/error TS or ADC writes fail startup and restore state; work thresholds never widened");

    const uint16_t bad[] = {0, 0x3FFF, 4700, 5600};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        verified_fixture(h, true); chip.reg[0xC6] = bad[i] >> 8; chip.reg[0xC7] = bad[i] & 0xFF;
        assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        assert(tg29_verify_otg_output(h, 160, &p) == ESP_ERR_TIMEOUT && !(chip.reg[0x19] & 0x10U));
        assert(p.vbus_voltage_valid == (i >= 2));
        assert(!h->ts_bias_owned && !h->vbus_adc_owned);
    }
    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK); delay_hook = disable_adc_during_settle;
    assert(tg29_verify_otg_output(h, 160, &p) == ESP_ERR_TIMEOUT && !p.vbus_voltage_valid);
    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK); delay_hook = detach_during_settle;
    assert(tg29_verify_otg_output(h, 2000, &p) == ESP_ERR_INVALID_STATE && fake_ticks == 40 && !(chip.reg[0x19] & 0x10U));
    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK); delay_hook = revoke_during_settle;
    assert(tg29_verify_otg_output(h, 2000, &p) == ESP_ERR_INVALID_STATE && fake_ticks == 40 && !(chip.reg[0x19] & 0x10U));
    assert(!h->ts_bias_owned && !h->vbus_adc_owned);
    puts("PASS zero/fullscale/out-of-window/disabled ADC cannot prove5V; detach and immediate atomic OFF abort settling without resurrecting boost");

    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    assert(tg29_verify_otg_output(h, 2000, &p) == ESP_OK);
    fail_write_reg = 0x19; fail_write_occurrence = writes[0x19] + 1;
    assert(tg29_set_otg_enabled(h, false) == ESP_FAIL && h->ts_bias_owned && h->vbus_adc_owned);
    assert(chip.reg[0x50] == 0xDE && chip.reg[0xC0] == 0x97);
    assert(tg29_set_otg_enabled(h, false) == ESP_OK && !h->ts_bias_owned && !h->vbus_adc_owned);
    for (unsigned reg = 0; reg < 2; ++reg) {
        verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        assert(tg29_verify_otg_output(h, 2000, &p) == ESP_OK);
        fail_write_reg = reg ? 0xC0 : 0x50;
        fail_write_occurrence = writes[fail_write_reg] + 1;
        assert(tg29_set_otg_enabled(h, false) == ESP_FAIL && !(chip.reg[0x19] & 0x10U));
        assert(reg ? h->vbus_adc_owned : h->ts_bias_owned);
        assert(tg29_set_otg_enabled(h, false) == ESP_OK && !h->ts_bias_owned && !h->vbus_adc_owned);
    }
    puts("PASS failed boost-OFF retains bias/ADC until verified OFF; individual restore errors retain owner and recover on retry");

    verified_fixture(h, true); chip.reg[0x08] = 3; chip.reg[0x48] = 7; chip.reg[0x4B] = 3;
    unsigned before = write_count;
    assert(tg29_get_otg_diagnostics(h, &d) == ESP_OK && d.ts_voltage_valid && d.ts_half_mv == 1000);
    assert(d.fault_raw == 3 && d.irq0_raw == 7 && d.irq3_raw == 3 && write_count == before);
    chip.reg[0xCD] = 2;
    assert(tg29_get_otg_diagnostics(h, &d) == ESP_OK && !d.ts_voltage_valid && !d.ts_half_mv);
    chip.reg[0xCD] = 0; chip.reg[0xC0] &= (uint8_t)~2U;
    assert(tg29_get_otg_diagnostics(h, &d) == ESP_OK && !d.ts_voltage_valid);
    fail_read_reg = 0x48; fail_read_occurrence = reads[0x48] + 1;
    memset(&d, 0xA5, sizeof(d)); assert(tg29_get_otg_diagnostics(h, &d) == ESP_FAIL && all_zero(&d, sizeof(d)));
    assert(!writes[0x08] && !writes[0x48] && !writes[0x4B] && !writes[0xCD]);
    puts("PASS slow diagnostics only read fault/TS state; disabled or non-TS selector is invalid; bus failure clears entire snapshot without ACK/mux changes");
    const uint8_t diagnostic_reads[] = {0xE3,0xE7,0x19,0x1E,0x00,0x0B,0xC0,0x50,0x56,0x57,0x08,0x48,0x4B,0xCD,0xCE,0xCF};
    for (size_t i = 0; i < sizeof(diagnostic_reads); ++i) {
        verified_fixture(h, true);
        fail_read_reg = diagnostic_reads[i]; fail_read_occurrence = reads[fail_read_reg] + 1;
        memset(&d, 0xA5, sizeof(d));
        assert(tg29_get_otg_diagnostics(h, &d) == ESP_FAIL && all_zero(&d, sizeof(d)));
    }
    for (unsigned which = 0; which < 3; ++which) {
        verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        if (which == 0) fail_mutex_take = true;
        if (which == 1) delay_hook = read_error_during_settle;
        if (which == 2) { fail_read_reg = 0xC0; fail_read_occurrence = reads[0xC0] + 2; }
        assert(tg29_verify_otg_output(h, 2000, &p) != ESP_OK && !(chip.reg[0x19] & 0x10U));
        assert(!p.snapshot_valid && all_zero(&p, sizeof(p)));
        assert(!h->ts_bias_owned && !h->vbus_adc_owned);
    }
    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK); delay_hook = epoch_cancel_during_settle;
    assert(tg29_verify_otg_output(h, 2000, &p) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10U));
    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK); delay_hook = hardware_boost_off_during_settle;
    assert(tg29_verify_otg_output(h, 2000, &p) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10U));
    for (unsigned reg = 0; reg < 2; ++reg) {
        verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        assert(tg29_verify_otg_output(h, 2000, &p) == ESP_OK);
        drop_write_reg = reg ? 0xC0 : 0x50;
        drop_write_occurrence = writes[drop_write_reg] + 1;
        assert(tg29_set_otg_enabled(h, false) == ESP_ERR_INVALID_RESPONSE && !(chip.reg[0x19] & 0x10U));
        assert(reg ? h->vbus_adc_owned : h->ts_bias_owned);
        assert(tg29_set_otg_enabled(h, false) == ESP_OK && !h->ts_bias_owned && !h->vbus_adc_owned);
    }
    puts("PASS every diagnostic read failure invalidates; verification lock/read errors shut down; OFF epoch and hardware-off abort; dropped restore writes retain/recover ownership");
    verified_fixture(h, false);
    assert(tg29_configure_cc_detection(h, false) == ESP_OK);
    revoke_on_mode_write = true;
    assert(tg29_configure_cc_detection(h, true) == ESP_ERR_INVALID_STATE);
    chip.reg[0xE7] = 6;
    assert(tg29_get_usb_power_state(h, &p) == ESP_OK && !p.source_allowed);
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x19] & 0x10U));
    puts("PASS GUI OFF during DRP configure cancels its authorization epoch; stale verified mode cannot regain source permission");


    verified_fixture(h, false); h->fixed_ts_10k = false;
    verification_handle = NULL;
}

static void remove_pc_during_input(void)
{
    if (fake_ticks >= 20) chip.reg[0x00] &= (uint8_t)~0x20U;
}
static void test_consistent_probe(tg29_handle_t h)
{
    tg29_otg_verify_result_t r;
    verified_fixture(h, true); chip.reg[0xC6] = chip.reg[0xC7] = 0;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    assert(tg29_verify_otg_output_ex(h, 160, &r) == ESP_ERR_TIMEOUT);
    assert(r.verify_error == ESP_ERR_TIMEOUT && r.cleanup_error == ESP_OK);
    assert(r.pre_cleanup_diag_attempted && r.pre_cleanup_diag_valid && r.diag_error == ESP_OK);
    assert(r.pre_cleanup_diag.power.snapshot_valid && r.pre_cleanup_diag.power.boost_enabled);
    assert(r.pre_cleanup_diag.power.module_enable_raw == (uint8_t)(chip.reg[0x19] | 0x10U));
    assert(r.pre_cleanup_diag.ts_config_raw == 0xDE && r.pre_cleanup_diag.ts_voltage_valid && r.pre_cleanup_diag.ts_half_mv == 1000);
    assert((r.pre_cleanup_diag.power.adc_enable_raw & 4U) && r.pre_cleanup_diag.power.vbus_raw == 0 && !r.pre_cleanup_diag.power.vbus_voltage_valid);
    assert(chip.reg[0x50] == 0xD2 && !(chip.reg[0x19] & 0x10U) && chip.reg[0xC0] == 0x93);
    puts("PASS voltage timeout retains complete pre-OFF R19/TS50/bias/ADC/fault phase while actual OFF restores idle settings; raw zero remains unverified");

    verified_fixture(h, true); chip.reg[0xC6] = chip.reg[0xC7] = 0;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    fail_read_reg = 0x50; fail_read_occurrence = reads[0x50] + 1;
    assert(tg29_verify_otg_output_ex(h, 160, &r) == ESP_ERR_TIMEOUT);
    assert(r.pre_cleanup_diag_attempted && !r.pre_cleanup_diag_valid && r.diag_error == ESP_FAIL);
    assert(all_zero(&r.pre_cleanup_diag, sizeof(r.pre_cleanup_diag)) && r.cleanup_error == ESP_OK);
    verified_fixture(h, true); chip.reg[0xC6] = chip.reg[0xC7] = 0;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    fail_write_reg = 0x19; fail_write_occurrence = writes[0x19] + 1;
    assert(tg29_verify_otg_output_ex(h, 160, &r) == ESP_ERR_TIMEOUT);
    assert(r.verify_error == ESP_ERR_TIMEOUT && r.cleanup_error == ESP_FAIL && h->ts_bias_owned);
    assert(tg29_set_otg_enabled(h, false) == ESP_OK);
    puts("PASS failed probe read invalidates whole diagnostic without replacing timeout cause; cleanup failure remains separately reportable/retryable");

    for (unsigned event = 0; event < 3; ++event) {
        verified_fixture(h, true); chip.reg[0xC6] = chip.reg[0xC7] = 0;
        assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        if (event == 0) delay_hook = revoke_during_settle;
        if (event == 1) delay_hook = detach_during_settle;
        if (event == 2) { fail_read_reg = 0xC6; fail_read_occurrence = reads[0xC6] + 1; }
        assert(tg29_verify_otg_output_ex(h, 160, &r) != ESP_OK);
        assert(!r.pre_cleanup_diag_attempted && !r.pre_cleanup_diag_valid && !reads[0x48]);
    }
    verified_fixture(h, true); chip.reg[0xC6] = chip.reg[0xC7] = 0;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    cancel_on_read_reg = 0x50;
    const unsigned prior56 = reads[0x56];
    assert(tg29_verify_otg_output_ex(h, 160, &r) == ESP_ERR_TIMEOUT);
    assert(r.pre_cleanup_diag_attempted && !r.pre_cleanup_diag_valid && r.diag_error == ESP_ERR_INVALID_STATE);
    assert(reads[0x56] == prior56 && !reads[0x48] && !(chip.reg[0x19] & 0x10U));
    puts("PASS OFF/role/read failures skip extra diagnostics; OFF arriving during probe prevents the next diagnostic transaction and discards partial data");
}
static void input_probe_fixture(tg29_handle_t h)
{
    usb_fixture(h); verification_handle = h;
    h->fixed_ts_10k = h->ts_bias_owned = h->vbus_adc_owned = false;
    chip.reg[0x00] = 0x28; chip.reg[0xC0] = 0x93;
    chip.reg[0x50] = 0x12; chip.reg[0xC6] = 0x14; chip.reg[0xC7] = 0x06;
}
static void test_input_probe(tg29_handle_t h)
{
    tg29_vbus_input_probe_result_t r;
    input_probe_fixture(h);
    assert(tg29_probe_vbus_input(h, 250, &r) == ESP_OK && r.attempted && r.cleanup_error == ESP_OK);
    assert(fake_ticks == 250 && r.trace_count == 2 && r.first_valid_elapsed_ms == 100 && r.power.snapshot_valid && r.power.vbus_voltage_valid && r.power.vbus_mv == 5126 && r.power.vbus_raw == 5126);
    assert(r.power.vbus_adc_high_raw == 0x14 && r.power.vbus_adc_low_raw == 0x06 && r.power.adc_enable_raw == 0x97);
    assert(chip.reg[0xC0] == 0x93 && !h->vbus_adc_owned && write_count == 2 && writes[0xC0] == 2);
    assert(!writes[0x19] && !writes[0x50] && !writes[0xE3] && !writes[0xCD] && !writes[0x61] && !writes[0x62]);
    input_probe_fixture(h); chip.reg[0xC0] |= 4U;
    assert(tg29_probe_vbus_input(h, 250, &r) == ESP_OK && !write_count && chip.reg[0xC0] == 0x97);
    const uint16_t raw[] = {0,0x3FFF,7000};
    for (unsigned n = 0; n < 3; ++n) {
        input_probe_fixture(h); chip.reg[0xC6] = raw[n] >> 8; chip.reg[0xC7] = raw[n] & 0xFF;
        assert(tg29_probe_vbus_input(h, 250, &r) == ESP_OK && r.power.vbus_raw == raw[n]);
        assert(r.power.vbus_voltage_valid == (n == 2) && r.cleanup_error == ESP_OK);
    }
    puts("PASS external-input baseline only toggles owned C0bit2, settles without mutex, preserves prior ADC owner and raw zero/fullscale/out-of-window readings without assuming5V");
    for (unsigned n = 0; n < 3; ++n) {
        input_probe_fixture(h);
        if (n == 0) chip.reg[0x00] &= (uint8_t)~0x20U;
        if (n == 1) chip.reg[0x19] |= 0x10U;
        if (n == 2) { atomic_store(&h->authorized_epoch, atomic_load(&h->source_epoch)); atomic_store(&h->source_allowed, true); }
        assert(tg29_probe_vbus_input(h, 250, &r) == ESP_ERR_INVALID_STATE && !r.attempted && !write_count);
    }
    input_probe_fixture(h); delay_hook = remove_pc_during_input;
    assert(tg29_probe_vbus_input(h, 250, &r) == ESP_ERR_INVALID_STATE && fake_ticks == 20 && r.cleanup_error == ESP_OK && chip.reg[0xC0] == 0x93);
    for (unsigned n = 0; n < 2; ++n) {
        input_probe_fixture(h);
        if (n) { drop_write_reg = 0xC0; drop_write_occurrence = 2; }
        else { fail_write_reg = 0xC0; fail_write_occurrence = 2; }
        assert(tg29_probe_vbus_input(h, 250, &r) == ESP_OK && r.cleanup_error != ESP_OK && h->vbus_adc_owned);
        assert(tg29_configure_cc_detection(h, false) == ESP_OK && !h->vbus_adc_owned && chip.reg[0xC0] == 0x93);
    }
    input_probe_fixture(h); fail_read_reg = 0xC6;
    assert(tg29_probe_vbus_input(h, 250, &r) == ESP_FAIL && !r.power.snapshot_valid && r.cleanup_error == ESP_OK && !h->vbus_adc_owned);
    assert(tg29_probe_vbus_input(h, 99, &r) == ESP_ERR_INVALID_ARG && !r.attempted);
    assert(tg29_probe_vbus_input(h, 2501, &r) == ESP_ERR_INVALID_ARG);
    input_probe_fixture(h); verification_handle = NULL;
    puts("PASS input baseline rejects source/boost/missingPC, aborts PC removal, reports independent restore failures and recovers ownership on checked-OFF retry");
}

static void disable_detection_during_settle(void)
{
    if (fake_ticks >= 40) chip.reg[0x0B] &= (uint8_t)~8U;
}
static void test_untrusted_control_settle(tg29_handle_t h)
{
    tg29_otg_verify_result_t r;
    verified_fixture(h, true); chip.reg[0xC6] = 0x3F; chip.reg[0xC7] = 0xFC;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    const unsigned before = write_count;
    assert(tg29_settle_otg_control(h, 100, &r) == ESP_OK && r.verify_error == ESP_OK && r.cleanup_error == ESP_OK);
    assert(fake_ticks == 100 && r.power.snapshot_valid && r.power.boost_enabled && !r.power.vbus_voltage_valid);
    assert(!r.pre_cleanup_diag_attempted && !r.pre_cleanup_diag_valid && !h->vbus_adc_owned && write_count == before && !writes[0xC0]);
    assert(!reads[0xC6] && !reads[0xC7]);
    assert(tg29_set_otg_enabled(h, false) == ESP_OK);

    verified_fixture(h, true); chip.reg[0xC0] |= 4U; chip.reg[0xC6] = 0x3F; chip.reg[0xC7] = 0xFC;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    assert(tg29_settle_otg_control(h, 100, &r) == ESP_OK && r.power.vbus_raw == 0x3FFC && !r.power.vbus_voltage_valid && !r.power.vbus_mv);
    assert(!writes[0xC0] && !h->vbus_adc_owned);
    assert(tg29_verify_otg_output_ex(h, 160, &r) == ESP_ERR_TIMEOUT && r.pre_cleanup_diag_valid);
    assert(r.pre_cleanup_diag.power.vbus_raw == 0x3FFC && !r.pre_cleanup_diag.power.vbus_voltage_valid);
    puts("PASS untrusted-ADC control settle checks100ms of fresh CC/BOOST without ADC setup or voltage proof; trusted verifier still rejects raw3FFC/out-of-table range");

    for (unsigned event = 0; event < 5; ++event) {
        verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        if (event == 0) delay_hook = revoke_during_settle;
        if (event == 1) delay_hook = detach_during_settle;
        if (event == 2) delay_hook = hardware_boost_off_during_settle;
        if (event == 3) delay_hook = disable_detection_during_settle;
        if (event == 4) { fail_read_reg = 0x19; fail_read_occurrence = reads[0x19] + 1; }
        assert(tg29_settle_otg_control(h, 100, &r) != ESP_OK && r.cleanup_error == ESP_OK);
        assert(!(chip.reg[0x19] & 0x10U) && !h->ts_bias_owned && !h->vbus_adc_owned && !r.pre_cleanup_diag_attempted);
    }
    const uint16_t raw[] = {0,8191,8192,0x3FFC,0x3FFF};
    for (unsigned i = 0; i < sizeof(raw)/sizeof(raw[0]); ++i) {
        input_probe_fixture(h); chip.reg[0xC6] = raw[i] >> 8; chip.reg[0xC7] = raw[i] & 0xFF;
        tg29_vbus_input_probe_result_t b;
        assert(tg29_probe_vbus_input(h, 250, &b) == ESP_OK && b.power.vbus_raw == raw[i]);
        assert(b.power.vbus_voltage_valid == (raw[i] > 0 && raw[i] <= 8191));
        if (!b.power.vbus_voltage_valid) assert(!b.power.vbus_mv);
    }
    assert(tg29_settle_otg_control(h, 99, &r) == ESP_ERR_INVALID_ARG);
    assert(tg29_settle_otg_control(h, 301, &r) == ESP_ERR_INVALID_ARG);
    input_probe_fixture(h); verification_handle = NULL;
    puts("PASS control-only settle fails closed on OFF/role/detector/boost/read loss; published VBUS validity obeys documented0..1FFF and never calls3FFC a16V measurement");
}

static unsigned baseline_valid_at;
static void delayed_input_adc_ready(void)
{
    if (fake_ticks >= baseline_valid_at) { chip.reg[0xC6] = 0x14; chip.reg[0xC7] = 0x06; }
}
static void input_source_on_during_probe(void)
{
    if (fake_ticks >= 40) {
        atomic_store(&verification_handle->authorized_epoch, atomic_load(&verification_handle->source_epoch));
        atomic_store(&verification_handle->source_allowed, true);
    }
}
static void test_continuous_input_probe(tg29_handle_t h)
{
    tg29_vbus_input_probe_result_t r;
    const unsigned expected[] = {100,250,500,1000,2000};
    input_probe_fixture(h); chip.reg[0xC6] = 0x3F; chip.reg[0xC7] = 0xFC;
    assert(tg29_probe_vbus_input(h, 2000, &r) == ESP_OK && r.cleanup_error == ESP_OK);
    assert(fake_ticks == 2000 && r.trace_count == 5 && !r.first_valid_elapsed_ms);
    for (unsigned n = 0; n < 5; ++n) {
        assert(r.trace[n].elapsed_ms == expected[n] && r.trace[n].adc_enable_raw == 0x97);
        assert(r.trace[n].vbus_raw == 0x3FFC && !r.trace[n].vbus_voltage_valid && !r.trace[n].vbus_mv);
    }
    assert(r.power.vbus_raw == 0x3FFC && !r.power.vbus_voltage_valid && chip.reg[0xC0] == 0x93 && !h->vbus_adc_owned);
    assert(write_count == 2 && writes[0xC0] == 2);
    for (unsigned ready = 500; ready <= 1000; ready += 500) {
        input_probe_fixture(h); chip.reg[0xC6] = 0x3F; chip.reg[0xC7] = 0xFC;
        baseline_valid_at = ready; delay_hook = delayed_input_adc_ready;
        assert(tg29_probe_vbus_input(h, 2000, &r) == ESP_OK && r.first_valid_elapsed_ms == ready && r.trace_count == 5);
        assert(r.power.vbus_voltage_valid && r.power.vbus_mv == 5126);
        for (unsigned n = 0; n < 5; ++n) {
            assert(r.trace[n].elapsed_ms == expected[n]);
            assert(r.trace[n].vbus_voltage_valid == (expected[n] >= ready));
            assert(r.trace[n].vbus_raw == (expected[n] >= ready ? 5126 : 0x3FFC));
        }
    }
    puts("PASS input ADC remains continuously enabled for2s with exactly5 bounded raw milestones; readiness at500/1000ms is distinguished from persistent3FFC");
    for (unsigned event = 0; event < 3; ++event) {
        input_probe_fixture(h);
        delay_hook = event == 0 ? revoke_during_settle : event == 1 ? input_source_on_during_probe : remove_pc_during_input;
        assert(tg29_probe_vbus_input(h, 2000, &r) == ESP_ERR_INVALID_STATE && fake_ticks <= 40 && r.trace_count == 0);
        assert(r.cleanup_error == ESP_OK && chip.reg[0xC0] == 0x93 && !h->vbus_adc_owned);
    }
    input_probe_fixture(h); assert(tg29_probe_vbus_input(h, 100, &r) == ESP_OK && r.trace_count == 1 && fake_ticks == 100);
    input_probe_fixture(h); assert(tg29_probe_vbus_input(h, 2500, &r) == ESP_OK && r.trace_count == 5 && fake_ticks == 2500);
    input_probe_fixture(h); chip.reg[0x00] &= (uint8_t)~0x20U;
    assert(tg29_probe_vbus_input(h, 2000, &r) == ESP_ERR_INVALID_STATE && !fake_ticks && !r.attempted && !write_count);
    verification_handle = NULL;
    puts("PASS input observation cancels on OFF epoch/sourceON/PC removal, restores only owned ADC, bounds100..2500ms and returns immediately without external input");
}

static void gate_loss_during_settle(void)
{
    if (fake_ticks >= 40) chip.reg[0x13] &= (uint8_t)~1U;
}
static void stale_pc_then_low_output(void)
{
    if (fake_ticks >= 800) { chip.reg[0xC6] = 0x03; chip.reg[0xC7] = 0xE8; }
}
static void test_rbfet_gate(tg29_handle_t h)
{
    tg29_usb_power_state_t p;
    for (unsigned v = 0; v < 256; ++v) {
        verified_fixture(h, false); chip.reg[0x1E] = 0x94; chip.reg[0x13] = (uint8_t)v;
        const unsigned order_start = write_order_count;
        assert(tg29_set_otg_enabled(h, true) == ESP_OK && chip.reg[0x13] == (uint8_t)(v | 1U));
        assert(tg29_get_usb_power_state(h, &p) == ESP_OK && p.rbfet_enabled && p.rbfet_control_raw == (uint8_t)(v | 1U));
        assert((chip.reg[0x19] & ~0x10U) == (0xAF & ~0x10U));
        if (!(v & 1U)) assert(write_order[order_start] == 0x13 && write_order[order_start+1] == 0x19);
        assert(tg29_set_otg_enabled(h, false) == ESP_OK && chip.reg[0x13] == (uint8_t)(v & ~1U));
        assert(!(chip.reg[0x19] & 0x10U));
    }
    puts("PASS all256 R13 bytes preserve unrelated bits: checked source-gate precedes BOOST; OFF clears both and fresh snapshot exposes gate state");
    for (unsigned kind = 0; kind < 4; ++kind) {
        verified_fixture(h, true); chip.reg[0x1E] = 0x94; chip.reg[0x13] = 0xA4;
        if (kind == 0) { fail_read_reg = 0x13; fail_read_occurrence = reads[0x13] + 1; }
        if (kind == 1) fail_write_reg = 0x13;
        if (kind == 2) drop_write_reg = 0x13;
        if (kind == 3) { fail_read_reg = 0x13; fail_read_occurrence = reads[0x13] + 3; }
        assert(tg29_set_otg_enabled(h, true) != ESP_OK && !(chip.reg[0x13] & 1U) && !(chip.reg[0x19] & 0x10U));
        assert(chip.reg[0x13] == 0xA4 && !h->ts_bias_owned);
    }
    for (unsigned reg = 0; reg < 2; ++reg) for (unsigned kind = 0; kind < 3; ++kind) {
        verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
        assert(tg29_verify_otg_output(h, 2000, &p) == ESP_OK);
        const int target = reg ? 0x19 : 0x13;
        if (kind == 0) { fail_read_reg = target; fail_read_occurrence = reads[target] + 1; }
        if (kind == 1) { fail_write_reg = target; fail_write_occurrence = writes[target] + 1; }
        if (kind == 2) { drop_write_reg = target; drop_write_occurrence = writes[target] + 1; }
        assert(tg29_set_otg_enabled(h, false) != ESP_OK);
        assert(reg ? !(chip.reg[0x13] & 1U) : !(chip.reg[0x19] & 0x10U));
        assert(h->ts_bias_owned && h->vbus_adc_owned && chip.reg[0x50] == 0xDE && chip.reg[0xC0] == 0x97);
        assert(tg29_set_otg_enabled(h, false) == ESP_OK && !(chip.reg[0x13] & 1U) && !(chip.reg[0x19] & 0x10U));
        assert(!h->ts_bias_owned && !h->vbus_adc_owned);
    }
    puts("PASS gate read/write/drop/readback failures prevent enable; either OFF path failing still clears the other, retains TS/ADC owner and recovers on retry");
    verified_fixture(h, true); chip.reg[0x13] = 0xFF; chip.reg[0x19] = 0xFF;
    assert(tg29_configure_cc_detection(h, false) == ESP_OK && chip.reg[0x13] == 0xFE && chip.reg[0x19] == 0xEF && !source_permission_current(h));
    verified_fixture(h, true); lose_gate_after_boost = true;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x13] & 1U) && !(chip.reg[0x19] & 0x10U));
    for (unsigned mode = 0; mode < 2; ++mode) {
        verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK); delay_hook = gate_loss_during_settle;
        tg29_otg_verify_result_t r;
        const esp_err_t e = mode ? tg29_verify_otg_output_ex(h, 2000, &r) : tg29_settle_otg_control(h, 100, &r);
        assert(e == ESP_ERR_INVALID_STATE && r.cleanup_error == ESP_OK && fake_ticks == 40);
        assert(!(chip.reg[0x13] & 1U) && !(chip.reg[0x19] & 0x10U) && !r.pre_cleanup_diag_attempted);
    }
    puts("PASS default OFF clears inherited RBFET+BOOST; enable postcheck and trusted/untrusted control sampling reject lost source gate immediately");
    verified_fixture(h, false); chip.reg[0x1E] = 0x94; good_after_gate = true;
    assert(tg29_set_otg_enabled(h, true) == ESP_OK && (chip.reg[0x00] & 0x20U));
    assert((chip.reg[0x13] & 1U) && (chip.reg[0x19] & 0x10U));
    assert(tg29_set_otg_enabled(h, false) == ESP_OK);
    verified_fixture(h, false); chip.reg[0x1E] = 0x94; chip.reg[0x00] |= 0x20U;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !writes[0x13] && !writes[0x19]);
    verified_fixture(h, false); chip.reg[0x1E] = 0x94; detach_after_gate = true;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !(chip.reg[0x13] & 1U) && !(chip.reg[0x19] & 0x10U));
    puts("PASS VBUSGOOD rising only after our gate opening is not misclassified as external; preexisting external GOOD and post-gate role loss remain refused");

    tg29_otg_verify_result_t r;
    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK); delay_hook = stale_pc_then_low_output;
    assert(tg29_verify_otg_output_ex(h, 2000, &r) == ESP_ERR_TIMEOUT && fake_ticks == 2000);
    assert(r.pre_cleanup_diag_valid && r.pre_cleanup_diag.power.vbus_mv == 1000);
    verified_fixture(h, true); assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    assert(tg29_verify_otg_output_ex(h, 2000, &r) == ESP_OK && fake_ticks == 1010);
    assert(r.power.rbfet_enabled && r.power.boost_enabled && r.power.vbus_mv == 5126);
    assert(tg29_set_otg_enabled(h, false) == ESP_OK);
    verified_fixture(h, false); verification_handle = NULL;
    puts("PASS strict verification rejects an early stale in-range PC sample that becomes low at800ms; stable good output waits1000ms floor plus second sample; untrusted100ms unchanged");
}

static void set_vbus_raw(uint16_t raw)
{
    chip.reg[0xC6] = raw >> 8;
    chip.reg[0xC7] = raw & 0xFFU;
}

static void manual_fixture(tg29_handle_t h, uint8_t cc)
{
    verified_fixture(h, true);
    chip.reg[0xE7] = cc;
    chip.reg[0x13] = 0xA4;
    chip.reg[0x1E] = 0x94;
    set_vbus_raw(0);
    assert(tg29_configure_manual_otg(h) == ESP_OK);
    assert(h->manual_otg && !(chip.reg[0x0B] & 0x08U) && (chip.reg[0xE3] & 3U) == TG29_CC_DISABLED);
    assert(!writes[0x13] && !writes[0x19]);
}

static void assert_manual_off(tg29_handle_t h, uint8_t adc)
{
    assert(chip.reg[0x13] == 0xA4 && chip.reg[0x19] == 0xAF && chip.reg[0xC0] == adc);
    assert(chip.reg[0x50] == 0xD2 && !h->vbus_adc_owned && !h->ts_bias_owned);
    assert(!atomic_load(&h->manual_start_active) && !h->manual_output_started && !h->mutex->depth);
}

static void late_vbus_rise(void)
{
    if (fake_ticks >= 1000) set_vbus_raw(801);
}

static void vbus_present_at_final_gate_guard(void)
{
    if (fake_ticks == 1010) vbus_good_on_read = reads[0x00] + 4;
}

static void interleaved_manual_off(void)
{
    if (fake_ticks != 10) return;
    tg29_handle_t h = verification_handle;
    assert(tg29_configure_manual_otg(h) == ESP_ERR_INVALID_STATE);
    assert(tg29_configure_cc_detection(h, true) == ESP_ERR_INVALID_STATE);
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE);
    assert(tg29_set_otg_enabled(h, false) == ESP_OK);
}

static void enable_detector_during_manual(void)
{
    if (fake_ticks >= 40) chip.reg[0x0B] |= 8U;
}

static void test_manual_otg(tg29_handle_t h)
{
    const uint8_t eligible[] = {TG29_CC_UNATTACH_SNK, TG29_CC_UNATTACH_SRC, TG29_CC_ATTACH_SRC};
    for (size_t i = 0; i < sizeof(eligible); ++i) {
        manual_fixture(h, eligible[i]);
        const bool borrowed_adc = i == 1;
        if (borrowed_adc) { chip.reg[0xC0] |= 4U; set_vbus_raw(800); }
        tg29_usb_power_state_t p;
        assert(tg29_get_usb_power_state(h, &p) == ESP_OK && p.otg_role_ready && p.source_allowed);
        assert(!p.attached_as_source && !p.attached_as_sink && !p.cc_detection_enabled);
        assert(tg29_set_otg_enabled(h, true) == ESP_OK && fake_ticks == 1010);
        assert(chip.reg[0x13] == 0xA5 && chip.reg[0x19] == 0xBF && chip.reg[0x50] == 0xDE);
        assert(h->vbus_adc_owned != borrowed_adc);
        assert(tg29_get_usb_power_state(h, &p) == ESP_OK && p.vbus_voltage_valid == borrowed_adc);
        set_vbus_raw(5126);
        assert(tg29_verify_otg_output(h, 2000, &p) == ESP_OK && fake_ticks == 2020 && p.vbus_mv == 5126);
        assert(tg29_set_otg_enabled(h, false) == ESP_OK);
        assert_manual_off(h, borrowed_adc ? 0x97 : 0x93);
        assert(!source_permission_current(h));
    }
    puts("PASS explicit CC-less manual OTG ignores historical E7 after verified detector/port OFF, still requires settled low VBUS and measured5V, and restores owned settings only");

    manual_fixture(h, TG29_CC_UNATTACH_SNK);
    for (uint8_t mode = 0; mode < 4; ++mode) for (uint8_t cc = 0; cc < 16; ++cc) {
        chip.reg[0xE3] = 0xD0U | mode;
        chip.reg[0xE7] = cc;
        tg29_usb_power_state_t p;
        assert(tg29_get_usb_power_state(h, &p) == ESP_OK);
        assert(p.otg_role_ready == (mode == TG29_CC_DISABLED));
    }
    manual_fixture(h, TG29_CC_ATTACH_SNK);
    chip.reg[0x0B] |= 8U;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !fake_ticks);
    assert(!writes[0x13] && !writes[0x19]);
    assert_manual_off(h, 0x93);
    manual_fixture(h, TG29_CC_UNATTACH_SNK);
    chip.reg[0x00] |= 0x20U;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && !fake_ticks && !writes[0xC0]);
    assert(!writes[0x13] && !writes[0x19]);
    assert_manual_off(h, 0x93);
    puts("PASS manual policy accepts all stale E7 states only with detector/port OFF; unexpected detector enable and external VBUSGOOD remain refused");

    for (unsigned fault = 0; fault < 7; ++fault) {
        manual_fixture(h, TG29_CC_UNATTACH_SNK);
        if (fault == 0) set_vbus_raw(0x3FFCU);
        if (fault == 1) delay_hook = late_vbus_rise; /* Stale early zero is insufficient. */
        if (fault == 2) delay_hook = vbus_present_at_final_gate_guard;
        if (fault == 3) drop_write_reg = 0xC0;
        if (fault == 4) fail_write_reg = 0xC0;
        if (fault == 5) fail_read_reg = 0xC6;
        if (fault == 6) delay_hook = disable_adc_during_settle;
        assert(tg29_set_otg_enabled(h, true) != ESP_OK);
        assert(!writes[0x13] && !writes[0x19]);
        assert_manual_off(h, 0x93);
    }
    puts("PASS stale/high/endpoint ADC, ADC enable/readback loss, and VBUSGOOD arriving immediately before RBFET all prevent source enable");

    for (unsigned event = 0; event < 3; ++event) {
        manual_fixture(h, TG29_CC_UNATTACH_SNK);
        delay_hook = event == 0 ? revoke_during_settle :
                     event == 1 ? enable_detector_during_manual : interleaved_manual_off;
        assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && fake_ticks <= 40);
        assert(!writes[0x13] && !writes[0x19]);
        assert_manual_off(h, 0x93);
    }
    puts("PASS OFF epoch, unexpected detector enable and checked OFF cancel unlocked manual preflight without permitting another start or losing ADC cleanup");

    tg29_otg_verify_result_t r;
    manual_fixture(h, TG29_CC_UNATTACH_SNK);
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    assert(tg29_settle_otg_control(h, 100, &r) == ESP_ERR_INVALID_STATE && r.cleanup_error == ESP_OK);
    assert_manual_off(h, 0x93);
    manual_fixture(h, TG29_CC_UNATTACH_SNK);
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    assert(tg29_verify_otg_output_ex(h, 2000, &r) == ESP_ERR_TIMEOUT && r.pre_cleanup_diag_valid);
    assert(!r.power.vbus_voltage_valid && r.pre_cleanup_diag.power.otg_role_ready);
    assert_manual_off(h, 0x93);
    manual_fixture(h, TG29_CC_UNATTACH_SNK);
    assert(tg29_set_otg_enabled(h, true) == ESP_OK);
    chip.reg[0xE3] = (chip.reg[0xE3] & ~3U) | TG29_CC_SINK;
    assert(tg29_verify_otg_output_ex(h, 2000, &r) == ESP_ERR_INVALID_STATE && !r.pre_cleanup_diag_attempted);
    assert_manual_off(h, 0x93);
    assert(tg29_configure_cc_detection(h, true) == ESP_OK && !h->manual_otg);
    chip.reg[0xE7] = TG29_CC_UNATTACH_SNK;
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE);
    puts("PASS manual mode cannot use control-only fallback, records unproved voltage safely, stops on unexpected port changes and leaves automatic ATTACH_SRC requirement intact");
    verification_handle = NULL;
}

static void test_manual_cc_reinstall(tg29_handle_t h)
{
    verified_fixture(h,true);
    chip.reg[0x0B]=0xB9;chip.reg[0xE3]=0xD3;chip.reg[0x13]=0xA5;chip.reg[0x19]=0xBF;
    chip.reg[0x1E]=0x94;
    for(unsigned attempt=0;attempt<2;++attempt) {
        chip.reg[0xE7]=0x40;
        manual_disable_guard=true;
        const unsigned detect_writes=writes[0x0B],mode_writes=writes[0xE3];
        assert(tg29_configure_manual_otg(h)==ESP_OK);
        assert(writes[0x0B]==detect_writes+1 && writes[0xE3]==mode_writes+1);
        assert(chip.reg[0x0B]==0xB0 && chip.reg[0xE3]==0xD0);
        assert(!(chip.reg[0x13]&1U) && !(chip.reg[0x19]&0x10U));
        tg29_usb_power_state_t p;
        assert(tg29_get_usb_power_state(h,&p)==ESP_OK && p.otg_role_ready && p.cc_raw==0x40);
        assert(p.cc_detection_raw==0xB0 && p.cc_mode_raw==0xD0 && !p.cc_detection_enabled);
        set_vbus_raw(0);
        assert(tg29_set_otg_enabled(h,true)==ESP_OK);
        set_vbus_raw(5134);
        assert(tg29_verify_otg_output(h,2000,&p)==ESP_OK && p.vbus_mv==5134);
        const uint8_t stale_states[]={0,1,4,6,3};
        const unsigned stable_writes=write_count;
        const TickType_t active_start=fake_ticks;
        for(unsigned second=0;second<61;++second) {
            chip.reg[0xE7]=0x40U|stale_states[second%sizeof(stale_states)];
            assert(read_otg_control_sample(h,atomic_load(&h->source_epoch),true,&p)==ESP_OK);
            assert(p.otg_role_ready && p.boost_enabled && p.rbfet_enabled && p.vbus_mv==5134);
            vTaskDelay(1000);
        }
        assert(fake_ticks-active_start>60000 && write_count==stable_writes);
        assert(tg29_set_otg_enabled(h,false)==ESP_OK && !source_permission_current(h));
        assert(tg29_configure_cc_detection(h,false)==ESP_OK && (chip.reg[0x0B]&8U) && (chip.reg[0xE3]&3U)==TG29_CC_SINK);
    }
    puts("PASS manual 5.134V output stays healthy beyond60s with disabled CC and stale E7 0/1/4/6/3; repeated OFF restores sink detection and explicit ON disables it again");

    for(unsigned stage=0;stage<2;++stage) for(unsigned dropped=0;dropped<2;++dropped) {
        manual_fixture(h,TG29_CC_UNATTACH_SRC);
        chip.reg[0x0B] |= 8U;
        chip.reg[0xE3]=0xD1; /* A dropped mode write cannot pass equal-value readback. */
        const int reg=stage==1 ? 0xE3 : 0x0B;
        const unsigned occurrence=writes[reg]+1;
        if(dropped){drop_write_reg=reg;drop_write_occurrence=occurrence;}
        else{fail_write_reg=reg;fail_write_occurrence=occurrence;}
        assert(tg29_configure_manual_otg(h)!=ESP_OK);
        assert(!source_permission_current(h) && !h->manual_otg);
        assert(!(chip.reg[0x13]&1U) && !(chip.reg[0x19]&0x10U));
        assert(!h->manual_output_started && !atomic_load(&h->manual_start_active));
    }
    puts("PASS every checked manual detector/port disable write/drop failure leaves source unauthorized and both output controls OFF");

    manual_fixture(h,TG29_CC_UNATTACH_SRC);
    const unsigned before=writes[0x0B];revoke_on_mode_write=true;
    assert(tg29_configure_manual_otg(h)==ESP_ERR_INVALID_STATE);
    assert(writes[0x0B]==before+1 && !(chip.reg[0x0B]&8U));
    assert(!source_permission_current(h) && !h->manual_otg);
    assert(!(chip.reg[0x13]&1U) && !(chip.reg[0x19]&0x10U));
    assert(tg29_configure_manual_otg(h)==ESP_OK && source_permission_current(h));
    assert(tg29_configure_cc_detection(h,false)==ESP_OK);
    verification_handle=NULL;
    puts("PASS OFF epoch during manual detector/port disable prevents authorization; only a later explicit configure can resume");
}

static void test_manual_physical_guards(tg29_handle_t h)
{
    const uint8_t stale[]={0,1,4,6,3};
    tg29_otg_verify_result_t result;
    for(unsigned i=0;i<sizeof(stale);++i) {
        manual_fixture(h,0x40U|stale[i]);chip.reg[0]|=0x20U;set_vbus_raw(5134);
        assert(tg29_set_otg_enabled(h,true)==ESP_ERR_INVALID_STATE && !fake_ticks);
        assert(!writes[0x13] && !writes[0x19]);
        manual_fixture(h,0x40U|stale[i]);set_vbus_raw(5134);
        assert(tg29_set_otg_enabled(h,true)==ESP_ERR_INVALID_STATE && fake_ticks>=1000);
        assert(!writes[0x13] && !writes[0x19]);
    }
    puts("PASS every historical E7 state still rejects external VBUSGOOD and physically high input ADC; disabling CC never skips settled low-voltage preflight");

    for(unsigned fault=0;fault<2;++fault) {
        manual_fixture(h,0x43);assert(tg29_set_otg_enabled(h,true)==ESP_OK);
        set_vbus_raw(5134);assert(tg29_verify_otg_output_ex(h,2000,&result)==ESP_OK);
        if(!fault)chip.reg[0x0B]|=8U;
        else chip.reg[0xE3]=(chip.reg[0xE3]&~3U)|TG29_CC_SINK;
        assert(tg29_verify_otg_output_ex(h,2000,&result)==ESP_ERR_INVALID_STATE);
        assert(result.cleanup_error==ESP_OK && !(chip.reg[0x19]&0x10U) && !(chip.reg[0x13]&1U));
    }
    verified_fixture(h,true);assert(tg29_set_otg_enabled(h,true)==ESP_OK);
    chip.reg[0xE7]=0x40;set_vbus_raw(5134);
    assert(tg29_verify_otg_output_ex(h,2000,&result)==ESP_ERR_INVALID_STATE && result.cleanup_error==ESP_OK);
    puts("PASS manual output rejects unexpected detector enable or port changes even at5.134V; automatic output still rejects CC state0 at the same healthy voltage");

    manual_fixture(h,0x40);
    const unsigned begin=write_order_count;
    assert(tg29_configure_cc_detection(h,false)==ESP_OK);
    assert(write_order_count==begin+2 && write_order[begin]==0xE3 && write_order[begin+1]==0x0B);
    assert((chip.reg[0xE3]&3U)==TG29_CC_SINK && (chip.reg[0x0B]&8U));
    verification_handle=NULL;
    puts("PASS manual OFF installs verified SINK port before re-enabling CC detection, without changing unrelated controls");
}

static void runtime_input_fixture(tg29_handle_t h)
{
    assert(!atomic_load(&h->input_token));
    input_probe_fixture(h);
    atomic_store(&h->manual_start_active, false);
    atomic_store(&h->input_probe_active, false);
    h->manual_otg = h->manual_output_started = false;
    chip.reg[0xE3] = 0xD1; chip.reg[0xE7] = TG29_CC_ATTACH_SNK;
    chip.reg[0x0B] = 8; chip.reg[0x13] = 0xA0;
    chip.reg[0x01] = 0x94; /* Charging complete, not actively charging. */
}

static void test_runtime_input_lease(tg29_handle_t h)
{
    uint32_t token, newer;
    tg29_usb_power_state_t p;
    tg29_vbus_input_probe_result_t probe;
    runtime_input_fixture(h);
    assert(tg29_vbus_input_acquire(h, &token) == ESP_OK && token);
    assert(h->input_adc_owned && atomic_load(&h->input_token) == token);
    assert(write_count == 1 && writes[0xC0] == 1 && chip.reg[0xC0] == 0x97);
    assert(tg29_vbus_input_read(h, token, &p) == ESP_OK && p.vbus_good && p.vbus_voltage_valid && p.vbus_mv == 5126);
    unsigned before = write_count;
    assert(tg29_configure_manual_otg(h) == ESP_ERR_INVALID_STATE);
    assert(tg29_configure_cc_detection(h, true) == ESP_ERR_INVALID_STATE);
    assert(tg29_set_otg_enabled(h, true) == ESP_ERR_INVALID_STATE && write_count == before);
    assert(tg29_probe_vbus_input(h, 100, &probe) == ESP_ERR_INVALID_STATE && !probe.attempted && write_count == before);
    assert(tg29_set_otg_enabled(h, false) == ESP_OK && (chip.reg[0xC0] & 4));
    assert(tg29_configure_cc_detection(h, false) == ESP_OK && (chip.reg[0xC0] & 4));
    assert(tg29_vbus_input_read(h, token, &p) == ESP_OK);
    chip.reg[0xC0] |= 0x40; /* Preserve another ADC user's later enable. */
    assert(tg29_vbus_input_release(h, token) == ESP_OK && chip.reg[0xC0] == 0xD3);
    assert(!h->input_adc_owned && !atomic_load(&h->input_token));
    puts("PASS runtime external input supports charge-complete VBUSGOOD, changes only owned ADC bit, excludes source ON, and survives ordinary source OFF");

    runtime_input_fixture(h); chip.reg[0xC0] |= 4;
    assert(tg29_vbus_input_acquire(h, &newer) == ESP_OK && newer != token && !h->input_adc_owned && !write_count);
    before = write_count;
    assert(tg29_vbus_input_release(h, token) == ESP_ERR_INVALID_STATE && write_count == before);
    assert(tg29_vbus_input_read(h, token, &p) == ESP_ERR_INVALID_STATE && all_zero(&p,sizeof(p)));
    assert(tg29_vbus_input_acquire(h, &token) == ESP_ERR_INVALID_STATE && !token);
    assert(tg29_vbus_input_release(h, newer) == ESP_OK && chip.reg[0xC0] == 0x97 && !write_count);
    puts("PASS borrowed external ADC remains enabled on release; stale input tokens cannot read, clear or overwrite a newer lease");

    runtime_input_fixture(h);
    assert(tg29_vbus_input_acquire(h, &token) == ESP_OK);
    const uint16_t raw[] = {0,0x3FFC,0x3FFF,0x2000,4700,5600};
    for (unsigned n=0;n<sizeof(raw)/sizeof(raw[0]);++n) {
        chip.reg[0xC6]=raw[n]>>8;chip.reg[0xC7]=raw[n]&0xFF;
        assert(tg29_vbus_input_read(h,token,&p)==ESP_OK);
        assert(p.vbus_voltage_valid==(raw[n]>0 && raw[n]<=0x1FFF));
    }
    const uint8_t regs[]={0xE3,0xE7,0x19,0x1E,0,0x0B,0xC0,0x13,0xC6,0xC7};
    for (unsigned n=0;n<sizeof(regs);++n) {
        fail_read_reg=regs[n];fail_read_occurrence=reads[regs[n]]+1;
        assert(tg29_vbus_input_read(h,token,&p)==ESP_FAIL && all_zero(&p,sizeof(p)));
        assert(atomic_load(&h->input_token)==token);
    }
    const uint8_t conflicts[]={0,0x19,0x13,0xE3,0xE7,0xC0};
    for (unsigned n=0;n<sizeof(conflicts);++n) {
        uint8_t reg=conflicts[n], saved=chip.reg[reg];
        if(reg==0)chip.reg[reg]&=~0x20U;
        else if(reg==0x19)chip.reg[reg]|=0x10;
        else if(reg==0x13)chip.reg[reg]|=1;
        else if(reg==0xE3)chip.reg[reg]=TG29_CC_SOURCE;
        else if(reg==0xE7)chip.reg[reg]=TG29_CC_ATTACH_SRC;
        else chip.reg[reg]&=~4U;
        assert(tg29_vbus_input_read(h,token,&p)==ESP_ERR_INVALID_STATE && all_zero(&p,sizeof(p)));
        chip.reg[reg]=saved;
    }
    assert(tg29_vbus_input_release(h,token)==ESP_OK);
    puts("PASS runtime input treats ADC readiness separately from range policy, clears all partial reads, and rejects input removal/source-role/gate/boost/ADC loss without dropping owner");

    runtime_input_fixture(h);chip.reg[0]&=~0x20U;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_NOT_FOUND && !token && !write_count);
    const uint8_t stale_states[]={0,1,4,6,3};
    for(unsigned detector=0;detector<2;++detector) for(unsigned n=0;n<sizeof(stale_states);++n) {
        runtime_input_fixture(h);chip.reg[0]&=~0x20U;
        chip.reg[0xE7]=0x40U|stale_states[n];
        chip.reg[0x0B]=detector?8U:0U;chip.reg[0xE3]=detector?0x11U:0x10U;
        assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_NOT_FOUND && !token && !write_count);
        assert(tg29_configure_manual_otg(h)==ESP_OK && source_permission_current(h));
        assert(!(chip.reg[0x0B]&8U) && (chip.reg[0xE3]&3U)==TG29_CC_DISABLED);
        assert(tg29_configure_cc_detection(h,false)==ESP_OK);
    }
    for(unsigned conflict=0;conflict<4;++conflict) {
        runtime_input_fixture(h);chip.reg[0]&=~0x20U;chip.reg[0xE7]=0x40;
        if(conflict==0)chip.reg[0x19]|=0x10U;
        if(conflict==1)chip.reg[0x13]|=1U;
        if(conflict==2){atomic_store(&h->authorized_epoch,atomic_load(&h->source_epoch));atomic_store(&h->source_allowed,true);}
        if(conflict==3)h->vbus_adc_owned=true;
        assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_INVALID_STATE && !token && !write_count);
        h->vbus_adc_owned=false;
    }
    runtime_input_fixture(h);chip.reg[0xE7]=0x40;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_INVALID_STATE && !token && !write_count);
    puts("PASS battery-only stale CC states permit NOT_FOUND handoff to explicit manual configuration; owners/source/BOOST/RBFET and known external input conflicts never become fallback permission");
    runtime_input_fixture(h);fail_read_reg=0;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_FAIL && !token && !write_count);
    runtime_input_fixture(h);atomic_store(&h->manual_start_active,true);
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_INVALID_STATE && !token);
    runtime_input_fixture(h);atomic_store(&h->input_probe_active,true);
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_INVALID_STATE && !token);
    runtime_input_fixture(h);h->vbus_adc_owned=true;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_INVALID_STATE && !token);h->vbus_adc_owned=false;
    runtime_input_fixture(h);chip.reg[0x13]|=1;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_INVALID_STATE && !token);
    runtime_input_fixture(h);fail_mutex_take=true;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_TIMEOUT && !token);
    assert(tg29_vbus_input_acquire(NULL,&token)==ESP_ERR_INVALID_ARG && !token);
    assert(tg29_vbus_input_acquire(h,NULL)==ESP_ERR_INVALID_ARG);
    assert(tg29_vbus_input_read(h,0,&p)==ESP_ERR_INVALID_ARG && all_zero(&p,sizeof(p)));
    assert(tg29_vbus_input_release(h,0)==ESP_ERR_INVALID_ARG);
    puts("PASS external input admission distinguishes definite absence from bus/role/source/preflight/probe/cleanup conflicts; invalid arguments never change hardware");

    runtime_input_fixture(h);drop_write_reg=0xC0;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_ERR_INVALID_RESPONSE && !token && !atomic_load(&h->input_token));
    runtime_input_fixture(h);fail_read_reg=0xC0;fail_read_occurrence=2;
    fail_write_reg=0xC0;fail_write_occurrence=2;
    assert(tg29_vbus_input_acquire(h,&token)==ESP_FAIL && token && h->input_adc_owned);
    assert(tg29_configure_manual_otg(h)==ESP_ERR_INVALID_STATE);
    fail_mutex_take=true;
    assert(tg29_vbus_input_release(h,token)==ESP_ERR_TIMEOUT && atomic_load(&h->input_token)==token);
    fail_write_reg=0xC0;fail_write_occurrence=writes[0xC0]+1;
    assert(tg29_vbus_input_release(h,token)==ESP_FAIL && atomic_load(&h->input_token)==token);
    assert(tg29_vbus_input_release(h,token)==ESP_OK && !atomic_load(&h->input_token) && !(chip.reg[0xC0]&4));
    verification_handle=NULL;
    puts("PASS failed input acquisition rolls back only its ADC bit; failed rollback or release retains a cleanup token and blocks source until successful retry");
}

int main(void)
{
    reset_chip();
    tg29_handle_t pmic = NULL;
    assert(tg29_new((void *)1, &pmic) == ESP_OK && pmic);
    tg29_power_profile_t profile = board_profile();
    require_off_before_voltage = true;
    emulate_clamp = true;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_OK);
    assert((chip.reg[0x80] & 0x07) == 0x02);
    assert(chip.reg[0x90] == 0x0A && (chip.reg[0x91] & 0x1F) == 0x05);
    const uint8_t board_voltage_registers[] = {0x94, 0x96, 0x97, 0x9B, 0x9D};
    for (size_t i = 0; i < sizeof(board_voltage_registers); ++i) {
        const uint8_t reg = board_voltage_registers[i];
        assert(chip.reg[reg] == 0xFC && reads[reg] >= 2 && writes[reg] == 1);
    }
    assert_unrelated_preserved();
    assert(!pmic->mutex->depth);
    puts("PASS board voltage encoding/readback, safe baseline and rail masks");

    /* Optional loads may be off, but the shared sensor I/O supply stays on. */
    reset_chip();
    profile = board_profile();
    profile.enable_codec_rail = false;
    profile.enable_touch_rail = profile.enable_display_rail = false;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_OK);
    assert_safe_off();
    reset_chip();
    profile = board_profile();
    profile.enable_sd_rail = true;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_OK);
    assert(chip.reg[0x90] == 0x1A && (chip.reg[0x91] & 0x1F) == 0x05);
    assert_unrelated_preserved();
    puts("PASS disabled peripheral profile and on-demand SD rail");

    /* Validation happens before any register change, including the baseline. */
    reset_chip();
    profile = board_profile();
    profile.cldo3_mv = 3350;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_ERR_INVALID_ARG);
    assert(!write_count);
    profile = board_profile();
    profile.enable_sensor_rail = false;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_ERR_INVALID_ARG);
    profile = board_profile();
    profile.aldo2_mv = 1800;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_ERR_INVALID_ARG);
    assert(tg29_set_rail_enabled(pmic, TG29_RAIL_ALDO2, false) != ESP_OK);
    assert(tg29_set_rail_enabled(pmic, TG29_RAIL_ALDO2, true) != ESP_OK);
    assert(tg29_set_ldo_voltage(pmic, TG29_RAIL_ALDO2, 1800) != ESP_OK);
    assert(tg29_set_ldo_voltage(pmic, TG29_RAIL_CPUSLDO, 3300) != ESP_OK);
    assert(tg29_set_ldo_voltage(pmic, TG29_RAIL_CPUSLDO, 900) != ESP_OK);
    assert(tg29_set_ldo_voltage(pmic, TG29_RAIL_DCDC2, 3300) != ESP_OK);
    assert(tg29_set_rail_enabled(pmic, TG29_RAIL_DCDC2, false) != ESP_OK);
    assert(!write_count && !pmic->mutex->depth);
    puts("PASS invalid profile, CPUSLDO and MCU rail argument guards");

    /* One-shot failures at each voltage write must not leave peripherals on. */
    profile = board_profile();
    for (size_t i = 0; i < sizeof(board_voltage_registers); ++i) {
        reset_chip();
        require_off_before_voltage = true;
        fail_write_reg = board_voltage_registers[i];
        assert(tg29_apply_power_profile(pmic, &profile) != ESP_OK);
        assert_safe_off();
        assert(!pmic->mutex->depth);
        reset_chip();
        require_off_before_voltage = true;
        drop_write_reg = board_voltage_registers[i];
        assert(tg29_apply_power_profile(pmic, &profile) != ESP_OK);
        assert_safe_off();
        assert(!pmic->mutex->depth);
    }
    puts("PASS voltage bus failure and acknowledged-but-lost write cleanup");

    /* A failed baseline transaction must not skip shutdown of other banks. */
    const uint8_t enable_registers[] = {0x80, 0x90, 0x91};
    for (size_t i = 0; i < sizeof(enable_registers); ++i) {
        reset_chip();
        fail_write_reg = enable_registers[i];
        if (enable_registers[i] == 0x90) fail_write_occurrence = 2;
        assert(tg29_apply_power_profile(pmic, &profile) == ESP_FAIL);
        assert_safe_off();
        reset_chip();
        drop_write_reg = enable_registers[i];
        if (enable_registers[i] == 0x90) drop_write_occurrence = 2;
        assert(tg29_apply_power_profile(pmic, &profile) == ESP_ERR_INVALID_RESPONSE);
        assert_safe_off();
    }
    /* Loads enabled successfully in one bank must be shut off again if the
     * other bank cannot be committed or its acknowledgement is misleading. */
    for (size_t i = 1; i < sizeof(enable_registers); ++i) {
        reset_chip();
        fail_write_reg = enable_registers[i];
        fail_write_occurrence = enable_registers[i] == 0x90 ? 3 : 2;
        assert(tg29_apply_power_profile(pmic, &profile) == ESP_FAIL);
        assert_safe_off();
        reset_chip();
        drop_write_reg = enable_registers[i];
        drop_write_occurrence = enable_registers[i] == 0x90 ? 3 : 2;
        assert(tg29_apply_power_profile(pmic, &profile) == ESP_ERR_INVALID_RESPONSE);
        assert_safe_off();
    }
    puts("PASS baseline and partial-enable bus/drop failure cleanup");

    for (size_t i = 0; i < sizeof(board_voltage_registers); ++i) {
        reset_chip();
        fail_read_reg = board_voltage_registers[i];
        fail_read_occurrence = 2; /* Voltage was written; verification fails. */
        assert(tg29_apply_power_profile(pmic, &profile) == ESP_FAIL);
        assert_safe_off();
    }
    reset_chip();
    fail_read_reg = 0x84;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_FAIL);
    assert_safe_off();
    puts("PASS voltage verification and MCU-voltage read failure cleanup");

    /* Even a bad factory MCU voltage must leave unused rails switched off. */
    reset_chip();
    chip.reg[0x84] = 0xD8;
    assert(tg29_apply_power_profile(pmic, &profile) != ESP_OK);
    assert((chip.reg[0x80] & 0x07) == 0x02);
    assert(chip.reg[0x90] == 0x02 && (chip.reg[0x91] & 0x1F) == 0);
    assert(chip.reg[0x84] == 0xD8 && !writes[0x84]);
    assert(writes[0x94] == 1); /* Shared supply is prepared before the MCU setting check. */
    for (size_t i = 1; i < sizeof(board_voltage_registers); ++i)
        assert(!writes[board_voltage_registers[i]]);
    puts("PASS invalid DCDC2 default preserves MCU voltage and disables loads");

    reset_chip();
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_OK);
    drop_write_reg = 0x90;
    drop_write_occurrence = writes[0x90] + 1;
    assert(tg29_set_rail_enabled(pmic, TG29_RAIL_BLDO1, true) != ESP_OK);
    assert(!(chip.reg[0x90] & 0x10));
    assert(tg29_set_rail_enabled(pmic, TG29_RAIL_BLDO1, true) == ESP_OK);
    assert(chip.reg[0x90] == 0x1A);
    assert(tg29_set_rail_enabled(pmic, TG29_RAIL_BLDO1, false) == ESP_OK);
    assert(chip.reg[0x90] == 0x0A);
    assert_unrelated_preserved();
    puts("PASS runtime enable readback and preservation of other rails");

    /* Fault telemetry is best effort and must not clear pending flags. */
    reset_chip();
    fail_read_reg = 0x08;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_OK);
    assert(reads[0x08] && chip.reg[0x08] == 0x31);
    assert_unrelated_preserved();
    assert(!pmic->mutex->depth);
    puts("PASS diagnostic read failure is nonfatal and retains fault evidence");

    /* Model a shared-bus sensor which clamps communication if its previously
     * powered rail is cut. Preparation and cleanup must preserve the rail. */
    reset_chip();
    emulate_clamp = true;
    require_off_before_voltage = true;
    fail_write_reg = 0x96;
    assert(tg29_apply_power_profile(pmic, &profile) != ESP_OK);
    assert_safe_off();
    assert(!lost_bus);
    reset_chip();
    chip.reg[0x90] &= (uint8_t)~0x02;
    assert(tg29_apply_power_profile(pmic, &profile) == ESP_OK);
    assert(chip.reg[0x90] == 0x0A && writes[0x94] == 1);
    puts("PASS initially-off preparation and powered shared-bus clamp scenario");

    /* Failure before ALDO2 voltage verification must never turn its supply on. */
    for (unsigned drop = 0; drop < 2; ++drop) {
        reset_chip();
        chip.reg[0x90] &= (uint8_t)~0x02;
        if (drop) drop_write_reg = 0x94; else fail_write_reg = 0x94;
        assert(tg29_apply_power_profile(pmic, &profile) != ESP_OK);
        assert(chip.reg[0x90] == 0 && (chip.reg[0x94] & 0x1F) == 3);
        assert((chip.reg[0x80] & 7) == 2 && (chip.reg[0x91] & 0x1F) == 0);
    }
    reset_chip();
    chip.reg[0x90] &= (uint8_t)~0x02;
    fail_write_reg = 0x94;
    fail_read_reg = 0x90;
    assert(tg29_apply_power_profile(pmic, &profile) != ESP_OK);
    assert(!writes[0x90] && chip.reg[0x90] == 0xFD);
    assert(writes[0x80] && writes[0x91]);
    assert((chip.reg[0x80] & 7) == 2 && (chip.reg[0x91] & 0x1F) == 0);
    assert_unrelated_preserved();
    assert(!pmic->mutex->depth);
    puts("PASS preparation failure never enables unknown voltage; unreadable bank preserved");

    reset_chip();
    chip.reg[0x41] = 0xA0;
    chip.reg[0x49] = 0xA9;
    chip.reg[0x48] = 0x5C;
    chip.reg[0x4B] = 0x77;
    uint8_t before_key_config[256];
    memcpy(before_key_config, chip.reg, sizeof(chip.reg));
    assert(tg29_configure_key_wakeup(pmic) == ESP_OK);
    assert(chip.reg[0x41] == 0xAF && last_write[0x49] == 0x09 && chip.reg[0x49] == 0xA0);
    for (unsigned reg = 0; reg < 256; ++reg)
        if (reg != 0x41 && reg != 0x49) assert(chip.reg[reg] == before_key_config[reg]);
    assert(!pmic->mutex->depth);
    puts("PASS PWRON key IRQ setup preserves unrelated enables and clears only old key latches");

    reset_chip();
    uint8_t events = 0xFF;
    chip.reg[0x49] = 0xA0 | TG29_KEY_EVENT_PRESS;
    key_arrive_on_ack = TG29_KEY_EVENT_RELEASE;
    assert(tg29_get_key_events(pmic, &events) == ESP_OK && events == TG29_KEY_EVENT_PRESS);
    assert(last_write[0x49] == TG29_KEY_EVENT_PRESS);
    assert(chip.reg[0x49] == (0xA0 | TG29_KEY_EVENT_RELEASE));
    assert(tg29_get_key_events(pmic, &events) == ESP_OK && events == TG29_KEY_EVENT_RELEASE);
    assert(chip.reg[0x49] == 0xA0);
    const unsigned ack_count = writes[0x49];
    assert(tg29_get_key_events(pmic, &events) == ESP_OK && !events && writes[0x49] == ack_count);
    chip.reg[0x49] |= TG29_KEY_EVENT_SHORT_PRESS | TG29_KEY_EVENT_LONG_PRESS;
    assert(tg29_get_key_events(pmic, &events) == ESP_OK);
    assert(events == (TG29_KEY_EVENT_SHORT_PRESS | TG29_KEY_EVENT_LONG_PRESS));
    assert(chip.reg[0x49] == 0xA0 && !writes[0x41] && !writes[0x48] && !writes[0x4A]);
    puts("PASS negative-edge wake snapshot, RW1C acknowledgement and a new edge arriving during ack");

    reset_chip();
    fail_read_reg = 0x49;
    assert(tg29_get_key_events(pmic, &events) == ESP_FAIL && events == 0 && !writes[0x49]);
    chip.reg[0x49] = 0xF0 | TG29_KEY_EVENT_PRESS;
    fail_write_reg = 0x49;
    assert(tg29_get_key_events(pmic, &events) == ESP_FAIL && events == TG29_KEY_EVENT_PRESS);
    assert(chip.reg[0x49] == (0xF0 | TG29_KEY_EVENT_PRESS));
    assert(tg29_get_key_events(pmic, &events) == ESP_OK && events == TG29_KEY_EVENT_PRESS);
    assert(chip.reg[0x49] == 0xF0 && !pmic->mutex->depth);
    assert(tg29_get_key_events(NULL, &events) == ESP_ERR_INVALID_ARG && events == 0);
    assert(tg29_get_key_events(pmic, NULL) == ESP_ERR_INVALID_ARG);
    assert(tg29_configure_key_wakeup(NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS key event read/ack failure retains evidence and invalid arguments are rejected");

    reset_chip();
    fail_read_reg = 0x49;
    assert(tg29_configure_key_wakeup(pmic) == ESP_FAIL && !write_count);
    reset_chip();
    chip.reg[0x49] = 0xF2;
    fail_write_reg = 0x49;
    assert(tg29_configure_key_wakeup(pmic) == ESP_FAIL && chip.reg[0x49] == 0xF2 && !writes[0x41]);
    reset_chip();
    chip.reg[0x41] = 0xA0;
    drop_write_reg = 0x41;
    assert(tg29_configure_key_wakeup(pmic) == ESP_ERR_INVALID_RESPONSE && chip.reg[0x41] == 0xA0);
    assert(!pmic->mutex->depth);
    assert_unrelated_preserved();
    test_battery(pmic);
    reset_chip();
    chip.reg[0x27] = 0xDC; /* Preserve reserved and both persistent control bits. */
    chip.reg[0x22] = 0xA7; chip.reg[0x26] = 0xDA;
    assert(tg29_request_power_off(pmic) == ESP_OK);
    assert(writes[0x27] == 1 && chip.reg[0x27] == 0xDD && reads[0x27] == 1);
    assert(!writes[0x22] && !writes[0x26] && chip.reg[0x22] == 0xA7 && chip.reg[0x26] == 0xDA);
    reset_chip(); chip.reg[0x27] = 0x06;
    assert(tg29_request_power_off(pmic) == ESP_ERR_INVALID_STATE && !write_count);
    reset_chip(); fail_read_reg = 0x27;
    assert(tg29_request_power_off(pmic) == ESP_FAIL && !write_count);
    reset_chip(); fail_write_reg = 0x27;
    assert(tg29_request_power_off(pmic) == ESP_FAIL && writes[0x27] == 1);
    reset_chip(); fail_mutex_take = true;
    assert(tg29_request_power_off(pmic) == ESP_ERR_TIMEOUT && !write_count);
    assert(tg29_request_power_off(NULL) == ESP_ERR_INVALID_ARG);
    assert(!pmic->mutex->depth);
    puts("PASS software off only commands REG27.bit0, preserves hard-off timing/controls and reports bus/lock/restart failures");
    test_usb_and_charge(pmic);
    test_verified_otg(pmic);
    test_consistent_probe(pmic);
    test_input_probe(pmic);
    test_untrusted_control_settle(pmic);
    test_continuous_input_probe(pmic);
    test_rbfet_gate(pmic);
    test_manual_otg(pmic);
    test_manual_cc_reinstall(pmic);
    test_manual_physical_guards(pmic);
    test_runtime_input_lease(pmic);
    assert(tg29_del(pmic) == ESP_OK);
    puts("PASS key setup failures are reported without touching shutdown/protection configuration");
    return 0;
}

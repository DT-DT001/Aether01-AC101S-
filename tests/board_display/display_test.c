#include "aether_board.h"
#include "aether_board_pins.h"
#include "co5300_display.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned commands[16], command_count, wait120, reset_count, init_count, on_count;
static unsigned rail_on_count, rail_off_count, gpio_count, touch_count, brightness_count;
static unsigned te_cancel_count;
static unsigned events[32], event_count;
enum { TOUCH_SLEEP = 1, SLEEP_SETTLED = 2, RESET_SETTLED = 3, GPIO_LOW = 300 };
static uint8_t brightness;
static int fail_command, fail_reset, fail_brightness, fail_rail_on, fail_rail_off, fail_touch, fail_gpio;
const char *esp_err_to_name(esp_err_t error) { return error ? "failure" : "OK"; }

static void reset_spies(void)
{
    memset(commands, 0, sizeof(commands));
    command_count = wait120 = reset_count = init_count = on_count = 0;
    rail_on_count = rail_off_count = gpio_count = touch_count = brightness_count = 0;
    te_cancel_count = 0;
    event_count = 0;
    fail_command = -1; fail_reset = fail_brightness = fail_rail_on = fail_rail_off = 0;
    fail_touch = 0; fail_gpio = -1;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command, const void *data, size_t length)
{
    assert(te_cancel_count); /* Transition must unblock a possible pending TE wait first. */
    assert(io && !data && !length && command_count < 16);
    assert(((unsigned)command & 0xFF0000FFU) == 0x02000000U);
    unsigned dcs = ((unsigned)command >> 8) & 255;
    commands[command_count++] = dcs;
    events[event_count++] = dcs;
    assert(dcs != 0x4F); /* Ordinary screen sleep must never enter deep standby. */
    if ((int)dcs == fail_command) { fail_command = -1; return ESP_ERR_TIMEOUT; }
    return ESP_OK;
}
void co5300_display_cancel_te(esp_lcd_panel_io_handle_t io) { assert(io); ++te_cancel_count; }
void vTaskDelay(TickType_t delay)
{
    if (delay == 120) { ++wait120; events[event_count++] = SLEEP_SETTLED; }
    else if (delay == 10) events[event_count++] = RESET_SETTLED;
}
esp_err_t gpio_set_level(int pin, int value)
{
    assert(pin == AETHER_GPIO_LCD_RST || pin == AETHER_GPIO_LCD_AVDD_EN ||
           pin == AETHER_GPIO_TOUCH_RST || pin == AETHER_GPIO_PA_ENABLE);
    assert(value == 0 || value == 1);
    if (!value) events[event_count++] = GPIO_LOW + pin;
    ++gpio_count;
    return pin == fail_gpio ? ESP_FAIL : ESP_OK;
}
esp_err_t tg29_set_rail_enabled(tg29_handle_t pmic, tg29_rail_t rail, bool enabled)
{
    assert(pmic && rail == TG29_RAIL_CLDO3);
    if (enabled) { ++rail_on_count; if (fail_rail_on) return ESP_ERR_TIMEOUT; }
    else { ++rail_off_count; if (fail_rail_off) return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel)
{
    assert(panel); ++reset_count;
    if (fail_reset) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel) { assert(panel); ++init_count; return ESP_OK; }
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on)
{ assert(panel && on); ++on_count; return ESP_OK; }
esp_err_t esp_lcd_panel_co5300_set_brightness(esp_lcd_panel_handle_t panel, uint8_t value)
{
    assert(panel); ++brightness_count; brightness = value;
    if (fail_brightness) { fail_brightness = 0; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t esp_lcd_touch_enter_sleep(esp_lcd_touch_handle_t touch)
{
    assert(touch);
    ++touch_count;
    events[event_count++] = TOUCH_SLEEP;
    return fail_touch ? ESP_ERR_TIMEOUT : ESP_OK;
}
esp_err_t esp_lcd_touch_exit_sleep(esp_lcd_touch_handle_t touch) { assert(touch); ++touch_count; return ESP_OK; }

static aether_board_t board(void)
{
    return (aether_board_t){.display=(void *)1, .display_io=(void *)2, .pmic=(void *)3,
        .touch=(void *)4, .display_ready=true, .touch_ready=true, .display_brightness=80};
}

static void test_retained_sleep(void)
{
    aether_board_t b = board();
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        reset_spies();
        assert(aether_board_set_display_sleep(&b, true) == ESP_OK);
        assert(b.display_sleeping && !b.display_needs_reinit && command_count == 2);
        assert(commands[0] == 0x28 && commands[1] == 0x10 && wait120 == 1);
        assert(te_cancel_count == 1);
        assert(aether_board_set_display_sleep(&b, true) == ESP_OK && command_count == 2);
        assert(aether_board_set_display_sleep(&b, false) == ESP_OK);
        assert(!b.display_sleeping && !b.display_needs_reinit && command_count == 4);
        assert(commands[2] == 0x11 && commands[3] == 0x29 && wait120 == 2);
        assert(te_cancel_count == 2);
        assert(brightness_count == 1 && brightness == 80);
        assert(!rail_on_count && !rail_off_count && !gpio_count && !reset_count && !touch_count);
        assert(aether_board_set_display_sleep(&b, false) == ESP_OK && command_count == 4);
    }
    puts("PASS 100 ordinary sleep/wake cycles: QSPI framing, 120ms settles, retained rails/reset/touch");
}

static void test_partial_sleep_cold_recovery(void)
{
    const int failures[] = {0x28, 0x10};
    for (unsigned i = 0; i < 2; ++i) {
        aether_board_t b = board(); reset_spies(); fail_command = failures[i];
        assert(aether_board_set_display_sleep(&b, true) == ESP_ERR_TIMEOUT);
        assert(b.display_sleeping && b.display_needs_reinit);
        assert(!gpio_count && !rail_off_count && !touch_count);
        assert(aether_board_set_display_sleep(&b, false) == ESP_OK);
        assert(!b.display_sleeping && !b.display_needs_reinit);
        assert(rail_on_count == 1 && reset_count == 1 && init_count == 1 && on_count == 1);
    }
    puts("PASS failed display-off/sleep-in marks unknown state and performs cold recovery");
}

static void test_partial_wake_cold_recovery(void)
{
    const int failures[] = {0x11, 0x29};
    for (unsigned i = 0; i < 2; ++i) {
        aether_board_t b = board(); b.display_sleeping = true; reset_spies(); fail_command = failures[i];
        assert(aether_board_set_display_sleep(&b, false) == ESP_OK);
        assert(!b.display_sleeping && !b.display_needs_reinit);
        assert(reset_count == 1 && init_count == 1 && rail_on_count == 1 && brightness_count == 1);
    }
    aether_board_t b = board(); b.display_sleeping = true; reset_spies(); fail_brightness = 1;
    assert(aether_board_set_display_sleep(&b, false) == ESP_OK);
    assert(reset_count == 1 && brightness_count == 2 && !b.display_sleeping);
    puts("PASS failed sleep-out/display-on/brightness automatically falls back to cold recovery");
}

static void test_failed_recovery_retry(void)
{
    aether_board_t b = board(); b.display_sleeping = b.display_needs_reinit = true;
    reset_spies(); fail_reset = 1; fail_rail_off = 1;
    assert(aether_board_set_display_sleep(&b, false) == ESP_ERR_TIMEOUT); /* Original error retained. */
    assert(b.display_sleeping && b.display_needs_reinit && rail_off_count == 1);
    fail_reset = fail_rail_off = 0;
    assert(aether_board_set_display_sleep(&b, false) == ESP_OK);
    assert(!b.display_sleeping && !b.display_needs_reinit && reset_count == 2);
    puts("PASS failed cold recovery rolls back, retains original error, and succeeds on later retry");
}

static void test_validation_and_cached_brightness(void)
{
    aether_board_t b = board(); reset_spies();
    assert(co5300_display_set_sleep(NULL, true) == ESP_ERR_INVALID_ARG);
    assert(aether_board_set_display_sleep(NULL, true) == ESP_ERR_INVALID_STATE);
    b.display_ready = false;
    assert(aether_board_set_display_sleep(&b, true) == ESP_ERR_INVALID_STATE);
    b.display_ready = true;
    assert(aether_board_set_display_brightness(&b, 101) == ESP_ERR_INVALID_ARG);
    b.display_sleeping = true;
    assert(aether_board_set_display_brightness(&b, 25) == ESP_OK && !brightness_count);
    assert(aether_board_set_display_sleep(&b, false) == ESP_OK && brightness == 25);
    assert(!touch_count);
    puts("PASS validation and asleep brightness caching/restoration without touch sleep");
}

static void test_shutdown_park(void)
{
    aether_board_t b = board();
    reset_spies();
    assert(aether_board_park_peripherals(&b) == ESP_OK);
    const unsigned expected[] = { GPIO_LOW + AETHER_GPIO_PA_ENABLE, TOUCH_SLEEP,
        GPIO_LOW + AETHER_GPIO_TOUCH_RST, 0x28, 0x10, SLEEP_SETTLED,
        GPIO_LOW + AETHER_GPIO_LCD_RST, RESET_SETTLED, GPIO_LOW + AETHER_GPIO_LCD_AVDD_EN };
    assert(event_count == sizeof(expected) / sizeof(expected[0]));
    assert(memcmp(expected, events, sizeof(expected)) == 0);
    assert(b.touch_sleeping && b.display_sleeping && b.display_needs_reinit);
    assert(!rail_on_count && !rail_off_count);
    assert(aether_board_set_touch_sleep(&b, false) == ESP_OK && !b.touch_sleeping);
    assert(aether_board_set_display_sleep(&b, false) == ESP_OK && !b.display_sleeping);
    assert(touch_count == 2 && reset_count == 1 && rail_on_count == 1);

    b = board(); b.display_sleeping = true; reset_spies();
    assert(aether_board_park_peripherals(&b) == ESP_OK && command_count == 0);
    assert(gpio_count == 4 && b.display_needs_reinit);
    puts("PASS shutdown parks the VSYS loads in order, preserves the PMIC rail policy and supports cold recovery");
}

static void test_shutdown_park_failures(void)
{
    aether_board_t b = board(); reset_spies(); fail_command = 0x10; fail_touch = 1;
    assert(aether_board_park_peripherals(&b) == ESP_ERR_TIMEOUT);
    assert(gpio_count == 2 && b.display_needs_reinit && b.touch_sleeping);
    assert(aether_board_set_touch_sleep(&b, false) == ESP_OK && !b.touch_sleeping);
    assert(aether_board_set_display_sleep(&b, false) == ESP_OK && !b.display_sleeping);

    b = board(); reset_spies(); fail_gpio = AETHER_GPIO_LCD_RST;
    assert(aether_board_park_peripherals(&b) == ESP_FAIL);
    assert(gpio_count == 3 && b.display_needs_reinit && !rail_off_count);

    b = (aether_board_t){0}; reset_spies();
    assert(aether_board_park_peripherals(&b) == ESP_OK && gpio_count == 1);
    assert(!command_count && !touch_count && !rail_off_count);
    assert(aether_board_park_peripherals(NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS shutdown panel/touch I/O and reset failures retain recovery without prematurely removing supplies");
}

int main(void)
{
    test_retained_sleep();
    test_partial_sleep_cold_recovery();
    test_partial_wake_cold_recovery();
    test_failed_recovery_retry();
    test_validation_and_cached_brightness();
    test_shutdown_park();
    test_shutdown_park_failures();
    return 0;
}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../cst9220.c"

static uint16_t project, chip = CST9220_CHIP_ID, mode;
static uint8_t report[CST9220_REPORT_LEN];
static unsigned acknowledgements, reads, removes;
static esp_err_t ack_error, read_error;
static bool acknowledgement_pending;
static uint16_t commands[32];
static size_t command_count;

void vTaskDelay(TickType_t ticks) { (void)ticks; }
esp_err_t gpio_config(const gpio_config_t *config) { assert(config->pin_bit_mask); return ESP_OK; }
esp_err_t gpio_set_level(int pin, int level) { (void)level; assert(pin == 16); return ESP_OK; }
esp_err_t gpio_reset_pin(int pin) { assert(pin == 16 || pin == 7); return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *dev)
{
    assert(bus == (void *)1 && config->device_address == 0x5A && config->scl_speed_hz == 400000);
    *dev = (void *)2; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{ assert(dev == (void *)2); ++removes; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t size, int timeout)
{
    assert(dev == (void *)2 && timeout == 20 && (size == 2 || size == 3));
    const uint16_t command = (uint16_t)(data[0] << 8) | data[1];
    if (size == 2) {
        assert(command_count < sizeof(commands) / sizeof(commands[0]));
        commands[command_count++] = command; mode = command; return ESP_OK;
    }
    assert(command == 0xD000 && data[2] == 0xAB && acknowledgement_pending);
    ++acknowledgements; acknowledgement_pending = false;
    /* Reproduce firmware incompatibility instead of accepting every write. */
    if (project == CST9220_PROJECT_LEGACY || mode != 0xD101) return ESP_ERR_INVALID_RESPONSE;
    return ack_error;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *address,
                                      size_t address_size, uint8_t *data, size_t size, int timeout)
{
    assert(dev == (void *)2 && address_size == 2 && timeout == 20);
    const uint16_t command = (uint16_t)(address[0] << 8) | address[1];
    if (command == 0xD204) {
        assert(size == 4 && mode == 0xD101);
        data[0] = project & 255; data[1] = project >> 8;
        data[2] = chip & 255; data[3] = chip >> 8; return ESP_OK;
    }
    assert(command == 0xD000 && size == 15);
    assert(mode == (project == CST9220_PROJECT_LEGACY ? 0xD109 : 0xD101));
    ++reads;
    if (read_error != ESP_OK) return read_error;
    acknowledgement_pending = project != CST9220_PROJECT_LEGACY;
    memcpy(data, report, size); return ESP_OK;
}

static esp_lcd_touch_handle_t create(uint16_t id)
{
    project = id; acknowledgements = reads = 0; ack_error = read_error = ESP_OK;
    acknowledgement_pending = false; command_count = 0; memset(report, 0, sizeof(report));
    esp_lcd_touch_config_t config = {.x_max = 480, .y_max = 480, .rst_gpio_num = 16, .int_gpio_num = 7};
    esp_lcd_touch_handle_t touch = NULL;
    assert(cst9220_new((void *)1, &config, &touch) == ESP_OK && touch);
    assert(((cst9220_t *)touch)->project_id == id);
    assert(mode == (id == CST9220_PROJECT_LEGACY ? 0xD109 : 0xD101));
    return touch;
}

static void point(unsigned index, uint8_t id, uint8_t status, uint16_t x, uint16_t y)
{
    uint8_t *p = report + (index ? index * 5 + 2 : 0);
    p[0] = (uint8_t)((id << 4) | status); p[1] = (uint8_t)(x >> 4); p[2] = (uint8_t)(y >> 4);
    p[3] = (uint8_t)((x << 4) | (y & 15)); p[4] = 0;
}

static void check_report_protocol(uint16_t id)
{
    esp_lcd_touch_handle_t touch = create(id);
    point(0, 4, 6, 123, 456); point(1, 9, 7, 479, 0);
    report[5] = 2; report[6] = 0xAB; report[14] = 0xAB;
    assert(touch->read_data(touch) == ESP_OK);
    assert(touch->data.points == 2 && !acknowledgement_pending);
    uint16_t x[2], y[2], strength[2]; uint8_t ids[2], count;
    assert(touch->get_xy(touch, x, y, strength, &count, 2) && count == 2);
    assert(x[0] == 123 && y[0] == 456 && x[1] == 479 && y[1] == 0);
    assert(touch->get_track_id(touch, ids, count) == ESP_OK && ids[0] == 4 && ids[1] == 9);
    assert(acknowledgements == (id == CST9220_PROJECT_LEGACY ? 0U : 1U));
    /* Release records keep coordinates and record count. */
    point(0, 4, 0, 123, 456); point(1, 9, 0, 479, 0);
    assert(touch->read_data(touch) == ESP_OK && touch->data.points == 0);
    memset(report, 0, sizeof(report));
    assert(touch->read_data(touch) == ESP_OK && touch->data.points == 0);
    report[0] = 0xAB; report[6] = 0xAB;
    assert(touch->read_data(touch) == ESP_OK && touch->data.points == 0);
    point(0, 0, 6, 1, 2); report[5] = 1; report[6] = 0;
    assert(touch->read_data(touch) == ESP_OK && touch->data.points == 0);
    point(0, 0, 0, 10, 15); point(1, 1, 6, 20, 30); report[5] = 127; report[6] = 0xAB;
    assert(touch->read_data(touch) == ESP_OK && touch->data.points == 1);
    assert(touch->data.coords[0].x == 20 && touch->data.coords[0].y == 30);
    point(0, 0, 6, 480, 1);
    assert(touch->read_data(touch) == ESP_ERR_INVALID_RESPONSE && touch->data.points == 1);
    point(0, 0, 0, 10, 15);
    const unsigned before = acknowledgements;
    read_error = ESP_ERR_TIMEOUT;
    assert(touch->read_data(touch) == ESP_ERR_TIMEOUT && touch->data.points == 1 && acknowledgements == before);
    read_error = ESP_OK;
    if (id != CST9220_PROJECT_LEGACY) {
        ack_error = ESP_ERR_INVALID_RESPONSE;
        assert(touch->read_data(touch) == ESP_ERR_INVALID_RESPONSE && touch->data.points == 1);
        ack_error = ESP_OK;
    }
    assert(touch->enter_sleep(touch) == ESP_OK && mode == 0xD105);
    assert(touch->exit_sleep(touch) == ESP_OK && mode == (id == CST9220_PROJECT_LEGACY ? 0xD109 : 0xD101));
    assert(touch->read_data(touch) == ESP_OK && touch->data.points == 1);
    touch->config.rst_gpio_num = GPIO_NUM_NC;
    assert(touch->enter_sleep(touch) == ESP_OK && mode == 0xD105);
    assert(touch->exit_sleep(touch) == ESP_OK && mode == (id == CST9220_PROJECT_LEGACY ? 0xD109 : 0xD101));
    assert(touch->read_data(touch) == ESP_OK && touch->data.points == 1);
    assert(touch->del(touch) == ESP_OK);
}

static void expect_snapshot(esp_lcd_touch_handle_t touch, uint8_t expected,
                              uint16_t x0, uint16_t y0, uint8_t id0)
{
    for (unsigned read = 0; read < 2; ++read) {
        uint16_t x[2], y[2]; uint8_t ids[2], count = 255;
        assert(touch->get_xy(touch, x, y, NULL, &count, 2) == (expected != 0));
        assert(count == expected);
        if (expected) {
            assert(x[0] == x0 && y[0] == y0);
            assert(touch->get_track_id(touch, ids, count) == ESP_OK && ids[0] == id0);
        }
    }
}

static void fresh(esp_lcd_touch_handle_t touch, uint8_t id, uint8_t status, uint16_t x, uint16_t y)
{
    memset(report, 0, sizeof(report)); point(0, id, status, x, y);
    report[5] = 1; report[6] = 0xAB;
    assert(touch->read_data(touch) == ESP_OK);
}

static void check_drag_and_hold(uint16_t project_id)
{
    esp_lcd_touch_handle_t touch = create(project_id);
    fresh(touch, 0, 6, 20, 30);
    expect_snapshot(touch, 1, 20, 30, 0);
    /* Real LVGL polls faster than new touch reports. ACK echoes, all-zero and
     * markerless polls must not manufacture RELEASED/CLICKED while held. */
    for (unsigned poll = 0; poll < 128; ++poll) {
        memset(report, 0, sizeof(report));
        if (poll % 3 == 0) { report[0] = 0xAB; report[6] = 0xAB; }
        if (poll % 3 == 1) { point(0, 0, 6, 20, 30); report[5] = 1; }
        assert(touch->read_data(touch) == ESP_OK);
        expect_snapshot(touch, 1, 20, 30, 0);
    }
    fresh(touch, 0, 7, 90, 150);
    expect_snapshot(touch, 1, 90, 150, 0);
    point(1, 9, 6, 300, 350); report[5] = 2;
    assert(touch->read_data(touch) == ESP_OK);
    expect_snapshot(touch, 2, 90, 150, 0);
    uint16_t x[1], y[1]; uint8_t count;
    assert(touch->get_xy(touch, x, y, NULL, &count, 1) && count == 1);
    expect_snapshot(touch, 2, 90, 150, 0); /* Limited read is not destructive. */
    point(0, 0, 0, 90, 150); /* First byte 00 is an ID0 release, not an empty packet. */
    assert(touch->read_data(touch) == ESP_OK);
    expect_snapshot(touch, 1, 300, 350, 9);
    fresh(touch, 9, 0, 300, 350);
    expect_snapshot(touch, 0, 0, 0, 0);
    fresh(touch, 0, 6, 40, 50);
    fresh(touch, 0, 0, 40, 50); /* Explicit ID0 release keeps count/coords. */
    expect_snapshot(touch, 0, 0, 0, 0);
    fresh(touch, 0, 6, 40, 50);
    memset(report, 0, sizeof(report)); report[6] = 0xAB;
    assert(touch->read_data(touch) == ESP_OK); /* Marked zero-record snapshot releases all. */
    expect_snapshot(touch, 0, 0, 0, 0);
    fresh(touch, 0, 6, 40, 50);
    point(0, 0, 6, 480, 50);
    assert(touch->read_data(touch) == ESP_ERR_INVALID_RESPONSE);
    expect_snapshot(touch, 1, 40, 50, 0);
    memset(report, 0, sizeof(report)); assert(touch->read_data(touch) == ESP_OK);
    expect_snapshot(touch, 1, 40, 50, 0);
    fresh(touch, 0, 6, 40, 50); point(1, 9, 6, 300, 350); report[5] = 2;
    assert(touch->read_data(touch) == ESP_OK);
    point(0, 0, 0, 40, 50); point(1, 9, 6, 300, 480);
    assert(touch->read_data(touch) == ESP_ERR_INVALID_RESPONSE);
    expect_snapshot(touch, 1, 300, 350, 9);
    memset(report, 0, sizeof(report)); assert(touch->read_data(touch) == ESP_OK);
    expect_snapshot(touch, 1, 300, 350, 9);
    fresh(touch, 9, 0, 300, 350); expect_snapshot(touch, 0, 0, 0, 0);
    fresh(touch, 0, 6, 40, 50);
    read_error = ESP_ERR_TIMEOUT;
    assert(touch->read_data(touch) == ESP_ERR_TIMEOUT);
    expect_snapshot(touch, 1, 40, 50, 0);
    read_error = ESP_OK; memset(report, 0, sizeof(report));
    assert(touch->read_data(touch) == ESP_OK);
    expect_snapshot(touch, 1, 40, 50, 0); /* Adapter must keep consuming until a real release. */
    fresh(touch, 0, 0, 40, 50);
    expect_snapshot(touch, 0, 0, 0, 0);
    if (project_id != CST9220_PROJECT_LEGACY) {
        fresh(touch, 0, 6, 40, 50); ack_error = ESP_ERR_INVALID_RESPONSE;
        assert(touch->read_data(touch) == ESP_ERR_INVALID_RESPONSE);
        expect_snapshot(touch, 1, 40, 50, 0);
        ack_error = ESP_OK; memset(report, 0, sizeof(report)); report[0] = 0xAB;
        assert(touch->read_data(touch) == ESP_OK);
        expect_snapshot(touch, 1, 40, 50, 0);
        memset(report, 0, sizeof(report)); point(0, 0, 0, 40, 50); report[5] = 1; report[6] = 0xAB;
        ack_error = ESP_ERR_INVALID_RESPONSE;
        assert(touch->read_data(touch) == ESP_ERR_INVALID_RESPONSE);
        expect_snapshot(touch, 0, 0, 0, 0); /* Release received; failed ACK must not retain a sticky point. */
        ack_error = ESP_OK; memset(report, 0, sizeof(report)); report[0] = 0xAB;
        assert(touch->read_data(touch) == ESP_OK);
        expect_snapshot(touch, 0, 0, 0, 0);
    }
    fresh(touch, 4, 6, 40, 50);
    assert(touch->enter_sleep(touch) == ESP_OK);
    expect_snapshot(touch, 0, 0, 0, 0);
    assert(touch->exit_sleep(touch) == ESP_OK);
    expect_snapshot(touch, 0, 0, 0, 0);
    assert(touch->del(touch) == ESP_OK);
}

static void check_report_sequence(void)
{
    esp_lcd_touch_t other = {0};
    assert(cst9220_report_sequence(NULL) == 0 && cst9220_report_sequence(&other) == 0);
    esp_lcd_touch_handle_t touch = create(0x6C76);
    assert(cst9220_report_sequence(touch) == 0);
    fresh(touch, 0, 6, 10, 20);
    assert(cst9220_report_sequence(touch) == 1);
    fresh(touch, 0, 6, 10, 20); /* Repeated valid coordinates are still fresh. */
    assert(cst9220_report_sequence(touch) == 2);
    report[6] = 0;
    assert(touch->read_data(touch) == ESP_OK && cst9220_report_sequence(touch) == 2);
    memset(report, 0, sizeof(report));
    assert(touch->read_data(touch) == ESP_OK && cst9220_report_sequence(touch) == 2);
    report[0] = report[6] = 0xAB;
    assert(touch->read_data(touch) == ESP_OK && cst9220_report_sequence(touch) == 2);
    report[5] = CST9220_MAX_POINTS + 1;
    point(0, 0, 6, 10, 20);
    assert(touch->read_data(touch) == ESP_OK && cst9220_report_sequence(touch) == 2);
    expect_snapshot(touch, 1, 10, 20, 0);

    fresh(touch, 0, 6, 10, 20);
    point(1, 9, 6, 30, 40); report[5] = 2;
    assert(touch->read_data(touch) == ESP_OK && cst9220_report_sequence(touch) == 4);
    point(0, 0, 0, 10, 20); point(1, 9, 6, 480, 40);
    assert(touch->read_data(touch) == ESP_ERR_INVALID_RESPONSE && cst9220_report_sequence(touch) == 4);
    expect_snapshot(touch, 1, 30, 40, 9); /* Mixed malformed reports still retire explicit IDs. */

    memset(report, 0, sizeof(report)); point(0, 9, 0, 30, 40);
    report[5] = 1; report[6] = 0xAB;
    read_error = ESP_ERR_TIMEOUT;
    assert(touch->read_data(touch) == ESP_ERR_TIMEOUT && cst9220_report_sequence(touch) == 4);
    expect_snapshot(touch, 1, 30, 40, 9);
    read_error = ESP_OK; ack_error = ESP_ERR_TIMEOUT;
    assert(touch->read_data(touch) == ESP_ERR_TIMEOUT && cst9220_report_sequence(touch) == 5);
    expect_snapshot(touch, 0, 0, 0, 0); /* Read a real release even though ACK failed. */
    memset(report, 0, sizeof(report)); report[0] = 0xAB;
    assert(touch->read_data(touch) == ESP_ERR_TIMEOUT && cst9220_report_sequence(touch) == 5);
    ack_error = ESP_OK;

    ((cst9220_t *)touch)->report_sequence = UINT32_MAX;
    fresh(touch, 0, 6, 10, 20);
    assert(cst9220_report_sequence(touch) == 1);
    assert(touch->enter_sleep(touch) == ESP_OK && cst9220_report_sequence(touch) == 1);
    assert(touch->exit_sleep(touch) == ESP_OK && cst9220_report_sequence(touch) == 1);
    assert(touch->del(touch) == ESP_OK);
}

int main(void)
{
    check_report_protocol(0x542F);
    puts("PASS CST9220 legacy project 542F uses D109 and never sends unsupported ACK");
    check_report_protocol(0x6854);
    puts("PASS CST9220 current project 6854 uses D101, burst-reads 15 bytes, ACKs empty/release/malformed reports");
    check_report_protocol(0x1234);
    puts("PASS unknown projects retain current protocol and ACK failures still propagate");
    check_drag_and_hold(0x542F); check_drag_and_hold(0x6854); check_drag_and_hold(0x6C76);
    puts("PASS press -> 128 ACK/no-fresh polls -> move -> dual/partial release -> explicit release; non-consuming getters");
    puts("PASS held/read-error/no-fresh retains physical press; valid release/ACK-error/no-fresh stays released; sleep clears cache");
    puts("PASS corrupt active coordinates return error without fake release; explicit released IDs retire from mixed bad snapshots");
    check_report_sequence();
    puts("PASS report freshness, unchanged coordinates, cached/invalid reports, release with failed ACK and sequence wrap");
    chip = 0x9217;
    esp_lcd_touch_config_t config = {.x_max = 480, .y_max = 480, .rst_gpio_num = 16, .int_gpio_num = 7};
    esp_lcd_touch_handle_t touch = NULL; unsigned before = removes; command_count = 0;
    assert(cst9220_new((void *)1, &config, &touch) == ESP_ERR_INVALID_RESPONSE && !touch && removes == before + 1);
    puts("PASS dual-point status 6/7, 4-bit tracking IDs, release clearing, bounds, read/ACK errors, wake page restoration, chip rejection");
    return 0;
}

#include "app_ui_internal.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>

extern const lv_font_t font_16, font_24;
static uint16_t draw_buffer[480 * 40];
static app_telemetry_snapshot_t samples = {
    .rtc = {.year = 2024, .month = 1, .day = 31, .hour = 13, .minute = 44, .second = 58, .weekday = 3},
    .barometer = {.pressure_pa = 99600, .temperature_c = 22},
};
static app_tools_snapshot_t tools = {
    .ready = true, .reference_pressure_pa = 101300, .power = {.dcdc2_mv = 3300},
    .battery = {.presence_valid = true, .battery_present = true, .percentage_valid = true, .percentage = 75},
};
static co5300_te_metrics_t te = {.available = true, .edges = 2, .last_period_us = 16666};
static aether_storage_status_t storage = {.mounted = true, .card_inserted = true, .capacity_bytes = 8ULL << 30};
static pcf85063_time_t request;
static uint32_t interest;
static unsigned alarm_opens, clock_writes;
static bool input_blocked, network_ready;

esp_err_t aether_storage_get_status(void *handle, aether_storage_status_t *out)
{ assert(handle); *out = storage; return ESP_OK; }
esp_err_t co5300_display_get_te_metrics(void *io, co5300_te_metrics_t *out)
{ assert(io); *out = te; return ESP_OK; }
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t desc = {"aether01-host", "6.1-host"}; return &desc; }
int64_t esp_timer_get_time(void) { return (int64_t)lv_tick_get() * 1000; }
size_t esp_psram_get_size(void) { return 32UL << 20; }
size_t heap_caps_get_free_size(uint32_t caps) { return caps == MALLOC_CAP_SPIRAM ? 25UL << 20 : 56UL << 10; }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { assert(caps == 3); return 42UL << 10; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { assert(caps == 3); return 32UL << 10; }
size_t strlcpy(char *out, const char *text, size_t size)
{ if (size) snprintf(out, size, "%s", text); return strlen(text); }
const char *esp_err_to_name(esp_err_t error)
{ return error == ESP_OK ? "ESP_OK" : error == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "ESP_ERR_INVALID_ARG"; }
void app_telemetry_get(app_telemetry_snapshot_t *out) { *out = samples; }
void app_telemetry_set_interest(uint32_t mask) { interest = mask; }
void app_tools_get_snapshot(app_tools_snapshot_t *out) { *out = tools; }
esp_err_t app_tools_set_reference_pressure(float pressure)
{
    if (pressure < 80000 || pressure > 120000) return ESP_ERR_INVALID_ARG;
    tools.reference_pressure_pa = pressure;
    tools.preferences_pending = true;
    return ESP_OK;
}
esp_err_t app_tools_set_clock(const pcf85063_time_t *time)
{
    assert(!tools.clock_pending);
    request = *time;
    tools.clock_pending = true;
    ++clock_writes;
    return ESP_OK;
}
void app_open_alarm(app_ui_t *ui) { assert(ui); ++alarm_opens; }
bool app_ui_input_blocked(const app_ui_t *ui) { assert(ui); return input_blocked; }
bool app_tools_clock_network_ready(void) { return network_ready; }
esp_err_t app_tools_sync_clock(void)
{
    assert(network_ready && !tools.network_clock_pending);
    tools.network_clock_pending = true;
    tools.network_clock_received = false;
    return ESP_OK;
}

const lv_font_t *app_ui_chinese_font(void) { return &font_16; }
const lv_font_t *app_ui_chinese_font_size(unsigned size) { return size >= 24 ? &font_24 : &font_16; }
void app_ui_label_text(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(text));
    lv_label_set_text(label, text);
}
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text, lv_align_t align,
                         int32_t x, int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, align, x, y);
    return label;
}
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    if (ui->page_cleanup) { ui->page_cleanup(ui); ui->page_cleanup = NULL; }
    interest = 0;
    memset(ui->app_obj, 0, sizeof(ui->app_obj));
    lv_obj_t *old = ui->screen;
    ui->screen = lv_obj_create(NULL);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 16, 0xFFFFFF, &font_24);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->content, 448, 386);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_pad_all(ui->content, 20, 0);
    lv_screen_load(ui->screen);
    if (old) lv_obj_delete(old);
    return ui->content;
}
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t period)
{ ui->timer = lv_timer_create(callback, period, ui); assert(ui->timer); }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 480 && area->y2 < 480 && pixels); lv_display_flush_ready(display); }
static void tick(unsigned ms) { lv_tick_inc(ms); lv_timer_handler(); }
static bool has_text(lv_obj_t *obj, const char *text) { return strstr(lv_label_get_text(obj), text) != NULL; }

static void click_named(lv_obj_t *content, const char *text)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(content); ++i) {
        lv_obj_t *button = lv_obj_get_child(content, i);
        if (lv_obj_check_type(button, &lv_button_class) && has_text(lv_obj_get_child(button, 0), text)) {
            lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
            return;
        }
    }
    assert(!"button missing");
}

static void test_clock(app_ui_t *ui)
{
    app_open_clock(ui);
    assert(has_text(ui->app_obj[0], "13:44:58") && has_text(ui->app_obj[1], "2024-01-31"));
    lv_obj_t *field[6];
    unsigned count = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(ui->content); ++i) {
        lv_obj_t *child = lv_obj_get_child(ui->content, i);
        if (lv_obj_check_type(child, &lv_dropdown_class)) { assert(count < 6); field[count++] = child; }
    }
    assert(count == 6 && lv_dropdown_get_option_count(field[0]) == 100);
    lv_dropdown_set_selected(field[1], 1);
    lv_obj_send_event(field[1], LV_EVENT_VALUE_CHANGED, NULL);
    assert(lv_dropdown_get_selected(field[2]) == 28);
    lv_dropdown_set_selected(field[0], 25);
    lv_obj_send_event(field[0], LV_EVENT_VALUE_CHANGED, NULL);
    assert(lv_dropdown_get_selected(field[2]) == 27);
    input_blocked = true;
    click_named(ui->content, "保存日期时间");
    assert(!tools.clock_pending && !clock_writes);
    input_blocked = false;
    for (unsigned i = 0; i < 2; ++i) {
        click_named(ui->content, "保存日期时间");
        assert(tools.clock_pending && request.year == 2025 && request.month == 2 && request.day == 28);
        tick(500);
        assert(lv_obj_has_state(ui->app_obj[3], LV_STATE_DISABLED));
        tools.clock_pending = false;
        tools.clock_error = i ? ESP_OK : ESP_ERR_TIMEOUT;
        ++tools.clock_revision;
        tick(500);
        assert(!lv_obj_has_state(ui->app_obj[3], LV_STATE_DISABLED));
        assert(has_text(ui->app_obj[2], i ? "RTC 已保存并核验" : "写入失败"));
    }
    assert(lv_obj_has_state(ui->app_obj[4], LV_STATE_DISABLED));
    network_ready = true;
    tick(500);
    assert(!lv_obj_has_state(ui->app_obj[4], LV_STATE_DISABLED));
    click_named(ui->content, "NTP 校时");
    assert(tools.network_clock_pending && lv_obj_has_state(ui->app_obj[3], LV_STATE_DISABLED));
    tools.network_clock_pending = false;
    tools.network_clock_error = ESP_ERR_TIMEOUT;
    ++tools.network_clock_revision;
    tick(500);
    assert(has_text(ui->app_obj[2], "网络校时超时") && !lv_obj_has_state(ui->app_obj[4], LV_STATE_DISABLED));
    click_named(ui->content, "NTP 校时");
    tools.network_clock_received = true;
    tick(500);
    assert(has_text(ui->app_obj[2], "正在写入并核验 RTC"));
    tools.network_clock_pending = false;
    tools.network_clock_error = ESP_OK;
    tools.clock_verified = (pcf85063_time_t){.year = 2026, .month = 11, .day = 23, .hour = 1};
    ++tools.network_clock_revision;
    tick(500);
    assert(has_text(ui->app_obj[2], "网络校时成功") && lv_dropdown_get_selected(field[0]) == 26 &&
           lv_dropdown_get_selected(field[1]) == 10 && lv_dropdown_get_selected(field[2]) == 22);
    samples.rtc.oscillator_stopped = true;
    tick(500);
    assert(has_text(ui->app_obj[1], "需要校时"));
    samples.rtc_error = ESP_ERR_TIMEOUT;
    tick(500);
    assert(has_text(ui->app_obj[0], "--:--:--") && has_text(ui->app_obj[1], "ESP_ERR_TIMEOUT"));
    click_named(ui->content, LV_SYMBOL_BELL);
    assert(clock_writes == 2 && alarm_opens == 1);
    puts("PASS AC101S Clock actual LVGL: leap-date clamps, shutdown gate, manual readback, Wi-Fi gate, NTP timeout/retry/readback and alarm");
}

static void test_status(app_ui_t *ui)
{
    app_open_status(ui);
    assert(interest == (APP_TELEMETRY_IMU | APP_TELEMETRY_BAROMETER));
    assert(has_text(ui->app_obj[0], "TE 60.0 Hz") && has_text(ui->app_obj[0], "已挂载") &&
           has_text(ui->app_obj[0], "PSRAM 总量 32 MiB") && has_text(ui->app_obj[0], "设置存储"));
    lv_obj_update_layout(ui->screen);
    lv_obj_scroll_to_y(ui->content, 8192, LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(ui->content) > 0);
    te.edges = 0;
    tick(1000);
    assert(has_text(ui->app_obj[0], "等待有效信号"));
    te.available = false;
    te.setup_error = tools.power_error = ESP_ERR_TIMEOUT;
    storage = (aether_storage_status_t){0};
    tick(1000);
    assert(has_text(ui->app_obj[0], "TE 不可用") &&
           has_text(ui->app_obj[0], "TG29 ESP_ERR_TIMEOUT") && has_text(ui->app_obj[0], "未插卡"));
    puts("PASS Device status actual LVGL: firmware, memory, battery, TE fallback, PMIC error, TF removal and scrolling");
}

static void test_barometer(app_ui_t *ui)
{
    app_open_barometer(ui);
    assert(interest == APP_TELEMETRY_BAROMETER && has_text(ui->app_obj[0], "996.00 hPa"));
    click_named(ui->content, "相对高度归零");
    tick(700);
    assert(has_text(ui->app_obj[1], "相对高度 +0.0 m"));
    click_named(ui->content, "+1");
    tick(700);
    assert(has_text(ui->app_obj[2], "1014.00 hPa") && has_text(ui->app_obj[3], "后台保存中"));
    samples.barometer.pressure_pa = NAN;
    tick(700);
    assert(has_text(ui->app_obj[0], "采样数据异常") && has_text(ui->app_obj[1], "等待有效采样"));
    samples.barometer_error = ESP_ERR_TIMEOUT;
    tick(700);
    assert(has_text(ui->app_obj[0], "ESP_ERR_TIMEOUT"));
    samples.barometer_error = ESP_OK;
    samples.barometer.pressure_pa = 100300;
    tick(700);
    assert(has_text(ui->app_obj[0], "1003.00 hPa") && has_text(ui->app_obj[3], "996.00 / 最高 1003.00 hPa"));
    puts("PASS Barometer actual LVGL: zero, reference edit, persistence pending, invalid/timeout, recovery and extrema");
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    aether_board_t board = {.display_io = (void *)1, .display_ready = true, .touch_ready = true,
        .audio_ready = true, .imu_ready = true, .rtc_ready = true, .barometer_ready = true};
    app_ui_t ui = {.board = &board, .storage = (void *)1};
    test_status(&ui);
    test_clock(&ui);
    test_barometer(&ui);
    for (unsigned i = 0; i < 40; ++i) {
        app_open_status(&ui);
        tick(25);
        app_open_clock(&ui);
        tick(25);
        app_open_barometer(&ui);
        tick(25);
        assert(lv_mem_test() == LV_RESULT_OK);
    }
    app_ui_begin_page(&ui, "桌面");
    assert(!ui.timer && !ui.page_context && !interest && lv_mem_test() == LV_RESULT_OK);
    lv_display_delete(display);
    lv_deinit();
    puts("PASS 120 repeated status/clock/barometer page entries and disposal with LVGL heap integrity");
    return 0;
}

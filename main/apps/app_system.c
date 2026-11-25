#include "aether_media_fs.h"
#include "aether_usb_audio.h"
#include "aether_usb_storage.h"
#include "aether_wifi.h"
#include "app_otg_control.h"
#include "app_ui_internal.h"
#include "app_connection_style.h"
#include "app_telemetry.h"
#include "app_utilities.h"
#include "app_tools.h"
#include "app_tools_math.h"
#include "co5300_display.h"
#include "app_audio_system.h"
#include "app_battery.h"

#include <stdio.h>
#include <stdlib.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"

static lv_obj_t *action(lv_obj_t *parent, const char *text, int x, int y,
                        int width, lv_event_cb_t callback, app_ui_t *ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 44);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x273B58), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x395E83), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    const bool icon = (unsigned char)text[0] == 0xEF && (unsigned char)text[1] < 0xA0;
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF,
                    icon ? &lv_font_montserrat_20 : app_ui_chinese_font());
    return button;
}

static void page_free(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

static lv_obj_t *section_card(lv_obj_t *parent, int y, int height,
                              const char *symbol, const char *title, uint32_t accent);
static lv_obj_t *section_text(lv_obj_t *card, const char *text, int y, uint32_t color);
static lv_obj_t *system_dropdown(lv_obj_t *card, int x, int y, int width, const char *options);

static const char *component_status(bool ready, esp_err_t error)
{
    if (!ready) return "未连接";
    return error == ESP_OK ? "正常" : esp_err_to_name(error);
}

static void update_status(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    app_telemetry_snapshot_t samples;
    app_telemetry_get(&samples);
    app_tools_snapshot_t tools;
    app_tools_get_snapshot(&tools);
    char battery_text[320];
    app_battery_format_detail(&tools, battery_text, sizeof(battery_text));
    aether_storage_status_t sd = {0};
    esp_err_t sd_error = aether_storage_get_status(ui->storage, &sd);
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint64_t seconds = esp_timer_get_time() / 1000000;
    const esp_app_desc_t *app = esp_app_get_description();
    co5300_te_metrics_t te = {0};
    const esp_err_t te_error = co5300_display_get_te_metrics(ui->board->display_io, &te);
    char te_text[128];
    if (te_error != ESP_OK || !te.available) {
        snprintf(te_text, sizeof(te_text), "TE 不可用：%s",
                 esp_err_to_name(te_error != ESP_OK ? te_error : te.setup_error));
    } else if (!te.edges || !te.last_period_us) {
        snprintf(te_text, sizeof(te_text), "TE 等待有效信号");
    } else {
        const uint32_t hz_x10 = te.last_period_us ? (uint32_t)(10000000ULL / te.last_period_us) : 0;
        snprintf(te_text, sizeof(te_text), "TE %lu.%lu Hz",
                 (unsigned long)(hz_x10 / 10), (unsigned long)(hz_x10 % 10));
    }
    app_ui_label_fmt(ui->app_obj[0], "%02llu:%02llu:%02llu",
        (unsigned long long)(seconds / 3600), (unsigned long long)(seconds / 60 % 60),
        (unsigned long long)(seconds % 60));
    app_ui_label_fmt(ui->app_obj[1], "固件 %s\nESP-IDF %s", app->version, app->idf_ver);
    app_ui_label_fmt(ui->app_obj[2], "显示 %s   触摸 %s   音频 %s\n%s\n六轴 %s\n气压 %s\nRTC %s",
        component_status(ui->board->display_ready, ESP_OK),
        component_status(ui->board->touch_ready, ESP_OK),
        component_status(ui->board->audio_ready, ESP_OK), te_text,
        component_status(ui->board->imu_ready, samples.imu_error),
        component_status(ui->board->barometer_ready, samples.barometer_error),
        component_status(ui->board->rtc_ready, samples.rtc_error));
    app_ui_label_fmt(ui->app_obj[3], "TG29 %s\nDCDC2 %u mV\nDCDC 0x%02X / LDO 0x%02X 0x%02X\n%s",
        tools.power_error == ESP_OK ? "通信正常" : esp_err_to_name(tools.power_error),
        tools.power.dcdc2_mv, tools.power.dcdc_enable, tools.power.ldo_enable_0, tools.power.ldo_enable_1,
        battery_text);
    app_ui_label_fmt(ui->app_obj[4], "PSRAM %u MiB / 空闲 %u KiB\n内部堆 %u KiB / 最低 %u KiB\n最大连续块 %u KiB",
        (unsigned)(esp_psram_get_size() / (1024U * 1024U)),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_free_size(caps) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(caps) / 1024));
    app_ui_label_fmt(ui->app_obj[5], "TF %s\n容量 %llu MiB / 错误 %s\n设置存储 %s",
        sd_error != ESP_OK ? esp_err_to_name(sd_error) :
            sd.mounted ? "已挂载" : sd.card_inserted ? "插入 / 尚未就绪" : "未插卡",
        (unsigned long long)(sd.capacity_bytes / (1024ULL * 1024ULL)),
        esp_err_to_name(sd.last_error),
        !tools.ready ? "正在加载" : tools.preferences_deferred ? "待媒体暂停后保存" :
        tools.preferences_pending ? "正在保存" : esp_err_to_name(tools.preferences_error));
}

void app_open_status(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "设备状态");
    app_telemetry_set_interest(APP_TELEMETRY_IMU | APP_TELEMETRY_BAROMETER);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_t *system = section_card(content, 0, LV_SIZE_CONTENT, LV_SYMBOL_PLAY, "运行时长", 0x86E3C9);
    ui->app_obj[0] = app_ui_add_text(system, "", LV_ALIGN_TOP_LEFT, 16, 58, 0x86E3C9,
                                    &lv_font_montserrat_48);
    ui->app_obj[1] = section_text(system, "", 122, 0xA6B4CD);
    static const struct { const char *icon, *name; uint32_t accent; } details[] = {
        {LV_SYMBOL_SETTINGS, "组件状态", 0x8FD7FF},
        {LV_SYMBOL_BATTERY_FULL, "供电", 0x86E3C9},
        {LV_SYMBOL_SAVE, "运行内存", 0xADC8FF},
        {LV_SYMBOL_SD_CARD, "存储", 0xBEBAFF},
    };
    for (unsigned i = 0; i < sizeof(details) / sizeof(details[0]); ++i) {
        lv_obj_t *card = section_card(content, 0, LV_SIZE_CONTENT, details[i].icon, details[i].name, details[i].accent);
        ui->app_obj[2 + i] = section_text(card, "", 58, 0xD3E2FA);
        lv_obj_set_style_text_line_space(ui->app_obj[2 + i], 4, 0);
    }
    app_ui_set_timer(ui, update_status, 1000);
    update_status(ui->timer);
}

typedef struct {
    lv_obj_t *fields[6];
    uint32_t clock_revision;
    uint32_t network_revision;
    bool submitted;
    bool initialized;
    bool edited;
} clock_page_t;

static void set_clock_fields(clock_page_t *page, const pcf85063_time_t *now)
{
    lv_dropdown_set_selected(page->fields[0], now->year - 2000);
    lv_dropdown_set_selected(page->fields[1], now->month - 1);
    lv_dropdown_set_selected(page->fields[2], now->day - 1);
    lv_dropdown_set_selected(page->fields[3], now->hour);
    lv_dropdown_set_selected(page->fields[4], now->minute);
    lv_dropdown_set_selected(page->fields[5], now->second);
}

static void clock_edit(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    clock_page_t *page = ui->page_context;
    if (!page) return;
    page->edited = true;
    const unsigned days = app_calendar_days(2000 + lv_dropdown_get_selected(page->fields[0]),
                                            1 + lv_dropdown_get_selected(page->fields[1]));
    if (lv_dropdown_get_selected(page->fields[2]) >= days) lv_dropdown_set_selected(page->fields[2], days - 1);
    app_ui_label_text(ui->app_obj[2], "尚未保存");
}

static void clock_save(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    clock_page_t *page = ui->page_context;
    if (!page || app_ui_input_blocked(ui)) return;
    const pcf85063_time_t time = {
        .year = 2000 + lv_dropdown_get_selected(page->fields[0]),
        .month = 1 + lv_dropdown_get_selected(page->fields[1]),
        .day = 1 + lv_dropdown_get_selected(page->fields[2]),
        .hour = lv_dropdown_get_selected(page->fields[3]),
        .minute = lv_dropdown_get_selected(page->fields[4]),
        .second = lv_dropdown_get_selected(page->fields[5]),
    };
    app_tools_snapshot_t snapshot;
    app_tools_get_snapshot(&snapshot);
    const esp_err_t err = app_tools_set_clock(&time);
    if (err == ESP_OK) {
        page->submitted = true;
        page->clock_revision = snapshot.clock_revision;
        for (unsigned i = 0; i < 6; ++i) lv_obj_add_state(page->fields[i], LV_STATE_DISABLED);
        lv_obj_add_state(ui->app_obj[3], LV_STATE_DISABLED);
        lv_obj_add_state(ui->app_obj[4], LV_STATE_DISABLED);
        app_ui_label_text(ui->app_obj[2], "正在写入并核验 RTC…");
    } else app_ui_label_fmt(ui->app_obj[2], "未保存：%s", esp_err_to_name(err));
}

static void clock_sync(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    clock_page_t *page = ui->page_context;
    if (!page || app_ui_input_blocked(ui)) return;
    if (!app_tools_clock_network_ready()) {
        app_ui_label_text(ui->app_obj[2], "请先连接 Wi-Fi");
        return;
    }
    const esp_err_t error = app_tools_sync_clock();
    if (error != ESP_OK) {
        if (error == ESP_ERR_NOT_FOUND) app_ui_label_text(ui->app_obj[2], "RTC 未连接");
        else app_ui_label_fmt(ui->app_obj[2], "无法开始校时：%s", esp_err_to_name(error));
        return;
    }
    for (unsigned i = 0; i < 6; ++i) lv_obj_add_state(page->fields[i], LV_STATE_DISABLED);
    lv_obj_add_state(ui->app_obj[3], LV_STATE_DISABLED);
    lv_obj_add_state(ui->app_obj[4], LV_STATE_DISABLED);
    app_ui_label_text(ui->app_obj[2], "正在获取网络时间…");
}

static void update_clock(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    clock_page_t *page = ui->page_context;
    if (!page) return;
    app_telemetry_snapshot_t telemetry;
    app_telemetry_get(&telemetry);
    if (telemetry.rtc_error == ESP_OK) {
        const pcf85063_time_t *now = &telemetry.rtc;
        app_ui_label_fmt(ui->app_obj[0], "%02u:%02u:%02u", now->hour, now->minute, now->second);
        static const char *const week[] = {"日", "一", "二", "三", "四", "五", "六"};
        app_ui_label_fmt(ui->app_obj[1], "%04u-%02u-%02u  星期%s%s", now->year, now->month, now->day,
                         now->weekday < 7 ? week[now->weekday] : "?", now->oscillator_stopped ? "  需要校时" : "");
        if (!page->initialized && !page->edited &&
            app_calendar_valid(now->year, now->month, now->day, now->hour, now->minute, now->second)) {
            set_clock_fields(page, now);
            page->initialized = true;
        }
    } else {
        app_ui_label_text(ui->app_obj[0], "--:--:--");
        app_ui_label_fmt(ui->app_obj[1], "RTC：%s", esp_err_to_name(telemetry.rtc_error));
    }
    app_tools_snapshot_t tools;
    app_tools_get_snapshot(&tools);
    const bool busy = tools.clock_pending || tools.network_clock_pending;
    for (unsigned i = 0; i < 6; ++i) {
        if (busy) lv_obj_add_state(page->fields[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(page->fields[i], LV_STATE_DISABLED);
    }
    if (busy) lv_obj_add_state(ui->app_obj[3], LV_STATE_DISABLED);
    else lv_obj_remove_state(ui->app_obj[3], LV_STATE_DISABLED);
    if (busy || !app_tools_clock_network_ready()) lv_obj_add_state(ui->app_obj[4], LV_STATE_DISABLED);
    else lv_obj_remove_state(ui->app_obj[4], LV_STATE_DISABLED);

    if (page->submitted && !tools.clock_pending && tools.clock_revision != page->clock_revision) {
        page->submitted = false;
        app_ui_label_fmt(ui->app_obj[2], "%s%s", tools.clock_error == ESP_OK ? "RTC 已保存并核验" : "写入失败：",
                         tools.clock_error == ESP_OK ? "" : esp_err_to_name(tools.clock_error));
    }
    if (tools.network_clock_pending) {
        app_ui_label_text(ui->app_obj[2], tools.network_clock_received ?
                          "正在写入并核验 RTC…" : "正在获取网络时间…");
    } else if (page->network_revision != tools.network_clock_revision) {
        page->network_revision = tools.network_clock_revision;
        if (tools.network_clock_error == ESP_OK) {
            set_clock_fields(page, &tools.clock_verified);
            page->initialized = true;
            page->edited = false;
            app_ui_label_text(ui->app_obj[2], "网络校时成功，RTC 已保存并核验");
        } else if (tools.network_clock_received) {
            app_ui_label_fmt(ui->app_obj[2], "系统已校时，RTC 写入失败：%s",
                             esp_err_to_name(tools.network_clock_error));
        } else if (tools.network_clock_error == ESP_ERR_TIMEOUT) {
            app_ui_label_text(ui->app_obj[2], "网络校时超时，请检查互联网连接");
        } else if (tools.network_clock_error == ESP_ERR_INVALID_STATE && !app_tools_clock_network_ready()) {
            app_ui_label_text(ui->app_obj[2], "校时中断，请重新连接 Wi-Fi");
        } else {
            app_ui_label_fmt(ui->app_obj[2], "网络校时失败：%s", esp_err_to_name(tools.network_clock_error));
        }
    }
}

static lv_obj_t *clock_field(lv_obj_t *parent, const char *name, int x, int y,
                              unsigned first, unsigned count, app_ui_t *ui)
{
    app_ui_add_text(parent, name, LV_ALIGN_TOP_LEFT, x + 4, y, 0x8FA9CD, app_ui_chinese_font());
    lv_obj_t *field = system_dropdown(parent, x, y + 24, 122, "");
    char options[512];
    size_t length = 0;
    for (unsigned i = 0; i < count; ++i) {
        const int written = snprintf(options + length, sizeof(options) - length, "%s%02u", i ? "\n" : "", first + i);
        if (written < 0 || (size_t)written >= sizeof(options) - length) break;
        length += written;
    }
    lv_dropdown_set_options(field, options);
    lv_obj_add_event_cb(field, clock_edit, LV_EVENT_VALUE_CHANGED, ui);
    return field;
}

static void show_alarm(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    app_open_alarm(ui);
    ui->page_back = app_open_clock;
}

static void show_stopwatch(lv_event_t *event)
{
    app_open_stopwatch(lv_event_get_user_data(event));
}

static void show_countdown(lv_event_t *event)
{
    app_open_countdown(lv_event_get_user_data(event));
}

void app_open_clock(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "时钟与日期");
    clock_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8C8C, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = page_free;
    lv_obj_t *face = lv_obj_create(content);
    lv_obj_set_size(face, 404, 96);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_set_style_radius(face, 20, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(0x1B2842), 0);
    lv_obj_set_style_border_color(face, lv_color_hex(0x34435E), 0);
    lv_obj_set_style_border_width(face, 1, 0);
    ui->app_obj[0] = app_ui_add_text(face, "--:--:--", LV_ALIGN_TOP_LEFT, 18, 5, 0x90E5D8, &lv_font_montserrat_48);
    ui->app_obj[1] = app_ui_add_text(face, "等待 RTC…", LV_ALIGN_BOTTOM_LEFT, 21, -10, 0xB9C8E1, app_ui_chinese_font());
    action(face, "闹钟", 232, 23, 50, show_alarm, ui);
    action(face, "秒表", 288, 23, 50, show_stopwatch, ui);
    action(face, "倒计时", 344, 23, 54, show_countdown, ui);
    page->fields[0] = clock_field(content, "年", 0, 110, 2000, 100, ui);
    page->fields[1] = clock_field(content, "月", 140, 110, 1, 12, ui);
    page->fields[2] = clock_field(content, "日", 280, 110, 1, 31, ui);
    page->fields[3] = clock_field(content, "时", 0, 185, 0, 24, ui);
    page->fields[4] = clock_field(content, "分", 140, 185, 0, 60, ui);
    page->fields[5] = clock_field(content, "秒", 280, 185, 0, 60, ui);
    ui->app_obj[3] = action(content, "保存日期时间", 0, 264, 192, clock_save, ui);
    lv_obj_set_style_bg_color(ui->app_obj[3], lv_color_hex(0x23615F), 0);
    ui->app_obj[4] = action(content, "NTP 校时", 210, 264, 192, clock_sync, ui);
    ui->app_obj[2] = app_ui_add_text(content, "连上 Wi-Fi 后可同步北京时间", LV_ALIGN_TOP_MID, 0, 320, 0x8FA9CD, app_ui_chinese_font());
    lv_obj_set_width(ui->app_obj[2], 402);
    lv_obj_set_style_text_align(ui->app_obj[2], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui->app_obj[2], LV_LABEL_LONG_WRAP);
    app_tools_snapshot_t tools;
    app_tools_get_snapshot(&tools);
    page->network_revision = tools.network_clock_revision;
    app_ui_set_timer(ui, update_clock, 500);
    update_clock(ui->timer);
}

typedef struct {
    lv_obj_t *volume, *volume_label, *mute_label, *mute_button, *output, *audio_status;
    lv_obj_t *wifi_status, *bluetooth_status;
    esp_err_t audio_submit_error;
    lv_obj_t *usb_switch, *usb_status;
    esp_err_t usb_submit_error;
    lv_obj_t *otg_switch, *otg_manual_switch, *otg_status, *otg_power, *otg_hid, *otg_help;
    esp_err_t otg_submit_error;
} settings_page_t;

enum { OTG_HID_Y = 380, OTG_HID_HEIGHT = 220, OTG_HELP_GAP = 16 };

static void refresh_usb(app_ui_t *ui, settings_page_t *page)
{
    app_io_usb_status_t status = {0};
    const esp_err_t read_error = app_io_usb_get_status(ui->io, &status);
    /* Keep the real previous mode while entering/exiting; never acknowledge a
     * requested toggle as an already completed filesystem ownership change. */
    const bool checked = status.active && (!status.pending || !status.requested_enabled);
    if (checked) lv_obj_add_state(page->usb_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(page->usb_switch, LV_STATE_CHECKED);
    if (read_error == ESP_OK && !status.pending && !app_io_is_quiescing(ui->io))
        lv_obj_remove_state(page->usb_switch, LV_STATE_DISABLED);
    else lv_obj_add_state(page->usb_switch, LV_STATE_DISABLED);
    const esp_err_t error = page->usb_submit_error != ESP_OK ? page->usb_submit_error :
                            read_error != ESP_OK ? read_error : status.last_error;
    if (status.pending) app_ui_label_text(page->usb_status, status.requested_enabled ?
        "正在停止媒体并开启 USB…" : "正在退出并恢复本地 TF…");
    else if (error == ESP_ERR_INVALID_STATE && status.active &&
             status.host_connected && !status.host_ejected)
        app_ui_label_text(page->usb_status, "未确认电脑已弹出；安全弹出后再关闭。");
    else if (error != ESP_OK) app_ui_label_fmt(page->usb_status, "USB 操作失败：%s", esp_err_to_name(error));
    else app_ui_label_text(page->usb_status, status.active ?
        "电脑正在使用 TF；先安全弹出，再关闭开关。" : "已关闭；本地使用 TF，串口可用于烧录。");
}

static void usb_switch_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    settings_page_t *page = ui->page_context;
    if (!page) return;
    const bool requested = lv_obj_has_state(page->usb_switch, LV_STATE_CHECKED);
    page->usb_submit_error = app_ui_usb_set_enabled(ui, requested);
    refresh_usb(ui, page);
}

static void usb_details(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    app_open_usb(ui);
    ui->page_back = app_open_settings;
}

static void refresh_otg_power(settings_page_t *page, const app_otg_power_status_t *power)
{
    if (power->age_ms == UINT32_MAX) {
        app_ui_label_text(page->otg_power, "电源诊断：尚未采样");
        return;
    }
    char vbus[32], ts[32], config[40], raw[8], cc[4], control[4], gate[4], adc[4];
    if (!power->vbus_adc_trusted)
        snprintf(vbus, sizeof(vbus), "ADC不可用");
    else if (power->valid && power->vbus_valid)
        snprintf(vbus, sizeof(vbus), "%u.%03u V", power->vbus_mv / 1000, power->vbus_mv % 1000);
    else snprintf(vbus, sizeof(vbus), "未测或过期");
    if (power->valid && power->diagnostics_valid && power->ts_valid)
        snprintf(ts, sizeof(ts), "%u.%03u V", power->ts_mv / 1000, power->ts_mv % 1000);
    else snprintf(ts, sizeof(ts), "未测或过期");
    if (power->valid && power->diagnostics_valid)
        snprintf(config, sizeof(config), "R50:%02X 温度:%02X", power->ts_config, power->work_faults);
    else snprintf(config, sizeof(config), "R50:-- 温度:--");
    if (power->valid && (power->adc_enable_raw & 0x04U))
        snprintf(raw, sizeof(raw), "%04X", power->vbus_raw);
    else snprintf(raw, sizeof(raw), "----");
    if (power->valid) {
        snprintf(cc, sizeof(cc), "%02X", power->cc_raw);
        snprintf(control, sizeof(control), "%02X", power->control_raw);
        snprintf(gate, sizeof(gate), "%02X", power->rbfet_control_raw);
        snprintf(adc, sizeof(adc), "%02X", power->adc_enable_raw);
    } else {
        snprintf(cc, sizeof(cc), "--");
        snprintf(control, sizeof(control), "--");
        snprintf(gate, sizeof(gate), "--");
        snprintf(adc, sizeof(adc), "--");
    }
    const char *phase = power->failure_snapshot ? (power->valid ? "失败时采样" : "失败诊断") :
                        power->valid ? "采样" : "快照已过期";
    app_ui_label_fmt(page->otg_power,
        "%s VBUS:%s TS:%s\nCC:%s R19:%s R13:%s %s\nC0:%s ADC原码:%s\nERR:%X OFF:%X DIAG:%X",
        phase, vbus, ts, cc, control, gate, config,
        adc, raw, (unsigned)power->verify_error,
        (unsigned)power->cleanup_error, (unsigned)power->diagnostic_error);
}

static const char *otg_hid_stage_text(aether_usb_hid_stage_t stage)
{
    switch (stage) {
    case AETHER_USB_HID_STAGE_DEVICE: return "读取设备";
    case AETHER_USB_HID_STAGE_INTERFACE: return "检查接口";
    case AETHER_USB_HID_STAGE_SET_PROTOCOL: return "切换键盘协议";
    case AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR: return "读取 HID 描述符";
    case AETHER_USB_HID_STAGE_REPORT_PARSE: return "解析 HID 描述符";
    case AETHER_USB_HID_STAGE_INPUT: return "读取输入";
    case AETHER_USB_HID_STAGE_READY: return "可用";
    case AETHER_USB_HID_STAGE_IGNORED: return "附加接口已跳过";
    default: return "待识别";
    }
}

static void refresh_otg_hid(settings_page_t *page, const aether_usb_host_status_t *status)
{
    aether_usb_audio_status_t audio = {0};
    aether_usb_storage_status_t storage = {0};
    (void)aether_usb_audio_get_status(&audio);
    (void)aether_usb_storage_get_status(aether_usb_storage_default(), &storage);
    const bool class_present = audio.playback_connected || audio.capture_connected ||
        audio.unsupported_interfaces || audio.last_error || storage.connected || storage.mounted ||
        storage.draining || storage.last_error;
    const bool present = class_present || status->vid || status->pid || status->hid_interfaces ||
        status->ignored_interfaces || status->hid_stage != AETHER_USB_HID_STAGE_NONE;
    if (!present) {
        lv_obj_add_flag(page->otg_hid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(page->otg_help, OTG_HID_Y);
        return;
    }
    lv_obj_remove_flag(page->otg_hid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(page->otg_help, OTG_HID_Y + OTG_HID_HEIGHT + OTG_HELP_GAP);
    char identity[32];
    if (status->vid || status->pid)
        snprintf(identity, sizeof(identity), "%04X:%04X", status->vid, status->pid);
    else snprintf(identity, sizeof(identity), "尚未读取");
    app_ui_label_fmt(page->otg_hid,
        "USB %s 输入:%u 跳过:%u\n阶段:%s 接口:%u 类型:%u/%u\n描述符:%u/%u 字节 传输:%ld\n解析:%X HID错误:%X\nUSB 音频 播放:%s 录音:%s\n音频错误:%X 不支持:%lu\nUSB U盘:%s %llu MiB\nU盘错误:%X",
        identity, status->hid_interfaces, status->ignored_interfaces,
        otg_hid_stage_text(status->hid_stage), status->last_interface,
        status->last_subclass, status->last_protocol, status->report_received,
        status->report_length, (long)status->transfer_status,
        (unsigned)status->report_parse_error, (unsigned)status->hid_error,
        audio.playback_connected ? "已接入" : "未接入", audio.capture_connected ? "已接入" : "未接入",
        (unsigned)audio.last_error, (unsigned long)audio.unsupported_interfaces,
        storage.draining ? "正在释放" : storage.mounted ? "已挂载" : storage.connected ? "挂载中" : "未接入",
        (unsigned long long)(storage.capacity_bytes/(1024U*1024U)), (unsigned)storage.last_error);
}

static bool otg_stopping(const aether_usb_host_status_t *status)
{
    return status->cleanup_pending || status->state == AETHER_OTG_STOPPING ||
           (!status->enabled && (status->state != AETHER_OTG_OFF || status->active || status->vbus_enabled));
}

static bool otg_start_blocked(app_ui_t *ui, const aether_usb_host_status_t *status)
{
    app_io_usb_status_t disk = {0};
    return status->enabled || otg_stopping(status) ||
           app_io_is_quiescing(ui->io) || app_io_usb_get_status(ui->io, &disk) != ESP_OK ||
           disk.active || disk.blocked || disk.pending;
}

static void refresh_otg_switch(lv_obj_t *control, bool checked, bool start_blocked)
{
    if (checked) lv_obj_add_state(control, LV_STATE_CHECKED);
    else lv_obj_remove_state(control, LV_STATE_CHECKED);
    /* Keep OFF available during startup and cleanup. */
    if (!checked && start_blocked) lv_obj_add_state(control, LV_STATE_DISABLED);
    else lv_obj_remove_state(control, LV_STATE_DISABLED);
}

static void refresh_otg(app_ui_t *ui, settings_page_t *page)
{
    aether_usb_host_status_t status = {0};
    const esp_err_t read_error = app_otg_get_status(&status);
    refresh_otg_hid(page, &status);
    app_otg_power_status_t power = {.age_ms = UINT32_MAX};
    const esp_err_t power_error = app_otg_get_power_status(&power);
    refresh_otg_power(page, &power);
    const bool manual = power.manual_source_requested;
    const bool manual_revoked = !manual && power.manual_source_failed && status.enabled;
    const bool stopping = otg_stopping(&status) || manual_revoked;
    const bool start_blocked = read_error != ESP_OK || power_error != ESP_OK || manual ||
                               otg_start_blocked(ui, &status);
    refresh_otg_switch(page->otg_switch, status.enabled && !manual && !manual_revoked, start_blocked);
    refresh_otg_switch(page->otg_manual_switch, manual, start_blocked);
    if (manual || status.enabled || stopping)
        lv_obj_add_state(page->usb_switch, LV_STATE_DISABLED);
    const esp_err_t error = page->otg_submit_error != ESP_OK ? page->otg_submit_error :
                            read_error != ESP_OK ? read_error :
                            power_error != ESP_OK ? power_error : status.last_error;
    if (stopping) app_ui_label_text(page->otg_status, "正在关闭 OTG 并释放接口…");
    else if (!manual && !status.enabled && power_error == ESP_OK && power.manual_source_failed &&
             power.last_error != ESP_OK && page->otg_submit_error == ESP_OK && read_error == ESP_OK)
        app_ui_label_fmt(page->otg_status, "手动 OTG 已停止：%s", esp_err_to_name(power.last_error));
    else if (error != ESP_OK) app_ui_label_fmt(page->otg_status, "OTG 操作失败：%s", esp_err_to_name(error));
    else if (!status.enabled && !manual) app_ui_label_text(page->otg_status, "已关闭；不对外供电。");
    else if (status.state == AETHER_OTG_STARTING)
        app_ui_label_text(page->otg_status, manual ? "手动 OTG：正在检查升压输出…" : "正在检查升压输出…");
    else if (!status.active) app_ui_label_text(page->otg_status, manual ?
        "正在准备手动供电…" : "等待 CC 确认受电设备…");
    else if (status.hid_interfaces)
        app_ui_label_fmt(page->otg_status, "USB HID 已连接：%u 个接口", status.hid_interfaces);
    else if (aether_usb_audio_playback_connected() || aether_usb_audio_capture_connected())
        app_ui_label_text(page->otg_status, "USB 音频已接入；可选择输出或录音输入");
    else if (aether_media_fs_path_ready(ui->storage, "/usb"))
        app_ui_label_text(page->otg_status, "USB U盘已挂载；请在文件浏览中选择");
    else if (!power.vbus_adc_trusted)
        app_ui_label_text(page->otg_status, "供电控制已开启；等待 USB 设备枚举…");
    else if (power.valid && power.output_verified)
        app_ui_label_text(page->otg_status, "供电已验证；等待 USB 设备枚举…");
    else app_ui_label_text(page->otg_status, "Host 已启动；供电状态待确认");
}

static void otg_switch_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    settings_page_t *page = ui->page_context;
    if (!page) return;
    lv_obj_t *control = lv_event_get_target_obj(event);
    const bool manual = control == page->otg_manual_switch;
    const bool enabled = lv_obj_has_state(control, LV_STATE_CHECKED);
    aether_usb_host_status_t status = {0};
    app_otg_power_status_t power = {0};
    if (enabled && (app_otg_get_status(&status) != ESP_OK || app_otg_get_power_status(&power) != ESP_OK ||
                    power.manual_source_requested || otg_start_blocked(ui, &status)))
        page->otg_submit_error = ESP_ERR_INVALID_STATE;
    else page->otg_submit_error = manual ? app_otg_set_manual_enabled(enabled) : app_otg_set_enabled(enabled);
    refresh_usb(ui, page);
    refresh_otg(ui, page);
}

static void settings_status(app_ui_t *ui, esp_err_t error)
{
    ui->app_value = error == ESP_OK ? 1 : 0;
    app_ui_label_fmt(ui->app_obj[2], "%s%s", error == ESP_OK ? "设置已提交，后台保存" : "设置失败：",
                     error == ESP_OK ? "" : esp_err_to_name(error));
}

static void refresh_setting_labels(app_ui_t *ui)
{
    const uint32_t timeout = app_power_get_timeout(ui->power);
    if (timeout) app_ui_label_fmt(ui->app_obj[0], "自动息屏：%lu 秒", (unsigned long)timeout);
    else app_ui_label_text(ui->app_obj[0], "自动息屏：永不");
    app_ui_label_fmt(ui->app_obj[1], "屏幕亮度：%u%%", app_power_get_brightness(ui->power));
}

static void timeout_event(lv_event_t *event)
{
    static const uint16_t values[] = {0,15,30,60,120,300,600,1800,3600};
    app_ui_t *ui = lv_event_get_user_data(event);
    const unsigned selected = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (selected < sizeof(values) / sizeof(values[0])) settings_status(ui, app_power_set_timeout(ui->power, values[selected], true));
    refresh_setting_labels(ui);
}

static void brightness_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    const uint8_t value = lv_slider_get_value(lv_event_get_target_obj(event));
    const bool persist = lv_event_get_code(event) == LV_EVENT_RELEASED;
    const esp_err_t err = app_power_set_brightness(ui->power, value, persist);
    if (persist || err != ESP_OK) settings_status(ui, err);
    refresh_setting_labels(ui);
}

static void sleep_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    const esp_err_t error = app_power_sleep_now(ui->power);
    ui->app_value = 0;
    app_ui_label_fmt(ui->app_obj[2], "%s%s", error == ESP_OK ? "正在息屏" : "息屏失败：",
                     error == ESP_OK ? "" : esp_err_to_name(error));
}

static void volume_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    settings_page_t *page = ui->page_context;
    if (!page) return;
    const uint8_t volume = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(event));
    const bool persist = lv_event_get_code(event) == LV_EVENT_RELEASED;
    page->audio_submit_error = app_audio_system_set_volume(volume, persist);
    app_ui_label_fmt(page->volume_label, "系统音量：%u%%%s", volume, volume ? "" : "  已静音");
}

static void mute_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    settings_page_t *page = ui->page_context;
    if (page) page->audio_submit_error = app_audio_system_toggle_mute(true);
}

static void output_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    settings_page_t *page = ui->page_context;
    if (!page) return;
    const unsigned output = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (output <= APP_AUDIO_OUTPUT_USB)
        page->audio_submit_error = app_audio_system_set_output((app_audio_output_t)output);
}

static void refresh_audio(settings_page_t *page)
{
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    if (!lv_obj_has_state(page->volume, LV_STATE_PRESSED))
        lv_slider_set_value(page->volume, audio.master_volume, LV_ANIM_OFF);
    if (!lv_dropdown_is_open(page->output)) lv_dropdown_set_selected(page->output, audio.output);
    app_ui_label_fmt(page->volume_label, "系统音量：%u%%%s",
                     audio.master_volume, audio.master_volume ? "" : "  已静音");
    app_ui_label_text(page->mute_label, audio.master_volume ? "静音" : "恢复声音");
    lv_obj_t *controls[] = {page->volume, page->mute_button, page->output};
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        if (audio.ready) lv_obj_remove_state(controls[i], LV_STATE_DISABLED);
        else lv_obj_add_state(controls[i], LV_STATE_DISABLED);
    }
    const esp_err_t error = page->audio_submit_error != ESP_OK ? page->audio_submit_error : audio.error;
    if (error != ESP_OK) app_ui_label_fmt(page->audio_status, "音频操作失败：%s", esp_err_to_name(error));
    else if (!audio.ready) app_ui_label_text(page->audio_status, "音频服务尚未就绪");
    else if (audio.pending) app_ui_label_text(page->audio_status, "正在调整音频…");
    else if (audio.persistence_error != ESP_OK && audio.persistence_error != ESP_ERR_NOT_FINISHED)
        app_ui_label_fmt(page->audio_status, "音量保存失败：%s，后台重试", esp_err_to_name(audio.persistence_error));
    else if (audio.persistence_deferred) app_ui_label_text(page->audio_status, "音量已生效，待媒体暂停后保存");
    else if (audio.persistence_pending) app_ui_label_text(page->audio_status, "音量已生效，正在后台保存");
    else app_ui_label_fmt(page->audio_status, "%s · %s",
                          audio.applied_output == APP_AUDIO_OUTPUT_USB ? "USB 音频输出" :
                          audio.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH ? "蓝牙音频输出" : "扬声器输出",
                          audio.applied_volume ? "音量已生效" : "已静音");
}

static void connection_status(lv_obj_t *label, bool ready, bool enabled, bool connected)
{
    app_ui_label_text(label, !ready ? "不可用" : connected ? "已连接" : enabled ? "已开启" : "已关闭");
    const lv_color_t color = lv_color_hex(connected ? CONNECTION_ACCENT : CONNECTION_DETAIL);
    if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), color))
        lv_obj_set_style_text_color(label, color, 0);
}

static void refresh_connections(settings_page_t *page)
{
    const aether_wifi_handle_t wifi = aether_wifi_get_default();
    aether_wifi_status_t station = {0};
    const bool wifi_ready = wifi && aether_wifi_get_status(wifi, &station) == ESP_OK;
    connection_status(page->wifi_status, wifi_ready,
                      station.peer_radio ? station.enabled_preference : station.enabled, station.connected);

    const aether_bluetooth_handle_t bluetooth = app_audio_system_bluetooth();
    aether_bluetooth_status_t audio = {0};
    aether_bluetooth_hid_status_t input = {0};
    const bool audio_ready = bluetooth && aether_bluetooth_get_status(bluetooth, &audio) == ESP_OK;
    const bool input_ready = bluetooth && aether_bluetooth_hid_get_status(bluetooth, &input) == ESP_OK;
    connection_status(page->bluetooth_status, audio_ready || input_ready,
                      audio.enabled || input.requested, audio.connected || input.connected);
}

static void settings_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    app_power_status_t status;
    const esp_err_t power_error = app_power_get_status(ui->power, &status);
    if (power_error != ESP_OK) {
        app_ui_label_fmt(ui->app_obj[2], "读取显示状态失败：%s", esp_err_to_name(power_error));
    } else if (status.display_error != ESP_OK) {
        app_ui_label_fmt(ui->app_obj[2], "显示操作失败：%s", esp_err_to_name(status.display_error));
    } else if (ui->app_value) {
        if (status.persistence_error != ESP_OK && status.persistence_error != ESP_ERR_NOT_FINISHED) {
            app_ui_label_fmt(ui->app_obj[2], "保存失败：%s，后台重试", esp_err_to_name(status.persistence_error));
        } else app_ui_label_text(ui->app_obj[2], status.persistence_deferred ? "待媒体暂停后保存设置…" :
                                status.persistence_pending ? "正在后台保存设置…" : "设置已保存");
    }
    settings_page_t *page = ui->page_context;
    if (page) {
        refresh_connections(page);
        refresh_audio(page);
        refresh_usb(ui, page);
        refresh_otg(ui, page);
    }
}

static void nes_controls_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    app_open_nes_controls(ui);
    ui->page_back = app_open_settings;
}

static lv_obj_t *section_card(lv_obj_t *parent, int y, int height,
                               const char *symbol, const char *title, uint32_t accent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), height);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x303B57), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2337), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *icon = app_ui_add_text(card, symbol, LV_ALIGN_TOP_LEFT, 16, 14, accent,
                                    &lv_font_montserrat_20);
    lv_obj_set_size(icon, 36, 34);
    lv_obj_set_style_pad_top(icon, 6, 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(0x303850), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon, 10, 0);
    app_ui_add_text(card, title, LV_ALIGN_TOP_LEFT, 63, 17, 0xEDF3FF, app_ui_chinese_font_size(24));
    return card;
}

static lv_obj_t *section_text(lv_obj_t *card, const char *text, int y, uint32_t color)
{
    lv_obj_t *label = app_ui_add_text(card, text, LV_ALIGN_TOP_LEFT, 16, y, color, app_ui_chinese_font());
    lv_obj_set_width(label, 380);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *system_dropdown(lv_obj_t *card, int x, int y, int width, const char *options)
{
    lv_obj_t *choice = lv_dropdown_create(card);
    lv_obj_set_pos(choice, x, y);
    lv_obj_set_size(choice, width, 44);
    lv_obj_set_style_text_font(choice, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(choice, &lv_font_montserrat_16, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(choice, lv_color_hex(0x29354F), 0);
    lv_obj_set_style_text_color(choice, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_border_width(choice, 0, 0);
    lv_obj_set_style_radius(choice, 13, 0);
    lv_obj_set_style_pad_top(choice, 12, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(choice), app_ui_chinese_font(), 0);
    lv_dropdown_set_options(choice, options);
    return choice;
}

static lv_obj_t *settings_switch(lv_obj_t *card, int y, lv_event_cb_t callback, app_ui_t *ui)
{
    lv_obj_t *control = lv_switch_create(card);
    lv_obj_set_pos(control, 324, y);
    lv_obj_set_size(control, 70, 36);
    lv_obj_add_event_cb(control, callback, LV_EVENT_VALUE_CHANGED, ui);
    return control;
}

enum { SETTINGS_WIFI, SETTINGS_BLUETOOTH, SETTINGS_DEVICE_STATUS };

static const struct { const char *name; void (*open)(app_ui_t *ui); } settings_subpages[] = {
    [SETTINGS_WIFI] = {"Wi-Fi", app_open_wifi},
    [SETTINGS_BLUETOOTH] = {"蓝牙", app_open_bluetooth},
    [SETTINGS_DEVICE_STATUS] = {"设备状态", app_open_status},
};

static void settings_open_subpage(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_current_target_obj(event);
    if (lv_obj_get_screen(row) != ui->screen) return;
    const size_t item = (uintptr_t)lv_obj_get_user_data(row);
    settings_subpages[item].open(ui);
    ui->page_back = app_open_settings;
}

static lv_obj_t *settings_subpage_row(lv_obj_t *card, int y, size_t item,
                                     lv_obj_t **status, app_ui_t *ui)
{
    lv_obj_t *row = lv_button_create(card);
    lv_obj_set_pos(row, 16, y);
    lv_obj_set_size(row, 380, 44);
    app_connection_button_style(row, false);
    lv_obj_set_user_data(row, (void *)(uintptr_t)item);
    lv_obj_add_event_cb(row, settings_open_subpage, LV_EVENT_CLICKED, ui);
    app_ui_add_text(row, settings_subpages[item].name, LV_ALIGN_LEFT_MID, 14, 0,
                    CONNECTION_TEXT, app_ui_chinese_font());
    app_ui_add_text(row, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, -10, 0,
                    CONNECTION_DETAIL, &lv_font_montserrat_16);
    if (status) *status = app_ui_add_text(row, "", LV_ALIGN_RIGHT_MID, -33, 0,
                                         CONNECTION_DETAIL, app_ui_chinese_font());
    return row;
}

void app_open_settings(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "设置");
    settings_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8C8C, app_ui_chinese_font());
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = page_free;
    lv_obj_set_style_pad_all(content, 16, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 14, 0);
    lv_obj_t *connections = section_card(content, 0, 160, LV_SYMBOL_WIFI, "连接", 0x85DCFF);
    lv_obj_t *wifi_row = settings_subpage_row(connections, 53, SETTINGS_WIFI, &page->wifi_status, ui);
    settings_subpage_row(connections, 100, SETTINGS_BLUETOOTH, &page->bluetooth_status, ui);

    lv_obj_t *sound = section_card(content, 0, 216, LV_SYMBOL_VOLUME_MAX, "声音", 0x86E3C9);
    page->volume_label = section_text(sound, "", 60, 0xD9E8FF);
    page->volume = lv_slider_create(sound);
    lv_obj_set_pos(page->volume, 24, 93);
    lv_obj_set_size(page->volume, 263, 16);
    lv_slider_set_range(page->volume, 0, 100);
    lv_obj_set_ext_click_area(page->volume, 14);
    lv_obj_set_style_bg_color(page->volume, lv_color_hex(0x79CDBA), LV_PART_INDICATOR);
    lv_obj_add_event_cb(page->volume, volume_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(page->volume, volume_event, LV_EVENT_RELEASED, ui);
    page->mute_button = action(sound, "静音", 310, 75, 86, mute_event, ui);
    lv_obj_set_style_bg_color(page->mute_button, lv_color_hex(0x29354F), 0);
    page->mute_label = lv_obj_get_child(page->mute_button, 0);
    section_text(sound, "音频输出", 137, 0xD9E8FF);
    page->output = system_dropdown(sound, 158, 124, 238, "扬声器\n蓝牙音频\nUSB 耳机 / 声卡");
    lv_obj_add_event_cb(page->output, output_event, LV_EVENT_VALUE_CHANGED, ui);
    page->audio_status = section_text(sound, "", 173, 0x9CCFBE);

    lv_obj_t *display = section_card(content, 0, 270, LV_SYMBOL_IMAGE, "显示", 0xADC8FF);
    ui->app_obj[1] = section_text(display, "", 59, 0xD9E8FF);
    lv_obj_t *brightness = lv_slider_create(display);
    lv_obj_set_pos(brightness, 24, 94);
    lv_obj_set_size(brightness, 360, 16);
    lv_slider_set_range(brightness, 5, 100);
    lv_slider_set_value(brightness, app_power_get_brightness(ui->power), LV_ANIM_OFF);
    lv_obj_set_ext_click_area(brightness, 12);
    lv_obj_add_event_cb(brightness, brightness_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(brightness, brightness_event, LV_EVENT_RELEASED, ui);
    ui->app_obj[0] = section_text(display, "", 137, 0xD9E8FF);
    lv_obj_t *timeout = system_dropdown(display, 232, 122, 164,
        "永不\n15 秒\n30 秒\n1 分钟\n2 分钟\n5 分钟\n10 分钟\n30 分钟\n1 小时");
    static const uint16_t values[] = {0,15,30,60,120,300,600,1800,3600};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i] == app_power_get_timeout(ui->power)) lv_dropdown_set_selected(timeout, i);
    }
    lv_obj_add_event_cb(timeout, timeout_event, LV_EVENT_VALUE_CHANGED, ui);
    action(display, "立即息屏", 16, 174, 140, sleep_event, ui);
    ui->app_obj[2] = section_text(display, "按电源键唤醒屏幕", 225, 0xA6B4CD);

    lv_obj_t *storage = section_card(content, 0, 206, LV_SYMBOL_SD_CARD, "存储与按键", 0xBEBAFF);
    section_text(storage, "USB 虚拟U盘", 67, 0xD9E8FF);
    page->usb_switch = settings_switch(storage, 58, usb_switch_event, ui);
    page->usb_status = section_text(storage, "", 100, 0x9CCFBE);
    action(storage, "传输详情", 16, 148, 183, usb_details, ui);
    action(storage, "NES 按键映射", 213, 148, 183, nes_controls_event, ui);
    lv_obj_t *otg = section_card(content, 0, LV_SIZE_CONTENT, LV_SYMBOL_USB, "USB 外设", 0x8FD7FF);
    section_text(otg, "自动 OTG（CC 检测）", 67, 0xD9E8FF);
    page->otg_switch = settings_switch(otg, 58, otg_switch_event, ui);
    section_text(otg, "手动 OTG（无 CC 外设）", 117, 0xD9E8FF);
    page->otg_manual_switch = settings_switch(otg, 108, otg_switch_event, ui);
    section_text(otg, "接充电器或电脑前，请关闭手动 OTG 并等供电停止。", 156, 0xF0C786);
    page->otg_status = section_text(otg, "", 208, 0x9CCFBE);
    page->otg_power = section_text(otg, "", 264, 0xA6B4CD);
    page->otg_hid = section_text(otg, "", OTG_HID_Y, 0xA6B4CD);
    lv_obj_set_height(page->otg_hid, OTG_HID_HEIGHT);
    page->otg_help = section_text(otg,
        "OTG 与虚拟U盘互斥。自动模式在 CC 确认后供电。支持 HID 键鼠、手柄、UAC 1.0 音频和 FAT32/exFAT U盘。USB 音频在声音与录音页选择，U盘在文件管理中打开。",
        OTG_HID_Y, 0xA6B4CD);
    lv_obj_t *device = section_card(content, 0, 113, LV_SYMBOL_SETTINGS, "设备", 0x92B8FF);
    settings_subpage_row(device, 53, SETTINGS_DEVICE_STATUS, NULL, ui);
    refresh_setting_labels(ui);
    refresh_connections(page);
    refresh_audio(page);
    refresh_usb(ui, page);
    refresh_otg(ui, page);
    /* Take focus before the previous screen's deferred deletion can move it
     * to one of this page's lower controls and start an automatic scroll. */
    lv_obj_update_layout(content);
    lv_group_focus_obj(wifi_row);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    app_ui_set_timer(ui, settings_timer, 300);
}

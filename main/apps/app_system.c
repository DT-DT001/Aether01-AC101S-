#include "aether_media_fs.h"
#include "aether_usb_audio.h"
#include "aether_usb_storage.h"
#include "app_otg_control.h"
#include "app_ui_internal.h"
#include "app_telemetry.h"
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
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2768D8), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
    return button;
}

static void page_free(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
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
    app_ui_label_fmt(ui->app_obj[0],
        "运行 %llu 小时 %02llu 分 %02llu 秒\n"
        "固件 %s / IDF %s\n\n"
        "TG29 %s\nDCDC2 %u mV\nDCDC 0x%02X / LDO 0x%02X 0x%02X\n"
        "%s\n\n"
        "显示 %s / 触摸 %s / 音频 %s\n"
        "%s\n"
        "六轴 %s\n气压 %s\nRTC %s\n\n"
        "PSRAM 总量 %u MiB / 空闲 %u KiB\n"
        "内部堆空闲 %u KiB / 历史最低 %u KiB\n"
        "最大连续内部内存 %u KiB\n\n"
        "TF %s\n容量 %llu MiB / 错误 %s\n设置存储 %s",
        (unsigned long long)(seconds / 3600), (unsigned long long)(seconds / 60 % 60),
        (unsigned long long)(seconds % 60), app->version, app->idf_ver,
        tools.power_error == ESP_OK ? "通信正常" : esp_err_to_name(tools.power_error),
        tools.power.dcdc2_mv, tools.power.dcdc_enable, tools.power.ldo_enable_0, tools.power.ldo_enable_1,
        battery_text,
        ui->board->display_ready ? "已初始化" : "不可用",
        ui->board->touch_ready ? "已初始化" : "不可用",
        ui->board->audio_ready ? "已初始化" : "不可用",
        te_text,
        ui->board->imu_ready ? esp_err_to_name(samples.imu_error) : "未连接",
        ui->board->barometer_ready ? esp_err_to_name(samples.barometer_error) : "未连接",
        ui->board->rtc_ready ? esp_err_to_name(samples.rtc_error) : "未连接",
        (unsigned)(esp_psram_get_size() / (1024U * 1024U)),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_free_size(caps) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(caps) / 1024),
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
    ui->app_obj[0] = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 0, 0xC9DCF8, app_ui_chinese_font());
    lv_obj_set_width(ui->app_obj[0], 402);
    lv_label_set_long_mode(ui->app_obj[0], LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->app_obj[0], 8, 0);
    app_ui_set_timer(ui, update_status, 1000);
    update_status(ui->timer);
}

typedef struct {
    lv_obj_t *fields[6];
    uint32_t clock_revision;
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
    if (!page) return;
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
        app_ui_label_text(ui->app_obj[2], "正在写入并核验 RTC…");
    } else app_ui_label_fmt(ui->app_obj[2], "未保存：%s", esp_err_to_name(err));
}

static void update_clock(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    clock_page_t *page = ui->page_context;
    app_telemetry_snapshot_t telemetry;
    app_telemetry_get(&telemetry);
    if (telemetry.rtc_error == ESP_OK) {
        const pcf85063_time_t *now = &telemetry.rtc;
        app_ui_label_fmt(ui->app_obj[0], "%02u:%02u:%02u", now->hour, now->minute, now->second);
        static const char *const week[] = {"日", "一", "二", "三", "四", "五", "六"};
        app_ui_label_fmt(ui->app_obj[1], "%04u-%02u-%02u  星期%s%s", now->year, now->month, now->day,
                         now->weekday < 7 ? week[now->weekday] : "?", now->oscillator_stopped ? "  需要校时" : "");
        if (page && !page->initialized && !page->edited &&
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
    if (page && page->submitted && !tools.clock_pending && tools.clock_revision != page->clock_revision) {
        page->submitted = false;
        for (unsigned i = 0; i < 6; ++i) lv_obj_remove_state(page->fields[i], LV_STATE_DISABLED);
        lv_obj_remove_state(ui->app_obj[3], LV_STATE_DISABLED);
        app_ui_label_fmt(ui->app_obj[2], "%s%s", tools.clock_error == ESP_OK ? "RTC 已保存并核验" : "写入失败：",
                         tools.clock_error == ESP_OK ? "" : esp_err_to_name(tools.clock_error));
    }
}

static lv_obj_t *clock_field(lv_obj_t *parent, const char *name, int x, int y,
                              unsigned first, unsigned count, app_ui_t *ui)
{
    app_ui_add_text(parent, name, LV_ALIGN_TOP_LEFT, x + 4, y, 0x8FA9CD, app_ui_chinese_font());
    lv_obj_t *field = lv_dropdown_create(parent);
    lv_obj_set_pos(field, x, y + 24);
    lv_obj_set_size(field, 122, 44);
    lv_obj_set_style_text_font(field, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(field), app_ui_chinese_font(), 0);
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
    ui->app_obj[0] = app_ui_add_text(content, "--:--:--", LV_ALIGN_TOP_MID, 0, 0, 0x77B8FF, &lv_font_montserrat_28);
    ui->app_obj[1] = app_ui_add_text(content, "等待 RTC…", LV_ALIGN_TOP_MID, 0, 38, 0xC9DCF8, app_ui_chinese_font());
    page->fields[0] = clock_field(content, "年", 0, 82, 2000, 100, ui);
    page->fields[1] = clock_field(content, "月", 140, 82, 1, 12, ui);
    page->fields[2] = clock_field(content, "日", 280, 82, 1, 31, ui);
    page->fields[3] = clock_field(content, "时", 0, 160, 0, 24, ui);
    page->fields[4] = clock_field(content, "分", 140, 160, 0, 60, ui);
    page->fields[5] = clock_field(content, "秒", 280, 160, 0, 60, ui);
    ui->app_obj[3] = action(content, "保存日期时间", 110, 246, 188, clock_save, ui);
    ui->app_obj[2] = app_ui_add_text(content, "选择字段后保存；星期自动计算", LV_ALIGN_TOP_MID, 0, 310, 0x8FA9CD, app_ui_chinese_font());
    lv_obj_set_width(ui->app_obj[2], 402);
    lv_obj_set_style_text_align(ui->app_obj[2], LV_TEXT_ALIGN_CENTER, 0);
    app_ui_set_timer(ui, update_clock, 500);
    update_clock(ui->timer);
}

typedef struct {
    lv_obj_t *volume, *volume_label, *mute_label, *mute_button, *output, *audio_status;
    esp_err_t audio_submit_error;
    lv_obj_t *usb_switch, *usb_status;
    esp_err_t usb_submit_error;
    lv_obj_t *otg_switch, *otg_manual_switch, *otg_status, *otg_power, *otg_hid, *otg_help;
    esp_err_t otg_submit_error;
} settings_page_t;

enum { OTG_HID_Y = 1386, OTG_HID_HEIGHT = 220, OTG_HELP_GAP = 22 };

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
    app_open_usb(lv_event_get_user_data(event));
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
                          audio.applied_output == APP_AUDIO_OUTPUT_BLUETOOTH ? "蓝牙耳机输出" : "扬声器输出",
                          audio.applied_volume ? "音量已生效" : "已静音");
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
    if (page) { refresh_audio(page); refresh_usb(ui, page); refresh_otg(ui, page); }
}

static void nes_controls_event(lv_event_t *event)
{
    app_open_nes_controls(lv_event_get_user_data(event));
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
    app_ui_add_text(content, "声音", LV_ALIGN_TOP_LEFT, 4, 0, 0xA9DDD5, app_ui_chinese_font_size(24));
    page->volume_label = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 42, 0xD9E8FF, app_ui_chinese_font());
    page->volume = lv_slider_create(content);
    lv_obj_set_pos(page->volume, 16, 92);
    lv_obj_set_size(page->volume, 270, 18);
    lv_slider_set_range(page->volume, 0, 100);
    lv_obj_set_ext_click_area(page->volume, 14);
    lv_obj_set_style_bg_color(page->volume, lv_color_hex(0x79CDBA), LV_PART_INDICATOR);
    lv_obj_add_event_cb(page->volume, volume_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(page->volume, volume_event, LV_EVENT_RELEASED, ui);
    page->mute_button = action(content, "静音", 312, 76, 88, mute_event, ui);
    page->mute_label = lv_obj_get_child(page->mute_button, 0);
    app_ui_add_text(content, "音频输出", LV_ALIGN_TOP_LEFT, 4, 144, 0xD9E8FF, app_ui_chinese_font());
    page->output = lv_dropdown_create(content);
    lv_obj_set_pos(page->output, 4, 174);
    lv_obj_set_size(page->output, 398, 44);
    lv_obj_set_style_text_font(page->output, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->output), app_ui_chinese_font(), 0);
    lv_dropdown_set_options(page->output, "扬声器\n蓝牙耳机\nUSB 耳机 / 声卡");
    lv_obj_add_event_cb(page->output, output_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_t *help = app_ui_add_text(content, "蓝牙耳机需先在主页连接；音乐与蓝牙页面共用系统音量。",
                                     LV_ALIGN_TOP_LEFT, 4, 234, 0x8FA9AD, app_ui_chinese_font());
    lv_obj_set_width(help, 398);
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    page->audio_status = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 288, 0x9CCFBE, app_ui_chinese_font());
    lv_obj_set_width(page->audio_status, 398);
    lv_label_set_long_mode(page->audio_status, LV_LABEL_LONG_WRAP);
    app_ui_add_text(content, "显示", LV_ALIGN_TOP_LEFT, 4, 354, 0xA8C8FF, app_ui_chinese_font_size(24));
    ui->app_obj[1] = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 398, 0xD9E8FF, app_ui_chinese_font());
    lv_obj_t *brightness = lv_slider_create(content);
    lv_obj_set_pos(brightness, 18, 448);
    lv_obj_set_size(brightness, 370, 18);
    lv_slider_set_range(brightness, 5, 100);
    lv_slider_set_value(brightness, app_power_get_brightness(ui->power), LV_ANIM_OFF);
    lv_obj_set_ext_click_area(brightness, 12);
    lv_obj_add_event_cb(brightness, brightness_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(brightness, brightness_event, LV_EVENT_RELEASED, ui);
    ui->app_obj[0] = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 496, 0xD9E8FF, app_ui_chinese_font());
    lv_obj_t *timeout = lv_dropdown_create(content);
    lv_obj_set_pos(timeout, 4, 526);
    lv_obj_set_size(timeout, 398, 44);
    lv_obj_set_style_text_font(timeout, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(timeout), app_ui_chinese_font(), 0);
    lv_dropdown_set_options(timeout, "永不\n15 秒\n30 秒\n1 分钟\n2 分钟\n5 分钟\n10 分钟\n30 分钟\n1 小时");
    static const uint16_t values[] = {0,15,30,60,120,300,600,1800,3600};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i] == app_power_get_timeout(ui->power)) lv_dropdown_set_selected(timeout, i);
    }
    lv_obj_add_event_cb(timeout, timeout_event, LV_EVENT_VALUE_CHANGED, ui);
    action(content, "立即息屏", 110, 598, 188, sleep_event, ui);
    ui->app_obj[2] = app_ui_add_text(content, "按电源键唤醒屏幕；触摸不唤醒", LV_ALIGN_TOP_LEFT, 4, 666, 0x8FA9CD, app_ui_chinese_font());
    lv_obj_set_width(ui->app_obj[2], 402);
    lv_obj_set_style_text_align(ui->app_obj[2], LV_TEXT_ALIGN_CENTER, 0);
    app_ui_add_text(content, "USB 虚拟U盘", LV_ALIGN_TOP_LEFT, 4, 738, 0xA8C8FF, app_ui_chinese_font_size(24));
    page->usb_switch = lv_switch_create(content);
    lv_obj_set_pos(page->usb_switch, 326, 738);
    lv_obj_set_size(page->usb_switch, 72, 36);
    lv_obj_add_event_cb(page->usb_switch, usb_switch_event, LV_EVENT_VALUE_CHANGED, ui);
    page->usb_status = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 790, 0x9CCFBE, app_ui_chinese_font());
    lv_obj_set_size(page->usb_status, 398, 48);
    lv_label_set_long_mode(page->usb_status, LV_LABEL_LONG_WRAP);
    lv_obj_t *usb_help = app_ui_add_text(content,
        "默认关闭，重启后关闭。开启后停止媒体并让电脑读写 TF；关闭后恢复本地使用和烧录串口。",
        LV_ALIGN_TOP_LEFT, 4, 850, 0x8FA9AD, app_ui_chinese_font());
    lv_obj_set_width(usb_help, 398);
    lv_label_set_long_mode(usb_help, LV_LABEL_LONG_WRAP);
    action(content, "NES 按键映射", 4, 930, 194, nes_controls_event, ui);
    action(content, "传输详情", 208, 930, 194, usb_details, ui);
    app_ui_add_text(content, "自动 OTG（CC 检测）", LV_ALIGN_TOP_LEFT, 4, 1020,
                    0xA8C8FF, app_ui_chinese_font_size(24));
    page->otg_switch = lv_switch_create(content);
    lv_obj_set_pos(page->otg_switch, 326, 1020);
    lv_obj_set_size(page->otg_switch, 72, 36);
    lv_obj_add_event_cb(page->otg_switch, otg_switch_event, LV_EVENT_VALUE_CHANGED, ui);
    app_ui_add_text(content, "手动 OTG（无 CC 外设）", LV_ALIGN_TOP_LEFT, 4, 1072,
                    0xA8C8FF, app_ui_chinese_font_size(24));
    page->otg_manual_switch = lv_switch_create(content);
    lv_obj_set_pos(page->otg_manual_switch, 326, 1072);
    lv_obj_set_size(page->otg_manual_switch, 72, 36);
    lv_obj_add_event_cb(page->otg_manual_switch, otg_switch_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_t *manual_warning = app_ui_add_text(content,
        "接充电器或电脑前，请关闭手动 OTG 并等供电停止。",
        LV_ALIGN_TOP_LEFT, 4, 1118, 0xE8C388, app_ui_chinese_font());
    lv_obj_set_width(manual_warning, 398);
    lv_label_set_long_mode(manual_warning, LV_LABEL_LONG_WRAP);
    page->otg_status = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 1170,
                                     0x9CCFBE, app_ui_chinese_font());
    lv_obj_set_size(page->otg_status, 398, 54);
    lv_label_set_long_mode(page->otg_status, LV_LABEL_LONG_WRAP);
    page->otg_power = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 1236,
                                    0x8FA9AD, app_ui_chinese_font());
    lv_obj_set_size(page->otg_power, 398, 128);
    lv_label_set_long_mode(page->otg_power, LV_LABEL_LONG_WRAP);
    page->otg_hid = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, OTG_HID_Y,
                                  0x8FA9AD, app_ui_chinese_font());
    lv_obj_set_size(page->otg_hid, 398, OTG_HID_HEIGHT);
    lv_label_set_long_mode(page->otg_hid, LV_LABEL_LONG_WRAP);
    page->otg_help = app_ui_add_text(content,
        "两种 OTG 均默认关闭，与虚拟U盘互斥。自动模式在 CC 确认受电设备后供电。支持标准 HID 键盘、鼠标、通用手柄，UAC 1.0 耳机/麦克风及 FAT32/exFAT U盘。USB 音频在输出/录音页选择，文件浏览切换到 USB U盘。NES 按 GPIO61/BOOT 用户键退出，键盘按键可在 NES 按键映射中设置。",
        LV_ALIGN_TOP_LEFT, 4, OTG_HID_Y, 0x8FA9AD, app_ui_chinese_font());
    lv_obj_set_width(page->otg_help, 398);
    lv_label_set_long_mode(page->otg_help, LV_LABEL_LONG_WRAP);
    refresh_setting_labels(ui);
    refresh_audio(page);
    refresh_usb(ui, page);
    refresh_otg(ui, page);
    app_ui_set_timer(ui, settings_timer, 300);
}

#include "app_displacement.h"
#include "app_ui_internal.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern const lv_font_t font_16, font_24;
void app_open_displacement(app_ui_t *ui);
static uint16_t frame[480 * 480], buffer[480 * 20];
static app_displacement_status_t status;
static unsigned starts, finishes, resumes, cancels;
static esp_err_t start_error;
static lv_point_t pointer;
static bool held;
static bool defer_finish;

void app_displacement_get_status(app_displacement_status_t *out) { *out = status; }
esp_err_t app_displacement_start(void)
{
    assert(status.available && !status.active);
    ++starts;
    if (start_error != ESP_OK) return start_error;
    status.active = true;
    status.state = APP_DISPLACEMENT_WAITING_STILL;
    status.result = (app_displacement_result_t){0};
    return ESP_OK;
}
esp_err_t app_displacement_finish(void)
{
    assert(status.active && (status.state == APP_DISPLACEMENT_READY || status.state == APP_DISPLACEMENT_MOVING));
    ++finishes;
    if (!defer_finish) status.state = APP_DISPLACEMENT_SETTLING;
    return ESP_OK;
}
esp_err_t app_displacement_resume(void)
{
    assert(status.active && status.result.stationary && !status.result.resume_pending);
    ++resumes;
    status.result.resume_pending = true;
    return ESP_OK;
}
void app_displacement_cancel(void)
{
    ++cancels;
    status = (app_displacement_status_t){.available=status.available};
}
const char *esp_err_to_name(esp_err_t error) { return error ? "ESP_FAIL" : "ESP_OK"; }

const lv_font_t *app_ui_chinese_font_size(unsigned px)
{
    static lv_font_t small, large;
    small = font_16; large = font_24;
    small.fallback = &lv_font_montserrat_16; large.fallback = &lv_font_montserrat_24;
    return px >= 24 ? &large : &small;
}
const lv_font_t *app_ui_chinese_font(void) { return app_ui_chinese_font_size(16); }
void app_ui_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    app_ui_label_text(label, text);
}
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text, lv_align_t align, int32_t x,
                          int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y);
    return label;
}
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    if (ui->page_cleanup) {
        void (*cleanup)(app_ui_t *) = ui->page_cleanup;
        ui->page_cleanup = NULL;
        cleanup(ui);
    }
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    ui->screen = lv_obj_create(NULL);
    lv_obj_remove_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x070A12), 0);
    lv_obj_set_style_text_font(ui->screen, app_ui_chinese_font(), 0);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 20, 0xEAF2FF, app_ui_chinese_font_size(24));
    ui->content = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->content, 448, 394);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(ui->content, 24, 0);
    lv_obj_set_style_bg_color(ui->content, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_pad_all(ui->content, 20, 0);
    lv_obj_set_style_border_width(ui->content, 0, 0);
    lv_screen_load_anim(ui->screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    return ui->content;
}
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t cb, uint32_t period)
{ ui->timer = lv_timer_create(cb, period, ui); }

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) { lv_tick_inc(1); lv_timer_handler(); }
}
static void input(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = pointer;
    data->state = held ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 480 && y >= 0 && y < 480);
            frame[y * 480 + x] = *source++;
        }
    lv_display_flush_ready(display);
}
static void snapshot(lv_display_t *display, const char *directory)
{
    lv_refr_now(display);
    char path[512];
    snprintf(path, sizeof(path), "%s/displacement-result.ppm", directory);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n480 480\n255\n");
    for (unsigned i = 0; i < 480 * 480; ++i) {
        const uint16_t pixel = frame[i];
        const uint8_t rgb[] = {((pixel>>11)&31)*255/31, ((pixel>>5)&63)*255/63, (pixel&31)*255/31};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(!fclose(file));
}
static lv_obj_t *find(lv_obj_t *parent, const char *text)
{
    if (lv_obj_check_type(parent, &lv_label_class) && !strcmp(lv_label_get_text(parent), text)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find(lv_obj_get_child(parent, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *button(app_ui_t *ui, const char *text)
{
    lv_obj_t *label = find(ui->screen, text);
    assert(label);
    return lv_obj_get_parent(label);
}
static void press(lv_obj_t *object)
{
    lv_obj_update_layout(lv_obj_get_screen(object));
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    pointer = (lv_point_t){.x=(area.x1+area.x2)/2, .y=(area.y1+area.y2)/2};
    held = true;
    advance(90);
}
static void release(void)
{
    held = false;
    advance(90);
}
static void tap(lv_obj_t *object)
{
    press(object);
    release();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setbuf(stdout, NULL);
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, input);
    app_ui_t ui = {0};
    app_open_displacement(&ui);
    advance(200);
    assert(find(ui.screen, "六轴传感器不可用"));
    lv_obj_t *primary = button(&ui, "开始测量");
    assert(lv_obj_has_state(primary, LV_STATE_DISABLED));
    tap(primary);
    assert(starts == 0);
    status.available = true; advance(200);
    assert(find(ui.screen, "将设备放稳并保持静止，再点开始。"));
    start_error = ESP_FAIL;
    tap(primary);
    assert(starts == 1 && find(ui.screen, "操作未受理 ESP_FAIL"));
    start_error = ESP_OK;
    tap(primary);
    assert(starts == 2 && status.active && find(ui.screen, "正在确认起点"));
    assert(lv_obj_has_state(primary, LV_STATE_DISABLED));
    tap(primary);
    assert(!finishes);
    status.state = APP_DISPLACEMENT_READY; advance(200);
    assert(find(ui.screen, "可以移动") && !lv_obj_has_state(primary, LV_STATE_DISABLED));
    assert(find(ui.screen, "平稳移动，尽量保持朝向，可查看实时位移。"));
    defer_finish = true;
    tap(primary);
    assert(finishes == 1 && find(ui.screen, "正在确认终点"));
    assert(find(ui.screen, "请保持静止，等待终点校准。"));
    assert(lv_obj_has_state(primary, LV_STATE_DISABLED));
    tap(primary);
    assert(finishes == 1 && status.state == APP_DISPLACEMENT_READY);
    status.state = APP_DISPLACEMENT_SETTLING;
    defer_finish = false;
    status.result.distance_m = 0.55f; advance(200);
    assert(!find(ui.screen, "55 cm"));
    status.active = false; status.state = APP_DISPLACEMENT_COMPLETE;
    status.result = (app_displacement_result_t){.samples=32,.max_gap_ms=20,.low_confidence=true};
    advance(200);
    assert(find(ui.screen, "未检测到明显移动") && !find(ui.screen, "<1 cm") &&
           !find(ui.screen, "距起点（终点修正）"));
    status.result.distance_m = 0.004f;
    status.result.elapsed_ms = 620;
    status.result.endpoint_corrected = true;
    advance(200);
    assert(find(ui.screen, "<1 cm") && find(ui.screen, "≈0 cm") && find(ui.screen, "接近起点"));
    status.result.distance_m = 0.006f;
    status.result.xyz_m[2] = -0.006f;
    advance(110);
    assert(find(ui.screen, "<1 cm") && find(ui.screen, "-0.6 cm") && find(ui.screen, "-Z"));
    tap(button(&ui, "清除结果"));
    assert(cancels == 1 && find(ui.screen, "等待测量") && !find(ui.screen, "<1 cm"));
    puts("PASS touch start, unavailable guard, error, stationary finish, settling and result clear");

    tap(primary);
    status.state = APP_DISPLACEMENT_MOVING;
    status.result.distance_m = 0.45f;
    advance(200);
    assert(lv_display_get_inactive_time(display) < 110);
    assert(find(ui.screen, "正在测量") && !find(ui.screen, "45 cm"));
    assert(find(ui.screen, "看实时位移，到终点停稳后轻点结束。"));
    status.result = (app_displacement_result_t){.distance_m=0.45f,.xyz_m={0.36f,-0.27f,0.0f},
        .samples=120,.max_gap_ms=12,.elapsed_ms=1200};
    advance(110);
    assert(find(ui.screen, "距起点（实时估算）") && find(ui.screen, "45 cm"));
    assert(find(ui.screen, "+36 cm") && find(ui.screen, "-27 cm") && find(ui.screen, "≈0 cm"));
    lv_obj_t *direction_title = find(ui.screen, "净方向");
    lv_obj_t *diagonal = find(ui.screen, "+X / -Y");
    assert(direction_title && diagonal && find(ui.screen, "方向以起点的传感器坐标轴为准。"));
    lv_obj_update_layout(ui.screen);
    lv_area_t distance_area, direction_area;
    lv_obj_get_coords(find(ui.screen, "45 cm"), &distance_area);
    lv_obj_get_coords(diagonal, &direction_area);
    assert(distance_area.x2 < direction_area.x1);
    assert(lv_obj_get_height(diagonal) == lv_font_get_line_height(app_ui_chinese_font_size(24)));
    status.result.xyz_m[0] = -0.36f;
    status.result.xyz_m[1] = 0.27f;
    status.result.xyz_m[2] = -0.27f;
    status.result.distance_m = 0.525f;
    advance(110);
    lv_obj_t *three_axes = find(ui.screen, "-X / +Y / -Z");
    assert(three_axes && find(ui.screen, "-36 cm") && find(ui.screen, "+27 cm"));
    lv_obj_update_layout(ui.screen);
    assert(lv_obj_get_height(three_axes) == lv_font_get_line_height(app_ui_chinese_font_size(24)));
    assert(find(ui.screen, "实时值可能漂移，结束时校准。"));
    status.result.xyz_m[1] = NAN;
    advance(110);
    assert(!find(ui.screen, "45 cm") && !find(ui.screen, "-27 cm") && !find(ui.screen, "-X / +Y / -Z"));
    status.result = (app_displacement_result_t){.distance_m=0.50f,.xyz_m={0.4f,-0.3f,0.0f},
        .samples=980,.max_gap_ms=12,.elapsed_ms=9800,.low_confidence=true};
    advance(110);
    assert(find(ui.screen, "50 cm") && find(ui.screen, "距起点（实时估算）"));
    assert(find(ui.screen, "采样 980  ·  耗时 9.8 s  ·  最大间隔 12 ms"));
    assert(find(ui.screen, "当前估算可信度偏低，可停稳后结束。"));
    assert(!lv_obj_has_state(primary, LV_STATE_DISABLED));
    status.result.preview_held = true;
    advance(110);
    assert(find(ui.screen, "正在确认停稳") && find(ui.screen, "读数已暂存，请保持设备静止。"));
    assert(find(ui.screen, "距起点（读数暂存）") && find(ui.screen, "50 cm"));
    status.result.stationary = true;
    advance(600);
    assert(lv_display_get_inactive_time(display) >= 600);
    status.result.preview_held = false;
    advance(110);
    lv_obj_t *stationary = find(ui.screen, "已停稳 · 读数锁定");
    lv_obj_t *resume = find(ui.screen, "点继续测量后再移动，或结束测量。");
    lv_obj_t *live_caption = find(ui.screen, "距起点（已锁定）");
    assert(stationary && resume && live_caption && find(ui.screen, "50 cm"));
    assert(find(ui.screen, "估算质量偏低，三轴读数已锁定。"));
    assert(find(ui.screen, "继续测量") && find(ui.screen, "结束测量"));
    assert(!lv_obj_has_state(primary, LV_STATE_DISABLED));
    lv_obj_update_layout(ui.screen);
    lv_area_t state_area, resume_area, result_area;
    lv_obj_get_coords(stationary, &state_area);
    lv_obj_get_coords(resume, &resume_area);
    lv_obj_get_coords(lv_obj_get_parent(live_caption), &result_area);
    assert(state_area.y2 < resume_area.y1 && resume_area.y2 < result_area.y1);
    assert(lv_obj_get_height(resume) == lv_font_get_line_height(app_ui_chinese_font()));

    tap(primary);
    assert(resumes == 1 && status.result.resume_pending && lv_obj_has_state(primary, LV_STATE_DISABLED));
    assert(find(ui.screen, "放稳片刻，再移动设备继续测量。") && find(ui.screen, "50 cm"));
    status.result.resume_pending = false;
    status.result.stationary = false;
    status.result.distance_m = 0.56f;
    status.result.xyz_m[0] = 0.47f;
    advance(110);
    assert(find(ui.screen, "正在测量") && !find(ui.screen, "已停稳 · 读数锁定"));
    assert(find(ui.screen, "56 cm") && find(ui.screen, "+47 cm"));
    /* The state may lock between press and release, but Finish stays Finish. */
    assert(button(&ui, "结束测量") == primary);
    press(primary);
    assert(lv_obj_has_state(primary, LV_STATE_PRESSED));
    status.result.stationary = true;
    advance(110);
    assert(find(ui.screen, "已停稳 · 读数锁定") &&
           !strcmp(lv_label_get_text(lv_obj_get_child(primary, 0)), "结束测量"));
    release();
    assert(finishes == 2 && resumes == 1 && status.state == APP_DISPLACEMENT_SETTLING);
    assert(find(ui.screen, "正在确认终点") && !find(ui.screen, "已停稳 · 读数锁定"));
    assert(find(ui.screen, "距起点（读数暂存）") && find(ui.screen, "56 cm"));
    status.active = false; status.state = APP_DISPLACEMENT_COMPLETE;
    status.result = (app_displacement_result_t){.distance_m=0.421f,.xyz_m={0.3f,-0.29f,0.001f},
        .samples=86,.max_gap_ms=24,.elapsed_ms=1680,.low_confidence=true,.endpoint_corrected=true};
    advance(200);
    assert(find(ui.screen, "距起点（终点修正）") && !find(ui.screen, "距起点（实时估算）"));
    assert(find(ui.screen, "42 cm") && find(ui.screen, "+30 cm") && find(ui.screen, "-29 cm"));
    assert(find(ui.screen, "质量偏低，建议重测"));
    assert(find(ui.screen, "采样 86  ·  耗时 1.7 s  ·  最大间隔 24 ms"));
    snapshot(display, argv[1]);
    status.result.endpoint_corrected = false;
    advance(110);
    assert(find(ui.screen, "结果不可用") && !find(ui.screen, "42 cm"));
    status.result.endpoint_corrected = true;
    status.result.measurement_limited = true;
    advance(110);
    assert(find(ui.screen, "未确认停稳") && !find(ui.screen, "42 cm"));
    status.result.stationary = true;
    advance(110);
    assert(find(ui.screen, "未确认停稳") && find(ui.screen, "距起点（已锁定）"));
    assert(find(ui.screen, "已保留上次停稳读数，请重新测量。") && find(ui.screen, "42 cm"));
    assert(find(ui.screen, "本段未确认终点，未计入位移。"));
    status.result.fault = APP_DISPLACEMENT_FAULT_ENDPOINT;
    advance(110);
    assert(find(ui.screen, "终点校准未通过") && find(ui.screen, "42 cm"));
    status.result.fault = APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT;
    assert(!lv_obj_has_state(button(&ui, "重新测量"), LV_STATE_DISABLED));
    status.result.xyz_m[1] = NAN; advance(200);
    assert(find(ui.screen, "未确认停稳") && !find(ui.screen, "42 cm"));
    static const struct {
        app_displacement_fault_t fault;
        const char *title, *instruction;
    } failures[] = {
        {APP_DISPLACEMENT_FAULT_START_STILL, "起点静止超时", "将设备放平、放稳，再重新测量。"},
        {APP_DISPLACEMENT_FAULT_WAIT_MOTION, "等待移动超时", "看到「可以移动」后，平稳移动设备。"},
        {APP_DISPLACEMENT_FAULT_MOTION_TIMEOUT, "未确认停稳", "请缩短移动时间，放稳后重新测量。"},
        {APP_DISPLACEMENT_FAULT_ENDPOINT, "终点校准未通过", "请保持朝向，缩短移动距离后重测。"},
        {APP_DISPLACEMENT_FAULT_STOP_STILL, "终点静止超时", "先放稳设备，再轻点结束。"},
        {APP_DISPLACEMENT_FAULT_TIMEOUT, "测量超时", "请缩短测量时间后重试。"},
        {APP_DISPLACEMENT_FAULT_GAP, "采样间隔过长", "请静置后重试。"},
    };
    status.state = APP_DISPLACEMENT_INVALID;
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        status.result.fault = failures[i].fault;
        advance(200);
        lv_obj_t *title = find(ui.screen, failures[i].title);
        lv_obj_t *hint = find(ui.screen, failures[i].instruction);
        lv_obj_t *caption = find(ui.screen, "距起点（净位移）");
        assert(title && hint && caption && !find(ui.screen, "42 cm"));
        assert(!lv_obj_has_state(button(&ui, "重新测量"), LV_STATE_DISABLED));
        assert(!lv_obj_has_state(button(&ui, "清除结果"), LV_STATE_DISABLED));
        lv_obj_update_layout(ui.screen);
        lv_area_t title_area, hint_area, card_area;
        lv_obj_get_coords(title, &title_area);
        lv_obj_get_coords(hint, &hint_area);
        lv_obj_get_coords(lv_obj_get_parent(caption), &card_area);
        assert(title_area.y2 < hint_area.y1 && hint_area.y2 < card_area.y1);
        assert(lv_obj_get_height(title) == lv_font_get_line_height(app_ui_chinese_font_size(24)));
        assert(lv_obj_get_height(hint) == lv_font_get_line_height(app_ui_chinese_font()));
    }
    tap(primary);
    assert(status.active);
    tap(button(&ui, "取消测量"));
    assert(!status.active && cancels == 2);
    tap(primary);
    assert(status.active);
    app_ui_begin_page(&ui, "首页");
    advance(200);
    assert(!status.active && cancels == 3 && lv_mem_test() == LV_RESULT_OK);
    lv_indev_delete(indev);
    lv_display_delete(display);
    lv_deinit();
    puts("PASS signed XYZ, provisional hold, Finish click across lock, explicit resume, timeout checkpoint, cancellation and cleanup");
}

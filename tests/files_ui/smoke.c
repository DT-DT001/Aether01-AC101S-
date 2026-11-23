#include "app_ui_internal.h"
#include "app_otg_control.h"
#include "app_tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const lv_font_t font_16,font_24;
static uint16_t framebuffer[480*480],buffer[480*40];
static bool mounted=true,held,usb_mounted;
static bool usb_blocked,failed_fs;
static bool external_keyboard;
static bool simulate_playback_page;
static bool promotional_samples;
static unsigned retry_requests;
static unsigned catalog_calls, operation_calls, usb_generation=1,tf_generation=1;
static size_t catalog_entries=5;
static lv_point_t point;
static app_io_request_t request;
static unsigned route;
static char opened[192];
static char deleted[192];
static const app_io_entry_t entries[]={
    {.path="/sdcard/Books",.directory=true},
    {.path="/sdcard/Mysterious Nue.m4a",.size=3265245},
    {.path="/sdcard/小说阅读测试：在山川与河流之间继续昨天未完的故事.txt",.size=280000},
    {.path="/sdcard/旅行视频.mp4",.size=127800000},
    {.path="/sdcard/backup.bin",.size=4096},
};
static const app_io_entry_t preview_entries[] = {
    {.path = "/sdcard/书架", .directory = true},
    {.path = "/sdcard/海边散步.m4a", .size = 3265245},
    {.path = "/sdcard/旅行记录.txt", .size = 280000},
    {.path = "/sdcard/周末短片.mp4", .size = 127800000},
    {.path = "/sdcard/备份.bin", .size = 4096},
};
_Static_assert(sizeof(preview_entries) == sizeof(entries), "example listings must have the same length");
uint32_t aether_media_fs_generation(aether_storage_handle_t h,const char *p){(void)h;return !strncmp(p,"/usb",4)?usb_generation:tf_generation;}
bool aether_media_fs_path_ready(aether_storage_handle_t h,const char *p){(void)h;return !strncmp(p,"/usb",4)?usb_mounted:mounted;}
esp_err_t aether_storage_get_diagnostics(aether_storage_handle_t h,aether_storage_diagnostics_t *d){(void)h;*d=(aether_storage_diagnostics_t){.attempted=failed_fs,.layout=AETHER_STORAGE_LAYOUT_MBR,.filesystem=AETHER_STORAGE_FS_FAT32};return ESP_OK;}
esp_err_t aether_storage_request_retry(aether_storage_handle_t h){(void)h;++retry_requests;return ESP_OK;}
bool aether_storage_is_mounted(aether_storage_handle_t h){(void)h;return mounted;}
bool app_io_usb_storage_blocked(app_io_handle_t h){(void)h;return usb_blocked;}
esp_err_t app_otg_get_status(aether_usb_host_status_t *status)
{ *status = (aether_usb_host_status_t){.active = external_keyboard, .keyboard_interfaces = external_keyboard}; return ESP_OK; }
esp_err_t aether_storage_get_status(aether_storage_handle_t h,aether_storage_status_t *s){(void)h;*s=(aether_storage_status_t){.mounted=mounted,.card_inserted=mounted||failed_fs};return ESP_OK;}
const char *esp_err_to_name(esp_err_t e){return e?"ESP_ERR_TIMEOUT":"ESP_OK";}
size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
const lv_font_t *app_ui_chinese_font_size(unsigned px){static lv_font_t s,l;s=font_16;l=font_24;s.fallback=&lv_font_montserrat_16;l.fallback=&lv_font_montserrat_24;return px>=24?&l:&s;}
const lv_font_t *app_ui_chinese_font(void){return app_ui_chinese_font_size(16);}
void app_ui_label_text(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
void app_ui_label_fmt(lv_obj_t *o,const char *f,...){char s[512];va_list a;va_start(a,f);vsnprintf(s,sizeof(s),f,a);va_end(a);app_ui_label_text(o,s);}
lv_obj_t *app_ui_add_text(lv_obj_t *p,const char *s,lv_align_t a,int32_t x,int32_t y,uint32_t c,const lv_font_t *f){lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_style_text_font(o,f?f:app_ui_chinese_font(),0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,a,x,y);return o;}
int64_t esp_timer_get_time(void) { return (int64_t)lv_tick_get() * 1000; }
void app_tools_get_snapshot(app_tools_snapshot_t *snapshot)
{
    *snapshot = (app_tools_snapshot_t){.ready = true, .battery_updated_ms = lv_tick_get(),
        .battery = {.presence_valid = true, .battery_present = true, .percentage_valid = true, .percentage = 83}};
}
void app_ui_set_user_key_exit(app_ui_t *ui, bool enabled) { (void)ui; (void)enabled; }

lv_obj_t *app_ui_new_screen(app_ui_t *ui)
{
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    if (ui->page_cleanup) { ui->page_cleanup(ui); ui->page_cleanup = NULL; }
    ui->io_complete = NULL;
    ui->page_back = NULL;
    ui->files_return = (app_files_return_t){0};
    ui->screen = lv_obj_create(NULL);
    lv_obj_remove_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x101321), 0);
    lv_obj_set_style_bg_opa(ui->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(ui->screen, app_ui_chinese_font(), 0);
    return ui->screen;
}
void app_ui_set_timer(app_ui_t *ui,lv_timer_cb_t cb,uint32_t p){ui->timer=lv_timer_create(cb,p,ui);}
lv_obj_t *app_ui_modal_msgbox(lv_obj_t *owner)
{
    lv_obj_t *box = lv_msgbox_create(NULL);
    lv_obj_set_parent(lv_obj_get_parent(box), owner);
    return box;
}
void app_ui_modal_focus(lv_obj_t *box, lv_obj_t *preferred)
{ (void)box; (void)preferred; }
void app_ui_request_io_ex(app_ui_t *ui,const app_io_request_t *r,void (*cb)(app_ui_t *,const app_io_result_t *))
{ if (r->operation == APP_IO_CATALOG) ++catalog_calls; else ++operation_calls; request=*r;ui->io_complete=cb; }

static void complete(app_ui_t *ui, app_io_result_t *result)
{
    assert(ui->io_complete);
    strlcpy(result->path, request.path, sizeof(result->path));
    void (*cb)(app_ui_t *, const app_io_result_t *) = ui->io_complete;
    ui->io_complete = NULL;
    cb(ui, result);
}

static void deliver(app_ui_t *ui,esp_err_t error)
{
    assert(ui->io_complete && request.operation == APP_IO_CATALOG);
    size_t count = catalog_entries < request.limit ? catalog_entries : request.limit;
    app_io_entry_t *items = calloc(count, sizeof(*items));
    assert(items && count >= sizeof(entries) / sizeof(entries[0]));
    memcpy(items, promotional_samples ? preview_entries : entries, sizeof(entries));
    const char *format = !strcmp(request.path, "/sdcard/Recordings") ?
        "/sdcard/Recordings/recording-%04u.WAV" : "/sdcard/track-%04u.m4a";
    for (size_t i = sizeof(entries) / sizeof(entries[0]); i < count; ++i)
        snprintf(items[i].path, sizeof(items[i].path), format, (unsigned)i);
    for (size_t i = 0; deleted[0] && i < count; ++i) {
        if (strcmp(items[i].path, deleted)) continue;
        --count;
        memmove(items + i, items + i + 1, (count - i) * sizeof(*items));
        break;
    }
    app_io_result_t r = {.operation=request.operation,.error=error,.entries=items,.count=count,
        .total_count=catalog_entries,.truncated=count < catalog_entries};
    complete(ui, &r);
    free(items);
}
#define OPEN(kind,number) void app_open_##kind##_path(app_ui_t *u,const char *p){(void)u;route=number;strlcpy(opened,p,sizeof(opened));}
OPEN(reader,1) OPEN(video,3) OPEN(nes,4)
void app_open_music_path(app_ui_t *ui, const char *path)
{
    route = 2;
    strlcpy(opened, path, sizeof(opened));
    if (simulate_playback_page) (void)app_ui_begin_page(ui, "回放");
}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){uint16_t *v=(uint16_t*)p;for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x)framebuffer[y*480+x]=*v++;lv_display_flush_ready(d);}
static void read_input(lv_indev_t *i,lv_indev_data_t *d){(void)i;d->point=point;d->state=held?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void advance(unsigned ms){for(unsigned i=0;i<ms;++i){lv_tick_inc(1);lv_timer_handler();}}
static lv_obj_t *find(lv_obj_t *p,const char *s){if(lv_obj_check_type(p,&lv_label_class)&&strstr(lv_label_get_text(p),s))return p;for(unsigned i=0;i<lv_obj_get_child_count(p);++i){lv_obj_t *f=find(lv_obj_get_child(p,i),s);if(f)return f;}return NULL;}
static void tap(lv_obj_t *o){lv_area_t a;lv_obj_get_coords(o,&a);point=(lv_point_t){.x=(a.x1+a.x2)/2,.y=(a.y1+a.y2)/2};held=true;advance(80);held=false;advance(80);}
static void long_press(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    point = (lv_point_t){.x = (area.x1 + area.x2) / 2, .y = (area.y1 + area.y2) / 2};
    held = true;
    advance(650);
    held = false;
    advance(100);
}

static void tap_text(lv_obj_t *scope, const char *text)
{
    lv_obj_t *label = find(scope, text);
    assert(label);
    tap(lv_obj_get_parent(label));
}

static lv_obj_t *find_type(lv_obj_t *object, const lv_obj_class_t *type)
{
    if (lv_obj_check_type(object, type)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_type(lv_obj_get_child(object, i), type);
        if (found) return found;
    }
    return NULL;
}

static bool search_caret_drawn(lv_display_t *display, lv_obj_t *field)
{
    lv_refr_now(display);
    lv_obj_t *label = lv_textarea_get_label(field);
    lv_area_t area;
    lv_obj_get_coords(label, &area);
    lv_point_t glyph;
    lv_label_get_letter_pos(label, lv_textarea_get_cursor_pos(field), &glyph);
    const uint16_t color = lv_color_to_u16(lv_color_hex(0x92E7F1));
    const int x = area.x1 + glyph.x;
    const int y = area.y1 + glyph.y;
    assert(x >= 5 && x + 5 < 480 && y >= 0 && y + 14 < 480);
    unsigned columns = 0;
    for (int dx = -5; dx <= 5; ++dx) {
        unsigned matches = 0;
        for (int dy = 4; dy <= 14; ++dy)
            matches += framebuffer[(y + dy) * 480 + x + dx] == color;
        if (matches >= 8) ++columns;
    }
    assert(columns == 0 || columns == 3);
    return columns == 3;
}

static void assert_search_caret_blinks(lv_display_t *display, lv_obj_t *field)
{
    bool shown = false, hidden = false;
    /* Moving the modal or touching a nearby key can change the phase. */
    for (unsigned elapsed = 0; elapsed <= 1100; elapsed += 100) {
        assert(lv_obj_has_state(field, LV_STATE_FOCUSED));
        if (search_caret_drawn(display, field)) shown = true;
        else hidden = true;
        if (shown && hidden) return;
        advance(100);
    }
    assert(shown && hidden);
}

static void assert_search_layout(lv_obj_t *box, lv_obj_t *field, lv_obj_t *keyboard, lv_obj_t *actions)
{
    lv_obj_update_layout(lv_obj_get_screen(box));
    lv_area_t modal, input, dock, alphabet, row;
    lv_obj_get_coords(box, &modal);
    lv_obj_get_coords(field, &input);
    lv_obj_get_coords(lv_obj_get_parent(keyboard), &dock);
    lv_obj_get_coords(keyboard, &alphabet);
    lv_obj_get_coords(actions, &row);
    assert(modal.y1 >= 0 && input.y1 > modal.y1 && input.y2 < modal.y2 && modal.y2 < dock.y1);
    assert(lv_area_get_width(&alphabet) == 472 && lv_area_get_height(&alphabet) == 136);
    assert(alphabet.x1 >= 0 && alphabet.x2 < 480 && alphabet.y2 < row.y1);
    assert(lv_area_get_height(&row) == 38 && row.x1 >= 0 && row.x2 < 480 && row.y2 < 480);
    const char *const labels[] = {"取消", "搜索"};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        lv_obj_t *label = find(box, labels[i]);
        assert(label && lv_obj_is_visible(label));
        lv_area_t button;
        lv_obj_get_coords(lv_obj_get_parent(label), &button);
        assert(lv_area_get_height(&button) >= 44 && button.y1 > input.y2 && button.y2 < dock.y1);
    }
}
static void recorder_page(app_ui_t *ui)
{
    (void)app_ui_begin_page(ui, "录音机");
}

static int32_t open_recording(app_ui_t *ui)
{
    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(find(ui->screen, "Mysterious Nue.m4a")));
    lv_obj_scroll_to_y(list, 12 * 72, LV_ANIM_OFF);
    advance(80);
    const int32_t scroll = lv_obj_get_scroll_y(list);
    lv_obj_t *recording = find(ui->screen, "recording-0013.WAV");
    assert(scroll > 0 && recording && lv_obj_is_visible(recording));
    tap(lv_obj_get_parent(recording));
    assert(route == 2 && !strcmp(opened, "/sdcard/Recordings/recording-0013.WAV"));
    assert(find(ui->screen, "回放") && ui->page_back);
    return scroll;
}

static void test_recordings_back(app_ui_t *ui)
{
    simulate_playback_page = true;
    catalog_entries = 24;
    for (unsigned remounted = 0; remounted < 2; ++remounted) {
        app_open_files_path(ui, "/sdcard/Recordings");
        ui->page_back = recorder_page;
        deliver(ui, ESP_OK);
        advance(80);
        const int32_t scroll = open_recording(ui);
        if (remounted) ++tf_generation;
        tap(lv_obj_get_child(ui->screen, 0));
        assert(find(ui->screen, "文件浏览") && ui->page_back == recorder_page);
        assert(ui->io_complete && !strcmp(request.path, "/sdcard/Recordings"));
        deliver(ui, ESP_OK);
        advance(80);
        lv_obj_t *row = find(ui->screen, remounted ? "Mysterious Nue.m4a" : "recording-0013.WAV");
        assert(row && lv_obj_is_visible(row));
        lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(row));
        assert(lv_obj_get_scroll_y(list) == (remounted ? 0 : scroll));
        tap(lv_obj_get_child(ui->screen, 0));
        assert(find(ui->screen, "录音机") && !ui->page_back);
    }
    puts("PASS Recording WAV playback returns to the same list and scroll, handles TF remount, then returns to Recorder");
}

static void snapshot(lv_display_t *d,const char *dir,const char *name){lv_refr_now(d);char p[1024];snprintf(p,sizeof(p),"%s/%s.ppm",dir,name);FILE *f=fopen(p,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");for(unsigned i=0;i<480*480;++i){uint16_t v=framebuffer[i];uint8_t rgb[]={(v>>11)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}fclose(f);}

static void test_search_candidates(lv_obj_t *keyboard, lv_obj_t *field, lv_display_t *display, const char *output)
{
    lv_area_t alphabet;
    lv_obj_get_coords(keyboard, &alphabet);
    const int width = lv_area_get_width(&alphabet), height = lv_area_get_height(&alphabet);
    /* Tap s and h on the staggered middle row, then i on the top row. */
    const lv_point_t letters[] = {{alphabet.x1 + width / 5, alphabet.y1 + height / 2},
        {alphabet.x1 + width * 3 / 5, alphabet.y1 + height / 2},
        {alphabet.x1 + width * 3 / 4, alphabet.y1 + height / 6}};
    for (size_t i = 0; i < sizeof(letters) / sizeof(letters[0]); ++i) {
        point = letters[i]; held = true; advance(80); held = false; advance(80);
    }
    lv_obj_t *primary = find(lv_obj_get_parent(keyboard), "是");
    assert(primary && lv_obj_is_visible(primary) && !lv_textarea_get_text(field)[0]);
    lv_obj_t *strip = lv_obj_get_parent(lv_obj_get_parent(primary));
    snapshot(display, output, "files-search-candidates");
    lv_area_t viewport;
    lv_obj_get_coords(strip, &viewport);
    point = (lv_point_t){viewport.x2 - 26, (viewport.y1 + viewport.y2) / 2};
    held = true;
    advance(60);
    for (unsigned i = 0; i < 16; ++i) { point.x -= 17; advance(20); }
    held = false;
    advance(550);
    assert(lv_obj_get_scroll_x(strip) > 110 && !lv_textarea_get_text(field)[0]);
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED));
    snapshot(display, output, "files-search-candidates-scrolled");

    lv_obj_t *candidate = NULL;
    for (unsigned i = 1; i < lv_obj_get_child_count(strip); ++i) {
        lv_obj_t *item = lv_obj_get_child(strip, i);
        lv_area_t area;
        lv_obj_get_coords(item, &area);
        lv_obj_t *label = lv_obj_get_child(item, 0);
        if (lv_obj_is_visible(item) && area.x1 >= viewport.x1 && area.x2 <= viewport.x2 &&
            strcmp(lv_label_get_text(label), LV_SYMBOL_RIGHT)) { candidate = item; break; }
    }
    assert(candidate);
    char expected[16];
    strlcpy(expected, lv_label_get_text(lv_obj_get_child(candidate, 0)), sizeof(expected));
    tap(candidate);
    assert(expected[0] && !strcmp(lv_textarea_get_text(field), expected));
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED));
    lv_textarea_set_text(field, "");
}

static lv_obj_t *open_options(app_ui_t *ui, const char *name)
{
    lv_obj_t *label = find(ui->screen, name);
    assert(label && lv_obj_is_visible(label));
    long_press(lv_obj_get_parent(label));
    lv_obj_t *box = find_type(ui->screen, &lv_msgbox_class);
    assert(box && find(box, name) && find(box, "项目操作"));
    return box;
}

static void test_file_operations(app_ui_t *ui, lv_display_t *display, const char *output)
{
    const unsigned before = operation_calls;
    catalog_entries = 28;
    app_open_files(ui);
    deliver(ui, ESP_OK);
    advance(100);

    lv_obj_t *box = open_options(ui, "Books");
    assert(!find(box, "复制") && !find(box, "删除"));
    tap_text(box, "详情");
    box = find_type(ui->screen, &lv_msgbox_class);
    assert(box && find(box, "/sdcard/Books") && find(box, "文件夹"));
    tap_text(box, "关闭");
    assert(!find_type(ui->screen, &lv_msgbox_class));

    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(find(ui->screen, "Mysterious Nue.m4a")));
    lv_obj_scroll_to_y(list, 12 * 72, LV_ANIM_OFF);
    advance(100);
    const int32_t scroll = lv_obj_get_scroll_y(list);
    box = open_options(ui, "track-0013.m4a");
    snapshot(display, output, "files-action-sheet");
    tap_text(box, "删除");
    box = find_type(ui->screen, &lv_msgbox_class);
    assert(box && find(box, "删除文件") && find(box, "track-0013.m4a"));
    snapshot(display, output, "files-delete-confirmation");
    tap_text(box, "取消");
    assert(operation_calls == before && lv_obj_get_scroll_y(list) == scroll);
    box = open_options(ui, "track-0013.m4a");
    tap_text(box, "删除");
    tap_text(find_type(ui->screen, &lv_msgbox_class), "确认删除");
    assert(request.operation == APP_IO_DELETE_FILE && !strcmp(request.path, "/sdcard/track-0013.m4a") &&
           request.media_generation == tf_generation);
    assert(lv_obj_has_state(lv_obj_get_parent(find(ui->screen, "查找")), LV_STATE_DISABLED));
    app_io_result_t result = {.operation = APP_IO_DELETE_FILE, .error = ESP_ERR_TIMEOUT};
    complete(ui, &result);
    assert(find(ui->screen, "删除失败") && lv_obj_get_scroll_y(list) == scroll && !ui->io_complete);
    box = open_options(ui, "track-0013.m4a");
    tap_text(box, "删除");
    tap_text(find_type(ui->screen, &lv_msgbox_class), "确认删除");
    strlcpy(deleted, request.path, sizeof(deleted));
    result.error = ESP_OK;
    complete(ui, &result);
    assert(ui->io_complete && request.operation == APP_IO_CATALOG && !strcmp(request.path, "/sdcard"));
    deliver(ui, ESP_OK);
    advance(100);
    assert(find(ui->screen, "已删除文件") && !find(ui->screen, "track-0013.m4a") &&
           lv_obj_get_scroll_y(list) == scroll);
    puts("PASS Files touch action sheet, folder details, exact file deletion confirm/cancel, busy guard, failure and same-scroll refresh");

    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    advance(100);
    box = open_options(ui, "Mysterious Nue.m4a");
    tap_text(box, "复制");
    assert(find(ui->screen, LV_SYMBOL_PASTE) && !ui->io_complete);
    tap_text(ui->screen, "Books");
    assert(request.operation == APP_IO_CATALOG && !strcmp(request.path, "/sdcard/Books"));
    deliver(ui, ESP_OK);
    advance(100);
    tap_text(ui->screen, LV_SYMBOL_PASTE);
    assert(request.operation == APP_IO_COPY_FILE && request.media_generation == tf_generation &&
           request.destination_generation == tf_generation && !strcmp(request.path, "/sdcard/Mysterious Nue.m4a") &&
           !strcmp(request.destination, "/sdcard/Books/Mysterious Nue.m4a"));
    result = (app_io_result_t){.operation = APP_IO_COPY_FILE, .error = ESP_OK};
    complete(ui, &result);
    deliver(ui, ESP_OK);
    advance(100);
    assert(find(ui->screen, "已复制文件"));
    tap_text(ui->screen, LV_SYMBOL_UP);
    deliver(ui, ESP_OK);
    advance(100);
    box = open_options(ui, "Mysterious Nue.m4a");
    tap_text(box, "移动");
    lv_obj_t *source = find_type(ui->content, &lv_dropdown_class);
    assert(source);
    usb_mounted = true;
    lv_dropdown_set_selected(source, 1);
    lv_obj_send_event(source, LV_EVENT_VALUE_CHANGED, NULL);
    deliver(ui, ESP_OK);
    advance(100);
    tap_text(ui->screen, LV_SYMBOL_PASTE);
    assert(request.operation == APP_IO_MOVE_FILE && request.destination_generation == usb_generation &&
           !strcmp(request.destination, "/usb/Mysterious Nue.m4a"));
    result = (app_io_result_t){.operation = APP_IO_MOVE_FILE, .error = ESP_ERR_INVALID_STATE, .target_exists = true};
    complete(ui, &result);
    assert(find(ui->screen, "目标已有同名文件") && find(ui->screen, LV_SYMBOL_PASTE));
    tap_text(ui->screen, LV_SYMBOL_PASTE);
    result = (app_io_result_t){.operation = APP_IO_MOVE_FILE, .error = ESP_FAIL, .source_retained = true};
    complete(ui, &result);
    deliver(ui, ESP_OK);
    advance(100);
    assert(find(ui->screen, "原文件仍保留") && !find(ui->screen, LV_SYMBOL_PASTE));
    puts("PASS Files copy and move keep exact clipboard source across directory/volume changes and report conflicts or retained source");

    lv_dropdown_set_selected(source, 0);
    lv_obj_send_event(source, LV_EVENT_VALUE_CHANGED, NULL);
    deliver(ui, ESP_OK);
    advance(100);
    tap_text(ui->screen, "查找");
    box = find_type(ui->screen, &lv_msgbox_class);
    lv_obj_t *field = find_type(box, &lv_textarea_class);
    lv_obj_t *keyboard = find_type(ui->screen, &lv_keyboard_class);
    assert(field && keyboard);
    tap(field);
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED) && search_caret_drawn(display, field));
    assert(lv_obj_get_style_anim_duration(field, LV_PART_CURSOR) == 500);
    advance(450);
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED) && !search_caret_drawn(display, field));
    advance(500);
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED) && search_caret_drawn(display, field));
    lv_obj_t *actions = find_type(lv_obj_get_parent(keyboard), &lv_buttonmatrix_class);
    assert(actions && actions != keyboard);
    assert_search_layout(box, field, keyboard, actions);
    tap(actions); /* Its center is the space key. */
    assert(!strcmp(lv_textarea_get_text(field), " ") && lv_textarea_get_cursor_pos(field) == 1);
    assert_search_caret_blinks(display, field);
    lv_textarea_set_text(field, "");
    snapshot(display, output, "files-search-keyboard");
    test_search_candidates(keyboard, field, display, output);
    tap_text(box, "搜索");
    assert(find(box, "请输入文件名的一部分") && !ui->io_complete);
    assert_search_layout(box, field, keyboard, actions);
    assert(!lv_obj_has_state(field, LV_STATE_FOCUSED) && !search_caret_drawn(display, field));
    advance(650);
    assert(!search_caret_drawn(display, field));
    lv_textarea_set_text(field, " 行程 ");
    tap_text(box, "搜索");
    assert(request.operation == APP_IO_SEARCH_FILES && !strcmp(request.path, "/sdcard") &&
           !strcmp(request.query, "行程") && request.media_generation == tf_generation);
    result = (app_io_result_t){.operation = APP_IO_SEARCH_FILES, .error = ESP_ERR_TIMEOUT};
    complete(ui, &result);
    assert(find(ui->screen, "查找失败") && find(ui->screen, "Mysterious Nue.m4a"));
    tap_text(ui->screen, "查找");
    box = find_type(ui->screen, &lv_msgbox_class);
    field = find_type(box, &lv_textarea_class);
    keyboard = find_type(ui->screen, &lv_keyboard_class);
    assert(keyboard && lv_obj_is_visible(keyboard));
    tap(field);
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED) && search_caret_drawn(display, field));
    advance(450);
    assert(lv_obj_has_state(field, LV_STATE_FOCUSED) && !search_caret_drawn(display, field));
    external_keyboard = true;
    advance(200);
    assert(!lv_obj_is_visible(keyboard) && lv_obj_is_visible(field));
    assert_search_caret_blinks(display, field);
    lv_textarea_set_text(field, "行程");
    external_keyboard = false;
    advance(200);
    assert(lv_obj_is_visible(keyboard) && !strcmp(lv_textarea_get_text(field), "行程"));
    assert_search_caret_blinks(display, field);
    lv_obj_send_event(field, LV_EVENT_READY, NULL);
    advance(100);
    assert(request.operation == APP_IO_SEARCH_FILES && !find_type(ui->screen, &lv_msgbox_class));
    app_io_entry_t hit = {.path = "/sdcard/Books/行程.txt", .size = 900, .modified_s = 1720000000U};
    result = (app_io_result_t){.operation = APP_IO_SEARCH_FILES, .error = ESP_OK, .entries = &hit, .count = 1, .total_count = 1};
    complete(ui, &result);
    external_keyboard = false;
    advance(200);
    assert(find(ui->screen, "查找 · 1 项") && find(ui->screen, "行程.txt") && find(ui->screen, "Books"));
    snapshot(display, output, "files-search-results");
    tap_text(ui->screen, LV_SYMBOL_CLOSE);
    assert(request.operation == APP_IO_CATALOG && !strcmp(request.path, "/sdcard"));
    deliver(ui, ESP_OK);
    advance(120);
    assert(find(ui->screen, "Mysterious Nue.m4a") && !find(ui->screen, "行程.txt"));
    tap_text(ui->screen, "查找");
    tap_text(find_type(ui->screen, &lv_msgbox_class), "取消");
    assert(!ui->io_complete && !find_type(ui->screen, &lv_msgbox_class));
    puts("PASS Files touch Pinyin search dock, empty validation, exact trimmed query, failure preserves listing, success and clear");
    puts("PASS Files search caret blinks on/off/on, stays active after Space and USB keyboard docking, and remains hidden on blur");
    puts("PASS Enlarged Files keyboard keeps the modal above the keys, scrolls and selects candidates by touch, and restores on USB keyboard removal");
}

static void capture_phone_files(app_ui_t *ui, lv_display_t *display, const char *output)
{
    promotional_samples = true;
    catalog_entries = sizeof(preview_entries) / sizeof(preview_entries[0]);
    app_open_files(ui);
    deliver(ui, ESP_OK);
    advance(250);
    snapshot(display, output, "files-promo-catalog");
    lv_obj_t *box = open_options(ui, "海边散步.m4a");
    snapshot(display, output, "files-promo-actions");
    tap_text(box, "取消");
}
int main(int argc,char **argv)
{
    assert(argc==3);bool before=!strcmp(argv[2],"before");setbuf(stdout,NULL);lv_init();lv_display_t *d=lv_display_create(480,480);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    lv_indev_t *in=lv_indev_create();lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_display(in,d);lv_indev_set_read_cb(in,read_input);
    app_ui_t ui={.storage=(void*)1};app_open_files(&ui);deliver(&ui,ESP_OK);advance(200);snapshot(d,argv[1],before?"files-before":"files-after");
    lv_obj_t *name=find(ui.screen,"Mysterious Nue.m4a");assert(name);tap(lv_obj_get_parent(name));
    if(before){assert(route==0&&find(ui.screen,"暂不支持"));puts("BASELINE actual file browser rejects supported .m4a and creates circular-scroll filenames");}
    else{
        assert(route==2&&!strcmp(opened,entries[1].path));
        lv_obj_t *list=lv_obj_get_parent(lv_obj_get_parent(name));
        route=0;point=(lv_point_t){.x=250,.y=300};held=true;advance(60);
        for(int y=290;y>=220;y-=10){point.y=y;advance(30);}held=false;advance(1100);
        assert(!route&&!ui.io_complete&&lv_obj_get_scroll_y(list)>40);
        snapshot(d,argv[1],"files-after-scrolled");
        point=(lv_point_t){.x=250,.y=280};held=true;advance(60);
        for(int y=290;y<=370;y+=10){point.y=y;advance(30);}held=false;advance(1100);
        assert(!route&&!ui.io_complete);lv_obj_scroll_to_y(list,0,LV_ANIM_OFF);advance(80);
        lv_obj_t *refresh=lv_obj_get_parent(find(ui.screen,LV_SYMBOL_REFRESH));tap(refresh);assert(ui.io_complete&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        deliver(&ui,ESP_ERR_TIMEOUT);advance(80);assert(find(ui.screen,"读取失败"));assert(!lv_obj_has_state(refresh,LV_STATE_DISABLED));snapshot(d,argv[1],"files-after-error");
        assert(!find(ui.screen,"上一页")&&!find(ui.screen,"下一页"));
        catalog_entries=4100;
        tap(refresh);
        assert(request.limit==4096&&request.offset==0);
        deliver(&ui,ESP_OK);advance(80);
        assert(lv_obj_get_child_count(list)==13&&find(ui.screen,"部分项目未索引"));
        const unsigned scans=catalog_calls;
        lv_obj_scroll_to_y(list,INT32_MAX,LV_ANIM_OFF);advance(120);
        lv_obj_t *last=find(ui.screen,"track-4095.m4a");
        assert(last&&lv_obj_is_visible(last)&&catalog_calls==scans);
        tap(lv_obj_get_parent(last));
        assert(route==2&&!strcmp(opened,"/sdcard/track-4095.m4a"));
        catalog_entries=5;
        tap(refresh);assert(ui.io_complete);deliver(&ui,ESP_OK);mounted=false;advance(550);assert(find(ui.screen,"存储设备已移除"));
        assert(lv_obj_has_state(refresh,LV_STATE_DISABLED));mounted=true;advance(550);assert(ui.io_complete);deliver(&ui,ESP_OK);assert(find(ui.screen,"Mysterious Nue.m4a"));
        lv_timer_delete(ui.timer);ui.page_cleanup(&ui);lv_obj_delete(ui.screen);ui=(app_ui_t){.storage=(void*)1};
        app_open_files_path(&ui,"/sdcard/Recordings");assert(!strcmp(request.path,"/sdcard/Recordings"));
        deliver(&ui,ESP_ERR_NOT_FOUND);advance(80);refresh=lv_obj_get_parent(find(ui.screen,LV_SYMBOL_REFRESH));
        assert(!lv_obj_has_state(refresh,LV_STATE_DISABLED));tap(refresh);assert(ui.io_complete);deliver(&ui,ESP_OK);
        tap(lv_obj_get_parent(find(ui.screen,LV_SYMBOL_UP)));assert(!strcmp(request.path,"/sdcard"));deliver(&ui,ESP_OK);
        mounted=false;usb_blocked=true;advance(550);const unsigned before_ready=catalog_calls;
        mounted=true;advance(550);assert(catalog_calls==before_ready&&!ui.io_complete&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        usb_blocked=false;advance(550);assert(catalog_calls==before_ready+1&&ui.io_complete);deliver(&ui,ESP_OK);
        advance(550);assert(catalog_calls==before_ready+1&&!ui.io_complete&&!lv_obj_has_state(refresh,LV_STATE_DISABLED));
        ++tf_generation;advance(550);assert(ui.io_complete&&!strcmp(request.path,"/sdcard"));deliver(&ui,ESP_OK);
        lv_obj_t *source=NULL;
        for(unsigned i=0;i<lv_obj_get_child_count(ui.content);++i){lv_obj_t *o=lv_obj_get_child(ui.content,i);if(lv_obj_check_type(o,&lv_dropdown_class))source=o;}
        assert(source);usb_mounted=true;lv_dropdown_set_selected(source,1);lv_obj_send_event(source,LV_EVENT_VALUE_CHANGED,NULL);
        assert(ui.io_complete&&!strcmp(request.path,"/usb"));deliver(&ui,ESP_OK);
        assert(lv_obj_has_state(lv_obj_get_parent(find(ui.screen,LV_SYMBOL_UP)),LV_STATE_DISABLED));
        ++usb_generation;advance(550);
        assert(ui.io_complete&&!strcmp(request.path,"/usb"));deliver(&ui,ESP_OK);
        usb_mounted=false;advance(550);assert(find(ui.screen,"USB U盘未就绪")&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        assert(mounted);usb_mounted=true;advance(550);assert(ui.io_complete&&!strcmp(request.path,"/usb"));deliver(&ui,ESP_OK);
        lv_dropdown_set_selected(source,0);lv_obj_send_event(source,LV_EVENT_VALUE_CHANGED,NULL);
        assert(ui.io_complete&&!strcmp(request.path,"/sdcard"));deliver(&ui,ESP_OK);
        puts("PASS real file volume switch selects USB root, prevents root escape and tracks USB detach independently of present TF");
        mounted=false;failed_fs=true;advance(550);
        assert(find(ui.screen,"TF 挂载失败")&&!lv_obj_has_state(refresh,LV_STATE_DISABLED));
        tap(refresh);assert(retry_requests==1&&lv_obj_has_state(refresh,LV_STATE_DISABLED));
        advance(1100);assert(!lv_obj_has_state(refresh,LV_STATE_DISABLED));
        failed_fs=false;mounted=true;advance(550);assert(ui.io_complete);deliver(&ui,ESP_OK);
        puts("PASS FAT32 mount failure shows cached format/layout, permits bounded manual retry without formatting, and refreshes after recovery");
        puts("PASS files auto-refresh waits for both remount and USB-gate release; exactly one fresh catalog replaces the blocked state");
        puts("PASS file browser pointer scrolling, catalog-tail selection with 12 recycled rows, error/retry, hotplug and parent navigation");
        test_recordings_back(&ui);
        test_file_operations(&ui, d, argv[1]);
        capture_phone_files(&ui, d, argv[1]);
    }
    if (ui.timer) lv_timer_delete(ui.timer);
    if (ui.page_cleanup) ui.page_cleanup(&ui);
    lv_obj_delete(ui.screen);lv_indev_delete(in);assert(lv_mem_test()==LV_RESULT_OK);lv_display_delete(d);lv_deinit();
}

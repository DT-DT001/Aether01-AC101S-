#include "app_ui_internal.h"
#include "app_reader_file.h"
#include "app_audio_system.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern const lv_font_t font_16, font_24;
static uint16_t framebuffer[480 * 480];
static uint16_t draw_buffer[480 * 40];
static const char *book_path;
static const char *fixture_directory;
static bool mounted = true;
static bool usb_blocked;
static unsigned book_catalog_calls,music_catalog_calls,text_calls;
static int64_t now_us;
static lv_point_t pointer;
static bool pointer_down;
static app_io_request_t saved;
static bool has_saved;
static aether_audio_player_status_t audio_status = {
    .state = AETHER_AUDIO_PLAYING, .active = true, .volume = 100,
    .duration_ms = 205000, .position_ms = 42000, .sample_rate = 44100,
    .channels = 2, .track_count = 2, .mp3 = true,
    .path = "/sdcard/Music/海边的风.mp3",
};
static app_audio_system_snapshot_t master_audio = {.ready=true,.master_volume=65,.applied_volume=65};
void app_audio_system_get_snapshot(app_audio_system_snapshot_t *s) {*s=master_audio;}
esp_err_t app_audio_system_set_volume(uint8_t value,bool persist)
{(void)persist;master_audio.master_volume=value;return ESP_OK;}
static char playlist[16][192];
static esp_err_t playlist_error;
static unsigned playlist_updates;
static unsigned track_selections;
static esp_err_t music_catalog_error;
static size_t music_catalog_count = 2;

size_t strlcpy(char *destination, const char *source, size_t size)
{
    size_t length = strlen(source);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(destination, source, n); destination[n] = 0; }
    return length;
}
const char *esp_err_to_name(esp_err_t error) { return error ? "IO ERROR" : "OK"; }
int64_t esp_timer_get_time(void) { return now_us; }
bool aether_storage_is_mounted(aether_storage_handle_t storage) { (void)storage; return mounted; }
bool app_io_usb_storage_blocked(app_io_handle_t io) { (void)io; return usb_blocked; }
const lv_font_t *app_ui_chinese_font_size(unsigned px) { return px >= 24 ? &font_24 : &font_16; }
const lv_font_t *app_ui_chinese_font(void) { return &font_16; }
esp_err_t app_io_bookmark_status(app_io_handle_t io) { (void)io; return ESP_OK; }
bool app_io_bookmark_pending(app_io_handle_t io) { (void)io; return false; }
esp_err_t app_io_submit_request(app_io_handle_t io, const app_io_request_t *q)
{ (void)io; assert(q->operation == APP_IO_READER_SAVE); saved = *q; has_saved = true; return ESP_OK; }
esp_err_t app_io_release_media(app_io_handle_t io) { (void)io; return ESP_OK; }

void app_ui_request_io_ex(app_ui_t *ui, const app_io_request_t *q,
                          void (*complete)(app_ui_t *, const app_io_result_t *))
{
    app_io_result_t r = {.operation = q->operation, .generation = ui->page_generation};
    if(q->operation==APP_IO_CATALOG){if(q->filter==APP_IO_MUSIC)++music_catalog_calls;else ++book_catalog_calls;}
    if(q->operation==APP_IO_READ_TEXT)++text_calls;
    if(usb_blocked&&q->operation!=APP_IO_READER_LOAD&&q->operation!=APP_IO_READER_SAVE){r.error=ESP_ERR_INVALID_STATE;complete(ui,&r);return;}
    if (q->operation == APP_IO_CATALOG) {
        if (q->filter == APP_IO_MUSIC && (music_catalog_error != ESP_OK || !music_catalog_count)) {
            r.error = music_catalog_error;
            complete(ui, &r);
            return;
        }
        struct stat info; assert(!stat(book_path, &info));
        r.count = r.total_count = q->filter == APP_IO_MUSIC ? 2 : 1;
        r.entries = calloc(r.count, sizeof(*r.entries)); assert(r.entries);
        strlcpy(r.entries->path, q->filter == APP_IO_MUSIC ?
                "/sdcard/Music/海边的风.mp3" : book_path, sizeof(r.entries->path));
        r.entries->size = (uint64_t)info.st_size;
        if (r.count == 2) {
            strlcpy(r.entries[1].path, "/sdcard/Music/远方的故事.wav", sizeof(r.entries[1].path));
            const char *paths[] = {r.entries[0].path, r.entries[1].path};
            r.error = aether_audio_player_set_playlist(ui->audio_player, paths, r.count);
            r.playlist_prepared = r.error == ESP_OK;
        }
    } else if (q->operation == APP_IO_READER_LOAD) {
        r.font_px = has_saved ? saved.font_px : 24; r.dark = has_saved ? saved.dark : true;
        r.offset = has_saved ? saved.offset : 0;
        r.encoding_hint = has_saved ? saved.encoding_hint : 0;
        r.turn_mode = has_saved ? saved.turn_mode : APP_READER_TURN_SWIPE_HORIZONTAL;
    } else if (q->operation == APP_IO_READ_TEXT) {
        const char *source = q->path;
        const size_t path_length = strlen(source);
        char lyric_fixture[1024];
        if (path_length >= 4 && !strcmp(source + path_length - 4, ".lrc")) {
            const char *name = strrchr(source, '/');
            assert(name);
            const int length = snprintf(lyric_fixture, sizeof(lyric_fixture), "%s/%s", fixture_directory, name + 1);
            assert(length > 0 && (size_t)length < sizeof(lyric_fixture));
            source = lyric_fixture;
        }
        app_reader_chunk_t chunk;
        r.error = app_reader_file_read_ex(source, q->offset, q->limit, q->source_boundary, q->encoding_hint, &chunk);
        r.text = chunk.text; r.offset = chunk.offset; r.next_offset = chunk.next_offset;
        r.source_offsets = chunk.source_offsets; r.encoding = chunk.encoding; r.invalid_count = chunk.invalid_count;
        r.file_size = chunk.file_size; r.text_start_offset = chunk.text_start_offset;
    } else assert(!"unexpected reader operation");
    complete(ui, &r);
    free(r.text); free(r.source_offsets); free(r.entries);
}

void app_ui_label_text(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char text[1024]; va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    lv_label_set_text(label, text);
}
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text, lv_align_t align,
                          int32_t x, int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent); lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0); lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y); return label;
}
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    ++ui->page_generation;
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    if (ui->page_cleanup) { ui->page_cleanup(ui); ui->page_cleanup = NULL; }
    ui->page_back = NULL;
    lv_obj_t *old = ui->screen;
    ui->screen = lv_obj_create(NULL); lv_obj_remove_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x070A12), 0);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 20, 0xEAF2FF, &font_24);
    lv_obj_t *back = lv_button_create(ui->screen);
    lv_obj_set_size(back, 72, 44); lv_obj_set_pos(back, 14, 12);
    lv_obj_set_style_radius(back, 18, 0); lv_obj_set_style_bg_color(back, lv_color_hex(0x18243A), 0);
    app_ui_add_text(back, LV_SYMBOL_LEFT, LV_ALIGN_CENTER, 0, 0, 0xBFD7FF, &lv_font_montserrat_16);
    ui->content = lv_obj_create(ui->screen); lv_obj_set_size(ui->content, 448, 394);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(ui->content, 24, 0); lv_obj_set_style_border_width(ui->content, 0, 0);
    lv_obj_set_style_bg_color(ui->content, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_pad_all(ui->content, 20, 0);
    lv_screen_load(ui->screen); if (old) lv_obj_delete(old);
    return ui->content;
}
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t ms)
{
    if (ui->timer) lv_timer_delete(ui->timer);
    ui->timer = lv_timer_create(callback, ms, ui);
}

esp_err_t aether_audio_player_get_status(aether_audio_player_handle_t h, aether_audio_player_status_t *status)
{ assert(h); *status = audio_status; return ESP_OK; }
esp_err_t aether_audio_player_get_resume_path(aether_audio_player_handle_t h, char *path, size_t size)
{ assert(h && path && size); return ESP_ERR_NOT_FOUND; }
esp_err_t aether_audio_player_restore(aether_audio_player_handle_t h)
{ assert(h); return ESP_ERR_NOT_FOUND; }
esp_err_t aether_audio_player_play(aether_audio_player_handle_t h, const char *path, int volume)
{
    assert(h && path && volume >= 0 && volume <= 100);
    strlcpy(audio_status.path, path, sizeof(audio_status.path));
    audio_status.volume = (uint8_t)volume;
    audio_status.active = true;
    audio_status.state = AETHER_AUDIO_PLAYING;
    return ESP_OK;
}
esp_err_t aether_audio_player_set_playlist(aether_audio_player_handle_t h, const char *const *paths, size_t count)
{
    assert(h && count <= 16);
    if (playlist_error != ESP_OK) return playlist_error;
    for (size_t i = 0; i < count; ++i) strlcpy(playlist[i], paths[i], sizeof(playlist[i]));
    audio_status.track_count = (uint16_t)count; ++playlist_updates; return ESP_OK;
}
esp_err_t aether_audio_player_select(aether_audio_player_handle_t h, size_t index)
{
    ++track_selections;
    assert(h && index < audio_status.track_count); audio_status.track_index = (uint16_t)index;
    strlcpy(audio_status.path, playlist[index], sizeof(audio_status.path));
    audio_status.active = true; audio_status.state = AETHER_AUDIO_PLAYING; audio_status.position_ms = 0; return ESP_OK;
}
esp_err_t aether_audio_player_next(aether_audio_player_handle_t h, int direction)
{
    int index = (int)audio_status.track_index + direction;
    if (index < 0) index = audio_status.track_count - 1;
    return aether_audio_player_select(h, (size_t)index % audio_status.track_count);
}
esp_err_t aether_audio_player_set_paused(aether_audio_player_handle_t h, bool paused)
{ assert(h); audio_status.state = paused ? AETHER_AUDIO_PAUSED : AETHER_AUDIO_PLAYING; return ESP_OK; }
esp_err_t aether_audio_player_stop(aether_audio_player_handle_t h)
{ assert(h); audio_status.state = AETHER_AUDIO_STOPPED; audio_status.active = false; return ESP_OK; }
esp_err_t aether_audio_player_set_repeat(aether_audio_player_handle_t h, aether_audio_repeat_t repeat)
{ assert(h); audio_status.repeat = repeat; return ESP_OK; }
esp_err_t aether_audio_player_set_eq(aether_audio_player_handle_t h, const aether_audio_eq_t *eq)
{ assert(h && eq); audio_status.eq = *eq; return ESP_OK; }
esp_err_t aether_audio_player_set_volume(aether_audio_player_handle_t h, int volume)
{ assert(h); audio_status.volume = (uint8_t)volume; return ESP_OK; }
esp_err_t aether_audio_player_seek(aether_audio_player_handle_t h, uint32_t position)
{ assert(h); audio_status.position_ms = position; return ESP_OK; }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 480 && y >= 0 && y < 480);
            framebuffer[y * 480 + x] = *source++;
        }
    lv_display_flush_ready(display);
}
static void tick(void)
{
    for (unsigned i = 0; i < 5; ++i) { lv_tick_inc(100); now_us += 100000; lv_timer_handler(); }
    lv_refr_now(NULL);
}
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = pointer;
    data->state = pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 10) {
        lv_tick_inc(10);
        now_us += 10000;
        lv_timer_handler();
    }
}

static void gesture(int x, int y, int end_x, int end_y, unsigned hold_ms)
{
    pointer = (lv_point_t){x, y};
    pointer_down = true;
    advance(hold_ms);
    if (x != end_x || y != end_y) {
        for (int i = 1; i <= 5; ++i) {
            pointer.x = x + (end_x - x) * i / 5;
            pointer.y = y + (end_y - y) * i / 5;
            advance(30);
        }
    }
    pointer_down = false;
    advance(70);
}

static void show_reader_toolbar(void) { gesture(220, 145, 220, 145, 700); }
static void swipe_next(void) { gesture(385, 150, 85, 150, 60); }
static void swipe_previous(void) { gesture(85, 150, 385, 150, 60); }
static void snapshot(const char *directory, const char *name)
{
    tick(); char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb"); assert(file); fprintf(file, "P6\n480 480\n255\n");
    for (size_t i = 0; i < 480U * 480U; ++i) {
        unsigned value = framebuffer[i];
        unsigned char rgb[] = {(unsigned char)(((value >> 11) & 31) * 255 / 31),
            (unsigned char)(((value >> 5) & 63) * 255 / 63), (unsigned char)((value & 31) * 255 / 31)};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(!fclose(file)); printf("RENDER %s\n", path);
}
static lv_obj_t *find_text(lv_obj_t *parent, const char *text)
{
    if (lv_obj_check_type(parent, &lv_label_class) && !strcmp(lv_label_get_text(parent), text)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(parent, (int32_t)i), text); if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_text_contains(lv_obj_t *parent, const char *text)
{
    if (lv_obj_check_type(parent,&lv_label_class) && strstr(lv_label_get_text(parent),text)) return parent;
    for (uint32_t i=0;i<lv_obj_get_child_count(parent);++i) {
        lv_obj_t *found=find_text_contains(lv_obj_get_child(parent,(int32_t)i),text); if(found)return found;
    }
    return NULL;
}
static void click(app_ui_t *ui, const char *text)
{
    lv_obj_t *label = find_text(ui->screen, text); assert(label);
    lv_obj_t *button = lv_obj_get_parent(label); assert(lv_obj_check_type(button, &lv_button_class));
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL); tick();
}
static lv_obj_t *find_slider(lv_obj_t *parent, unsigned wanted, unsigned *seen)
{
    if (lv_obj_check_type(parent, &lv_slider_class) && (*seen)++ == wanted) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find_slider(lv_obj_get_child(parent, (int32_t)i), wanted, seen); if (found) return found;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 3); book_path = argv[1];
    lv_init();
    lv_display_t *display = lv_display_create(480, 480); assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, read_pointer);
    app_ui_t ui = {0}; app_open_reader(&ui); snapshot(argv[2], "reader-shelf");
    app_open_reader_path(&ui, book_path); snapshot(argv[2], "reader-24-night");
    lv_obj_t *toolbar = lv_obj_get_parent(lv_obj_get_parent(find_text(ui.screen, "左右滑动")));
    assert(lv_obj_has_flag(toolbar, LV_OBJ_FLAG_HIDDEN));
    assert(!find_text(ui.screen, "下一页") && !find_text(ui.screen, "上一页"));
    const unsigned before_modes = text_calls;
    show_reader_toolbar();
    assert(!lv_obj_has_flag(toolbar, LV_OBJ_FLAG_HIDDEN) && text_calls == before_modes);
    assert(find_text_contains(ui.screen,"0.0% · ") && !find_text_contains(ui.screen,"缺字0/坏码0"));
    snapshot(argv[2], "reader-controls");
    click(&ui, "左右滑动");
    gesture(400, 160, 400, 160, 60);
    gesture(70, 160, 70, 160, 60);
    assert(text_calls == before_modes + 2);
    swipe_next();
    assert(text_calls == before_modes + 2);
    click(&ui, "左右点击");
    gesture(220, 310, 220, 110, 60);
    gesture(220, 110, 220, 310, 60);
    assert(text_calls == before_modes + 4);
    click(&ui, "上下滑动");
    gesture(220, 310, 220, 310, 60);
    gesture(220, 110, 220, 110, 60);
    assert(text_calls == before_modes + 6);
    advance(3200);
    assert(lv_obj_has_flag(toolbar, LV_OBJ_FLAG_HIDDEN));
    app_open_reader_path(&ui, book_path);
    assert(saved.turn_mode == APP_READER_TURN_TAP_VERTICAL && find_text(ui.screen, "上下点击"));
    show_reader_toolbar();
    click(&ui, "上下点击");
    click(&ui, "24号字"); snapshot(argv[2], "reader-16-night");
    click(&ui, "日间"); snapshot(argv[2], "reader-16-day");
    click(&ui, "16号字"); snapshot(argv[2], "reader-24-day");
    for (unsigned i = 0; i < 50; ++i) { swipe_next(); swipe_previous(); }
    swipe_next(); tick(); tick(); tick(); tick();
    assert(has_saved && saved.offset > 3);
    const uint64_t resume = saved.offset;
    app_open_reader_path(&ui, book_path); tick(); tick(); tick(); tick();
    assert(saved.offset == resume); snapshot(argv[2], "reader-resumed");
    mounted = false; tick(); snapshot(argv[2], "reader-card-removed");
    mounted = true; tick(); snapshot(argv[2], "reader-card-restored");
    unsigned seek_seen=0; lv_obj_t *reader_seek=find_slider(ui.screen,0,&seek_seen); assert(reader_seek);
    lv_slider_set_value(reader_seek,1000,LV_ANIM_OFF); lv_obj_send_event(reader_seek,LV_EVENT_RELEASED,NULL);
    tick(); tick(); tick(); tick();
    assert(find_text_contains(ui.screen,"  完") && !find_text(ui.screen,"已到书末"));
    const uint64_t last_page=saved.offset;
    app_open_reader_path(&ui,book_path); advance(100); swipe_previous(); tick(); tick(); tick(); tick();
    assert(saved.offset<last_page && last_page-saved.offset<4096 && !find_text(ui.screen,"已到书末"));
    swipe_next(); tick(); tick(); tick(); tick();
    assert(saved.offset==last_page);
    click(&ui, "自动编码");
    app_open_reader_path(&ui, book_path);
    assert(has_saved && saved.encoding_hint == 1 && find_text(ui.screen,"UTF-8"));
    click(&ui,"UTF-8"); click(&ui,"GBK"); click(&ui,"UTF-16LE"); click(&ui,"UTF-16BE");
    assert(find_text(ui.screen,"自动编码"));
    for (unsigned i = 0; i < 50; ++i) { app_open_reader(&ui); app_open_reader_path(&ui, book_path); tick(); }
    ui.audio_player = (aether_audio_player_handle_t)1;
    fixture_directory = argv[2];
    app_open_music(&ui); snapshot(argv[2], "music-playing");
    lv_obj_t *active_lyric = find_text(ui.screen, "海风轻轻穿过长街");
    assert(active_lyric && lv_color_eq(lv_obj_get_style_text_color(active_lyric, LV_PART_MAIN), lv_color_hex(0x79DBB7)));
    click(&ui, LV_SYMBOL_PAUSE); assert(audio_status.state == AETHER_AUDIO_PAUSED);
    snapshot(argv[2], "music-paused"); click(&ui, LV_SYMBOL_PLAY);
    click(&ui, LV_SYMBOL_NEXT); assert(audio_status.track_index == 1);
    click(&ui, LV_SYMBOL_PREV); assert(audio_status.track_index == 0);
    click(&ui, "列表循环"); assert(audio_status.repeat == AETHER_AUDIO_REPEAT_ONE);
    unsigned seen = 0; lv_obj_t *volume = find_slider(ui.screen, 1, &seen); assert(volume);
    lv_slider_set_value(volume, 27, LV_ANIM_OFF); lv_obj_send_event(volume, LV_EVENT_VALUE_CHANGED, NULL);
    assert(master_audio.master_volume == 27 && audio_status.volume == 100);
    seen = 0; lv_obj_t *progress = find_slider(ui.screen, 0, &seen); assert(progress);
    lv_obj_send_event(progress, LV_EVENT_PRESSED, NULL); lv_slider_set_value(progress, 500, LV_ANIM_OFF);
    lv_obj_send_event(progress, LV_EVENT_RELEASED, NULL); assert(audio_status.position_ms == 102500);
    click(&ui, "曲库"); snapshot(argv[2], "music-library");
    click(&ui, "远方的故事.wav"); assert(audio_status.track_index == 1);
    snapshot(argv[2], "music-selected");
    const unsigned previous_updates = playlist_updates;
    playlist_error = ESP_ERR_INVALID_STATE;
    click(&ui, "刷新"); click(&ui, "曲库");
    click(&ui, "海边的风.mp3"); assert(audio_status.track_index == 0);
    assert(playlist_updates == previous_updates && audio_status.track_count == 2);
    playlist_error = ESP_OK; tick(); assert(playlist_updates > previous_updates);
    playlist_error = ESP_ERR_NO_MEM;
    click(&ui, "刷新"); click(&ui, "曲库");
    click(&ui, "远方的故事.wav"); assert(audio_status.track_index == 1);
    playlist_error = ESP_OK;
    puts("PASS actual music UI retains working row indices after busy/OOM playlist refresh and retries busy refresh");
    /* File-browser direct play must not turn a failed media-owner stop barrier
     * into a new playlist/select. NOT_FOUND is ambiguous: storage can emit it. */
    const unsigned before_barrier_updates = playlist_updates;
    const unsigned before_barrier_selections = track_selections;
    const esp_err_t catalog_failures[] = {ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_NO_MEM};
    for (size_t i = 0; i < sizeof(catalog_failures) / sizeof(catalog_failures[0]); ++i) {
        music_catalog_error = catalog_failures[i];
        app_open_music_path(&ui, "/sdcard/Other/requested.mp3");
        tick();
        assert(playlist_updates == before_barrier_updates);
        assert(track_selections == before_barrier_selections);
        /* A later successful user refresh retains and services the request. */
        music_catalog_error = ESP_OK;
    }
    music_catalog_count = 0;
    click(&ui, "刷新");
    assert(playlist_updates == before_barrier_updates + 1);
    assert(track_selections == before_barrier_selections + 1);
    assert(!strcmp(audio_status.path, "/sdcard/Other/requested.mp3"));
    music_catalog_count = 2;
    puts("PASS actual music UI blocks direct-play fallback on busy/timeout/NOT_FOUND/OOM; successful empty scan can retry retained request");
    for (unsigned i = 0; i < 50; ++i) { app_open_reader(&ui); app_open_music(&ui); tick(); }
    mounted=false;usb_blocked=true;app_open_reader(&ui);unsigned books_before=book_catalog_calls;
    mounted=true;tick();tick();assert(book_catalog_calls==books_before);
    usb_blocked=false;tick();tick();assert(book_catalog_calls==books_before+1);tick();tick();assert(book_catalog_calls==books_before+1);
    app_open_reader_path(&ui,book_path);usb_blocked=true;tick();tick();unsigned text_before=text_calls;
    tick();tick();assert(text_calls==text_before);usb_blocked=false;tick();tick();assert(text_calls==text_before+1);
    mounted=false;usb_blocked=true;app_open_music(&ui);unsigned music_before=music_catalog_calls;
    mounted=true;tick();tick();assert(music_catalog_calls==music_before);
    usb_blocked=false;tick();tick();assert(music_catalog_calls==music_before+1);tick();tick();assert(music_catalog_calls==music_before+1);
    puts("PASS reader shelf/resume and music wait for mounted plus USB-gate release, then refresh once without consuming the ready edge early");
    assert(lv_mem_test() == LV_RESULT_OK);
    if (ui.timer) lv_timer_delete(ui.timer);
    if (ui.page_cleanup) ui.page_cleanup(&ui);
    lv_obj_delete(ui.screen);
    lv_indev_delete(input);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_deinit();
    puts("PASS actual LVGL reader/music rendering, source-offset resume, manual encoding persistence, 100 turns, 200 page rebuilds, hot-unplug/reinsert and allocator integrity");
    return 0;
}

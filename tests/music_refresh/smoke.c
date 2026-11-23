/* Reuse hardware/service doubles. Supply a controllable async IO queue. */
#define APP_VOLUME_UI_DOUBLES_ONLY
#define aether_audio_player_seek count_music_seek
#include "../volume_ui/smoke.c"
#undef aether_audio_player_seek
#include "app_lyrics.h"

static unsigned io_requests;
static unsigned lyric_requests;
static void (*pending_complete)(app_ui_t *, const app_io_result_t *);
static app_io_operation_t pending_operation;
static char requested_directory[192];
static char requested_lyrics[192];
static bool asynchronous_lyrics;
static aether_audio_player_status_t saved_music;
static const app_io_result_t *preparing_catalog;
static unsigned resume_checks, play_requests;
static uint32_t play_position;
static bool restore_busy_once;
static bool reject_seek;

esp_err_t aether_audio_player_seek(aether_audio_player_handle_t h, uint32_t position_ms)
{
    if (reject_seek) return ESP_ERR_INVALID_STATE;
    const esp_err_t error = count_music_seek(h, position_ms);
    music.seeking = true;
    music.seek_requested_ms = position_ms;
    return error;
}

esp_err_t aether_audio_player_get_resume_path(aether_audio_player_handle_t h, char *path, size_t size)
{
    assert(h && path && size >= sizeof(saved_music.path));
    if (!saved_music.path[0]) return ESP_ERR_NOT_FOUND;
    strlcpy(path, saved_music.path, size);
    return ESP_OK;
}

esp_err_t aether_audio_player_restore(aether_audio_player_handle_t h)
{
    assert(h && preparing_catalog && preparing_catalog->playlist_prepared);
    ++resume_checks;
    if (restore_busy_once) {
        restore_busy_once = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (music.active || music.path[0]) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < preparing_catalog->count; ++i) {
        if (strcmp(saved_music.path, preparing_catalog->entries[i].path)) continue;
        strlcpy(music.path, saved_music.path, sizeof(music.path));
        music.position_ms = saved_music.position_ms;
        music.duration_ms = saved_music.duration_ms;
        music.track_index = i;
        music.track_count = preparing_catalog->count;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t aether_audio_player_play(aether_audio_player_handle_t h, const char *path, int volume)
{
    assert(h && !music.active && !strcmp(path, music.path) && volume == music.volume);
    ++play_requests;
    play_position = music.position_ms;
    music.active = true;
    music.state = AETHER_AUDIO_PLAYING;
    return ESP_OK;
}

void app_ui_request_io_ex(app_ui_t *ui,const app_io_request_t *q,
                         void (*complete)(app_ui_t *,const app_io_result_t *))
{
    assert(ui&&q&&complete&&!pending_complete);
    if (q->operation == APP_IO_READ_TEXT) {
        assert(q->limit == APP_LYRICS_READ_LIMIT && strlen(q->path) >= 4 &&
               !strcmp(q->path + strlen(q->path) - 4, ".lrc"));
        if (!asynchronous_lyrics) {
            const app_io_result_t absent = {.operation=APP_IO_READ_TEXT, .error=ESP_ERR_NOT_FOUND};
            complete(ui, &absent);
            return;
        }
        strlcpy(requested_lyrics, q->path, sizeof(requested_lyrics));
        ++lyric_requests;
    } else {
        assert(q->operation == APP_IO_CATALOG && q->filter == APP_IO_MUSIC);
        strlcpy(requested_directory, q->path, sizeof(requested_directory));
        ++io_requests;
    }
    pending_operation = q->operation;
    pending_complete = complete;
}

static void complete_catalog(app_ui_t *ui, const app_io_result_t *result)
{
    assert(pending_complete && !preparing_catalog && pending_operation == APP_IO_CATALOG);
    void (*complete)(app_ui_t *,const app_io_result_t *) = pending_complete;
    pending_complete = NULL;
    preparing_catalog = result;
    complete(ui, result);
    preparing_catalog = NULL;
}

static void complete_lyrics(app_ui_t *ui, esp_err_t error, const char *text)
{
    assert(pending_complete && pending_operation == APP_IO_READ_TEXT);
    app_io_result_t result = {.operation=APP_IO_READ_TEXT, .error=error, .text=(char *)text};
    strlcpy(result.path, requested_lyrics, sizeof(result.path));
    if (text) result.next_offset = result.file_size = strlen(text);
    void (*complete)(app_ui_t *, const app_io_result_t *) = pending_complete;
    pending_complete = NULL;
    complete(ui, &result);
}

static void complete_io(app_ui_t *ui, esp_err_t error)
{
    const app_io_result_t result = {.operation=APP_IO_CATALOG, .error=error};
    complete_catalog(ui, &result);
}

static void test_music_eq(app_ui_t *ui)
{
    lv_obj_t *entry = find(ui->screen, "均衡器");
    lv_obj_t *band_label = find(ui->screen, "100 Hz");
    assert(entry && band_label);
    lv_obj_t *panel = lv_obj_get_parent(band_label);
    assert(lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(lv_obj_get_parent(entry), LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t *sliders[AETHER_AUDIO_EQ_BANDS + 1];
    unsigned count = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(panel); ++i) {
        lv_obj_t *child = lv_obj_get_child(panel, i);
        if (!lv_obj_check_type(child, &lv_slider_class)) continue;
        assert(count < AETHER_AUDIO_EQ_BANDS + 1);
        sliders[count++] = child;
    }
    assert(count == AETHER_AUDIO_EQ_BANDS + 1);
    lv_slider_set_value(sliders[0], 2, LV_ANIM_OFF);
    lv_obj_send_event(sliders[0], LV_EVENT_VALUE_CHANGED, NULL);
    assert(music.eq.enabled && music.eq.bands_db[0] == 2);
    lv_slider_set_value(sliders[AETHER_AUDIO_EQ_BANDS], 4, LV_ANIM_OFF);
    lv_obj_send_event(sliders[AETHER_AUDIO_EQ_BANDS], LV_EVENT_VALUE_CHANGED, NULL);
    assert(music.eq.gain_db == 4);

    lv_obj_t *compare = find(panel, "对比原声");
    assert(compare);
    lv_obj_send_event(lv_obj_get_parent(compare), LV_EVENT_CLICKED, NULL);
    assert(!music.eq.enabled && music.eq.bands_db[0] == 2 && music.eq.gain_db == 4);
    lv_obj_t *back = find(panel, "返回");
    assert(back);
    lv_obj_send_event(lv_obj_get_parent(back), LV_EVENT_CLICKED, NULL);
    assert(lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN));

    lv_obj_send_event(lv_obj_get_parent(entry), LV_EVENT_CLICKED, NULL);
    assert(lv_slider_get_value(sliders[0]) == 2);
    assert(lv_slider_get_value(sliders[AETHER_AUDIO_EQ_BANDS]) == 4);
    lv_obj_t *reset = find(panel, "恢复默认");
    assert(reset);
    lv_obj_send_event(lv_obj_get_parent(reset), LV_EVENT_CLICKED, NULL);
    assert(!music.eq.enabled && !music.eq.gain_db && !music.eq.bands_db[0]);
    assert(!lv_slider_get_value(sliders[0]) && !lv_slider_get_value(sliders[AETHER_AUDIO_EQ_BANDS]));
    lv_obj_send_event(lv_obj_get_parent(back), LV_EVENT_CLICKED, NULL);
    assert(music.active && music.state == AETHER_AUDIO_PLAYING && !music_page_releases);
    puts("PASS live music EQ controls, bypass, reopen and reset preserve playback");
}

static void open_music(app_ui_t *ui, const char *path)
{
    app_open_music_path(ui, path);
    assert(pending_complete);
}

static void click_label(app_ui_t *ui, const char *text)
{
    lv_obj_t *label = find(ui->screen, text);
    assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
}

static void test_resume(app_ui_t *ui)
{
    app_io_entry_t library[] = {{.path="/sdcard/Music/first.mp3"}, {.path="/sdcard/Music/second.mp3"}};
    const app_io_result_t standard = {.operation=APP_IO_CATALOG, .entries=library, .count=2, .total_count=2,
                                      .playlist_prepared=true};
    strlcpy(music.path, library[1].path, sizeof(music.path));
    music.position_ms = 42150;
    saved_music = music;
    open_music(ui, NULL);
    assert(!requested_directory[0] && !music.active && music_page_releases == 1);
    restore_busy_once = true;
    complete_catalog(ui, &standard);
    assert(!music.path[0] && !pending_complete);
    const unsigned early_selects = ui_playlist_selects;
    click_label(ui, LV_SYMBOL_PLAY);
    assert(ui_playlist_selects == early_selects && !play_requests);
    advance(400);
    assert(pending_complete && !requested_directory[0]);
    complete_catalog(ui, &standard);
    advance(130);
    assert(find(ui->screen, "second") && find(ui->screen, "00:42") && find(ui->screen, "02:00") && find(ui->screen, "2 / 2"));
    assert(!music.active && !play_requests && !pending_complete);
    click_label(ui, LV_SYMBOL_PLAY);
    assert(music.active && play_requests == 1 && play_position == 42150);

    const unsigned checks = resume_checks, selects = ui_playlist_selects;
    open_music(ui, library[0].path);
    assert(!strcmp(requested_directory, "/sdcard/Music"));
    complete_catalog(ui, &standard);
    assert(resume_checks == checks && ui_playlist_selects == selects + 1 && ui_playlist_selected_index == 0);
    puts("PASS music reopens on the saved song and position without autoplay, Play resumes, explicit open stays fresh");

    app_io_entry_t album[] = {{.path="/sdcard/Albums/Electro/saved.m4a"}};
    const app_io_result_t nested = {.operation=APP_IO_CATALOG, .entries=album, .count=1, .total_count=1,
                                    .playlist_prepared=true};
    strlcpy(saved_music.path, album[0].path, sizeof(saved_music.path));
    open_music(ui, NULL);
    complete_catalog(ui, &standard);
    assert(pending_complete && !strcmp(requested_directory, "/sdcard/Albums/Electro"));
    complete_catalog(ui, &nested);
    advance(130);
    assert(!pending_complete && !music.active && find(ui->screen, "saved") &&
           find(ui->screen, "00:42") && find(ui->screen, "02:00") && find(ui->screen, "1 / 1"));

    open_music(ui, NULL);
    complete_catalog(ui, &standard);
    assert(pending_complete && !strcmp(requested_directory, "/sdcard/Albums/Electro"));
    complete_io(ui, ESP_ERR_NOT_FOUND);
    assert(pending_complete && !requested_directory[0]);
    complete_catalog(ui, &standard);
    advance(130);
    assert(!pending_complete && !music.active && !music.path[0] && find(ui->screen, "first.mp3") &&
           !strcmp(saved_music.path, album[0].path));
    const unsigned fallback_selects = ui_playlist_selects;
    click_label(ui, LV_SYMBOL_PLAY);
    assert(ui_playlist_selects == fallback_selects + 1 && ui_playlist_selected_index == 0);
    puts("PASS saved nested directory restores asynchronously and missing saved directory falls back without erasing the record");
}

static void test_recording_playlist_scope(app_ui_t *ui)
{
    app_io_entry_t recording = {.path = "/sdcard/Recordings/test.wav"};
    app_io_entry_t song = {.path = "/sdcard/Music/song.mp3"};
    app_io_result_t catalog = {.operation = APP_IO_CATALOG, .count = 1, .total_count = 1,
                               .playlist_prepared = true};
    open_music(ui, recording.path);
    assert(!strcmp(requested_directory, "/sdcard/Recordings"));
    catalog.entries = &recording;
    complete_catalog(ui, &catalog);
    assert(find(ui->screen, "test.wav") && !find(ui->screen, "song.mp3"));

    strlcpy(saved_music.path, song.path, sizeof(saved_music.path));
    open_music(ui, NULL);
    assert(!requested_directory[0]);
    /* The backend may still be finishing the outgoing recording stream. */
    strlcpy(music.path, recording.path, sizeof(music.path));
    music.closing = music.active = true;
    music.state = AETHER_AUDIO_PLAYING;
    advance(130);
    click_label(ui, LV_SYMBOL_PLAY);
    assert(pending_complete && !find(ui->screen, "test") && music.state == AETHER_AUDIO_PLAYING);

    music.closing = music.active = false;
    music.state = AETHER_AUDIO_STOPPED;
    music.path[0] = '\0';
    catalog.entries = &song;
    complete_catalog(ui, &catalog);
    advance(130);
    assert(!pending_complete && !music.active && !strcmp(music.path, song.path) &&
           find(ui->screen, "song.mp3") && !find(ui->screen, "test.wav"));
    puts("PASS recording playlist and standalone music playlist stay isolated across async session release");
}

static lv_obj_t *progress_slider(app_ui_t *ui)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(ui->content); ++i) {
        lv_obj_t *child = lv_obj_get_child(ui->content, i);
        if (lv_obj_check_type(child, &lv_slider_class) && lv_slider_get_max_value(child) == 1000)
            return child;
    }
    return NULL;
}

static void recordings_cleanup(app_ui_t *ui)
{
    ui->page_context = NULL;
}

static void test_seek_screen_handoff(app_ui_t *ui, lv_indev_t *input)
{
    lv_obj_t *old_screen = ui->screen;
    lv_obj_t *old_slider = progress_slider(ui);
    assert(old_slider);
    music.active = true;
    music.state = AETHER_AUDIO_PLAYING;
    music.duration_ms = 60000;
    lv_slider_set_value(old_slider, 250, LV_ANIM_OFF);
    lv_obj_send_event(old_slider, LV_EVENT_PRESSED, input);
    lv_obj_send_event(old_slider, LV_EVENT_RELEASED, input);
    const unsigned requests = music_seek_requests;
    assert(requests == 1 && music_seek_position == 15000);

    lv_obj_send_event(old_slider, LV_EVENT_PRESSED, input);
    lv_obj_send_event(old_slider, LV_EVENT_INDEV_RESET, input);
    lv_obj_send_event(old_slider, LV_EVENT_RELEASED, input);
    assert(music_seek_requests == requests);

    lv_obj_send_event(old_slider, LV_EVENT_PRESSED, input);
    char directory[192] = "/sdcard/Recordings";
    app_ui_begin_page(ui, "录音文件");
    ui->page_context = directory;
    ui->page_cleanup = recordings_cleanup;
    assert(lv_obj_is_valid(old_slider));
    lv_obj_send_event(old_slider, LV_EVENT_INDEV_RESET, input);
    advance(20);
    assert(!lv_obj_is_valid(old_screen) && !lv_obj_is_valid(old_slider));
    assert(ui->page_context == directory && !strcmp(directory, "/sdcard/Recordings"));
    assert(music_seek_requests == requests);

    app_io_entry_t track = {.path = "/sdcard/Recordings/test.wav"};
    const app_io_result_t catalog = {.operation = APP_IO_CATALOG, .entries = &track, .count = 1,
                                     .total_count = 1, .playlist_prepared = true};
    open_music(ui, track.path);
    complete_catalog(ui, &catalog);
    old_screen = ui->screen;
    old_slider = progress_slider(ui);
    assert(old_slider);
    lv_obj_send_event(old_slider, LV_EVENT_PRESSED, input);

    open_music(ui, track.path);
    complete_catalog(ui, &catalog);
    lv_obj_t *current_slider = progress_slider(ui);
    assert(current_slider && current_slider != old_slider && lv_obj_is_valid(old_slider));
    music.active = true;
    music.state = AETHER_AUDIO_PLAYING;
    music.duration_ms = 60000;
    lv_slider_set_value(current_slider, 750, LV_ANIM_OFF);
    lv_obj_send_event(current_slider, LV_EVENT_PRESSED, input);
    lv_obj_send_event(old_slider, LV_EVENT_INDEV_RESET, input);
    lv_obj_send_event(old_slider, LV_EVENT_RELEASED, input);
    assert(music_seek_requests == requests);
    lv_obj_send_event(current_slider, LV_EVENT_RELEASED, input);
    assert(music_seek_requests == requests + 1 && music_seek_position == 45000);
    advance(20);
    assert(!lv_obj_is_valid(old_screen) && !lv_obj_is_valid(old_slider));
    assert(lv_obj_is_valid(current_slider) && music_seek_requests == requests + 1);
    puts("PASS real LVGL deferred player deletion and stale slider reset/release preserve recordings and reopened seek state");
}

static void test_seek_feedback(app_ui_t *ui, lv_indev_t *input)
{
    lv_obj_t *slider = progress_slider(ui);
    assert(slider);
    music.state = AETHER_AUDIO_PLAYING;
    music.active = true;
    music.seeking = false;
    music.duration_ms = 61337;
    music.position_ms = 6000;
    advance(150);
    const unsigned requests = music_seek_requests;
    assert(find(ui->screen, "00:06"));

    lv_obj_send_event(slider, LV_EVENT_PRESSED, input);
    lv_slider_set_value(slider, 671, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
    assert(find(ui->screen, "00:41") && music_seek_requests == requests);
    advance(250);
    assert(lv_slider_get_value(slider) == 671 && find(ui->screen, "00:41"));
    lv_obj_send_event(slider, LV_EVENT_RELEASED, input);
    assert(music_seek_requests == requests + 1 && music_seek_position == 41157);

    music.position_ms = 6350; /* Old playhead can advance until the backend applies the request. */
    advance(260);
    assert(lv_slider_get_value(slider) == 671 && find(ui->screen, "00:41") && find(ui->screen, "正在定位…"));

    lv_obj_send_event(slider, LV_EVENT_PRESSED, input);
    lv_slider_set_value(slider, 333, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
    assert(find(ui->screen, "00:20"));
    lv_obj_send_event(slider, LV_EVENT_RELEASED, input);
    assert(music_seek_requests == requests + 2 && music_seek_position == 20425);
    advance(260);
    assert(lv_slider_get_value(slider) == 333 && find(ui->screen, "00:20"));

    music.seeking = false;
    music.position_ms = 22000;
    music.state = AETHER_AUDIO_PAUSED;
    advance(150);
    assert(lv_slider_get_value(slider) == 359 && find(ui->screen, "00:22") && !find(ui->screen, "正在定位…"));

    reject_seek = true;
    lv_obj_send_event(slider, LV_EVENT_PRESSED, input);
    lv_slider_set_value(slider, 900, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, input);
    assert(lv_slider_get_value(slider) == 359 && find(ui->screen, "00:22"));
    reject_seek = false;

    lv_obj_send_event(slider, LV_EVENT_PRESSED, input);
    lv_slider_set_value(slider, 900, LV_ANIM_OFF);
    strlcpy(music.path, "/sdcard/Music/next.mp3", sizeof(music.path));
    music.position_ms = 1000;
    lv_obj_send_event(slider, LV_EVENT_RELEASED, input);
    assert(music_seek_requests == requests + 2 && find(ui->screen, "00:01"));
    puts("PASS drag time preview, pending seek replacement, acknowledgement, rejection, and track handoff");
}

static void test_lrc_parser(void)
{
    app_lyrics_t *lyrics = app_lyrics_parse("\xef\xbb\xbf[ti:测试]\r\n[00:04]最终\r\n"
        "[00:01.2][00:03:00] 前奏 \n[00:01.200]重复\n[00:03.550]\n[00:99]错误\n"
        "[offset:+240]\n[00:06]不完整", false);
    assert(lyrics && app_lyrics_count(lyrics) == 4);
    assert(app_lyrics_at(lyrics, 959) == SIZE_MAX);
    assert(app_lyrics_at(lyrics, 960) == 0 && !strcmp(app_lyrics_line(lyrics, 0), "前奏"));
    assert(app_lyrics_at(lyrics, 2760) == 1 && !strcmp(app_lyrics_line(lyrics, 1), "前奏"));
    assert(app_lyrics_at(lyrics, 3310) == 2 && !*app_lyrics_line(lyrics, 2));
    assert(app_lyrics_at(lyrics, UINT32_MAX) == 3 && !strcmp(app_lyrics_line(lyrics, 3), "最终"));
    assert(!*app_lyrics_line(lyrics, SIZE_MAX));
    free(lyrics);

    lyrics = app_lyrics_parse("[offset:-500]\n[00:00]第一行", true);
    assert(lyrics && app_lyrics_at(lyrics, 499) == SIZE_MAX && app_lyrics_at(lyrics, 500) == 0);
    free(lyrics);
    assert(!app_lyrics_parse("[ti:无时间戳]", true));
    puts("PASS LRC ordering, repeated and duplicate cues, signed offset, BOM, blank interval and incomplete tail");
}

static bool highlighted(app_ui_t *ui, const char *text)
{
    lv_obj_t *line = find(ui->screen, text);
    return line && lv_color_eq(lv_obj_get_style_text_color(line, LV_PART_MAIN), lv_color_hex(0x79DBB7));
}

static void test_synchronized_lyrics(app_ui_t *ui)
{
    static const char text[] = "[00:01.00]第一行\n[00:04.00]第二行\n[00:09.00]第三行";
    app_io_entry_t tracks[] = {{.path="/sdcard/Music/a.song.mp3"}, {.path="/sdcard/Music/b.wav"}};
    const app_io_result_t catalog = {.operation=APP_IO_CATALOG,.entries=tracks,.count=2,
                                      .total_count=2,.playlist_prepared=true};
    asynchronous_lyrics = true;
    open_music(ui, tracks[0].path);
    complete_catalog(ui, &catalog);
    strlcpy(music.path, tracks[0].path, sizeof(music.path));
    music.active = true;
    music.position_ms = 1100;
    advance(140);
    assert(lyric_requests == 1 && !strcmp(requested_lyrics, "/sdcard/Music/a.song.lrc"));
    complete_lyrics(ui, ESP_OK, text);
    assert(highlighted(ui, "第一行") && find(ui->screen, "第二行"));
    music.position_ms = 4100;
    advance(140);
    assert(highlighted(ui, "第二行") && find(ui->screen, "第一行") && find(ui->screen, "第三行"));
    music.position_ms = 0;
    advance(140);
    assert(!highlighted(ui, "第一行") && find(ui->screen, "第一行") && !find(ui->screen, "第二行"));
    strlcpy(music.path, tracks[1].path, sizeof(music.path));
    advance(140);
    assert(lyric_requests == 2 && !strcmp(requested_lyrics, "/sdcard/Music/b.lrc") && !find(ui->screen, "第一行"));
    complete_lyrics(ui, ESP_ERR_NOT_FOUND, NULL);
    advance(140);
    assert(lyric_requests == 3 && !strcmp(requested_lyrics, "/sdcard/Lyrics/b.lrc"));
    strlcpy(music.path, tracks[0].path, sizeof(music.path));
    advance(140);
    complete_lyrics(ui, ESP_OK, "[00:00]旧曲歌词");
    assert(!find(ui->screen, "旧曲歌词"));
    advance(140);
    assert(lyric_requests == 4 && !strcmp(requested_lyrics, "/sdcard/Music/a.song.lrc"));

    const unsigned scans = io_requests;
    click_label(ui, "刷新");
    advance(140);
    assert(io_requests == scans && pending_operation == APP_IO_READ_TEXT);
    complete_lyrics(ui, ESP_OK, text);
    advance(140);
    assert(io_requests == scans + 1 && pending_operation == APP_IO_CATALOG);
    complete_catalog(ui, &catalog);
    asynchronous_lyrics = false;
    puts("PASS music finds adjacent and Lyrics-folder LRC, follows seek, discards stale lyrics and defers a refresh safely");
}

int main(void)
{
    test_lrc_parser();
    lv_init();lv_display_t *d=lv_display_create(480,480);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input,d);lv_indev_set_read_cb(input,read_pointer);
    lv_group_t *group=lv_group_create();lv_group_set_default(group);
    app_ui_t ui={.storage=(void*)1,.audio_player=(void*)2,.io=(void*)4};
    app_open_music(&ui);assert(io_requests==1&&pending_complete);advance(20);
    lv_obj_t *label=find(ui.screen,"刷新");assert(label);lv_obj_t *refresh=lv_obj_get_parent(label);
    for(unsigned i=0;i<40;++i)lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);
    advance(500);assert(io_requests==1&&pending_complete);
    complete_io(&ui,ESP_FAIL);
    lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);assert(io_requests==2&&pending_complete);
    for(unsigned i=0;i<40;++i)lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);
    assert(io_requests==2);complete_io(&ui,ESP_OK);
    lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);assert(io_requests==3&&pending_complete);
    assert(pending_complete);
    app_io_entry_t entries[2]={{.path="/sdcard/Music/first.mp3"},{.path="/sdcard/Music/second.mp3"}};
    app_io_result_t prepared={.operation=APP_IO_CATALOG,.entries=entries,.count=2,.total_count=2,.playlist_prepared=true};
    unsigned copies=ui_playlist_copies;complete_catalog(&ui,&prepared);
    assert(ui_playlist_copies==copies); /* No second backend copy on LVGL. */
    lv_obj_t *old_track=find(ui.screen,"first.mp3");assert(old_track);
    lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);assert(pending_complete);
    const unsigned old_selects=ui_playlist_selects;
    lv_obj_send_event(lv_obj_get_parent(old_track),LV_EVENT_CLICKED,NULL);
    assert(ui_playlist_selects==old_selects); /* Old indices disabled while worker replaces list. */
    complete_catalog(&ui,&prepared);
    assert(ui_playlist_copies==copies);

    enum { LONG_CATALOG_COUNT = 96 };
    app_io_entry_t long_catalog[LONG_CATALOG_COUNT] = {0};
    for (size_t i=0;i<LONG_CATALOG_COUNT;++i)
        snprintf(long_catalog[i].path,sizeof(long_catalog[i].path),"/sdcard/Music/track-%02u.mp3",(unsigned)i);
    lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);assert(pending_complete);
    prepared.entries=long_catalog;prepared.count=prepared.total_count=LONG_CATALOG_COUNT;
    complete_catalog(&ui,&prepared);
    lv_obj_t *library_button=lv_obj_get_parent(find(ui.screen,"曲库"));
    lv_obj_send_event(library_button,LV_EVENT_CLICKED,NULL);advance(120);
    lv_obj_t *first=find(ui.screen,"track-00.mp3");assert(first&&lv_obj_is_visible(first));
    lv_obj_t *list=lv_obj_get_parent(lv_obj_get_parent(first));
    assert(lv_obj_get_child_count(list)==13&&!find(ui.screen,"上一页")&&!find(ui.screen,"下一页"));
    const unsigned scans=io_requests;
    lv_group_focus_obj(lv_obj_get_parent(first));
    for (size_t i=1;i<LONG_CATALOG_COUNT;++i) {
        uint32_t key=LV_KEY_DOWN;
        lv_obj_send_event(lv_group_get_focused(group),LV_EVENT_KEY,&key);
        assert((size_t)(uintptr_t)lv_obj_get_user_data(lv_group_get_focused(group))==i);
    }
    advance(120);
    lv_obj_t *last=find(ui.screen,"track-95.mp3");assert(last&&lv_obj_is_visible(last));
    lv_area_t bounds;lv_obj_get_coords(lv_obj_get_parent(last),&bounds);
    pointer=(lv_point_t){(bounds.x1+bounds.x2)/2,(bounds.y1+bounds.y2)/2};
    held=true;advance(80);held=false;advance(80);
    assert(ui_playlist_selects==old_selects+1&&ui_playlist_selected_index==95&&io_requests==scans);
    puts("PASS music catalog keyboard navigation reaches recycled offscreen rows and pointer selects the actual track index");
    assert(music.active&&music.state==AETHER_AUDIO_PLAYING&&music.volume==100&&!music_updates);
    test_music_eq(&ui);
    test_resume(&ui);
    test_recording_playlist_scope(&ui);
    test_seek_screen_handoff(&ui, input);
    test_seek_feedback(&ui, input);
    test_synchronized_lyrics(&ui);
    if(ui.timer)lv_timer_delete(ui.timer);
    const unsigned releases=music_page_releases;
    if(ui.page_cleanup)ui.page_cleanup(&ui);
    assert(music_page_releases==releases+1&&!music.active&&!music.track_count&&!music.path[0]);
    lv_obj_delete(ui.screen);lv_group_delete(group);lv_indev_delete(input);
    assert(lv_mem_test()==LV_RESULT_OK);lv_display_delete(d);lv_deinit();
    puts("PASS real music UI async catalog ownership: repeated Refresh never queues/overwrites a pending completion, failed and empty scans allow retry, existing playback is preserved only while open; IO-prepared catalogs avoid GUI playlist copies and page exit requests complete media release");
    return 0;
}

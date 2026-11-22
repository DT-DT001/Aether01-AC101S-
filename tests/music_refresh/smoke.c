/* Reuse hardware/service doubles. Supply a controllable async IO queue. */
#define APP_VOLUME_UI_DOUBLES_ONLY
#include "../volume_ui/smoke.c"

static unsigned io_requests;
static void (*pending_complete)(app_ui_t *, const app_io_result_t *);
void app_ui_request_io_ex(app_ui_t *ui,const app_io_request_t *q,
                         void (*complete)(app_ui_t *,const app_io_result_t *))
{
    assert(ui&&q&&q->operation==APP_IO_CATALOG&&q->filter==APP_IO_MUSIC);
    assert(!pending_complete);++io_requests;pending_complete=complete;
}
static void complete_io(app_ui_t *ui,esp_err_t error)
{
    assert(pending_complete);void (*complete)(app_ui_t *,const app_io_result_t *)=pending_complete;
    pending_complete=NULL;const app_io_result_t result={.operation=APP_IO_CATALOG,.error=error};
    complete(ui,&result);
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

int main(void)
{
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
    void(*catalog_complete)(app_ui_t*,const app_io_result_t*)=pending_complete;pending_complete=NULL;
    app_io_entry_t entries[2]={{.path="/sdcard/Music/first.mp3"},{.path="/sdcard/Music/second.mp3"}};
    app_io_result_t prepared={.operation=APP_IO_CATALOG,.entries=entries,.count=2,.total_count=2,.playlist_prepared=true};
    unsigned copies=ui_playlist_copies;catalog_complete(&ui,&prepared);
    assert(ui_playlist_copies==copies); /* No second backend copy on LVGL. */
    lv_obj_t *old_track=find(ui.screen,"first.mp3");assert(old_track);
    lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);assert(pending_complete);
    const unsigned old_selects=ui_playlist_selects;
    lv_obj_send_event(lv_obj_get_parent(old_track),LV_EVENT_CLICKED,NULL);
    assert(ui_playlist_selects==old_selects); /* Old indices disabled while worker replaces list. */
    catalog_complete=pending_complete;pending_complete=NULL;catalog_complete(&ui,&prepared);
    assert(ui_playlist_copies==copies);

    enum { LONG_CATALOG_COUNT = 96 };
    app_io_entry_t long_catalog[LONG_CATALOG_COUNT] = {0};
    for (size_t i=0;i<LONG_CATALOG_COUNT;++i)
        snprintf(long_catalog[i].path,sizeof(long_catalog[i].path),"/sdcard/Music/track-%02u.mp3",(unsigned)i);
    lv_obj_send_event(refresh,LV_EVENT_CLICKED,NULL);assert(pending_complete);
    catalog_complete=pending_complete;pending_complete=NULL;
    prepared.entries=long_catalog;prepared.count=prepared.total_count=LONG_CATALOG_COUNT;
    catalog_complete(&ui,&prepared);
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
    if(ui.timer)lv_timer_delete(ui.timer);
    const unsigned releases=music_page_releases;
    if(ui.page_cleanup)ui.page_cleanup(&ui);
    assert(music_page_releases==releases+1&&!music.active&&!music.track_count&&!music.path[0]);
    lv_obj_delete(ui.screen);lv_group_delete(group);lv_indev_delete(input);
    assert(lv_mem_test()==LV_RESULT_OK);lv_display_delete(d);lv_deinit();
    puts("PASS real music UI async catalog ownership: repeated Refresh never queues/overwrites a pending completion, failed and empty scans allow retry, existing playback is preserved only while open; IO-prepared catalogs avoid GUI playlist copies and page exit requests complete media release");
    return 0;
}

#include "app_ui_internal.h"
#include "app_instrument_ui.h"
#include "app_telemetry.h"
#include "app_motion_filter.h"
#include "app_ui_gyro_calibration.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    app_motion_filter_t filter;
    app_ui_gyro_calibration_t calibration;
    lv_obj_t *model, *trace, *state, *acceleration, *rotation, *values[3];
    uint32_t draw_ms, label_ms;
    uint32_t state_color;
    float shown_roll,shown_pitch,shown_yaw,shown_linear;
    app_motion_action_t shown_action;
    float previous_gravity[3];
    uint32_t zero_requested_ms;
    bool sleeping, zero_pending, have_gravity;
} motion_page_t;
typedef struct {float x,y,z;} vector_t;
/* The relative zero belongs to this power session, not a single screen. */
static float zero_reference[4] = {1.0f};
static uint32_t action_color(app_motion_action_t a) {
    if(a==APP_MOTION_IMPACT || a==APP_MOTION_FREEFALL) return 0xFF9791;
    if(a==APP_MOTION_SHAKING || a==APP_MOTION_TURNING) return 0xF4CB87;
    return 0x7EDDC8;
}
static void line(lv_layer_t *layer,float x,float y,float x2,float y2,uint32_t color,int width) {
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);
    d.p1=(lv_point_precise_t){x,y};d.p2=(lv_point_precise_t){x2,y2};
    d.color=lv_color_hex(color);d.width=width;d.opa=LV_OPA_COVER;
    lv_draw_line(layer,&d);
}
static void triangle(lv_layer_t *layer,vector_t a,vector_t b,vector_t c,uint32_t color) {
    lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);
    d.p[0]=(lv_point_precise_t){a.x,a.y};d.p[1]=(lv_point_precise_t){b.x,b.y};d.p[2]=(lv_point_precise_t){c.x,c.y};
    d.color=lv_color_hex(color);d.opa=LV_OPA_COVER;lv_draw_triangle(layer,&d);
}
static vector_t project(const float q[4],vector_t p,float cx,float cy) {
    const float w=q[0],x=q[1],y=q[2],z=q[3];
    vector_t r={
        (1-2*(y*y+z*z))*p.x+2*(x*y-w*z)*p.y+2*(x*z+w*y)*p.z,
        2*(x*y+w*z)*p.x+(1-2*(x*x+z*z))*p.y+2*(y*z-w*x)*p.z,
        2*(x*z-w*y)*p.x+2*(y*z+w*x)*p.y+(1-2*(x*x+y*y))*p.z};
    /* Fixed oblique camera; the object rotation itself is the measured pose. */
    float a=r.x*.9063f+r.z*-.4226f,c=r.x*.4226f+r.z*.9063f;
    float b=r.y*.9397f-c*.3420f;
    c=r.y*.3420f+c*.9397f;
    float scale=210/(260-c);
    return (vector_t){cx+a*scale,cy+b*scale,c};
}
static void model_draw(lv_event_t *event) {
    motion_page_t *page=lv_event_get_user_data(event);
    lv_layer_t *layer=lv_event_get_layer(event);lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event),&area);
    const float cx=(area.x1+area.x2)*.5f,cy=(area.y1+area.y2)*.5f;
    static const vector_t base[8]={{-45,-64,-8},{45,-64,-8},{45,64,-8},{-45,64,-8},{-45,-64,8},{45,-64,8},{45,64,8},{-45,64,8}};
    static const unsigned faces[6][4]={{0,3,2,1},{4,5,6,7},{0,1,5,4},{3,7,6,2},{0,4,7,3},{1,2,6,5}};
    static const uint32_t colors[6]={0x1B3446,0x257D86,0x53ADAE,0x14394A,0x286577,0x429CA7};
    vector_t points[8];float depths[6];unsigned order[6]={0,1,2,3,4,5};
    for(unsigned i=0;i<8;++i) points[i]=project(page->filter.relative,base[i],cx,cy);
    for(unsigned i=0;i<6;++i) {
        depths[i]=0;for(unsigned j=0;j<4;++j) depths[i]+=points[faces[i][j]].z;
    }
    for(unsigned i=1;i<6;++i) {
        unsigned next=order[i],j=i;
        while(j && depths[order[j-1]]>depths[next]) {order[j]=order[j-1];--j;}order[j]=next;
    }
    uint32_t grid=0x20334B;
    line(layer,area.x1+12,cy,area.x2-12,cy,grid,1);
    line(layer,cx,area.y1+8,cx,area.y2-8,grid,1);
    for(unsigned k=0;k<6;++k) {
        unsigned f=order[k];const unsigned *v=faces[f];
        triangle(layer,points[v[0]],points[v[1]],points[v[2]],colors[f]);
        triangle(layer,points[v[0]],points[v[2]],points[v[3]],colors[f]);
        if(f==1) {
            static const vector_t screen[4]={{-35,-49,9},{35,-49,9},{35,42,9},{-35,42,9}};
            vector_t s[4];for(unsigned i=0;i<4;++i) s[i]=project(page->filter.relative,screen[i],cx,cy);
            triangle(layer,s[0],s[1],s[2],0x153246);triangle(layer,s[0],s[2],s[3],0x153246);
            vector_t a=project(page->filter.relative,(vector_t){-21,-28,10},cx,cy);
            vector_t b=project(page->filter.relative,(vector_t){21,-28,10},cx,cy);
            line(layer,a.x,a.y,b.x,b.y,action_color(page->filter.action),4);
            a=project(page->filter.relative,(vector_t){-21,-13,10},cx,cy);
            b=project(page->filter.relative,(vector_t){10,-13,10},cx,cy);
            line(layer,a.x,a.y,b.x,b.y,0x497E9B,3);
        }
        for(unsigned i=0;i<4;++i) {
            vector_t a=points[v[i]],b=points[v[(i+1)%4]];
            line(layer,a.x,a.y,b.x,b.y,f==1?0xA0E4DA:0x4F8D9B,1);
        }
    }
}
static void trace_draw(lv_event_t *event) {
    motion_page_t *page=lv_event_get_user_data(event);lv_area_t a;
    lv_obj_get_coords(lv_event_get_target_obj(event),&a);lv_layer_t *layer=lv_event_get_layer(event);
    float width=a.x2-a.x1-2,height=a.y2-a.y1-4;
    line(layer,a.x1,a.y2-2,a.x2,a.y2-2,0x283E58,1);
    for(unsigned i=1;i<48;++i) {
        float p=page->filter.history[(page->filter.history_next+i-1)%48];
        float n=page->filter.history[(page->filter.history_next+i)%48];
        line(layer,a.x1+(i-1)*width/47,a.y2-2-fminf(p/12,1)*height,
             a.x1+i*width/47,a.y2-2-fminf(n/12,1)*height,action_color(page->filter.action),2);
    }
}
static void motion_cleanup(app_ui_t *ui)
{
    motion_page_t *page = ui->page_context;
    if (!page) return;
    app_ui_gyro_calibration_close(&page->calibration);
    free(page);
    ui->page_context = NULL;
}
static void motion_state(motion_page_t *page, const char *text, uint32_t color)
{
    app_ui_label_text(page->state, text);
    if (page->state_color != color) {
        lv_obj_set_style_text_color(page->state, lv_color_hex(color), 0);
        page->state_color = color;
    }
}

static void update_labels(motion_page_t *page) {
    app_ui_label_fmt(page->values[0],"%+.0f°",page->filter.roll);
    app_ui_label_fmt(page->values[1],"%+.0f°",page->filter.pitch);
    app_ui_label_fmt(page->values[2],"%+.0f°",page->filter.yaw);
    motion_state(page, app_motion_action_name(page->filter.action), action_color(page->filter.action));
    app_ui_label_fmt(page->acceleration, "%.1f m/s^2", page->filter.linear);
    app_ui_label_fmt(page->rotation, "%.0f°/s", page->filter.rotation);
}
static bool fresh_imu(const app_telemetry_snapshot_t *sample)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return sample->imu_error == ESP_OK && !sample->gyro_calibrating && sample->imu_updated_ms &&
           now - sample->imu_updated_ms <= 250;
}

static bool apply_zero(motion_page_t *page, const app_telemetry_snapshot_t *sample)
{
    if (!fresh_imu(sample) || !app_motion_filter_zero_at(&page->filter, sample->imu.accel_mps2,
                                                       sample->imu_updated_ms)) return false;
    memcpy(zero_reference, page->filter.reference, sizeof(zero_reference));
    page->zero_pending = false;
    update_labels(page);
    motion_state(page, "已归零", INSTRUMENT_TEAL);
    lv_obj_invalidate(page->model);
    return true;
}

static void motion_zero(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    motion_page_t *page = ui->page_context;
    if (!page || app_power_is_sleeping(ui->power)) return;
    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    if (!apply_zero(page, &sample)) {
        page->zero_pending = true;
        page->zero_requested_ms = (uint32_t)(esp_timer_get_time() / 1000);
        motion_state(page, "将设备放稳", INSTRUMENT_MUTED);
    }
}

static const float *motion_rates(motion_page_t *page, const qmi8658a_sample_t *imu, float adjusted[3])
{
    const float *accel = imu->accel_mps2, *gyro = imu->gyro_dps;
    const float norm = sqrtf(accel[0]*accel[0] + accel[1]*accel[1] + accel[2]*accel[2]);
    bool steady_gravity = false;
    if (norm >= 7.8f && norm <= 11.8f) {
        float delta = 0.0f;
        for (unsigned i = 0; i < 3; ++i) {
            const float direction = accel[i] / norm;
            const float difference = direction - page->previous_gravity[i];
            delta += difference * difference;
            page->previous_gravity[i] = direction;
        }
        steady_gravity = page->have_gravity && delta < 0.0001f;
        page->have_gravity = true;
    } else page->have_gravity = false;
    if (imu->gyro_stationary) return adjusted;
    if (!steady_gravity) return gyro;

    /* A steady gravity vector contradicts transverse angular drift. Keep
     * heading about gravity, which the accelerometer cannot observe. */
    float heading = 0.0f;
    for (unsigned i = 0; i < 3; ++i) heading += gyro[i] * page->previous_gravity[i];
    if (fabsf(heading) > 1.2f)
        for (unsigned i = 0; i < 3; ++i) adjusted[i] = heading * page->previous_gravity[i];
    return adjusted;
}
static void motion_calibrate(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    motion_page_t *page = ui->page_context;
    if (page) app_ui_gyro_calibration_open(ui->screen, &page->calibration);
}
static void motion_tick(lv_timer_t *timer) {
    app_ui_t *ui=lv_timer_get_user_data(timer);motion_page_t *page=ui->page_context;
    if(!page) return;
    bool asleep=app_power_is_sleeping(ui->power);
    if(asleep) {
        if(!page->sleeping) app_telemetry_set_interest(0);
        app_ui_gyro_calibration_close(&page->calibration);
        page->zero_pending = page->have_gravity = false;
        page->sleeping=true;return;
    }
    if(page->sleeping) {page->sleeping=false;app_telemetry_set_interest(APP_TELEMETRY_IMU);}
    app_ui_gyro_calibration_refresh(&page->calibration);
    app_telemetry_snapshot_t sample;app_telemetry_get(&sample);
    if(sample.gyro_calibrating) return;
    const uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    if (page->zero_pending) {
        if (now - page->zero_requested_ms > 800) page->zero_pending = false;
        else if (apply_zero(page, &sample)) return;
    }
    if(sample.imu_error!=ESP_OK) {
        motion_state(page, "等待采样", INSTRUMENT_MUTED);
        app_ui_label_text(page->acceleration, "--");
        app_ui_label_text(page->rotation, "--");
        return;
    }
    for(unsigned i=0;i<3;++i) if(!isfinite(sample.imu.accel_mps2[i]) || !isfinite(sample.imu.gyro_dps[i])) {
        motion_state(page, "数据异常", 0xFF9791);
        app_ui_label_text(page->acceleration, "--");
        app_ui_label_text(page->rotation, "--");
        return;
    }
    if (page->filter.initialized && sample.imu_updated_ms == page->filter.stamp) return;
    float adjusted[3] = {0};
    const float *gyro = motion_rates(page, &sample.imu, adjusted);
    if(!app_motion_filter_update_tilt(&page->filter,sample.imu.accel_mps2,gyro,
                                      sample.imu_updated_ms,sample.imu.gyro_bias_valid)) return;
    bool changed=fabsf(page->filter.roll-page->shown_roll)>.15f || fabsf(page->filter.pitch-page->shown_pitch)>.15f ||
        fabsf(page->filter.yaw-page->shown_yaw)>.15f || fabsf(page->filter.linear-page->shown_linear)>.12f || page->shown_action!=page->filter.action;
    if(changed && now-page->draw_ms>=33) {
        lv_obj_invalidate(page->model);lv_obj_invalidate(page->trace);page->draw_ms=now;
        page->shown_roll=page->filter.roll;page->shown_pitch=page->filter.pitch;page->shown_yaw=page->filter.yaw;
        page->shown_linear=page->filter.linear;page->shown_action=page->filter.action;
    }
    if(now-page->label_ms>=100) {update_labels(page);page->label_ms=now;}
}
static lv_obj_t *text(lv_obj_t *parent,const char *value,int x,int y,uint32_t color,unsigned px) {
    return app_ui_add_text(parent,value,LV_ALIGN_TOP_LEFT,x,y,color,app_ui_chinese_font_size(px));
}
typedef struct {
    lv_obj_t *bubble, *state, *roll_value, *pitch_value;
    uint32_t stamp;
    float roll, pitch;
    bool ready, sleeping, level;
} level_page_t;

static void level_cleanup(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

static void level_wait(level_page_t *page)
{
    if (!page->ready) return;
    page->ready = false;
    lv_obj_add_flag(page->bubble, LV_OBJ_FLAG_HIDDEN);
    app_ui_label_text(page->state, "将设备放稳");
    app_ui_label_text(page->roll_value, "--");
    app_ui_label_text(page->pitch_value, "--");
}

static void level_tick(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    level_page_t *page = ui->page_context;
    if (app_power_is_sleeping(ui->power)) {
        if (!page->sleeping) app_telemetry_set_interest(0);
        page->sleeping = true;
        level_wait(page);
        return;
    }
    if (page->sleeping) {
        page->sleeping = false;
        app_telemetry_set_interest(APP_TELEMETRY_IMU);
    }
    app_telemetry_snapshot_t sample;
    app_telemetry_get(&sample);
    const float *gyro = sample.imu.gyro_dps;
    const float rotation_squared = gyro[0] * gyro[0] + gyro[1] * gyro[1] + gyro[2] * gyro[2];
    float roll, pitch;
    if (!fresh_imu(&sample) || !isfinite(rotation_squared) ||
        (!sample.imu.gyro_stationary && rotation_squared > 45.0f * 45.0f) ||
        !app_motion_level_angles(sample.imu.accel_mps2, zero_reference, &roll, &pitch)) {
        level_wait(page);
        return;
    }
    if (page->ready && sample.imu_updated_ms == page->stamp) return;
    const bool was_ready = page->ready;
    const float blend = was_ready ? .32f : 1.0f;
    page->stamp = sample.imu_updated_ms;
    page->ready = true;
    page->roll += blend * (roll - page->roll);
    page->pitch += blend * (pitch - page->pitch);
    const float angle = hypotf(page->roll, page->pitch);
    const float scale = 82.0f / fmaxf(12.0f, angle);
    if (!was_ready) lv_obj_remove_flag(page->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(page->bubble, LV_ALIGN_CENTER, (int)lroundf(-page->pitch * scale),
                 (int)lroundf(-page->roll * scale));
    const bool level = angle <= (page->level && was_ready ? .6f : .45f);
    if (!was_ready || page->level != level) {
        page->level = level;
        lv_obj_set_style_bg_color(page->bubble, lv_color_hex(level ? INSTRUMENT_TEAL : INSTRUMENT_BLUE), 0);
        app_ui_label_text(page->state, level ? "已水平" : "倾斜");
    }
    app_ui_label_fmt(page->roll_value, "%+.1f°", page->roll);
    app_ui_label_fmt(page->pitch_value, "%+.1f°", page->pitch);
}

static void open_level(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    lv_obj_t *content = app_ui_begin_page(ui, "水平仪");
    ui->page_back = app_open_motion;
    level_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        text(content, "内存不足", 0, 0, 0xFF9791, 16);
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = level_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *card = instrument_card(content, 0, 0, 408, 272);
    lv_obj_t *dial = lv_obj_create(card);
    lv_obj_remove_style_all(dial);
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dial, 210, 210);
    lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dial, 2, 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(INSTRUMENT_MUTED), 0);
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *mark = lv_obj_create(dial);
        lv_obj_remove_style_all(mark);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(mark, i == 0 ? 166 : i == 1 ? 2 : 48, i == 0 ? 2 : i == 1 ? 166 : 48);
        lv_obj_center(mark);
        if (i == 2) {
            lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(mark, 2, 0);
            lv_obj_set_style_border_color(mark, lv_color_hex(INSTRUMENT_TEAL), 0);
        } else {
            lv_obj_set_style_bg_color(mark, lv_color_hex(0x334965), 0);
            lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        }
    }
    page->bubble = lv_obj_create(dial);
    lv_obj_remove_style_all(page->bubble);
    lv_obj_remove_flag(page->bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page->bubble, 32, 32);
    lv_obj_set_style_radius(page->bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(page->bubble, LV_OPA_COVER, 0);
    lv_obj_add_flag(page->bubble, LV_OBJ_FLAG_HIDDEN);
    page->state = app_ui_add_text(card, "将设备放稳", LV_ALIGN_BOTTOM_MID, 0, -15,
                                  INSTRUMENT_TEAL, app_ui_chinese_font_size(20));
    lv_obj_t *axes = instrument_card(content, 0, 282, 408, 70);
    instrument_text(axes, "横滚", 16, 8, 160, INSTRUMENT_MUTED, app_ui_chinese_font());
    instrument_text(axes, "俯仰", 220, 8, 160, INSTRUMENT_MUTED, app_ui_chinese_font());
    page->roll_value = instrument_text(axes, "--", 16, 29, 180, INSTRUMENT_TEXT, &lv_font_montserrat_28);
    page->pitch_value = instrument_text(axes, "--", 220, 29, 180, INSTRUMENT_TEXT, &lv_font_montserrat_28);
    app_telemetry_set_interest(APP_TELEMETRY_IMU);
    app_ui_set_timer(ui, level_tick, 50);
    level_tick(ui->timer);
}

void app_open_motion(app_ui_t *ui) {
    lv_obj_t *content=app_ui_begin_page(ui,"姿态");
    motion_page_t *page=calloc(1,sizeof(*page));
    if(!page) {text(content,"内存不足",0,0,0xFF9791,16);return;}
    ui->page_context=page;ui->page_cleanup=motion_cleanup;app_motion_filter_init(&page->filter);
    memcpy(page->filter.reference, zero_reference, sizeof(zero_reference));
    app_telemetry_set_interest(APP_TELEMETRY_IMU);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    const lv_font_t *small = app_ui_chinese_font();
    const lv_font_t *large = app_ui_chinese_font_size(24);
    lv_obj_t *overview = instrument_card(content, 0, 0, 408, 178);
    page->model=lv_obj_create(overview);lv_obj_remove_style_all(page->model);
    lv_obj_set_pos(page->model,4,6);lv_obj_set_size(page->model,252,166);
    lv_obj_remove_flag(page->model,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(page->model,model_draw,LV_EVENT_DRAW_MAIN,page);
    page->state = instrument_text(overview, "等待采样", 259, 9, 140, INSTRUMENT_MUTED, large);
    instrument_text(overview, "动态加速度", 259, 46, 140, INSTRUMENT_MUTED, small);
    page->acceleration = instrument_text(overview, "--", 259, 63, 142, INSTRUMENT_TEAL, large);
    instrument_text(overview, "转速", 259, 96, 134, INSTRUMENT_MUTED, small);
    page->rotation = instrument_text(overview, "--", 259, 113, 142, INSTRUMENT_TEXT, large);
    page->trace=lv_obj_create(overview);lv_obj_remove_style_all(page->trace);
    lv_obj_set_pos(page->trace,259,147);lv_obj_set_size(page->trace,133,20);
    lv_obj_remove_flag(page->trace,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(page->trace,trace_draw,LV_EVENT_DRAW_MAIN,page);
    static const char *const labels[]={"横滚","俯仰","相对航向"};
    for(unsigned i=0;i<3;++i) {
        lv_obj_t *axis = instrument_card(content, (int)i * 138, 188, 132, 77);
        instrument_text(axis, labels[i], 14, 9, 102, INSTRUMENT_MUTED, small);
        page->values[i] = instrument_text(axis, "--", 14, 33, 106, INSTRUMENT_BLUE, &lv_font_montserrat_28);
    }
    lv_obj_t *zero=lv_button_create(content);lv_obj_set_pos(zero,0,276);lv_obj_set_size(zero,132,46);
    instrument_button_style(zero, INSTRUMENT_SECONDARY);
    lv_obj_add_event_cb(zero,motion_zero,LV_EVENT_CLICKED,ui);
    app_ui_add_text(zero,"姿态归零",LV_ALIGN_CENTER,0,0,0xFFFFFF,app_ui_chinese_font());
    lv_obj_t *calibrate=lv_button_create(content);
    lv_obj_set_pos(calibrate,138,276);lv_obj_set_size(calibrate,132,46);
    instrument_button_style(calibrate, INSTRUMENT_PRIMARY);
    lv_obj_add_event_cb(calibrate,motion_calibrate,LV_EVENT_CLICKED,ui);
    app_ui_add_text(calibrate,"漂移校准",LV_ALIGN_CENTER,0,0,0xFFFFFF,app_ui_chinese_font());
    lv_obj_t *level = lv_button_create(content);
    lv_obj_set_pos(level, 276, 276);
    lv_obj_set_size(level, 132, 46);
    instrument_button_style(level, INSTRUMENT_SECONDARY);
    lv_obj_add_event_cb(level, open_level, LV_EVENT_CLICKED, ui);
    app_ui_add_text(level, "水平仪", LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, small);
    app_ui_set_timer(ui,motion_tick,20);motion_tick(ui->timer);
}

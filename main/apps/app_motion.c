#include "app_ui_internal.h"
#include "app_telemetry.h"
#include "app_motion_filter.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    app_motion_filter_t filter;
    lv_obj_t *model, *trace, *state, *metrics, *values[3];
    uint32_t draw_ms, label_ms;
    float shown_roll,shown_pitch,shown_yaw,shown_linear;
    app_motion_action_t shown_action;
    bool sleeping;
} motion_page_t;
typedef struct {float x,y,z;} vector_t;
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
static void motion_cleanup(app_ui_t *ui) {free(ui->page_context);ui->page_context=NULL;}
static void update_labels(motion_page_t *page) {
    app_ui_label_fmt(page->values[0],"%+.0f°",page->filter.roll);
    app_ui_label_fmt(page->values[1],"%+.0f°",page->filter.pitch);
    app_ui_label_fmt(page->values[2],"%+.0f°",page->filter.yaw);
    app_ui_label_text(page->state,app_motion_action_name(page->filter.action));
    lv_obj_set_style_text_color(page->state,lv_color_hex(action_color(page->filter.action)),0);
    app_ui_label_fmt(page->metrics,"动态加速度 %.1f m/s^2  转速 %.0f°/s",page->filter.linear,page->filter.rotation);
}
static void motion_zero(lv_event_t *e) {
    app_ui_t *ui=lv_event_get_user_data(e);motion_page_t *page=ui->page_context;
    if(!page || !page->filter.initialized) return;
    app_motion_filter_zero(&page->filter);update_labels(page);lv_obj_invalidate(page->model);
}
static void motion_tick(lv_timer_t *timer) {
    app_ui_t *ui=lv_timer_get_user_data(timer);motion_page_t *page=ui->page_context;
    if(!page) return;
    bool asleep=app_power_is_sleeping(ui->power);
    if(asleep) {
        if(!page->sleeping) app_telemetry_set_interest(0);
        page->sleeping=true;return;
    }
    if(page->sleeping) {page->sleeping=false;app_telemetry_set_interest(APP_TELEMETRY_IMU);}
    app_telemetry_snapshot_t sample;app_telemetry_get(&sample);
    if(sample.imu_error!=ESP_OK) {
        app_ui_label_text(page->state,"等待采样");
        app_ui_label_fmt(page->metrics,"六轴传感器：%s",esp_err_to_name(sample.imu_error));return;
    }
    for(unsigned i=0;i<3;++i) if(!isfinite(sample.imu.accel_mps2[i]) || !isfinite(sample.imu.gyro_dps[i])) {
        app_ui_label_text(page->state,"数据异常");
        app_ui_label_text(page->metrics,"传感器样本无效，已暂停姿态更新");return;
    }
    if(!app_motion_filter_update(&page->filter,sample.imu.accel_mps2,sample.imu.gyro_dps,sample.imu_updated_ms)) return;
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
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
void app_open_motion(app_ui_t *ui) {
    lv_obj_t *content=app_ui_begin_page(ui,"运动与姿态");
    motion_page_t *page=calloc(1,sizeof(*page));
    if(!page) {text(content,"内存不足",0,0,0xFF9791,16);return;}
    ui->page_context=page;ui->page_cleanup=motion_cleanup;app_motion_filter_init(&page->filter);
    app_telemetry_set_interest(APP_TELEMETRY_IMU);
    page->state=text(content,"等待采样",8,4,0x7EDDC8,24);
    text(content,"实时六轴融合",274,9,0x7898B4,16);
    page->model=lv_obj_create(content);lv_obj_remove_style_all(page->model);
    lv_obj_set_pos(page->model,78,30);lv_obj_set_size(page->model,252,166);
    lv_obj_remove_flag(page->model,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(page->model,model_draw,LV_EVENT_DRAW_MAIN,page);
    static const char *const labels[]={"横滚","俯仰","相对航向"};
    for(unsigned i=0;i<3;++i) {
        int x=8+i*134;text(content,labels[i],x,204,0x7898B4,16);
        page->values[i]=text(content,"--",x,228,0xD9E8FF,24);
    }
    page->metrics=text(content,"等待有效传感器数据",8,270,0xA8BED3,16);
    lv_obj_set_width(page->metrics,396);lv_label_set_long_mode(page->metrics,LV_LABEL_LONG_DOT);
    page->trace=lv_obj_create(content);lv_obj_remove_style_all(page->trace);
    lv_obj_set_pos(page->trace,8,302);lv_obj_set_size(page->trace,280,30);
    lv_obj_remove_flag(page->trace,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(page->trace,trace_draw,LV_EVENT_DRAW_MAIN,page);
    lv_obj_t *zero=lv_button_create(content);lv_obj_set_pos(zero,306,296);lv_obj_set_size(zero,94,36);
    lv_obj_set_style_bg_color(zero,lv_color_hex(0x2768D8),0);
    lv_obj_add_event_cb(zero,motion_zero,LV_EVENT_CLICKED,ui);
    app_ui_add_text(zero,"姿态归零",LV_ALIGN_CENTER,0,0,0xFFFFFF,app_ui_chinese_font());
    text(content,"六轴航向为相对值，长时间使用会漂移",8,334,0x6985A3,16);
    app_ui_set_timer(ui,motion_tick,20);motion_tick(ui->timer);
}

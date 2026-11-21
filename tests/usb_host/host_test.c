#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/aether_usb_host/aether_usb_host.c"
static int critical,route=-1,install_error,uninstall_error,query_error,on_error,off_error,lib_error,route_off_error;
static unsigned installs,uninstalls,route_writes,on_calls,off_calls,root_on,root_off,allocs,frees;
static bool sink,physical_vbus,cancel_allowed,core_live;
static unsigned generic_keyboard_events,generic_mouse_events,generic_resets;
static aether_usb_input_event_t last_keyboard,last_mouse;
static unsigned role_calls,control_allocations,parser_allocations;
static esp_err_t control_submit_error;
void *heap_caps_calloc(size_t n,size_t z,unsigned caps){
 if(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)){assert(n==1&&z==sizeof(struct aether_usb_host));++control_allocations;}
 else{assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)&&n==HID_SLOTS&&z==sizeof(hid_interface_t));++parser_allocations;}
 return calloc(n,z);}
void heap_caps_free(void*p){free(p);}
static uint16_t delivered;
static TickType_t ticks;
static usb_transfer_t *pending[32];
static const usb_device_desc_t descriptor={.bMaxPacketSize0=64,.idVendor=0x1234,.idProduct=0x5678};
static const uint8_t config[]={9,2,34,0,1,1,0,0x80,50,9,4,0,0,1,3,1,1,0,9,0x21,0x11,1,0,1,0x22,63,0,7,5,0x81,3,8,0,10};
static const uint8_t *active_config=config;
void test_enter(int *p){(void)p;assert(!critical);critical=1;}
void test_exit(int *p){(void)p;assert(critical);critical=0;}
int64_t esp_timer_get_time(void){return (int64_t)ticks*1000;}
BaseType_t xTaskCreate(void (*f)(void*),const char*n,unsigned s,void*a,unsigned prio,TaskHandle_t*out){(void)f;(void)a;assert(!strcmp(n,"usb_hid")&&s==6144&&prio==4);*out=(void*)1;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t h){assert(h==(void*)1);}
unsigned ulTaskNotifyTake(BaseType_t b,TickType_t t){(void)b;(void)t;return 0;}
TickType_t xTaskGetTickCount(void){return ticks;}
void vTaskDelay(TickType_t t){ticks+=t;}
esp_err_t gpio_set_level(int p,int level){assert(p==51&&!critical);++route_writes;if(!level&&route_off_error)return route_off_error;route=level;return ESP_OK;}
esp_err_t usb_host_install(const usb_host_config_t*c){assert(!critical&&c->root_port_unpowered&&!c->skip_phy_setup&&c->peripheral_map==0);++installs;if(!install_error)core_live=true;return install_error;}
esp_err_t usb_host_uninstall(void){assert(!critical);++uninstalls;if(!uninstall_error)core_live=false;return uninstall_error;}
esp_err_t usb_host_client_register(const usb_host_client_config_t*c,usb_host_client_handle_t*h){assert(c->async.callback_arg);*h=(void*)2;return ESP_OK;}
esp_err_t usb_host_client_deregister(usb_host_client_handle_t h){assert(h==(void*)2);return ESP_OK;}
esp_err_t usb_host_device_free_all(void){return ESP_OK;}
esp_err_t usb_host_lib_set_root_port_power(bool b){if(b){assert(physical_vbus);++root_on;}else ++root_off;return ESP_OK;}
esp_err_t usb_host_lib_handle_events(TickType_t t,uint32_t*e){assert(!t);*e=0;return lib_error;}
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t h,TickType_t t){assert(h==(void*)2&&!t);
 if(cancel_allowed)for(unsigned j=0;j<32;++j)if(pending[j]){usb_transfer_t*x=pending[j];pending[j]=NULL;x->status=USB_TRANSFER_STATUS_CANCELED;x->actual_num_bytes=0;x->callback(x);}
 return ESP_OK;}
esp_err_t usb_host_transfer_alloc(size_t n,int iso,usb_transfer_t**out){assert(!iso);*out=calloc(1,sizeof(**out));assert(*out);(*out)->data_buffer=calloc(1,(n+63)/64*64);assert((*out)->data_buffer);(*out)->data_buffer_size=(n+63)/64*64;++allocs;return ESP_OK;}
esp_err_t usb_host_transfer_free(usb_transfer_t*x){for(unsigned j=0;j<32;++j)assert(pending[j]!=x);free(x->data_buffer);free(x);++frees;return ESP_OK;}
static esp_err_t enqueue(usb_transfer_t*t){for(unsigned j=0;j<32;++j)if(!pending[j]){pending[j]=t;return ESP_OK;}assert(0);return ESP_FAIL;}
esp_err_t usb_host_transfer_submit(usb_transfer_t*t){assert(t->num_bytes<= (int)t->data_buffer_size);return enqueue(t);}
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t h,usb_transfer_t*t){assert(h==(void*)2&&t->num_bytes<=(int)t->data_buffer_size);
 if(t->data_buffer[0]&0x80)assert((t->num_bytes-8)%64==0);
 if(control_submit_error)return control_submit_error;
 return enqueue(t);}
esp_err_t usb_host_device_open(usb_host_client_handle_t h,uint8_t addr,usb_device_handle_t*out){assert(h==(void*)2&&addr);*out=(void*)(uintptr_t)(10+addr);return ESP_OK;}
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t d,const usb_config_desc_t**out){assert(d);*out=(const usb_config_desc_t*)active_config;return ESP_OK;}
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t d,const usb_device_desc_t**out){assert(d);*out=&descriptor;return ESP_OK;}
esp_err_t usb_host_interface_claim(usb_host_client_handle_t h,usb_device_handle_t d,uint8_t n,uint8_t alt){assert(h&&d&&n<8&&!alt);return ESP_OK;}
esp_err_t usb_host_interface_release(usb_host_client_handle_t h,usb_device_handle_t d,uint8_t n){assert(h&&d&&n<8);return ESP_OK;}
esp_err_t usb_host_device_close(usb_host_client_handle_t h,usb_device_handle_t d){assert(h&&d);return ESP_OK;}
esp_err_t usb_host_endpoint_halt(usb_device_handle_t d,uint8_t ep){assert(d&&ep);return ESP_OK;}
esp_err_t usb_host_endpoint_flush(usb_device_handle_t d,uint8_t ep){assert(d&&ep);return ESP_OK;}
static esp_err_t role(void*c,bool*yes){assert(!critical&&c==(void*)3);++role_calls;*yes=sink;return query_error;}
static esp_err_t vbus(void*c,bool on){assert(!critical&&c==(void*)3);if(on){++on_calls;physical_vbus=true;return on_error;}
 ++off_calls;if(!off_error)physical_vbus=false;return off_error;}
static void input(void*c,uint16_t b){assert(c==(void*)3&&!critical);delivered=b;}
static void generic_input(void*c,const aether_usb_input_event_t *event){
 assert(c==(void*)3&&!critical);
 if(event->type==AETHER_USB_INPUT_KEYBOARD){last_keyboard=*event;++generic_keyboard_events;}
 else if(event->type==AETHER_USB_INPUT_MOUSE){last_mouse=*event;++generic_mouse_events;}
 else{assert(event->type==AETHER_USB_INPUT_RESET);++generic_resets;}
}
static aether_usb_host_handle_t create(void){aether_usb_host_handle_t h=NULL;
 aether_usb_host_config_t c={.context=(void*)3,.query_sink=role,.set_vbus=vbus,.input=input,.hid_input=generic_input};assert(aether_usb_host_new(&c,&h)==ESP_OK);return h;}
static void start(aether_usb_host_handle_t h,int64_t at){sink=true;assert(aether_usb_host_set_enabled(h,true)==ESP_OK);service(h,at);service(h,at+20000);service(h,at+40000);assert(h->status.active&&physical_vbus);}
static void finish(aether_usb_host_handle_t h,int64_t at){assert(aether_usb_host_set_enabled(h,false)==ESP_OK);cancel_allowed=true;uninstall_error=off_error=0;service(h,at);service(h,at+2000);assert(h->status.state==AETHER_OTG_OFF&&!h->status.cleanup_pending&&!physical_vbus&&aether_usb_bus_get_owner()==AETHER_USB_BUS_NONE);free(h->hid);free(h);}
static void complete(usb_transfer_t*t,const void*data,size_t n,int actual){for(unsigned j=0;j<32;++j)if(pending[j]==t)pending[j]=NULL;
 if(data)memcpy(t->data_buffer+8,data,n);
 t->actual_num_bytes=actual;t->status=USB_TRANSFER_STATUS_COMPLETED;t->callback(t);}
/* USB-IF boot-keyboard layout including LED Output items (63 bytes). */
static const uint8_t keyboard[]={0x05,1,0x09,6,0xa1,1,0x05,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
 0x95,1,0x75,8,0x81,1,0x95,5,0x75,1,0x05,8,0x19,1,0x29,5,0x91,2,0x95,1,0x75,3,0x91,1,
 0x95,6,0x75,8,0x15,0,0x25,0x65,0x05,7,0x19,0,0x29,0x65,0x81,0,0xc0};
/* Layout-inspired fixtures: Logitech-authored Linux v6.12
 * hid-logitech-dj.c reconstructs Consumer reportID3 with16bit arrays and HID++
 * vendor pageFF00 short/long reports. These are not this user's USB dump. */
static const uint8_t receiver_consumer[]={
 0x05,0x0c,0x09,1,0xa1,1,0x85,3,0x15,1,0x26,0xff,2,0x19,1,0x2a,0xff,2,0x75,16,0x95,2,0x81,0,0xc0};
static const uint8_t receiver_vendor[]={
 0x06,0,0xff,0x09,1,0xa1,1,0x85,0x10,0x15,0,0x26,0xff,0,0x75,8,0x95,6,0x09,1,0x81,0,0x09,1,0x91,0,
 0x85,0x11,0x95,19,0x09,1,0x81,0,0x09,1,0x91,0,0xc0};
/* USB-IF boot mouse application with physical Pointer collection. */
static const uint8_t receiver_mouse[]={
 0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,0x05,9,0x19,1,0x29,3,0x15,0,0x25,1,
 0x95,3,0x75,1,0x81,2,0x95,1,0x75,5,0x81,3,0x05,1,0x09,0x30,0x09,0x31,
 0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0,0xc0};
static const uint8_t decoder_limit[]={
 0x05,1,0x09,5,0xa1,1,0x15,0,0x25,1,0x09,0x30,0x75,33,0x95,1,0x81,2,0xc0};
static void parser_classification_tests(void){
 aether_hid_report_t p;
 assert(aether_hid_parse_ex(&p,keyboard,sizeof keyboard)==AETHER_HID_PARSE_MAPPED);
 assert(aether_hid_parse_ex(&p,receiver_consumer,sizeof receiver_consumer)==AETHER_HID_PARSE_UNMAPPED);
 assert(aether_hid_parse_ex(&p,receiver_vendor,sizeof receiver_vendor)==AETHER_HID_PARSE_UNMAPPED);
 assert(aether_hid_parse_ex(&p,receiver_mouse,sizeof receiver_mouse)==AETHER_HID_PARSE_MAPPED);
 uint8_t broken[sizeof receiver_consumer];memcpy(broken,receiver_consumer,sizeof broken);broken[sizeof broken-1]=0x75;
 assert(aether_hid_parse_ex(&p,broken,sizeof broken)==AETHER_HID_PARSE_MALFORMED && p.count==0);
 assert(aether_hid_parse_ex(&p,decoder_limit,sizeof decoder_limit)==AETHER_HID_PARSE_LIMIT);
 assert(aether_hid_parse_ex(&p,receiver_consumer,sizeof receiver_consumer-1)==AETHER_HID_PARSE_MALFORMED);
 puts("PASS descriptor parser distinguishes real-layout mapped, valid-unmapped, malformed-before-first-field and bounded-decoder-limit outcomes");
}
static void complete_status(usb_transfer_t*t,int status){
 for(unsigned j=0;j<32;++j)if(pending[j]==t)pending[j]=NULL;
 t->actual_num_bytes=0;t->status=status;t->callback(t);
}
static void complete_descriptor(hid_interface_t*i,const uint8_t*d,size_t n){
 if(i->control_stage==0)complete(i->control,NULL,0,8);
 assert(i->control_stage==1 && i->identity.report_length==n);
 complete(i->control,d,n,(int)n+8);
}
static void key_report(hid_interface_t*i,uint8_t k){
 usb_transfer_t*t=i->in;for(unsigned j=0;j<32;++j)if(pending[j]==t)pending[j]=NULL;
 memset(t->data_buffer,0,8);t->data_buffer[2]=k;t->actual_num_bytes=8;
 t->status=USB_TRANSFER_STATUS_COMPLETED;t->callback(t);
}
static void receiver_config(uint8_t *out){
 memcpy(out,config,9);out[2]=109;out[3]=0;out[4]=4;
 const uint16_t lengths[]={sizeof keyboard,sizeof receiver_consumer,sizeof receiver_mouse,sizeof receiver_vendor};
 for(unsigned n=0;n<4;++n){
  uint8_t*p=out+9+25*n;memcpy(p,config+9,25);p[2]=(uint8_t)n;
  p[6]=(n==0||n==2)?1:0;p[7]=n==0?1:n==2?2:0;
  p[16]=(uint8_t)lengths[n];p[17]=(uint8_t)(lengths[n]>>8);p[20]=(uint8_t)(0x81+n);
  p[22]=n==3?64:8;
 }
}
static void composite_compatibility_tests(void){
 parser_classification_tests();
 uint8_t cfg[109];receiver_config(cfg);active_config=cfg;
 aether_usb_host_handle_t h=create();start(h,100000);cancel_allowed=false;inspect_device(h,1);
 complete_descriptor(&h->hid[0],keyboard,sizeof keyboard);key_report(&h->hid[0],0x52);
 complete_descriptor(&h->hid[1],receiver_consumer,sizeof receiver_consumer);
 complete_descriptor(&h->hid[2],receiver_mouse,sizeof receiver_mouse);
 complete_descriptor(&h->hid[3],receiver_vendor,sizeof receiver_vendor);
 assert(h->status.active && h->status.hid_interfaces==2 && h->status.ignored_interfaces==2);
 assert(h->status.last_error==ESP_OK && h->status.hid_error==ESP_OK && h->status.report_parse_error==ESP_OK);
 assert(delivered==AETHER_HID_UP && h->hid[0].ready && h->hid[0].in_pending);
 reap(h);assert(h->hid[0].used && !h->hid[1].used && h->hid[2].used && !h->hid[3].used);
 key_report(&h->hid[0],0x1d);assert(delivered==AETHER_HID_A);
 finish(h,200000);assert(allocs==frees);
 puts("PASS wireless-receiver keyboard stays usable while consumer-array/vendor interfaces are ignored while the boot mouse remains usable");

 receiver_config(cfg);active_config=cfg;h=create();start(h,300000);cancel_allowed=false;inspect_device(h,1);
 uint8_t malformed[sizeof receiver_consumer];memcpy(malformed,receiver_consumer,sizeof malformed);malformed[sizeof malformed-1]=0x75;
 complete_descriptor(&h->hid[1],malformed,sizeof malformed);
 assert(h->status.hid_error==ESP_ERR_INVALID_RESPONSE && h->status.hid_stage==AETHER_USB_HID_STAGE_REPORT_PARSE);
 assert(h->status.last_interface==1 && h->status.last_subclass==0 && h->status.last_protocol==0);
 assert(h->status.vid==0x1234 && h->status.pid==0x5678 && h->status.report_length==sizeof receiver_consumer);
 assert(h->status.transfer_status==USB_TRANSFER_STATUS_COMPLETED && h->status.report_received==sizeof receiver_consumer);
 complete_descriptor(&h->hid[0],keyboard,sizeof keyboard);key_report(&h->hid[0],0x1d);
 complete_descriptor(&h->hid[2],receiver_mouse,sizeof receiver_mouse);complete_descriptor(&h->hid[3],receiver_vendor,sizeof receiver_vendor);
 assert(h->status.hid_interfaces==2 && h->status.ignored_interfaces==1 && delivered==AETHER_HID_A);
 assert(h->status.hid_error==ESP_ERR_INVALID_RESPONSE && h->status.report_parse_error==ESP_ERR_INVALID_RESPONSE);
 assert(h->status.hid_stage==AETHER_USB_HID_STAGE_REPORT_PARSE && h->status.last_interface==1 && h->status.last_error==ESP_ERR_INVALID_RESPONSE);
 snapshot_error(h,ESP_FAIL);key_report(&h->hid[0],0x52);
 assert(h->status.last_error==ESP_FAIL && h->status.hid_error==ESP_ERR_INVALID_RESPONSE && h->status.last_interface==1);
 finish(h,400000);
 puts("PASS malformed sibling remains a real error: its coherent VID/PID/interface/stage/length/status tuple survives READY, ignored siblings, input and later bus errors");

 receiver_config(cfg);active_config=cfg;h=create();start(h,500000);cancel_allowed=false;inspect_device(h,1);
 complete_status(h->hid[1].control,USB_TRANSFER_STATUS_STALL);
 complete_descriptor(&h->hid[0],keyboard,sizeof keyboard);complete_descriptor(&h->hid[2],receiver_mouse,sizeof receiver_mouse);
 complete_descriptor(&h->hid[3],receiver_vendor,sizeof receiver_vendor);
 assert(h->status.hid_interfaces==2 && h->status.hid_error==ESP_ERR_NOT_SUPPORTED);
 assert(h->status.hid_stage==AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR && h->status.last_interface==1);
 assert(h->status.transfer_status==USB_TRANSFER_STATUS_STALL && h->status.report_parse_error==ESP_OK);
 finish(h,600000);
 puts("PASS actual GET_REPORT_DESCRIPTOR stall remains NOT_SUPPORTED with exact failing interface and transfer status despite a usable keyboard");

 active_config=config;h=create();start(h,700000);cancel_allowed=false;inspect_device(h,1);
 hid_interface_t*i=&h->hid[0];assert(i->control_stage==0);
 complete_status(i->control,USB_TRANSFER_STATUS_STALL);
 assert(i->control_stage==1 && i->control_pending && !i->closing && h->status.hid_error==ESP_OK);
 complete_descriptor(i,keyboard,sizeof keyboard);key_report(i,0x28);assert(delivered==AETHER_HID_START);
 finish(h,800000);
 uint8_t nonboot[sizeof config];memcpy(nonboot,config,sizeof nonboot);nonboot[15]=0;active_config=nonboot;
 h=create();start(h,900000);cancel_allowed=false;inspect_device(h,1);i=&h->hid[0];
 assert(i->control_stage==1 && i->control->data_buffer[1]==6);
 complete_descriptor(i,keyboard,sizeof keyboard);assert(i->ready);finish(h,1000000);
 puts("PASS SET_PROTOCOL is Boot-subclass-only; its STALL falls back to report descriptor while non-Boot protocol1 skips the boot request");

 const int statuses[]={USB_TRANSFER_STATUS_TIMED_OUT,USB_TRANSFER_STATUS_NO_DEVICE,USB_TRANSFER_STATUS_CANCELED,USB_TRANSFER_STATUS_ERROR};
 const esp_err_t errors[]={ESP_ERR_TIMEOUT,ESP_ERR_NOT_FOUND,ESP_ERR_INVALID_STATE,ESP_FAIL};
 active_config=config;
 for(unsigned n=0;n<4;++n){
  h=create();start(h,1100000+n*100000);cancel_allowed=false;inspect_device(h,1);i=&h->hid[0];
  complete_status(i->control,statuses[n]);
  assert(i->closing && !i->control_pending && i->control_stage==0 && h->status.hid_error==errors[n]);
  assert(h->status.hid_stage==AETHER_USB_HID_STAGE_SET_PROTOCOL && h->status.transfer_status==statuses[n]);
  finish(h,1150000+n*100000);
 }
 h=create();start(h,1600000);cancel_allowed=false;inspect_device(h,1);i=&h->hid[0];
 control_submit_error=ESP_ERR_NO_MEM;complete_status(i->control,USB_TRANSFER_STATUS_STALL);control_submit_error=ESP_OK;
 assert(i->closing && !i->control_pending && h->status.hid_error==ESP_ERR_NO_MEM && h->status.hid_stage==AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR);
 finish(h,1650000);
 puts("PASS SET_PROTOCOL cancellation/disconnect/timeout/error and fallback submit failures remain real, with no unsafe fallback or pending-buffer leak");

 h=create();start(h,1700000);cancel_allowed=false;inspect_device(h,1);i=&h->hid[0];complete(i->control,NULL,0,8);
 complete(i->control,keyboard,sizeof keyboard-1,(int)sizeof keyboard+7);
 assert(i->closing && h->status.hid_error==ESP_ERR_INVALID_RESPONSE && h->status.report_parse_error==ESP_OK);
 assert(h->status.hid_stage==AETHER_USB_HID_STAGE_REPORT_DESCRIPTOR && h->status.report_received==sizeof keyboard-1);
 finish(h,1750000);
 h=create();start(h,1800000);cancel_allowed=false;inspect_device(h,1);i=&h->hid[0];complete(i->control,NULL,0,8);
 uint8_t padded[64];memcpy(padded,keyboard,sizeof keyboard);padded[63]=0x75;
 complete(i->control,padded,sizeof padded,72);
 assert(i->ready && h->status.hid_error==ESP_OK && h->status.report_received==64);
 finish(h,1850000);
 puts("PASS EP0 short descriptors fail precisely; rounded transport padding is bounded and never fed into descriptor parsing");

 uint8_t limited[sizeof config];memcpy(limited,config,sizeof limited);limited[15]=limited[16]=0;limited[25]=sizeof decoder_limit;active_config=limited;
 h=create();start(h,1900000);cancel_allowed=false;inspect_device(h,1);i=&h->hid[0];
 complete_descriptor(i,decoder_limit,sizeof decoder_limit);
 assert(i->closing && h->status.hid_error==ESP_ERR_NOT_SUPPORTED && h->status.report_parse_error==ESP_ERR_NOT_SUPPORTED);
 assert(h->status.hid_stage==AETHER_USB_HID_STAGE_REPORT_PARSE);
 assert(aether_usb_host_set_enabled(h,false)==ESP_OK);cancel_allowed=true;service(h,1950000);service(h,1952000);
 assert(h->status.state==AETHER_OTG_OFF && h->status.hid_error==ESP_ERR_NOT_SUPPORTED);
 active_config=config;start(h,2000000);
 assert(h->status.hid_error==ESP_OK && h->status.report_parse_error==ESP_OK && !h->status.ignored_interfaces && h->status.transfer_status==-1);
 finish(h,2050000);active_config=config;assert(allocs==frees);
 puts("PASS bounded unsupported fields remain explicit parse-limit errors; a new Host session clears old diagnostic latch and ignored count");
}
static const uint8_t wheel_mouse[]={
 0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,0x05,9,0x19,1,0x29,5,0x15,0,0x25,1,
 0x95,5,0x75,1,0x81,2,0x95,1,0x75,3,0x81,3,0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,
 0x15,0x81,0x25,0x7f,0x75,8,0x95,3,0x81,6,0xc0,0xc0};
static void generic_input_tests(void){
 aether_hid_report_t parser;uint16_t nes;aether_hid_input_t generic;
 assert(aether_hid_parse(&parser,wheel_mouse,sizeof wheel_mouse));
 uint8_t motion[]={3,0xfb,7,0xff};
 assert(aether_hid_decode_input(&parser,motion,sizeof motion,&nes,&generic));
 assert(!nes && generic.mouse&&!generic.keyboard&&generic.dx==-5&&generic.dy==7&&generic.wheel==-1&&generic.buttons==3);
 uint8_t both[sizeof keyboard+sizeof wheel_mouse+4];
 memcpy(both,keyboard,6);both[6]=0x85;both[7]=1;memcpy(both+8,keyboard+6,sizeof keyboard-6);
 const size_t mouse_at=sizeof keyboard+2;
 memcpy(both+mouse_at,wheel_mouse,6);both[mouse_at+6]=0x85;both[mouse_at+7]=2;
 memcpy(both+mouse_at+8,wheel_mouse+6,sizeof wheel_mouse-6);
 assert(aether_hid_parse(&parser,both,sizeof both));
 uint8_t keys[]={1,0x02,0,0x04,0,0,0,0,0};
 assert(aether_hid_decode_input(&parser,keys,sizeof keys,&nes,&generic));
 assert(generic.keyboard&&generic.mouse&&(generic.keys[0x04/8]&(1U<<(0x04%8)))&&(generic.keys[0xe1/8]&(1U<<(0xe1%8))));
 uint8_t mouse_report[]={2,1,0xfd,4,1};
 assert(aether_hid_decode_input(&parser,mouse_report,sizeof mouse_report,&nes,&generic));
 assert(generic.buttons==1&&generic.dx==-3&&generic.dy==4&&generic.wheel==1&&(generic.keys[0]&(1U<<4)));
 memset(keys+1,0,sizeof keys-1);assert(aether_hid_decode_input(&parser,keys,sizeof keys,&nes,&generic));
 assert(generic.buttons==1&&!generic.dx&&!generic.dy&&!generic.wheel&&!generic.keys[0]);
 assert(aether_hid_decode_input(&parser,mouse_report,1,&nes,&generic));
 assert(!generic.buttons&&!nes); /* Truncated relevant report releases safely. */
 puts("PASS generic keyboard usages/modifiers and signed mouse movement/wheel/buttons preserve report-ID state without replaying old relative deltas");

 uint8_t cfg[sizeof config];memcpy(cfg,config,sizeof cfg);cfg[16]=2;cfg[25]=sizeof wheel_mouse;active_config=cfg;
 aether_usb_host_handle_t h=create();start(h,2200000);cancel_allowed=false;inspect_device(h,1);
 complete_descriptor(&h->hid[0],wheel_mouse,sizeof wheel_mouse);
 usb_transfer_t*t=h->hid[0].in;for(unsigned n=0;n<32;++n)if(pending[n]==t)pending[n]=NULL;
 memcpy(t->data_buffer,motion,sizeof motion);t->actual_num_bytes=sizeof motion;t->status=USB_TRANSFER_STATUS_COMPLETED;t->callback(t);
 assert(last_mouse.dx==-5&&last_mouse.dy==7&&last_mouse.wheel==-1&&last_mouse.buttons==3&&!delivered);
 const unsigned before_reset=generic_resets;
 usb_host_client_event_msg_t gone={.event=USB_HOST_CLIENT_EVENT_DEV_GONE,.dev_gone={.dev_hdl=(void*)11}};
 client_event(&gone,h);assert(last_mouse.buttons==0&&!last_mouse.dx&&!last_mouse.dy);
 finish(h,2300000);assert(generic_resets==before_reset+1);active_config=config;
 puts("PASS real Host generic callback emits mouse deltas/button snapshots, hot-unplug release and one RESET on Host teardown");
}
typedef struct {unsigned starts,requests,polls;bool running,drained;esp_err_t start_error,poll_error;} class_fake_t;
static esp_err_t class_start(void *context){class_fake_t*c=context;assert(!critical&&core_live&&!physical_vbus);++c->starts;
 if(c->start_error){return c->start_error;}c->running=true;return ESP_OK;}
static void class_request(void *context){class_fake_t*c=context;assert(!critical&&c->running);++c->requests;}
static esp_err_t class_poll(void *context){class_fake_t*c=context;assert(!critical&&c->running);++c->polls;
 if(c->poll_error){return c->poll_error;}if(!c->drained){return ESP_ERR_NOT_FINISHED;}c->running=false;return ESP_OK;}
static void non_hid_class_policy_tests(void){
 /* Valid AudioControl, MSC BOT and vendor interfaces belong to other clients;
  * the HID inspector must not claim them or report them as unsupported. */
 static const uint8_t classes[]={
  9,2,50,0,3,1,0,0x80,50,
  9,4,0,0,0,1,1,0,0,
  9,4,1,0,2,8,6,0x50,0,7,5,0x81,2,64,0,0,7,5,2,2,64,0,0,
  9,4,2,0,0,0xff,0,0,0};
 active_config=classes;
 aether_usb_host_handle_t h=create();start(h,2000000);
 const unsigned before=allocs;
 inspect_device(h,1);
 assert(h->status.active && h->status.hid_interfaces==0 && h->status.ignored_interfaces==0);
 assert(h->status.last_error==ESP_OK && h->status.hid_error==ESP_OK && allocs==before);
 for(unsigned n=0;n<HID_SLOTS;++n)assert(!h->hid[n].used);
 assert(reap(h));for(unsigned n=0;n<DEVICE_SLOTS;++n)assert(!h->devices[n]);
 finish(h,2100000);active_config=config;
 puts("PASS valid UAC/MSC/vendor-only interfaces do not claim HID endpoints or pollute Host status with NOT_SUPPORTED");
}
static void class_lifecycle_tests(void){
 class_fake_t c[3]={0};aether_usb_host_class_ops_t ops[3];
 for(unsigned n=0;n<3;++n)ops[n]=(aether_usb_host_class_ops_t){.context=&c[n],.start=class_start,.request_stop=class_request,.poll_stopped=class_poll};
 aether_usb_host_config_t cfg={.context=(void*)3,.query_sink=role,.set_vbus=vbus,.input=input,.hid_input=generic_input,.classes=ops,.class_count=3};
 aether_usb_host_handle_t h=NULL;assert(aether_usb_host_new(&cfg,&h)==ESP_OK);start(h,2400000);
 assert(h->started_classes==7&&c[0].starts==1&&c[1].starts==1&&c[2].starts==1);
 unsigned before=uninstalls;assert(aether_usb_host_set_enabled(h,false)==ESP_OK);service(h,2500000);
 assert(!physical_vbus&&core_live&&h->status.cleanup_pending&&uninstalls==before);
 assert(c[0].requests==1&&c[1].requests==1&&c[2].requests==1);
 c[0].drained=true;service(h,2520000);assert(h->started_classes==6&&core_live);
 c[1].drained=c[2].drained=true;service(h,2540000);
 assert(!h->started_classes&&!core_live&&!h->status.cleanup_pending&&h->status.state==AETHER_OTG_OFF);
 assert(c[0].requests==1&&c[1].requests==1&&c[2].requests==1);finish(h,2560000);
 puts("PASS one Host core installs3 class clients before root ON, requests all stops once, cuts VBUS promptly, and defers core uninstall until every class drains");

 memset(c,0,sizeof c);c[1].start_error=ESP_ERR_NO_MEM;
 assert(aether_usb_host_new(&cfg,&h)==ESP_OK);sink=true;assert(aether_usb_host_set_enabled(h,true)==ESP_OK);
 service(h,2600000);service(h,2620000);service(h,2640000);
 assert(!h->status.active&&!physical_vbus&&h->status.cleanup_pending&&core_live);
 assert(c[0].starts==1&&c[0].requests==1&&c[1].starts==1&&!c[1].requests&&!c[2].starts);
 c[0].drained=true;service(h,2660000);finish(h,2680000);
 cfg.class_count=4;assert(aether_usb_host_new(&cfg,&h)==ESP_ERR_INVALID_ARG);
 cfg.class_count=1;cfg.classes=NULL;assert(aether_usb_host_new(&cfg,&h)==ESP_ERR_INVALID_ARG);
 puts("PASS partial class startup rolls back only successful classes; missing/oversized hook arrays are rejected without a second Host install");
}
static uint16_t remap_keyboard(void *context,const uint8_t keys[32])
{
 assert(context==(void*)3&&!critical);
 uint16_t b=0;
 if(keys[0x0a/8]&(1U<<(0x0a%8)))b|=AETHER_HID_A;
 if(keys[0xe0/8]&(1U<<(0xe0%8)))b|=AETHER_HID_SELECT;
 return b;
}
static void custom_keyboard_map_tests(void)
{
 aether_usb_host_handle_t h=create();h->config.keyboard_map=remap_keyboard;start(h,14000000);
 hid_interface_t *kbd=&h->hid[0],*pad=&h->hid[1];kbd->used=kbd->ready=true;
 assert(aether_hid_parse(&kbd->parser,keyboard,sizeof keyboard));
 uint8_t report[8]={0,0,0x1d};
 assert(aether_hid_decode_input(&kbd->parser,report,sizeof report,&kbd->buttons,&kbd->generic));
 assert(kbd->buttons==AETHER_HID_A&&!kbd->generic.non_keyboard_buttons);
 assert(!aggregate(h)&&!delivered); /* A custom map really removes legacy Z=A. */
 report[2]=0x0a;
 assert(aether_hid_decode_input(&kbd->parser,report,sizeof report,&kbd->buttons,&kbd->generic));
 assert(aggregate(h)==AETHER_HID_A&&delivered==AETHER_HID_A);
 static const uint8_t four_buttons[]={0x05,1,0x09,5,0xa1,1,0x05,9,0x19,1,0x29,4,0x15,0,0x25,1,0x75,1,0x95,4,0x81,2,0xc0};
 pad->used=pad->ready=true;assert(aether_hid_parse(&pad->parser,four_buttons,sizeof four_buttons));
 uint8_t buttons=2;
 assert(aether_hid_decode_input(&pad->parser,&buttons,1,&pad->buttons,&pad->generic));
 assert(pad->generic.non_keyboard_buttons==AETHER_HID_B);
 assert(aggregate(h)==(AETHER_HID_A|AETHER_HID_B));
 report[0]=1;
 assert(aether_hid_decode_input(&kbd->parser,report,sizeof report,&kbd->buttons,&kbd->generic));
 assert(aggregate(h)==(AETHER_HID_A|AETHER_HID_B|AETHER_HID_SELECT));
 memset(report,0,sizeof report);
 assert(aether_hid_decode_input(&kbd->parser,report,sizeof report,&kbd->buttons,&kbd->generic));
 assert(aggregate(h)==AETHER_HID_B);
 buttons=0;assert(aether_hid_decode_input(&pad->parser,&buttons,1,&pad->buttons,&pad->generic));assert(!aggregate(h));
 kbd->used=kbd->ready=pad->used=pad->ready=false;finish(h,14100000);
 puts("PASS actual keyboard reports use custom keys instead of legacy shortcuts while modifiers, gamepad input and independent releases remain correct");

 const uint8_t combined[]={0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,0x19,0,0x29,0x7f,0x15,0,0x25,1,0x75,1,0x95,128,0x81,2,0xc0,
  0x05,1,0x09,5,0xa1,1,0x85,2,0x05,9,0x19,1,0x29,8,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0xc0};
 aether_hid_report_t model;assert(aether_hid_parse(&model,combined,sizeof combined));
 aether_hid_input_t generic;uint16_t legacy;uint8_t keyreport[17]={1};keyreport[1+0x1d/8]|=1U<<(0x1d%8);
 assert(aether_hid_decode_input(&model,keyreport,sizeof keyreport,&legacy,&generic)&&legacy==AETHER_HID_A&&!generic.non_keyboard_buttons);
 const uint8_t padreport[]={2,2};assert(aether_hid_decode_input(&model,padreport,2,&legacy,&generic));
 assert(legacy==(AETHER_HID_A|AETHER_HID_B)&&generic.non_keyboard_buttons==AETHER_HID_B);
 keyreport[1+0x1d/8]=0;assert(aether_hid_decode_input(&model,keyreport,sizeof keyreport,&legacy,&generic));
 assert(legacy==AETHER_HID_B&&generic.non_keyboard_buttons==AETHER_HID_B);
 aether_hid_release(&model);assert(aether_hid_decode_input(&model,keyreport,sizeof keyreport,&legacy,&generic)&&!legacy&&!generic.non_keyboard_buttons);
 puts("PASS mixed keyboard/gamepad report IDs preserve only physical pad state across key release and clear all sources on disconnect");
}
static void parser_tests(void){
 aether_hid_report_t p;assert(aether_hid_parse(&p,keyboard,sizeof keyboard));uint16_t b;uint8_t keys[8]={0x20,0,0x52,0x1d,0x28,0,0,0};
 assert(aether_hid_decode(&p,keys,8,&b)&&b==(AETHER_HID_SELECT|AETHER_HID_UP|AETHER_HID_A|AETHER_HID_START));
 memset(keys,0,8);assert(aether_hid_decode(&p,keys,8,&b)&&!b);keys[2]=0x29;assert(aether_hid_decode(&p,keys,8,&b)&&b==AETHER_HID_EXIT);
 keys[2]=1;assert(aether_hid_decode(&p,keys,8,&b)&&!b);assert(aether_hid_decode(&p,keys,1,&b)&&!b);
 const uint8_t game[]={0x05,1,0x09,5,0xa1,1,0x85,7,0x15,0x81,0x25,0x7f,0x09,0x30,0x09,0x31,0x75,8,0x95,2,0x81,2,
 0x15,0,0x25,7,0x09,0x39,0x75,4,0x95,1,0x81,0x42,0x75,4,0x95,1,0x81,1,0x05,9,0x19,1,0x29,16,0x15,0,0x25,1,0x75,1,0x95,16,0x81,2,0xc0};
 assert(aether_hid_parse(&p,game,sizeof game));uint8_t report[]={7,0x81,0x7f,8,1,3};
 assert(aether_hid_decode(&p,report,sizeof report,&b)&&b==(AETHER_HID_LEFT|AETHER_HID_DOWN|AETHER_HID_A|AETHER_HID_SELECT|AETHER_HID_START));
 report[1]=report[2]=0;report[3]=1;report[4]=report[5]=0;assert(aether_hid_decode(&p,report,sizeof report,&b)&&b==(AETHER_HID_UP|AETHER_HID_RIGHT));
 report[3]=8;assert(aether_hid_decode(&p,report,sizeof report,&b)&&!b);report[0]=8;assert(!aether_hid_decode(&p,report,sizeof report,&b));
 const uint8_t four_hat[]={0x05,1,0x09,5,0xa1,1,0x09,0x39,0x15,0,0x25,3,0x75,4,0x95,1,0x81,0x42,0xc0};
 assert(aether_hid_parse(&p,four_hat,sizeof four_hat));
 const uint16_t cardinal[]={AETHER_HID_UP,AETHER_HID_RIGHT,AETHER_HID_DOWN,AETHER_HID_LEFT};
 for(uint8_t j=0;j<4;++j)assert(aether_hid_decode(&p,&j,1,&b)&&b==cardinal[j]);
 uint8_t null_hat=4;assert(aether_hid_decode(&p,&null_hat,1,&b)&&!b);
 uint8_t one_based[sizeof four_hat];memcpy(one_based,four_hat,sizeof four_hat);one_based[9]=1;one_based[11]=4;
 assert(aether_hid_parse(&p,one_based,sizeof one_based));
 for(uint8_t j=1;j<=4;++j)assert(aether_hid_decode(&p,&j,1,&b)&&b==cardinal[j-1]);
 null_hat=0;assert(aether_hid_decode(&p,&null_hat,1,&b)&&!b);
 const uint8_t null_axis[]={0x05,1,0x09,5,0xa1,1,0x09,0x30,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,0x42,0x05,9,0x09,1,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,0xc0};
 assert(aether_hid_parse(&p,null_axis,sizeof null_axis));uint8_t na[]={0x80,1};
 assert(aether_hid_decode(&p,na,sizeof na,&b)&&b==AETHER_HID_A);
 na[0]=0x81;assert(aether_hid_decode(&p,na,sizeof na,&b)&&b==(AETHER_HID_LEFT|AETHER_HID_A));
 na[0]=0x7f;assert(aether_hid_decode(&p,na,sizeof na,&b)&&b==(AETHER_HID_RIGHT|AETHER_HID_A));
 na[0]=0;assert(aether_hid_decode(&p,na,sizeof na,&b)&&b==AETHER_HID_A);
 puts("PASS four-way hats map cardinal quarter turns for0..3 and1..4 with null release; nullable out-of-range axes stay neutral while independent pad buttons remain held");
 for(size_t n=0;n<sizeof game;++n){aether_hid_report_t truncated;aether_hid_parse(&truncated,game,n);}
 uint8_t invalid[]={0x75,33,0x95,1,0x05,9,0x09,1,0x81,2};assert(!aether_hid_parse(&p,invalid,sizeof invalid));
 const uint8_t nkro[]={0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,0x19,0,0x29,0x7f,0x15,0,0x25,1,0x75,1,0x95,128,0x81,2,0xc0,
 0x05,1,0x09,5,0xa1,1,0x85,2,0x05,9,0x19,1,0x29,8,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0xc0};
 assert(aether_hid_parse(&p,nkro,sizeof nkro));uint8_t nk[17]={1};nk[1+0x1d/8]|=1U<<(0x1d%8);
 assert(aether_hid_decode(&p,nk,sizeof nk,&b)&&b==AETHER_HID_A);
 uint8_t second[]={2,2};assert(aether_hid_decode(&p,second,2,&b)&&b==(AETHER_HID_A|AETHER_HID_B));
 nk[1+0x1d/8]=0;assert(aether_hid_decode(&p,nk,sizeof nk,&b)&&b==AETHER_HID_B);
 assert(aether_hid_decode(&p,nk,0,&b)&&!b);
 const uint8_t mouse[]={0x05,1,0x09,2,0xa1,1,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0};
 assert(aether_hid_parse(&p,mouse,sizeof mouse)&&p.has_mouse);
 for(unsigned j=0;j<10000;++j){uint8_t fuzz[64];for(unsigned k=0;k<sizeof fuzz;++k)fuzz[k]=(uint8_t)(j*73+k*97+(j>>(k%13)));
   if(aether_hid_parse(&p,fuzz,sizeof fuzz))aether_hid_decode(&p,fuzz,sizeof fuzz,&b);}
 puts("PASS NKRO bitmap,interleaved report-ID state,unknown/empty reports,separate generic mouse collection handling and10000 bounded malformed descriptors");
 const uint8_t four_buttons[]={0x05,1,0x09,5,0xa1,1,0x05,9,0x19,1,0x29,4,0x15,0,0x25,1,0x75,1,0x95,4,0x81,2,0xc0};
 assert(aether_hid_parse(&p,four_buttons,sizeof four_buttons));uint8_t four=0x0c;
 assert(aether_hid_decode(&p,&four,1,&b)&&b==(AETHER_HID_SELECT|AETHER_HID_START));
 assert(aether_hid_parse(&p,game,sizeof game));uint8_t clicks[]={7,0,0,8,0,0x1c};
 assert(aether_hid_decode(&p,clicks,sizeof clicks,&b)&&b==0);
 const uint8_t home[]={0x05,0x0c,0x09,1,0xa1,1,0x0a,0x23,2,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,0xc0};
 assert(aether_hid_parse(&p,home,sizeof home));uint8_t one=1;
 assert(aether_hid_decode(&p,&one,1,&b)&&b==AETHER_HID_EXIT);
 puts("PASS four-button pad3/4=Select/Start; larger pad9/10; L3/R3 numbered11/12/13 never exit/reset; standard AC Home exits");
 puts("PASS real report parsing: keyboard modifiers/arrows/Z/Enter/Esc/rollover/truncation; gamepad report IDs,signed XY,hat diagonals/null,16buttons; malformed bounds");
}
int main(void){parser_tests();
 aether_usb_host_handle_t h=create();unsigned r=route_writes;
 for(unsigned j=0;j<10;++j)service(h,j*20000);
 assert(!installs&&!on_calls&&!off_calls&&route_writes==r);
 assert(aether_usb_bus_claim(AETHER_USB_BUS_MSC)==ESP_OK);route=1;service(h,300000);assert(route==1&&route_writes==r);
 assert(aether_usb_host_set_enabled(h,true)==ESP_ERR_INVALID_STATE);assert(aether_usb_bus_release(AETHER_USB_BUS_MSC)==ESP_OK);
 assert(aether_usb_host_set_enabled(h,true)==ESP_OK);sink=false;service(h,400000);assert(!installs&&!on_calls);
 query_error=ESP_FAIL;service(h,420000);assert(!installs&&!on_calls);query_error=0;
 finish(h,500000);assert(off_calls==1);puts("PASS default OFF never touches PHY,mux or VBUS; MSC excludes Host; waiting/unknown CC cannot source; OFF withdraws CC policy even without boost");
 h=create();start(h,1000000);assert(root_on==1&&route==1);uninstall_error=ESP_ERR_TIMEOUT;assert(aether_usb_host_set_enabled(h,false)==ESP_OK);
 service(h,1100000);assert(!physical_vbus&&h->status.cleanup_pending&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST&&route==1);
 finish(h,1200000);puts("PASS confirmed sink starts root-unpowered,thenVBUS,thenlogicalport; failed host teardown still cuts VBUS and retains reservation/mux until clean");
 h=create();sink=true;on_error=ESP_FAIL;assert(aether_usb_host_set_enabled(h,true)==ESP_OK);
 service(h,2000000);service(h,2020000);service(h,2040000);assert(!physical_vbus&&!h->status.active&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);
 on_error=0;finish(h,2100000);puts("PASS boost write/readback failure rolls back physical power without hidden active state");
 h=create();start(h,3000000);cancel_allowed=false;inspect_device(h,1);hid_interface_t*i=&h->hid[0];assert(i->used&&i->control_pending&&i->control->num_bytes==8);
 complete(i->control,NULL,0,8);assert(i->control_pending&&i->control->num_bytes==72&&i->control->data_buffer_size>=72);
 complete(i->control,keyboard,sizeof keyboard,(int)sizeof keyboard+8);assert(i->ready&&i->in_pending);
 unsigned before=frees;assert(aether_usb_host_set_enabled(h,false)==ESP_OK);service(h,3100000);
 assert(!physical_vbus&&h->status.cleanup_pending&&frees==before&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);
 finish(h,3200000);assert(allocs==frees&&delivered==0);puts("PASS boot keyboard SET_PROTOCOL,EP0 rounded report request,descriptor decoder; in-flight cancellation never frees DMA before callback and releases all buttons");
 h=create();start(h,4000000);sink=false;service(h,4080000);assert(!physical_vbus&&!h->status.active&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);finish(h,4100000);
 h=create();start(h,4300000);off_error=ESP_FAIL;assert(aether_usb_host_set_enabled(h,false)==ESP_OK);service(h,4380000);
 assert(physical_vbus&&h->status.cleanup_pending&&h->status.vbus_enabled&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);
 finish(h,4400000);
 h=create();start(h,4500000);route_off_error=ESP_FAIL;assert(aether_usb_host_set_enabled(h,false)==ESP_OK);service(h,4580000);
 assert(!physical_vbus&&h->status.cleanup_pending&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);
 route_off_error=0;finish(h,4600000);
 h=create();start(h,4700000);lib_error=ESP_FAIL;service(h,4780000);
 assert(!physical_vbus&&!h->status.active&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);lib_error=0;finish(h,4800000);
 puts("PASS PMIC-off and route-restore failures retain ownership until verified recovery; USB event-pump fault cuts VBUS and releases input");
 uint8_t composite[59];memcpy(composite,config,34);memcpy(composite+34,config+9,25);
 composite[2]=59;composite[4]=2;composite[36]=1;composite[54]=0x82;active_config=composite;
 h=create();start(h,4900000);cancel_allowed=false;inspect_device(h,1);inspect_device(h,2);
 for(unsigned j=0;j<4;++j){hid_interface_t*k=&h->hid[j];assert(k->used&&k->control_pending);
     complete(k->control,NULL,0,8);complete(k->control,keyboard,sizeof keyboard,(int)sizeof keyboard+8);assert(k->ready&&k->in_pending);}
 uint8_t held_keys[8]={0,0,0x52};usb_transfer_t*first=h->hid[0].in;
 for(unsigned j=0;j<32;++j)if(pending[j]==first)pending[j]=NULL;
 memcpy(first->data_buffer,held_keys,8);first->actual_num_bytes=8;first->status=USB_TRANSFER_STATUS_COMPLETED;first->callback(first);
 usb_transfer_t*other=h->hid[2].in;held_keys[2]=0x1d;
 for(unsigned j=0;j<32;++j)if(pending[j]==other)pending[j]=NULL;
 memcpy(other->data_buffer,held_keys,8);other->actual_num_bytes=8;other->status=USB_TRANSFER_STATUS_COMPLETED;other->callback(other);
 assert(delivered==(AETHER_HID_UP|AETHER_HID_A));
 usb_host_client_event_msg_t gone={.event=USB_HOST_CLIENT_EVENT_DEV_GONE,.dev_gone={.dev_hdl=(void*)11}};
 client_event(&gone,h);assert(delivered==AETHER_HID_A);
 for(unsigned k=0;k<2;++k){usb_transfer_t*t=h->hid[k].in;for(unsigned j=0;j<32;++j)if(pending[j]==t)pending[j]=NULL;t->status=USB_TRANSFER_STATUS_CANCELED;t->callback(t);}
 reap(h);assert(!h->hid[0].used&&!h->hid[1].used&&h->hid[2].used&&h->hid[3].used&&delivered==AETHER_HID_A);
 finish(h,4990000);active_config=config;assert(allocs==frees);
 puts("PASS two composite HID devices/four interfaces combine input; hot-unplug releases only removed device and preserves the other held button");
 h=create();start(h,6000000);off_error=ESP_FAIL;lib_error=ESP_FAIL;service(h,6080000);lib_error=0;
 assert(h->status.cleanup_pending&&physical_vbus);unsigned blocked_installs=installs,blocked_on=on_calls,blocked_roles=role_calls;
 for(unsigned j=0;j<12;++j)service(h,6100000+j*20000);
 assert(installs==blocked_installs&&on_calls==blocked_on&&role_calls==blocked_roles&&h->status.cleanup_pending);
 off_error=0;service(h,6500000);assert(h->status.state==AETHER_OTG_WAIT_SINK&&!h->status.cleanup_pending&&!physical_vbus);
 assert(installs==blocked_installs&&aether_usb_bus_get_owner()==AETHER_USB_BUS_HOST);
 service(h,8510000);service(h,8530000);service(h,8550000);assert(h->status.active&&on_calls==blocked_on+1);finish(h,8600000);
 h=create();start(h,9000000);route_off_error=ESP_FAIL;sink=false;service(h,9080000);assert(h->status.cleanup_pending&&!physical_vbus);sink=true;
 blocked_installs=installs;blocked_on=on_calls;blocked_roles=role_calls;
 for(unsigned j=0;j<12;++j)service(h,9100000+j*20000);
 assert(installs==blocked_installs&&on_calls==blocked_on&&role_calls==blocked_roles&&h->status.cleanup_pending);
 route_off_error=0;service(h,9500000);assert(h->status.state==AETHER_OTG_WAIT_SINK&&!h->status.cleanup_pending&&!physical_vbus);
 assert(installs==blocked_installs);finish(h,9600000);
 h=create();start(h,10000000);sink=false;service(h,10080000);
 assert(h->status.enabled&&h->status.state==AETHER_OTG_WAIT_SINK&&!h->status.cleanup_pending&&!physical_vbus);finish(h,10100000);
 h=create();h->hid[0].used=h->hid[0].ready=true;
 h->hid[0].buttons=AETHER_HID_UP|AETHER_HID_SELECT|AETHER_HID_START;ticks=100;
 assert((aggregate(h)&AETHER_HID_EXIT)==0);ticks=1099;assert((aggregate(h)&AETHER_HID_EXIT)==0);
 ticks=1100;assert(aggregate(h)&AETHER_HID_EXIT);h->hid[0].buttons=0;assert(!aggregate(h));
 h->hid[0].used=h->hid[0].ready=false;finish(h,12000000);ticks=0;
 puts("PASS enabled/sink-true cannot bypass pending PMIC-off or mux cleanup; fresh positive samples restart only after cleanup; unplug returns WAIT_SINK");
 printf("PASS allocation split: control=%zu internal bytes, interfaces=%zu PSRAM bytes; no large internal parser allocation\n",sizeof(struct aether_usb_host),HID_SLOTS*sizeof(hid_interface_t));
 for(unsigned loop=0;loop<100;++loop){h=create();start(h,5000000+(int64_t)loop*200000);finish(h,5100000+(int64_t)loop*200000);}
 assert(allocs==frees);puts("PASS external-source/unplug cuts session;100 enable/disable cycles preserve exclusive ownership and default download route");
 composite_compatibility_tests();generic_input_tests();class_lifecycle_tests();non_hid_class_policy_tests();custom_keyboard_map_tests();return 0;}

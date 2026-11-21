#include <assert.h>
#include <stdio.h>
#include <string.h>
#define tskNO_AFFINITY (-1)
#include "../../components/aether_usb_storage/aether_usb_storage.c"

static int critical,task_creates,driver_installs,driver_uninstalls,device_installs,device_uninstalls,mounts,unmounts,notifies;
static int driver_error,device_error,mount_error,unmount_error,device_off_error,driver_off_error;
static bool driver_live,device_live,vfs_live,remove_during_install,stop_during_mount;
static msc_host_driver_config_t driver_config;
static bool lifecycle_held,stop_during_lifecycle;
static esp_err_t lifecycle_error;
esp_err_t esp_vfs_fat_lifecycle_acquire(uint32_t timeout_ms)
{
 assert(!critical&&!lifecycle_held&&(timeout_ms==200||timeout_ms==20));
 if(lifecycle_error)return lifecycle_error;
 lifecycle_held=true;
 if(stop_during_lifecycle){stop_during_lifecycle=false;aether_usb_storage_class_request_stop(&s_storage);}
 return ESP_OK;
}
void esp_vfs_fat_lifecycle_release(void){assert(lifecycle_held&&!critical);lifecycle_held=false;}

#define DEVICE ((msc_host_device_handle_t)(uintptr_t)0x1234)
#define VFS ((msc_host_vfs_handle_t)(uintptr_t)0x5678)
void test_enter(int *lock){(void)lock;assert(!critical);critical=1;}
void test_exit(int *lock){(void)lock;assert(critical);critical=0;}
BaseType_t xTaskCreate(void(*f)(void*),const char*n,unsigned stack,void*arg,unsigned priority,TaskHandle_t*out)
{assert(!critical && f==storage_worker && !strcmp(n,"usb_disk")&&stack==6144&&priority==3&&arg==&s_storage);++task_creates;*out=(void*)1;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t task){assert(task==(void*)1);++notifies;}
unsigned ulTaskNotifyTake(BaseType_t clear,TickType_t wait){(void)clear;(void)wait;return 0;}
esp_err_t msc_host_install(const msc_host_driver_config_t *c){
 assert(!critical&&!driver_live&&c->create_backround_task&&c->stack_size==4096&&c->task_priority==4);
 ++driver_installs;driver_config=*c;if(driver_error){return driver_error;}driver_live=true;return ESP_OK;}
esp_err_t msc_host_uninstall(void){assert(!critical&&driver_live&&!device_live&&!vfs_live);++driver_uninstalls;
 if(driver_off_error){return driver_off_error;}driver_live=false;return ESP_OK;}
esp_err_t msc_host_install_device(uint8_t address,msc_host_device_handle_t *out){
 assert(!critical&&driver_live&&!device_live&&address);++device_installs;
 if(remove_during_install){remove_during_install=false;msc_host_event_t e={.event=MSC_DEVICE_DISCONNECTED,.device.handle=DEVICE};driver_config.callback(&e,driver_config.callback_arg);}
 if(device_error){return device_error;}device_live=true;*out=DEVICE;return ESP_OK;}
esp_err_t msc_host_uninstall_device(msc_host_device_handle_t d){
 assert(!critical&&d==DEVICE&&device_live&&!vfs_live&&!s_storage.status.users);++device_uninstalls;
 if(device_off_error){return device_off_error;}device_live=false;return ESP_OK;}
esp_err_t msc_host_get_device_info(msc_host_device_handle_t d,msc_host_device_info_t *info){
 assert(!critical&&d==DEVICE&&device_live);*info=(msc_host_device_info_t){.sector_count=100000,.sector_size=512};return ESP_OK;}
esp_err_t msc_host_vfs_register(msc_host_device_handle_t d,const char *path,const esp_vfs_fat_mount_config_t *c,msc_host_vfs_handle_t *out){
 assert(lifecycle_held&&!critical&&d==DEVICE&&!vfs_live&&!strcmp(path,"/usb")&&!c->format_if_mount_failed&&c->max_files==16);++mounts;
 if(stop_during_mount){stop_during_mount=false;aether_usb_storage_class_request_stop(&s_storage);}
 if(mount_error){return mount_error;}vfs_live=true;*out=VFS;return ESP_OK;}
esp_err_t msc_host_vfs_unregister(msc_host_vfs_handle_t v){
 assert(lifecycle_held&&!critical&&v==VFS&&vfs_live&&!s_storage.status.users);++unmounts;
 if(unmount_error){return unmount_error;}vfs_live=false;return ESP_OK;}
static void connect_disk(uint8_t address){msc_host_event_t e={.event=MSC_DEVICE_CONNECTED,.device.address=address};driver_config.callback(&e,driver_config.callback_arg);}
static void disconnect_disk(msc_host_device_handle_t d){msc_host_event_t e={.event=MSC_DEVICE_DISCONNECTED,.device.handle=d};driver_config.callback(&e,driver_config.callback_arg);}
static aether_usb_storage_status_t snapshot(void){aether_usb_storage_status_t s;assert(aether_usb_storage_get_status(&s_storage,&s)==ESP_OK);return s;}
static void start(void){assert(aether_usb_storage_class_start(&s_storage)==ESP_OK);}
static void mount_disk(void){connect_disk(1);process_storage(&s_storage);assert(snapshot().mounted&&vfs_live);}
static void stop(void){aether_usb_storage_class_request_stop(&s_storage);assert(aether_usb_storage_class_poll_stopped(&s_storage)==ESP_ERR_NOT_FINISHED);process_storage(&s_storage);assert(aether_usb_storage_class_poll_stopped(&s_storage)==ESP_OK);}
int main(void){
 assert(aether_usb_storage_default()==&s_storage);assert(aether_usb_storage_class_poll_stopped(NULL)==ESP_OK);
 aether_usb_storage_lease_t a={0},b={0};assert(aether_usb_storage_acquire(&s_storage,&a)==ESP_ERR_INVALID_STATE);
 assert(!aether_usb_storage_is_available(&a)&&!task_creates&&!driver_installs);
 assert(aether_usb_storage_get_status(NULL,&(aether_usb_storage_status_t){0})==ESP_ERR_INVALID_ARG);
 puts("PASS lazy default/status/lease APIs perform no USB/task/power operations; invalid arguments and offline admission are safe");

 start();mount_disk();aether_usb_storage_status_t s=snapshot();assert(s.capacity_bytes==51200000&&s.sector_size==512);
 assert(aether_usb_storage_acquire(&s_storage,&a)==ESP_OK&&aether_usb_storage_is_available(&a));
 assert(aether_usb_storage_acquire(&s_storage,&b)==ESP_OK&&snapshot().users==2);
 FILE *f=tmpfile();assert(f);assert(fwrite("USB file payload",1,16,f)==16);rewind(f);char data[17]={0};assert(fread(data,1,16,f)==16&&!strcmp(data,"USB file payload"));
 connect_disk(2);assert(snapshot().ignored_devices==1);disconnect_disk((void*)0x9876);assert(aether_usb_storage_is_available(&a));
 disconnect_disk(DEVICE);assert(!aether_usb_storage_is_available(&a)&&snapshot().draining&&snapshot().mounted);
 const int before=unmounts;process_storage(&s_storage);assert(unmounts==before&&vfs_live&&device_live);
 aether_usb_storage_lease_t denied={0};assert(aether_usb_storage_acquire(&s_storage,&denied)==ESP_ERR_INVALID_STATE);
 assert(fclose(f)==0);aether_usb_storage_release(&a);process_storage(&s_storage);assert(unmounts==before&&snapshot().users==1);
 aether_usb_storage_release(&b);process_storage(&s_storage);assert(unmounts==before+1&&!vfs_live&&!device_live&&snapshot().driver_running);
 stop();assert(!driver_live&&snapshot().users==0);
 puts("PASS mounted real FILE read/write holds leases; detach invalidates immediately and waits for every close/release before VFS/device teardown");

 start();mount_disk();assert(aether_usb_storage_acquire(&s_storage,&a)==ESP_OK);
 aether_usb_storage_lease_t copied=a;const uint32_t old_generation=a.generation;
 aether_usb_storage_release(&a);assert(snapshot().users==0);
 assert(aether_usb_storage_acquire(&s_storage,&b)==ESP_OK);aether_usb_storage_release(&copied);
 assert(snapshot().users==1&&aether_usb_storage_is_available(&b));
 aether_usb_storage_class_request_stop(NULL);assert(!aether_usb_storage_is_available(&b)&&snapshot().generation!=old_generation);
 int base=driver_uninstalls;
 for(unsigned n=0;n<20;++n){assert(aether_usb_storage_class_poll_stopped(NULL)==ESP_ERR_NOT_FINISHED);process_storage(&s_storage);}
 assert(driver_uninstalls==base&&snapshot().users==1);
 aether_usb_storage_release(&b);process_storage(&s_storage);assert(aether_usb_storage_class_poll_stopped(NULL)==ESP_OK);
 puts("PASS generation/cookie leases reject stale copies; stop admission/draining/poll are nonblocking and never unmount an active user");

 start();mount_disk();aether_usb_storage_lease_t leases[LEASE_LIMIT+1]={0};
 for(unsigned n=0;n<LEASE_LIMIT;++n)assert(aether_usb_storage_acquire(&s_storage,&leases[n])==ESP_OK);
 assert(aether_usb_storage_acquire(&s_storage,&leases[LEASE_LIMIT])==ESP_ERR_NO_MEM);
 for(unsigned n=0;n<LEASE_LIMIT;++n){aether_usb_storage_release(&leases[n]);}
 stop();
 puts("PASS bounded multi-user lease table is exhausted safely and releases without underflow");

 start();remove_during_install=true;connect_disk(1);process_storage(&s_storage);
 assert(!snapshot().mounted&&!vfs_live);process_storage(&s_storage);assert(!device_live);stop();
 start();stop_during_mount=true;connect_disk(1);process_storage(&s_storage);
 assert(!snapshot().mounted&&snapshot().draining);process_storage(&s_storage);assert(aether_usb_storage_class_poll_stopped(NULL)==ESP_OK&&!vfs_live&&!device_live);
 puts("PASS detach during device initialization and OFF during VFS mount never publish a stale ready mount");

 driver_error=ESP_ERR_NO_MEM;assert(aether_usb_storage_class_start(NULL)==ESP_ERR_NO_MEM&&!driver_live);
 assert(aether_usb_storage_class_poll_stopped(NULL)==ESP_OK);driver_error=0;
 start();device_error=ESP_FAIL;connect_disk(1);process_storage(&s_storage);assert(snapshot().last_error==ESP_FAIL&&!snapshot().mounted);device_error=0;stop();
 start();mount_error=ESP_FAIL;connect_disk(1);process_storage(&s_storage);assert(device_live&&!vfs_live&&!snapshot().mounted&&snapshot().last_error==ESP_FAIL);
 mount_error=0;disconnect_disk(DEVICE);process_storage(&s_storage);assert(!device_live);stop();
 puts("PASS constructor/device/mount failures are explicit, do not autoformat media, and drain safely");

 start();mount_disk();assert(aether_usb_storage_acquire(&s_storage,&a)==ESP_OK);
 aether_usb_storage_class_request_stop(NULL);unmount_error=ESP_FAIL;
 process_storage(&s_storage);assert(snapshot().users==1&&vfs_live&&device_live);
 aether_usb_storage_release(&a);const int before_failed_unmount=unmounts;
 for(unsigned n=0;n<3;++n){process_storage(&s_storage);assert(snapshot().draining&&!snapshot().mounted&&vfs_live&&device_live&&driver_live&&!s_storage.cleanup_fatal);}
 assert(unmounts==before_failed_unmount+3&&aether_usb_storage_class_poll_stopped(NULL)==ESP_ERR_NOT_FINISHED);
 unmount_error=0;process_storage(&s_storage);
 assert(!vfs_live&&!device_live&&!driver_live&&aether_usb_storage_class_poll_stopped(NULL)==ESP_OK);
 start();mount_disk();stop();
 puts("PASS failed VFS unregister retains live VFS/device/class ownership, remains retryable after lease drain, and recovers without restart");

 start();lifecycle_error=ESP_ERR_TIMEOUT;const int install_before_gate=device_installs,mount_before_gate=mounts;
 connect_disk(1);
 for(unsigned n=0;n<3;++n){process_storage(&s_storage);assert(device_live&&!vfs_live&&!snapshot().mounted&&s_storage.pending_address==1&&mounts==mount_before_gate);}
 assert(device_installs==install_before_gate+1&&!lifecycle_held);
 lifecycle_error=0;process_storage(&s_storage);assert(snapshot().mounted&&mounts==mount_before_gate+1&&device_installs==install_before_gate+1);
 aether_usb_storage_class_request_stop(NULL);lifecycle_error=ESP_ERR_TIMEOUT;const int unmount_before_gate=unmounts;
 for(unsigned n=0;n<3;++n){process_storage(&s_storage);assert(vfs_live&&device_live&&driver_live&&!s_storage.cleanup_fatal&&unmounts==unmount_before_gate);}
 lifecycle_error=0;process_storage(&s_storage);assert(!vfs_live&&!device_live&&!driver_live&&!lifecycle_held);
 start();stop_during_lifecycle=true;connect_disk(1);const int mount_before_stop=mounts;
 process_storage(&s_storage);assert(device_live&&!vfs_live&&snapshot().draining&&mounts==mount_before_stop&&!lifecycle_held);
 process_storage(&s_storage);assert(!device_live&&!driver_live);
 puts("PASS shared filesystem registration lock gates SDK mount/unmount, retries contention without duplicate device installation, and rechecks OFF after waiting before publishing VFS");

 for(unsigned n=0;n<100;++n){start();mount_disk();stop();}
 assert(!driver_live&&!device_live&&!vfs_live&&snapshot().users==0&&task_creates==1);
 puts("PASS100 host disk sessions reuse one worker and release VFS/device/class ownership each time");

 start();mount_disk();device_off_error=ESP_FAIL;aether_usb_storage_class_request_stop(NULL);process_storage(&s_storage);
 assert(aether_usb_storage_class_poll_stopped(NULL)==ESP_FAIL&&driver_live);
 const int failed= device_uninstalls;
 for(unsigned n=0;n<10;++n)process_storage(&s_storage);
 assert(device_uninstalls==failed&&driver_live&&s_storage.cleanup_fatal);
 puts("PASS a partially failed official device teardown retains ownership and is never retried/freed unsafely");
 return 0;
}

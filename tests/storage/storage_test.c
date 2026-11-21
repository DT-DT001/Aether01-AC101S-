/* Exercise production mount publication, concurrent leases and power-off retry. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void test_free(void *);
#define free test_free
#include "../../components/aether_storage/aether_storage.c"
#undef free

static uint32_t ticks;
static bool detected, remove_during_mount, stop_during_mount;
static int off_failures, off_calls, unmounts, pin_parks;
static int mount_error;
static int lifecycle_error, unmount_error;
static bool lifecycle_held;
esp_err_t esp_vfs_fat_lifecycle_acquire(uint32_t timeout_ms) {
    assert(timeout_ms == 200 || timeout_ms == 20);
    if (lifecycle_error) return lifecycle_error;
    assert(!lifecycle_held); lifecycle_held = true; return ESP_OK;
}
void esp_vfs_fat_lifecycle_release(void) { assert(lifecycle_held); lifecycle_held = false; }
static unsigned mount_calls, worker_polls, worker_stop_after, worker_manual_retry_at, worker_remove_at, worker_reinsert_at;
static aether_storage_handle_t active;
static sdmmc_card_t card = {.csd = {.capacity = 1024, .sector_size = 512}};
static aether_board_t board = {.pmic = (void *)1};
static unsigned char reserved_dma[4096] __attribute__((aligned(64)));
static void *owned_dma;
static bool fail_dma, fail_task;
static bool pump_worker, in_pump;
static int raw_init_fail, raw_read_fail, raw_write_fail, raw_deinit_fail;
static int raw_initializations, raw_deinits;
static int card_initializations, last_card_clock, high_card_fail, all_card_fail, slot_fail, fail_above_clock;
static int attempted_clocks[8];
static bool unknown_real_clock;
static int on_failures, on_calls, off_fail_at_call;
static bool reject_dma_alignment;
static unsigned raw_reads, raw_writes, direct_reads, direct_writes;
static size_t max_direct_sectors, max_bounce_sectors;
static bool remove_after_transfer;
static unsigned char direct_buffer[32768] __attribute__((aligned(64)));
bool esp_ptr_dma_capable(const void *pointer)
{
    const uintptr_t at=(uintptr_t)pointer, first=(uintptr_t)direct_buffer;
    return at>=first && at<first+sizeof(direct_buffer);
}
bool test_sd_buffer_alignment(int slot,const void *pointer,size_t bytes)
{
    (void)slot;
    return !reject_dma_alignment && !((uintptr_t)pointer%64) && bytes && !(bytes%64);
}
static unsigned char disk[1024*512];
struct test_raw_mutex { bool locked; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1,sizeof(struct test_raw_mutex)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t ticks_wait) { (void)ticks_wait; if(!m || m->locked) return pdFALSE; m->locked=true; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { assert(m && m->locked); m->locked=false; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t m) { free(m); }
int test_sd_host_init(void) { ++raw_initializations; return raw_init_fail; }
int test_sd_host_deinit(void) { ++raw_deinits; return raw_deinit_fail; }
int test_sd_host_deinit_p(int slot) { (void)slot; return test_sd_host_deinit(); }
int sdmmc_host_init_slot(int slot,const sdmmc_slot_config_t *s) { (void)slot; assert(s->width==4 && s->clk==AETHER_GPIO_TF_CLK); return slot_fail; }
int sdmmc_card_init(const sdmmc_host_t *host,sdmmc_card_t *c) {
    assert(card_initializations<8); attempted_clocks[card_initializations++]=host->max_freq_khz; last_card_clock=host->max_freq_khz;
    c->csd=card.csd; c->host=*host; c->real_freq_khz=unknown_real_clock ? 0 : host->max_freq_khz==32000 ? 30769 : host->max_freq_khz;
    return all_card_fail || (fail_above_clock && host->max_freq_khz>fail_above_clock) || (host->max_freq_khz==SDMMC_FREQ_HIGHSPEED && high_card_fail) ? ESP_FAIL : ESP_OK;
}
int sdmmc_get_status(sdmmc_card_t *c) { assert(c && c!=&card); return detected ? ESP_OK : ESP_ERR_INVALID_STATE; }
static void record_transfer(sdmmc_card_t *c,const void *buffer,size_t lba,size_t count,bool writing)
{
    assert(c && c!=&card && count && lba+count<=1024);
    if(writing) ++raw_writes; else ++raw_reads;
    if(buffer==active->dma_buffer) {
        assert(count<=8); if(count>max_bounce_sectors) max_bounce_sectors=count;
    } else {
        assert(esp_ptr_dma_capable(buffer) && test_sd_buffer_alignment(c->host.slot,buffer,count*512) && count<=32);
        if(writing) ++direct_writes; else ++direct_reads;
        if(count>max_direct_sectors) max_direct_sectors=count;
    }
}
int sdmmc_read_sectors(sdmmc_card_t *c,void *dst,size_t lba,size_t count) {
    record_transfer(c,dst,lba,count,false);
    if(raw_read_fail) return raw_read_fail;
    memcpy(dst,disk+lba*512,count*512);
    if(remove_after_transfer) detected=false;
    return ESP_OK;
}
int sdmmc_write_sectors(sdmmc_card_t *c,const void *src,size_t lba,size_t count) {
    record_transfer(c,src,lba,count,true);
    if(raw_write_fail) return raw_write_fail;
    memcpy(disk+lba*512,src,count*512);
    if(remove_after_transfer) detected=false;
    return ESP_OK;
}
static unsigned dma_frees;
static void test_free(void *pointer)
{
    if (pointer && pointer == owned_dma) { ++dma_frees; owned_dma = NULL; }
    free(pointer);
}
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps)
{
    assert(alignment == 64 && bytes == 4096 && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    assert(!owned_dma);
    return owned_dma = fail_dma ? NULL : aligned_alloc(alignment, bytes);
}
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                       void *argument, unsigned priority, void *out)
{
    (void)entry; (void)name; (void)stack; (void)argument; (void)priority; (void)out;
    return fail_task ? 0 : pdPASS;
}

void vTaskDelete(void *task){(void)task;}
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay) {
    ticks += delay;
    if(worker_stop_after && delay==AETHER_STORAGE_POLL_MS){
        ++worker_polls;
        if(worker_manual_retry_at==worker_polls)assert(aether_storage_request_retry(active)==ESP_OK);
        if(worker_remove_at==worker_polls)detected=false;
        if(worker_reinsert_at==worker_polls)detected=true;
        if(worker_polls>=worker_stop_after)active->stop_requested=true;
    }
    if(pump_worker && !in_pump && active) {
        in_pump=true;
        if(!service_shutdown(active) && !service_usb(active)) {
            if(active->status.state==AETHER_STORAGE_REMOVING)
                (void)unmount_card(active);
            else if(!active->card && detected && !active->stop_requested)
                (void)mount_card(active);
        }
        in_pump=false;
    }
}
int gpio_get_level(int pin) { assert(pin == 6); return !detected; }
int gpio_config(const gpio_config_t *config) { (void)config; ++pin_parks; return ESP_OK; }
esp_err_t aether_board_set_sd_power(aether_board_t *target, bool enabled)
{
    assert(target == &board);
    if (enabled) { ++on_calls; if(on_failures) { --on_failures; return ESP_FAIL; } return ESP_OK; }
    ++off_calls;
    if (off_fail_at_call==off_calls) return ESP_FAIL;
    if (off_failures) { --off_failures; return ESP_FAIL; }
    return ESP_OK;
}
int esp_vfs_fat_sdmmc_mount(const char *path, const sdmmc_host_t *host,
                           const sdmmc_slot_config_t *slot,
                           const esp_vfs_fat_sdmmc_mount_config_t *config,
                           sdmmc_card_t **output)
{
    (void)path; (void)slot; (void)config;
    ++mount_calls;
    assert(lifecycle_held);
    assert(!config->format_if_mount_failed);
    if(mount_error){*output=NULL;return mount_error;}
    assert(host->max_freq_khz==SDMMC_FREQ_DEFAULT);
    assert(host->dma_aligned_buffer == active->dma_buffer && host->dma_aligned_buffer);
    assert(host->unaligned_multi_block_rw_max_chunk_size == 8 && !(host->flags & SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF));
    *output = &card;
    if (remove_during_mount) detected = false;
    if (stop_during_mount) active->stop_requested = true;
    return ESP_OK;
}
int esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *input)
{
    (void)path;
    assert(input == &card);
    ++unmounts;
    assert(lifecycle_held);
    return unmount_error;
}

static void reset(struct aether_storage_dev_t *handle)
{
    memset(handle, 0, sizeof(*handle));
    handle->board = &board;
    handle->dma_buffer = reserved_dma;
    static struct test_raw_mutex raw_mutex; raw_mutex.locked=false; handle->raw_lock=&raw_mutex;
    handle->status.card_inserted = true;
    active = handle;
    ticks = 0;
    card.csd.capacity=1024;card.csd.sector_size=512;
    mount_error=0;mount_calls=worker_polls=worker_stop_after=worker_manual_retry_at=worker_remove_at=worker_reinsert_at=0;
    assert(!lifecycle_held); lifecycle_error=unmount_error=0;
    memset(disk,0,sizeof(disk));
    detected = true;
    remove_during_mount = stop_during_mount = false;
    off_failures = off_calls = unmounts = pin_parks = 0;
    raw_init_fail=raw_read_fail=raw_write_fail=raw_deinit_fail=0;
    raw_initializations=raw_deinits=0;
    card_initializations=last_card_clock=high_card_fail=all_card_fail=slot_fail=fail_above_clock=0;
    memset(attempted_clocks,0,sizeof(attempted_clocks)); unknown_real_clock=false;
    on_failures=on_calls=off_fail_at_call=0;
    raw_reads=raw_writes=direct_reads=direct_writes=0;
    max_direct_sectors=max_bounce_sectors=0;
    remove_after_transfer=reject_dma_alignment=false;
    pump_worker=in_pump=false;
}

static void put16(uint8_t *p,uint16_t value){p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);}
static void put32(uint8_t *p,uint32_t value){put16(p,(uint16_t)value);put16(p+2,(uint16_t)(value>>16));}
static void fat32_boot(uint8_t *b,uint32_t sectors)
{
    memset(b,0,512);b[0]=0xeb;b[1]=0x58;b[2]=0x90;
    put16(b+11,512);b[13]=64;put16(b+14,32);b[16]=2;
    put32(b+32,sectors);put32(b+36,8192);put32(b+44,2);memcpy(b+82,"FAT32   ",8);put16(b+510,0xaa55);
}
static void test_format_diagnostics_and_retry(void)
{
    struct aether_storage_dev_t h;
    reset(&h);card.csd.capacity=67108864;fat32_boot(disk,card.csd.capacity);
    mount_error=ESP_FAIL;
    assert(mount_card(&h)==ESP_FAIL);
    aether_storage_diagnostics_t d;
    assert(aether_storage_get_diagnostics(&h,&d)==ESP_OK&&d.attempted&&d.bpb_valid);
    assert(d.capacity_bytes==UINT64_C(34359738368)&&d.filesystem==AETHER_STORAGE_FS_FAT32);
    assert(d.sector_bytes==512&&d.sectors_per_cluster==64&&!d.retry_suspended);
    assert(raw_reads==1&&raw_writes==0&&!h.raw_card&&!h.raw_host_initialized);
    assert(d.failure_count==1&&d.retry_delay_ms==2000);
    assert(mount_card(&h)==ESP_FAIL&&raw_reads==1); /* Probe only once per insertion. */
    assert(aether_storage_request_retry(&h)==ESP_OK&&mount_card(&h)==ESP_FAIL&&raw_reads==2);
    assert(aether_storage_usb_owned(&h)==false);
    puts("PASS failed32GiB FAT32 mount records read-only BPB/capacity, closes diagnostic host, never writes and only reprobes on explicit retry");

    reset(&h);mount_error=ESP_FAIL;worker_stop_after=120;storage_task(&h);
    assert(mount_calls==1&&h.diagnostics.retry_suspended&&raw_reads==1&&raw_writes==0);
    reset(&h);mount_error=ESP_FAIL;worker_stop_after=120;worker_manual_retry_at=40;storage_task(&h);
    assert(mount_calls==2&&raw_reads==2&&raw_writes==0);
    reset(&h);mount_error=ESP_FAIL;worker_stop_after=120;worker_remove_at=40;worker_reinsert_at=50;storage_task(&h);
    assert(mount_calls==2&&raw_reads==2&&raw_writes==0);
    puts("PASS invalid filesystem stops repeated power/mount churn until explicit retry or debounced removal/reinsert");

    reset(&h);card.csd.capacity=67108864;fat32_boot(disk,card.csd.capacity);
    mount_error=ESP_FAIL;worker_stop_after=120;storage_task(&h);
    assert(mount_calls==2&&!h.diagnostics.retry_suspended&&h.diagnostics.retry_delay_ms==4000);
    assert(mount_retry_delay_ms(1)==2000&&mount_retry_delay_ms(2)==4000&&mount_retry_delay_ms(3)==8000&&
        mount_retry_delay_ms(4)==16000&&mount_retry_delay_ms(5)==30000&&mount_retry_delay_ms(UINT32_MAX)==30000);
    reset(&h);card.csd.capacity=67108864;fat32_boot(disk,card.csd.capacity);
    mount_error=ESP_FAIL;worker_stop_after=200;storage_task(&h);
    assert(mount_calls==3&&h.diagnostics.retry_suspended&&h.diagnostics.retry_delay_ms==0&&raw_reads==1);
    reset(&h);card.csd.capacity=67108864;fat32_boot(disk,card.csd.capacity);
    mount_error=ESP_FAIL;worker_stop_after=200;worker_manual_retry_at=180;storage_task(&h);
    assert(mount_calls==4&&!h.diagnostics.retry_suspended&&h.diagnostics.failure_count==1&&raw_reads==2);
    puts("PASS transient/recognized-FAT failures back off2/4s and stop after3 failed mounts; explicit retry resets the budget without recurring idle power/initialization churn");

    reset(&h);mount_error=ESP_FAIL;raw_deinit_fail=ESP_FAIL;
    assert(mount_card(&h)==ESP_FAIL&&h.raw_card&&h.raw_host_initialized);
    assert(h.status.state==AETHER_STORAGE_REMOVING&&!h.status.mounted&&aether_storage_acquire(&h,0)==ESP_ERR_INVALID_STATE);
    raw_deinit_fail=ESP_OK;assert(service_usb(&h)&&!h.raw_card&&!h.raw_host_initialized);
    assert(h.diagnostics.retry_suspended);
    puts("PASS diagnostic cleanup failure retains raw host/card and gates local access until checked worker cleanup, preserving failure diagnosis");

    reset(&h);pump_worker=true;
    assert(aether_storage_usb_begin(&h,100)==ESP_OK);
    mount_error=ESP_FAIL;
    assert(aether_storage_usb_end(&h,300)==ESP_OK);
    assert(!aether_storage_usb_owned(&h)&&!h.raw_card&&!h.raw_host_initialized&&!h.status.mounted);
    assert(h.status.state==AETHER_STORAGE_ERROR&&h.status.last_error==ESP_FAIL&&h.diagnostics.retry_suspended);
    assert(aether_storage_acquire(&h,0)==ESP_ERR_INVALID_STATE);
    assert(aether_storage_request_retry(&h)==ESP_OK&&!h.diagnostics.retry_suspended);
    reset(&h);assert(mount_card(&h)==ESP_OK);pump_worker=true;
    assert(aether_storage_prepare_shutdown(&h,100)==ESP_OK);
    mount_error=ESP_FAIL;
    assert(aether_storage_cancel_shutdown(&h,300)==ESP_OK);
    assert(!h.shutdown_requested&&!h.shutdown_transition&&!h.status.mounted&&h.status.state==AETHER_STORAGE_ERROR);
    assert(h.status.last_error==ESP_FAIL&&h.diagnostics.retry_suspended&&!h.raw_host_initialized);
    puts("PASS failed filesystem restoration can release USB/shutdown ownership after exact host/lease/power cleanup while preserving local ERROR and explicit retry");
}

static void test_probe_shutdown_cleanup(void)
{
    struct aether_storage_dev_t h;
    reset(&h); mount_error=ESP_FAIL; raw_deinit_fail=ESP_FAIL;
    assert(mount_card(&h)==ESP_FAIL && h.raw_card && h.raw_host_initialized);
    sdmmc_card_t *const retained=h.raw_card;
    const int off_before=off_calls, parks_before=pin_parks, deinits_before=raw_deinits;
    assert(aether_storage_prepare_shutdown(&h,0)==ESP_ERR_TIMEOUT);
    /* The same mutex also protects any synchronous raw transfer. */
    assert(xSemaphoreTake(h.raw_lock,0)==pdTRUE);
    assert(service_shutdown(&h) && raw_deinits==deinits_before);
    assert(!h.shutdown_complete && off_calls==off_before && pin_parks==parks_before);
    xSemaphoreGive(h.raw_lock);
    pump_worker=true;
    assert(aether_storage_prepare_shutdown(&h,100)==ESP_ERR_TIMEOUT);
    assert(raw_deinits>deinits_before && h.raw_card==retained && h.raw_host_initialized);
    assert(h.shutdown_requested && !h.shutdown_complete && h.shutdown_error==ESP_OK);
    assert(h.status.state==AETHER_STORAGE_REMOVING && !h.status.mounted);
    assert(off_calls==off_before && pin_parks==parks_before && mount_calls==1 && !raw_writes);
    raw_deinit_fail=ESP_OK;
    assert(aether_storage_prepare_shutdown(&h,100)==ESP_OK && h.shutdown_complete);
    assert(!h.raw_card && !h.raw_host_initialized && !h.power_retry_pending);
    assert(off_calls>off_before && pin_parks>parks_before && mount_calls==1 && !raw_writes);
    puts("PASS shutdown retries a retained diagnostic host under raw mutex before pin parking/power-off, preserves ownership on deinit failure and completes after recovery within the existing caller budget");

    reset(&h); mount_error=ESP_FAIL; raw_deinit_fail=ESP_FAIL;
    assert(mount_card(&h)==ESP_FAIL && h.raw_card && h.raw_host_initialized);
    sdmmc_card_t *const cancelled_retained=h.raw_card;
    const int cancel_off=off_calls, cancel_parks=pin_parks, cancel_deinits=raw_deinits;
    assert(aether_storage_prepare_shutdown(&h,0)==ESP_ERR_TIMEOUT);
    assert(service_shutdown(&h) && !h.shutdown_complete);
    pump_worker=true;
    assert(aether_storage_cancel_shutdown(&h,100)==ESP_ERR_TIMEOUT);
    assert(!h.shutdown_requested && !h.shutdown_transition && !h.shutdown_complete);
    assert(h.raw_card==cancelled_retained && h.raw_host_initialized && raw_deinits>cancel_deinits);
    assert(off_calls==cancel_off && pin_parks==cancel_parks && mount_calls==1 && !raw_writes);
    assert(aether_storage_acquire(&h,0)==ESP_ERR_INVALID_STATE);
    raw_deinit_fail=ESP_OK;
    assert(aether_storage_cancel_shutdown(&h,100)==ESP_OK);
    assert(!h.raw_card && !h.raw_host_initialized && !h.power_retry_pending && !h.status.mounted);
    assert(h.status.state==AETHER_STORAGE_ERROR && h.diagnostics.failure_count==1 && h.diagnostics.retry_suspended);
    assert(off_calls>cancel_off && pin_parks>cancel_parks && mount_calls==1 && !raw_writes);
    puts("PASS cancelling shutdown preserves failed diagnostic-host ownership and blocks local remount until service_usb retries deinit and checked power cleanup succeeds");
}

int main(void)
{
    struct aether_storage_dev_t handle;
    reset(&handle);
    assert(mount_card(&handle) == ESP_OK && handle.status.mounted);
    assert(aether_storage_acquire(&handle, 0) == ESP_OK);
    assert(aether_storage_acquire(&handle, 0) == ESP_OK);
    handle.status.card_inserted = false;
    assert(!unmount_card(&handle));
    assert(!handle.status.mounted && !unmounts && !off_calls);
    assert(aether_storage_acquire(&handle, 0) == ESP_ERR_INVALID_STATE);
    aether_storage_release(&handle);
    assert(!unmount_card(&handle) && !unmounts);
    aether_storage_release(&handle);
    assert(unmount_card(&handle) && unmounts == 1 && off_calls == 1);
    assert(handle.status.state == AETHER_STORAGE_NO_CARD);
    puts("PASS concurrent lease drain and rejection during removal");

    reset(&handle);
    remove_during_mount = true;
    assert(mount_card(&handle) == ESP_ERR_INVALID_STATE);
    assert(!handle.status.mounted && !handle.card && unmounts == 1 && off_calls == 1);
    assert(aether_storage_acquire(&handle, 0) == ESP_ERR_INVALID_STATE);
    reset(&handle);
    stop_during_mount = true;
    assert(mount_card(&handle) == ESP_ERR_INVALID_STATE);
    assert(!handle.status.mounted && !handle.card && unmounts == 1 && off_calls == 1);
    puts("PASS removed/stopped during blocking mount cannot publish READY");

    reset(&handle);
    handle.status.card_inserted = false;
    off_failures = 1;
    ticks = UINT32_MAX - 1000U;
    assert(!unmount_card(&handle));
    assert(handle.status.state == AETHER_STORAGE_REMOVING && off_calls == 1);
    ticks += 1999U; /* Deliberately cross the 32-bit tick rollover. */
    assert(!unmount_card(&handle) && off_calls == 1);
    ++ticks;
    assert(unmount_card(&handle) && off_calls == 2);
    assert(handle.status.state == AETHER_STORAGE_NO_CARD && handle.status.last_error == ESP_OK);
    puts("PASS BLDO1-off failure retries with tick rollover");
    aether_storage_handle_t dynamic = (void *)1;
    fail_dma = true;
    assert(aether_storage_new(&board, &dynamic) == ESP_ERR_NO_MEM && !dynamic && !owned_dma);
    fail_dma = false; fail_task = true;
    assert(aether_storage_new(&board, &dynamic) == ESP_ERR_NO_MEM && !dynamic && !owned_dma && dma_frees == 1);
    fail_task = false;
    assert(aether_storage_new(&board, &dynamic) == ESP_OK && dynamic->dma_buffer == owned_dma);
    assert(aether_storage_generation(dynamic)==0 && aether_storage_generation(NULL)==0);
    assert(!((uintptr_t)owned_dma % 64));
    assert(aether_storage_del(dynamic) == ESP_ERR_TIMEOUT && owned_dma && dma_frees == 1);
    dynamic->task_running = false;
    assert(aether_storage_del(dynamic) == ESP_OK && !owned_dma && dma_frees == 2);
    puts("PASS 4KiB aligned DMA reservation precedes worker; OOM rollback and delayed teardown preserve ownership");
    reset(&handle); assert(mount_card(&handle)==ESP_OK);
    assert(aether_storage_acquire(&handle,0)==ESP_OK && handle.active_users==1);
    assert(aether_storage_gate_new_users(&handle,true)==ESP_OK);
    assert(aether_storage_card_inserted(&handle) && aether_storage_is_mounted(&handle));
    assert(handle.status.mounted && handle.card==&card && !off_calls && !unmounts);
    assert(aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE && handle.active_users==1);
    aether_storage_release(&handle); assert(handle.active_users==0);
    assert(aether_storage_gate_new_users(&handle,false)==ESP_OK);
    assert(aether_storage_acquire(&handle,0)==ESP_OK); aether_storage_release(&handle);
    handle.usb_requested=true;
    assert(aether_storage_gate_new_users(&handle,false)==ESP_OK && aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE);
    handle.usb_requested=false; handle.shutdown_requested=true;
    assert(aether_storage_gate_new_users(&handle,false)==ESP_OK && aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE);
    handle.shutdown_requested=false; handle.status.mounted=false;
    assert(aether_storage_gate_new_users(&handle,false)==ESP_OK && aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE);
    assert(aether_storage_gate_new_users(NULL,true)==ESP_ERR_INVALID_ARG);
    puts("PASS manual new-user gate preserves old holder/card/mount for flush-close; clearing cannot bypass USB/shutdown/unmounted guards");

    reset(&handle);
    assert(mount_card(&handle)==ESP_OK);
    assert(aether_storage_acquire(&handle,0)==ESP_OK);
    assert(aether_storage_usb_begin(&handle,0)==ESP_ERR_TIMEOUT);
    assert(aether_storage_usb_owned(&handle) && !aether_storage_card_inserted(&handle));
    assert(aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE);
    assert(service_usb(&handle) && !unmounts && !off_calls);
    assert(aether_storage_usb_end(&handle,0)==ESP_ERR_TIMEOUT);
    assert(service_usb(&handle) && handle.card==&card && handle.status.mounted && !off_calls);
    assert(!aether_storage_usb_owned(&handle));
    aether_storage_release(&handle);
    puts("PASS USB handover rejects new leases and cancellation cannot power off a live local mount");

    reset(&handle); assert(mount_card(&handle)==ESP_OK);
    assert(aether_storage_acquire(&handle,0)==ESP_OK);
    assert(aether_storage_usb_begin(&handle,0)==ESP_ERR_TIMEOUT);
    /* Emulate the exact debounced publication path while USB waits an old
     * FILE lease. Reinsert must never make its old FatFS/card object valid. */
    const uint32_t old_generation=aether_storage_generation(&handle);assert(old_generation);
    detected=false;publish_card_presence_locked(&handle,false);assert(service_usb(&handle));
    const uint32_t removed_generation=aether_storage_generation(&handle);assert(removed_generation && removed_generation!=old_generation);
    publish_card_presence_locked(&handle,false);assert(aether_storage_generation(&handle)==removed_generation);
    detected=true;publish_card_presence_locked(&handle,true);assert(service_usb(&handle));
    assert(aether_storage_generation(&handle)==removed_generation);
    assert(handle.local_mount_invalidated && !aether_storage_card_inserted(&handle));
    assert(aether_storage_usb_end(&handle,0)==ESP_ERR_TIMEOUT);assert(service_usb(&handle));
    assert(handle.card==&card && !handle.status.mounted && handle.status.state==AETHER_STORAGE_REMOVING);
    assert(!aether_storage_is_mounted(&handle) && !aether_storage_card_inserted(&handle));
    assert(aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE);
    assert(!unmount_card(&handle) && !unmounts && !off_calls);
    aether_storage_release(&handle);
    assert(unmount_card(&handle) && unmounts==1 && off_calls==1);
    assert(mount_card(&handle)==ESP_OK && !handle.local_mount_invalidated);
    assert(aether_storage_generation(&handle)!=removed_generation);
    assert(aether_storage_is_mounted(&handle) && aether_storage_card_inserted(&handle));
    assert(aether_storage_usb_end(&handle,0)==ESP_OK);
    assert(aether_storage_acquire(&handle,0)==ESP_OK);aether_storage_release(&handle);
    puts("PASS debounced card replacement during USB lease drain cannot revive an old mount on cancellation; fresh unmount/remount is required");

    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true;
    assert(aether_storage_usb_begin(&handle,200)==ESP_OK);
    assert(unmounts==1 && raw_initializations==1 && handle.card==NULL && handle.raw_card && handle.raw_card!=&card);
    assert(handle.status.usb_exported && !handle.status.mounted && aether_storage_usb_owned(&handle));
    for(size_t i=0;i<sizeof(disk);++i) disk[i]=(unsigned char)(i*17);
    unsigned char written[9000],readback[9000],before[20];
    memset(written,0xa7,sizeof(written)); memcpy(before,disk,sizeof(before));
    assert(aether_storage_usb_write(&handle,17,written,sizeof(written))==ESP_OK);
    assert(!memcmp(disk,before,17) && disk[9017]==(unsigned char)(9017*17));
    assert(aether_storage_usb_read(&handle,17,readback,sizeof(readback))==ESP_OK && !memcmp(written,readback,sizeof(written)));
    assert(aether_storage_usb_read(&handle,sizeof(disk)-3,readback,4)==ESP_ERR_INVALID_SIZE);
    assert(aether_storage_usb_read(&handle,UINT64_MAX,readback,1)==ESP_ERR_INVALID_SIZE);
    raw_read_fail=ESP_FAIL; assert(aether_storage_usb_read(&handle,0,readback,512)==ESP_FAIL); raw_read_fail=0;
    raw_write_fail=ESP_FAIL; assert(aether_storage_usb_write(&handle,0,written,512)==ESP_FAIL); raw_write_fail=0;
    assert(aether_storage_usb_sync(&handle)==ESP_OK);
    assert(aether_storage_prepare_shutdown(&handle,100)==ESP_ERR_INVALID_STATE);
    assert(aether_storage_usb_end(&handle,300)==ESP_OK && raw_deinits==1 && !aether_storage_usb_owned(&handle) && handle.status.mounted);
    puts("PASS fresh raw SD ownership, aligned DMA read/write, partial-sector preservation, bounds/errors and local remount");

    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true;
    assert(aether_storage_usb_begin(&handle,200)==ESP_OK);
    detected=false; handle.status.card_inserted=false;
    assert(aether_storage_usb_read(&handle,0,readback,512)==ESP_ERR_INVALID_STATE);
    service_usb(&handle);
    assert(!handle.raw_card && !handle.raw_host_initialized && !handle.status.usb_exported && aether_storage_usb_owned(&handle));
    detected=true; handle.status.card_inserted=true;
    service_usb(&handle); assert(raw_initializations==1 && !handle.status.mounted);
    assert(aether_storage_usb_end(&handle,300)==ESP_OK && handle.status.mounted);
    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; raw_init_fail=ESP_FAIL;
    assert(aether_storage_usb_begin(&handle,200)==ESP_FAIL && aether_storage_usb_owned(&handle) && !handle.raw_card);
    raw_init_fail=0; assert(aether_storage_usb_end(&handle,300)==ESP_OK && handle.status.mounted);
    puts("PASS raw USB removal/reinsert stays gated; initialization failure rolls back only on explicit exit");

    /* USB can use faster SD clocks without changing local FatFS's 20MHz. */
    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true;
    assert(aether_storage_usb_begin(&handle,500)==ESP_OK);
    assert(card_initializations==1 && attempted_clocks[0]==40000 && handle.status.usb_clock_khz==40000 && !handle.status.usb_high_speed_fallback);
    assert(aether_storage_usb_end(&handle,300)==ESP_OK && !handle.status.usb_clock_khz);
    const int success_clocks[]={32000,25000,20000};
    const unsigned expected_attempts[]={2,3,4};
    for(unsigned i=0;i<3;++i) {
        reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; fail_above_clock=success_clocks[i];
        assert(aether_storage_usb_begin(&handle,600)==ESP_OK);
        assert(card_initializations==(int)expected_attempts[i] && raw_initializations==card_initializations && raw_deinits==card_initializations-1);
        assert(last_card_clock==success_clocks[i] && handle.status.usb_high_speed_fallback);
        assert(handle.status.usb_clock_khz==(uint32_t)(success_clocks[i]==32000 ? 30769 : success_clocks[i]));
        assert(attempted_clocks[0]==40000 && attempted_clocks[1]==32000);
        if(i>0) assert(attempted_clocks[2]==25000);
        if(i>1) assert(attempted_clocks[3]==20000);
        assert(aether_storage_usb_end(&handle,300)==ESP_OK && handle.status.mounted && !handle.status.usb_high_speed_fallback);
    }
    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; unknown_real_clock=true;
    assert(aether_storage_usb_begin(&handle,500)==ESP_OK && handle.status.usb_clock_khz==0);
    assert(aether_storage_usb_end(&handle,300)==ESP_OK);
    puts("PASS USB40/32/25/20MHz initialization ladder, negotiated real clock and unknown clock; local FatFS stays20MHz");

    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; all_card_fail=true;
    assert(aether_storage_usb_begin(&handle,600)==ESP_FAIL && card_initializations==4 && raw_deinits==4);
    assert(!handle.raw_card && !handle.raw_host_initialized && aether_storage_usb_owned(&handle) && !handle.status.mounted);
    all_card_fail=false; assert(aether_storage_usb_end(&handle,300)==ESP_OK && handle.status.mounted);
    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; raw_init_fail=ESP_FAIL;
    assert(aether_storage_usb_begin(&handle,300)==ESP_FAIL && raw_initializations==1 && !card_initializations);
    raw_init_fail=0; assert(aether_storage_usb_end(&handle,300)==ESP_OK);
    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; slot_fail=ESP_FAIL;
    assert(aether_storage_usb_begin(&handle,300)==ESP_FAIL && raw_initializations==1 && !card_initializations && raw_deinits==1);
    slot_fail=0; assert(aether_storage_usb_end(&handle,300)==ESP_OK);
    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; on_failures=1;
    assert(aether_storage_usb_begin(&handle,300)==ESP_FAIL && !raw_initializations && !card_initializations);
    assert(aether_storage_usb_end(&handle,300)==ESP_OK);
    puts("PASS all card-clock attempts failing retain USB gate; power/host/slot failures never trigger frequency retries");

    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; high_card_fail=true; raw_deinit_fail=ESP_FAIL;
    assert(aether_storage_usb_begin(&handle,300)==ESP_FAIL && card_initializations==1 && raw_deinits==1);
    sdmmc_card_t *retained=handle.raw_card;
    assert(retained && handle.raw_host_initialized && aether_storage_usb_owned(&handle) && !handle.status.mounted);
    assert(raw_open(&handle)==ESP_ERR_INVALID_STATE && handle.raw_card==retained && raw_initializations==1);
    raw_deinit_fail=0; high_card_fail=false;
    assert(aether_storage_usb_end(&handle,300)==ESP_OK && !handle.raw_card && !handle.raw_host_initialized && handle.status.mounted);
    puts("PASS failed fallback teardown retains raw host/card without overwrite or remount until explicit recovery");

    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true; high_card_fail=true; off_fail_at_call=2;
    assert(aether_storage_usb_begin(&handle,300)==ESP_FAIL && card_initializations==1 && raw_deinits==1);
    assert(!handle.raw_card && !handle.raw_host_initialized && handle.power_retry_pending && aether_storage_usb_owned(&handle));
    assert(aether_storage_usb_end(&handle,100)==ESP_ERR_TIMEOUT && !handle.status.mounted);
    ticks+=2000; high_card_fail=false; off_fail_at_call=0;
    assert(aether_storage_usb_end(&handle,300)==ESP_OK && handle.status.mounted && !aether_storage_usb_owned(&handle));
    puts("PASS failed fallback power-off blocks lower-clock init and local remount until real power cleanup succeeds");

    reset(&handle); assert(mount_card(&handle)==ESP_OK); pump_worker=true;
    assert(aether_storage_usb_begin(&handle,300)==ESP_OK);
    for(size_t i=0;i<sizeof(direct_buffer);++i) direct_buffer[i]=(unsigned char)(i*37+9);
    raw_reads=raw_writes=direct_reads=direct_writes=0;
    assert(aether_storage_usb_write(&handle,512,direct_buffer,8192)==ESP_OK && raw_writes==1 && direct_writes==1);
    assert(!memcmp(disk+512,direct_buffer,8192));
    memset(direct_buffer,0,8192);
    assert(aether_storage_usb_read(&handle,512,direct_buffer,8192)==ESP_OK && raw_reads==1 && direct_reads==1 && !memcmp(disk+512,direct_buffer,8192));
    raw_reads=raw_writes=direct_reads=direct_writes=0;
    assert(aether_storage_usb_write(&handle,1024,direct_buffer,16384)==ESP_OK && raw_writes==1 && direct_writes==1);
    assert(aether_storage_usb_read(&handle,1024,direct_buffer,16384)==ESP_OK && raw_reads==1 && direct_reads==1 && max_direct_sectors==32);
    raw_reads=raw_writes=direct_reads=direct_writes=0;
    assert(aether_storage_usb_write(&handle,2048,direct_buffer,32768)==ESP_OK && raw_writes==2 && direct_writes==2);
    assert(aether_storage_usb_read(&handle,2048,direct_buffer,32768)==ESP_OK && raw_reads==2 && direct_reads==2 && !memcmp(disk+2048,direct_buffer,32768));
    raw_reads=raw_writes=direct_reads=direct_writes=0;
    assert(aether_storage_usb_write(&handle,0,direct_buffer+1,8192)==ESP_OK && raw_writes==2 && !direct_writes);
    assert(aether_storage_usb_read(&handle,0,direct_buffer+1,8192)==ESP_OK && raw_reads==2 && !direct_reads);
    reject_dma_alignment=true; raw_reads=raw_writes=direct_reads=direct_writes=0;
    assert(aether_storage_usb_write(&handle,0,direct_buffer,8192)==ESP_OK && raw_writes==2 && !direct_writes);
    assert(aether_storage_usb_read(&handle,0,direct_buffer,8192)==ESP_OK && raw_reads==2 && !direct_reads);
    reject_dma_alignment=false;
    unsigned reads_before=raw_reads,writes_before=raw_writes;
    assert(aether_storage_usb_write(&handle,19,direct_buffer,33)==ESP_OK && raw_reads==reads_before+1 && raw_writes==writes_before+1);
    assert(!memcmp(disk+19,direct_buffer,33) && max_bounce_sectors==8);
    puts("PASS aligned8/16KiB USB DMA uses one SD command,32KiB uses two; unaligned/capability/host-alignment/partial sectors retain4KiB bounce with equal data");
    const int init_count=card_initializations;
    raw_write_fail=ESP_FAIL; raw_writes=0;
    assert(aether_storage_usb_write(&handle,0,direct_buffer,16384)==ESP_FAIL && raw_writes==1 && card_initializations==init_count);
    raw_write_fail=0; raw_read_fail=ESP_FAIL; raw_reads=0;
    assert(aether_storage_usb_read(&handle,0,direct_buffer,16384)==ESP_FAIL && raw_reads==1 && card_initializations==init_count);
    raw_read_fail=0; remove_after_transfer=true; raw_writes=0;
    assert(aether_storage_usb_write(&handle,0,direct_buffer,32768)==ESP_ERR_INVALID_STATE && raw_writes==1 && card_initializations==init_count);
    assert(aether_storage_usb_owned(&handle) && !handle.status.mounted);
    remove_after_transfer=false; detected=true;
    assert(aether_storage_usb_end(&handle,300)==ESP_OK && handle.status.mounted);
    puts("PASS direct SD error/physical removal propagates immediately without hidden write retry, reclocking or local remount");

    reset(&handle); assert(mount_card(&handle)==ESP_OK);
    assert(aether_storage_acquire(&handle,0)==ESP_OK);
    assert(aether_storage_prepare_shutdown(&handle,0)==ESP_ERR_TIMEOUT);
    assert(!aether_storage_card_inserted(&handle) && aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE);
    assert(service_shutdown(&handle) && !unmounts && !off_calls);
    pump_worker=true;
    assert(aether_storage_cancel_shutdown(&handle,100)==ESP_OK && handle.status.mounted && !off_calls);
    aether_storage_release(&handle);
    off_failures=1;
    assert(aether_storage_prepare_shutdown(&handle,100)==ESP_ERR_TIMEOUT && handle.shutdown_requested && !handle.shutdown_complete);
    ticks+=2000;
    assert(aether_storage_prepare_shutdown(&handle,100)==ESP_OK && handle.shutdown_complete && !handle.card && !handle.power_retry_pending);
    assert(aether_storage_usb_begin(&handle,0)==ESP_ERR_INVALID_STATE);
    assert(aether_storage_cancel_shutdown(&handle,300)==ESP_OK && handle.status.mounted);
    puts("PASS shutdown waits real lease drain/unmount/power-off retry, blocks USB, and rollback restores local mount");

    reset(&handle); assert(mount_card(&handle)==ESP_OK);
    assert(aether_storage_acquire(&handle,0)==ESP_OK);
    assert(aether_storage_prepare_shutdown(&handle,0)==ESP_ERR_TIMEOUT);
    assert(service_shutdown(&handle) && !unmounts);
    detected=false;publish_card_presence_locked(&handle,false);assert(service_shutdown(&handle));
    detected=true;publish_card_presence_locked(&handle,true);assert(service_shutdown(&handle));
    assert(aether_storage_cancel_shutdown(&handle,0)==ESP_ERR_TIMEOUT);assert(service_shutdown(&handle));
    assert(handle.card==&card && !handle.status.mounted && handle.status.state==AETHER_STORAGE_REMOVING);
    assert(handle.local_mount_invalidated && !aether_storage_card_inserted(&handle));
    assert(aether_storage_acquire(&handle,0)==ESP_ERR_INVALID_STATE && !unmount_card(&handle));
    aether_storage_release(&handle);
    assert(unmount_card(&handle) && unmounts==1 && off_calls==1);
    assert(mount_card(&handle)==ESP_OK && !handle.local_mount_invalidated);
    assert(aether_storage_cancel_shutdown(&handle,0)==ESP_OK && aether_storage_is_mounted(&handle));
    puts("PASS shutdown cancellation after debounced card replacement preserves invalidation and rejects stale users until a new mount");

    reset(&handle);assert(aether_storage_generation(&handle)==0);
    publish_card_presence_locked(&handle,false);publish_card_presence_locked(&handle,true);
    assert(aether_storage_generation(&handle)==0); /* No old mount to invalidate. */
    handle.local_generation=UINT32_MAX;
    assert(mount_card(&handle)==ESP_OK && aether_storage_generation(&handle)==1);
    handle.local_generation=UINT32_MAX;
    detected=false;publish_card_presence_locked(&handle,false);
    assert(aether_storage_generation(&handle)==1);
    publish_card_presence_locked(&handle,false);detected=true;publish_card_presence_locked(&handle,true);
    assert(aether_storage_generation(&handle)==1);
    assert(unmount_card(&handle));assert(mount_card(&handle)==ESP_OK);
    const int before_getter_on=on_calls,before_getter_off=off_calls,before_getter_unmount=unmounts;
    assert(aether_storage_generation(&handle)==2 && aether_storage_generation(&handle)==2);
    assert(on_calls==before_getter_on && off_calls==before_getter_off && unmounts==before_getter_unmount);
    puts("PASS cache-only TF generation is0 before first mount, changes on invalidation/fresh mount, stays stable across absence/reinsert and skips0 on rollover");
    test_format_diagnostics_and_retry();
    test_probe_shutdown_cleanup();
    reset(&handle);
    lifecycle_error=ESP_ERR_TIMEOUT;
    assert(mount_card(&handle)==ESP_ERR_TIMEOUT && !mount_calls && !on_calls);
    assert(handle.diagnostics.failure_count==0 && !handle.diagnostics.retry_suspended);
    lifecycle_error=ESP_OK;assert(mount_card(&handle)==ESP_OK);
    const int old_off=off_calls, old_parks=pin_parks;
    ticks=123;
    lifecycle_error=ESP_ERR_TIMEOUT;
    assert(!unmount_card(&handle) && handle.card && !unmounts);
    assert(handle.drain_started==ticks && !handle.drain_warned);
    assert(off_calls==old_off && pin_parks==old_parks);
    lifecycle_error=ESP_OK;unmount_error=ESP_FAIL;
    assert(!unmount_card(&handle) && handle.card && unmounts==1);
    assert(off_calls==old_off && pin_parks==old_parks);
    ticks+=100;unmount_error=ESP_OK;
    assert(unmount_card(&handle) && !handle.card && unmounts==2);
    reset(&handle);remove_during_mount=true;unmount_error=ESP_FAIL;
    assert(mount_card(&handle)==ESP_FAIL && handle.card && handle.status.state==AETHER_STORAGE_REMOVING);
    assert(!off_calls && !pin_parks);
    unmount_error=ESP_OK;assert(unmount_card(&handle) && !handle.card);
    reset(&handle);assert(mount_card(&handle)==ESP_OK);
    unmount_error=ESP_FAIL;pump_worker=true;
    assert(aether_storage_prepare_shutdown(&handle,100)!=ESP_OK);
    assert(handle.card && handle.local_mount_invalidated && !handle.status.mounted);
    assert(aether_storage_cancel_shutdown(&handle,100)!=ESP_OK);
    assert(handle.card && !handle.status.mounted && handle.local_mount_invalidated);
    unmount_error=ESP_OK;
    assert(aether_storage_cancel_shutdown(&handle,500)==ESP_OK && handle.status.mounted);
    assert(!handle.local_mount_invalidated);pump_worker=false;
    puts("PASS shared lifecycle admission defers mounts without bad-card retries; failed rollback/unmount retains card/power until checked release");
    return 0;
}


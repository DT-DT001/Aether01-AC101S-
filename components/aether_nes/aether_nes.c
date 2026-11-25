#include "aether_nes_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define NES_TASK_STACK 12288U
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
#define NES_TASK_CORE 0
#else
/* Leave core 0 for LVGL and networking, as the video decoder does. */
#define NES_TASK_CORE 1
#endif

static const char *TAG = "aether_nes";
/* Nofrendo has global machine state and supports only one instance. */
static atomic_bool s_emulator_busy;

static void set_detail(aether_nes_handle_t handle, const char *detail)
{
    portENTER_CRITICAL(&handle->lock);
    strlcpy(handle->detail,detail,sizeof(handle->detail));
    portEXIT_CRITICAL(&handle->lock);
}

static void *alloc_media_memory(size_t size)
{
#if CONFIG_SPIRAM
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory) {
        return memory;
    }
    ESP_LOGW(TAG, "PSRAM allocation of %u bytes failed; trying internal RAM",
             (unsigned)size);
#endif
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static esp_err_t load_rom(aether_nes_handle_t handle)
{
    FILE *file = fopen(handle->path, "rb");
    if (!file) {
        set_detail(handle,"无法打开ROM，请检查TF卡和文件路径");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t result = ESP_OK;
    if (fseek(file, 0, SEEK_END) != 0) {
        set_detail(handle,"读取ROM长度失败");
        result = ESP_FAIL;
        goto done;
    }
    const long length = ftell(file);
    if (length < 16 || length > (long)AETHER_NES_ROM_MAX_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        set_detail(handle,"ROM长度无效：支持16字节至8MiB的iNES镜像");
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    handle->rom_data = alloc_media_memory((size_t)length);
    if (!handle->rom_data) {
        set_detail(handle,"ROM内存分配失败");
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    for (size_t offset = 0; offset < (size_t)length;) {
        if (atomic_load(&handle->stop_requested) ||
            !aether_media_fs_available(&handle->media_lease)) {
            set_detail(handle,"已停止，或TF卡已拔出");
            result = ESP_ERR_INVALID_STATE;
            goto done;
        }
        size_t bytes = (size_t)length - offset;
        if (bytes > 16384U) {
            bytes = 16384U;
        }
        if (fread(handle->rom_data + offset, 1, bytes, file) != bytes) {
            set_detail(handle,"ROM读取不完整，请检查TF卡");
            result = ESP_FAIL;
            goto done;
        }
        offset += bytes;
    }
    portENTER_CRITICAL(&handle->lock);
    handle->rom_size = (size_t)length;
    portEXIT_CRITICAL(&handle->lock);
    if (memcmp(handle->rom_data, "NES\x1A", 4) != 0) {
        set_detail(handle,"文件不是iNES格式，不能直接播放压缩包");
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    ESP_LOGI(TAG,"ROM header raw: bytes=%lu PRG16k=%u CHR8k=%u flags6=%02x flags7=%02x tv9=%02x tv10=%02x",
        (unsigned long)handle->rom_size,handle->rom_data[4],handle->rom_data[5],handle->rom_data[6],
        handle->rom_data[7],handle->rom_data[9],handle->rom_data[10]);
    if (!handle->rom_data[4] || (handle->rom_data[7] & 0x0CU) == 0x08U) {
        set_detail(handle,!handle->rom_data[4] ? "ROM缺少程序数据" : "NES 2.0镜像暂不支持，请使用NTSC iNES 1.0");
        /* This core consumes iNES 1.0 bank counts, not NES 2.0 extensions. */
        result = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    /* Exact historical signature only. The bundled core already repairs its
     * mapper high nibble; its ASCII byte9 must not be interpreted as PAL. */
    const bool diskdude = !memcmp(handle->rom_data + 7,"DiskDude!",9);
    if (!diskdude && ((handle->rom_data[9] & 0x01U) ||
        (handle->rom_data[10] & 0x03U) == 0x02U)) {
        set_detail(handle,"PAL专用时序暂不支持，请使用NTSC iNES 1.0");
        /* This build uses the NTSC 262-scanline CPU/PPU loop. Merely changing
         * the timer to 50Hz would not implement PAL's 312 scanlines/clocks. */
        result = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    const size_t expected = 16U +
                            (size_t)handle->rom_data[4] * 16U * 1024U +
                            (size_t)handle->rom_data[5] * 8U * 1024U +
                            ((handle->rom_data[6] & 0x04U) ? 512U : 0U);
    if (expected > handle->rom_size) {
        set_detail(handle,"ROM数据被截断，长度小于头部声明");
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    const unsigned mapper = (handle->rom_data[6] >> 4) | (diskdude ? 0U : handle->rom_data[7] & 0xF0U);
    portENTER_CRITICAL(&handle->lock);
    handle->mapper_number = (uint16_t)mapper;
    handle->prg_bytes = (uint32_t)handle->rom_data[4] * 16384U;
    handle->chr_bytes = (uint32_t)handle->rom_data[5] * 8192U;
    handle->repaired_diskdude = diskdude;
    portEXIT_CRITICAL(&handle->lock);
    ESP_LOGI(TAG,"ROM header: mapper=%u PRG=%lu CHR=%lu trainer=%u DiskDude=%u timing=NTSC",
        mapper,(unsigned long)handle->prg_bytes,(unsigned long)handle->chr_bytes,
        (handle->rom_data[6]&4U)!=0,diskdude);
    if (!aether_nes_port_mapper_supported(mapper)) {
        char detail[128]; snprintf(detail,sizeof(detail),"Mapper %u未包含在此核心中，无法运行此ROM",mapper);
        set_detail(handle,detail); result = ESP_ERR_NOT_SUPPORTED; goto done;
    }
    set_detail(handle,diskdude ? "NTSC iNES 1.0；已识别DiskDude脏头，按核心规则兼容" : "NTSC iNES 1.0；Mapper已包含，具体游戏仍需验证");

done:
    fclose(file);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,"ROM stage=header/read rejected: error=%s detail=%s",esp_err_to_name(result),handle->detail);
        free(handle->rom_data);
        handle->rom_data = NULL;
        portENTER_CRITICAL(&handle->lock);
        handle->rom_size = 0;
        portEXIT_CRITICAL(&handle->lock);
    }
    return result;
}

static void nes_task(void *argument)
{
    aether_nes_handle_t handle = argument;
    /* Retain a lease through Nofrendo shutdown: config and battery SRAM I/O
     * also touch the card after the initial ROM read. */
    bool storage_leased = false;
    esp_err_t result = aether_media_fs_acquire(handle->storage, handle->path, &handle->media_lease);
    if (result != ESP_OK) {
        set_detail(handle,"TF卡未就绪，无法获取ROM访问权");
        goto done;
    }
    storage_leased = true;
    result = load_rom(handle);
    if (result != ESP_OK) {
        goto done;
    }

    if (!handle->frame565) {
        handle->frame565 = alloc_media_memory(AETHER_NES_OUTPUT_WIDTH *
                                               AETHER_NES_OUTPUT_HEIGHT * 2U);
    }
    if (!handle->frame565) {
        set_detail(handle,"NES画面缓存内存不足");
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    if (atomic_load(&handle->stop_requested)) {
        goto done;
    }
    if (!atomic_load(&handle->screen_paused) && atomic_load(&handle->pause_request) != 1) {
        handle->audio_started = handle->audio && ac101s_audio_start(handle->audio, 65, 0.0f) == ESP_OK;
        if (!handle->audio_started) ESP_LOGW(TAG, "NES audio unavailable; emulator continues silently");
    }

    aether_nes_port_set_active(handle);
    portENTER_CRITICAL(&handle->lock);
    handle->state = AETHER_NES_RUNNING;
    portEXIT_CRITICAL(&handle->lock);
    if (aether_nes_port_run() != 0 && !atomic_load(&handle->stop_requested)) {
        char detail[128]; snprintf(detail,sizeof(detail),"核心启动失败（Mapper %u），请核对ROM和兼容性",handle->mapper_number);
        if (handle->persistence_error == ESP_OK) set_detail(handle,detail);
        result = ESP_FAIL;
    }
    if (handle->state == AETHER_NES_ERROR) result = handle->last_error;
    const esp_err_t port_stopped = aether_nes_port_shutdown();
    handle->port_cleanup_pending = port_stopped != ESP_OK;
    if (port_stopped != ESP_OK) { result = port_stopped; set_detail(handle, "NES定时器停止失败，等待清理后重试"); }

done:
    if (handle->audio_started) {
        const esp_err_t stopped_audio = ac101s_audio_stop(handle->audio);
        handle->audio_started = stopped_audio != ESP_OK;
        if (stopped_audio != ESP_OK) {
            result = stopped_audio;
            set_detail(handle, "音频停止失败，尚未交还音频访问权");
        }
    }
    free(handle->rom_data);
    handle->rom_data = NULL;
    atomic_store(&handle->external_input, 0);
    if (storage_leased) {
        aether_media_fs_release(&handle->media_lease);
    }
    const bool stopped = atomic_load(&handle->stop_requested);
    if (result != ESP_OK && !stopped) {
        ESP_LOGE(TAG, "emulator stopped: error=%s mapper=%u detail=%s",esp_err_to_name(result),handle->mapper_number,handle->detail);
    }
    uint16_t *release = NULL;
    if (!handle->audio_started && !handle->port_cleanup_pending)
        atomic_store(&s_emulator_busy, false);
    portENTER_CRITICAL(&handle->lock);
    handle->rom_size = 0;
    if (handle->release_frame_requested) {
        release = handle->frame565;
        handle->frame565 = NULL;
        handle->release_frame_requested = false;
    }
    if (result == ESP_OK && handle->persistence_error != ESP_OK) result = handle->persistence_error;
    if (result != ESP_OK && (!stopped || handle->state == AETHER_NES_ERROR || handle->audio_started || handle->port_cleanup_pending ||
                            handle->persistence_error != ESP_OK)) {
        handle->last_error = result;
        handle->state = AETHER_NES_ERROR;
    } else {
        handle->last_error = ESP_OK;
        handle->state = AETHER_NES_STOPPED;
    }
    /* Publish completion after the last raw-touch access, frame callback,
     * audio operation and filesystem close. No handle accesses after unlock. */
    handle->running = false;
    portEXIT_CRITICAL(&handle->lock);
    free(release);
    vTaskDelete(NULL);
}

esp_err_t aether_nes_new(aether_storage_handle_t storage,
                         ac101s_audio_handle_t audio,
                         esp_lcd_touch_handle_t touch,
                         aether_nes_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(storage && touch && out_handle, ESP_ERR_INVALID_ARG,
                        TAG, "invalid argument");
    aether_nes_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    handle->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    atomic_init(&handle->stop_requested, false);
    atomic_init(&handle->paused, false);
    atomic_init(&handle->screen_paused, false);
    atomic_init(&handle->pause_request, -1);
    atomic_init(&handle->external_input, 0);
    handle->storage = storage;
    handle->audio = audio;
    handle->touch = touch;
    handle->state = AETHER_NES_STOPPED;
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t aether_nes_start(aether_nes_handle_t handle, const char *rom_path,
                           aether_nes_frame_cb_t frame_cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(handle && rom_path && rom_path[0] && frame_cb,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(strlen(rom_path) < sizeof(handle->path),
                        ESP_ERR_INVALID_SIZE, TAG, "path too long");
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_emulator_busy, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&handle->lock);
    if (handle->running || handle->stop_barrier || handle->audio_started || handle->port_cleanup_pending) {
        portEXIT_CRITICAL(&handle->lock);
        atomic_store(&s_emulator_busy, false);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(handle->path, rom_path, sizeof(handle->path));
    handle->frame_cb = frame_cb;
    handle->user_ctx = user_ctx;
    handle->stop_requested = false;
    handle->paused = false;
    atomic_store(&handle->external_input, 0);
    atomic_store(&handle->pause_request, -1);
    handle->frame_number = 0;
    handle->emulated_frames = 0;
    handle->skipped_frames = 0;
    handle->audio_errors = 0;
    handle->input_errors = 0;
    handle->emulation_fps_x100 = 0;
    handle->render_fps_x100 = 0;
    handle->mapper_number = 0; handle->prg_bytes = handle->chr_bytes = 0;
    handle->repaired_diskdude = false;
    strlcpy(handle->detail,"正在加载ROM并检查兼容性",sizeof(handle->detail));
    handle->core_us_per_frame = handle->apu_us_per_frame = handle->audio_output_us_per_frame = 0;
    handle->video_convert_us_per_frame = handle->frame_callback_us_per_frame = 0;
    handle->input_us_per_frame = handle->yield_us_per_frame = 0;
    handle->last_error = ESP_OK;
    handle->persistence_error = ESP_OK;
    handle->release_frame_requested = false;
    handle->state = AETHER_NES_STARTING;
    handle->running = true;
    portEXIT_CRITICAL(&handle->lock);
    if (xTaskCreatePinnedToCore(nes_task, "nes", NES_TASK_STACK, handle, 7, NULL, NES_TASK_CORE) != pdPASS) {
        portENTER_CRITICAL(&handle->lock);
        handle->last_error = ESP_ERR_NO_MEM;
        handle->state = AETHER_NES_ERROR;
        handle->running = false;
        portEXIT_CRITICAL(&handle->lock);
        atomic_store(&s_emulator_busy, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t aether_nes_stop(aether_nes_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    atomic_store(&handle->stop_requested, true);
    atomic_store(&handle->external_input, 0);
    return ESP_OK;
}

esp_err_t aether_nes_set_external_input(aether_nes_handle_t handle, uint16_t buttons)
{
    if (!handle || (buttons & ~0x7ffU)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&handle->lock);
    const bool allowed = !buttons || (handle->running && !handle->stop_barrier &&
        !atomic_load(&handle->stop_requested));
    if (allowed) atomic_store(&handle->external_input, buttons);
    portEXIT_CRITICAL(&handle->lock);
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t aether_nes_set_input_gate(aether_nes_handle_t handle, bool (*gate)(void *), void *context)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&handle->lock);
    if (handle->running) { portEXIT_CRITICAL(&handle->lock); return ESP_ERR_INVALID_STATE; }
    handle->input_gate = gate;
    handle->input_gate_context = context;
    portEXIT_CRITICAL(&handle->lock);
    return ESP_OK;
}

esp_err_t aether_nes_stop_wait(aether_nes_handle_t handle, uint32_t timeout_ms)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&handle->lock);
    if (handle->stop_barrier) { portEXIT_CRITICAL(&handle->lock); return ESP_ERR_INVALID_STATE; }
    handle->stop_barrier = true;
    portEXIT_CRITICAL(&handle->lock);
    aether_nes_stop(handle);
    const TickType_t start = xTaskGetTickCount(), timeout = pdMS_TO_TICKS(timeout_ms);
    esp_err_t error = ESP_OK;
    for (;;) {
        portENTER_CRITICAL(&handle->lock);
        const bool running = handle->running;
        const bool audio_pending = !running && handle->audio_started;
        const bool port_pending = !running && handle->port_cleanup_pending;
        portEXIT_CRITICAL(&handle->lock);
        if (!running) {
            if ((audio_pending || port_pending) && !timeout_ms) { error = ESP_ERR_TIMEOUT; break; }
            if (audio_pending && (TickType_t)(xTaskGetTickCount() - start) + pdMS_TO_TICKS(1000) > timeout) {
                error = ESP_ERR_TIMEOUT; break;
            }
            if (port_pending) {
                error = aether_nes_port_shutdown();
                portENTER_CRITICAL(&handle->lock);
                if (error == ESP_OK) handle->port_cleanup_pending = false;
                else handle->last_error = error;
                portEXIT_CRITICAL(&handle->lock);
            }
            /* No worker can still access the codec. A prior stop failure keeps
             * this ownership bit set until a subsequent barrier really closes. */
            if (audio_pending) {
                const esp_err_t audio_error = ac101s_audio_stop(handle->audio);
                portENTER_CRITICAL(&handle->lock);
                if (audio_error == ESP_OK) handle->audio_started = false;
                else handle->last_error = audio_error;
                portEXIT_CRITICAL(&handle->lock);
                if (audio_error != ESP_OK) error = audio_error;
            }
            if ((audio_pending || port_pending) && error == ESP_OK)
                atomic_store(&s_emulator_busy, false);
            break;
        }
        if (xTaskGetTickCount() - start >= timeout) { error = ESP_ERR_TIMEOUT; break; }
        vTaskDelay(1);
    }
    portENTER_CRITICAL(&handle->lock); handle->stop_barrier = false; portEXIT_CRITICAL(&handle->lock);
    return error;
}

void aether_nes_release_frame(aether_nes_handle_t handle)
{
    if (!handle) {
        return;
    }
    uint16_t *release = NULL;
    portENTER_CRITICAL(&handle->lock);
    if (handle->running) {
        handle->release_frame_requested = true;
    } else {
        release = handle->frame565;
        handle->frame565 = NULL;
    }
    portEXIT_CRITICAL(&handle->lock);
    free(release);
}

esp_err_t aether_nes_get_status(aether_nes_handle_t handle,
                                aether_nes_status_t *status)
{
    ESP_RETURN_ON_FALSE(handle && status, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    portENTER_CRITICAL(&handle->lock);
    status->state = handle->state;
    status->last_error = handle->last_error;
    status->external_buttons = (uint16_t)atomic_load(&handle->external_input);
    status->frame_number = handle->frame_number;
    status->emulated_frames = handle->emulated_frames;
    status->skipped_frames = handle->skipped_frames;
    status->audio_errors = handle->audio_errors;
    status->input_errors = handle->input_errors;
    status->emulation_fps_x100 = handle->state == AETHER_NES_RUNNING ? handle->emulation_fps_x100 : 0;
    status->render_fps_x100 = handle->state == AETHER_NES_RUNNING ? handle->render_fps_x100 : 0;
    status->mapper_number = handle->mapper_number; status->prg_bytes = handle->prg_bytes; status->chr_bytes = handle->chr_bytes;
    status->repaired_diskdude = handle->repaired_diskdude;
    strlcpy(status->detail,handle->detail,sizeof(status->detail));
    status->core_us_per_frame = handle->core_us_per_frame; status->apu_us_per_frame = handle->apu_us_per_frame;
    status->audio_output_us_per_frame = handle->audio_output_us_per_frame;
    status->video_convert_us_per_frame = handle->video_convert_us_per_frame;
    status->frame_callback_us_per_frame = handle->frame_callback_us_per_frame;
    status->input_us_per_frame = handle->input_us_per_frame; status->yield_us_per_frame = handle->yield_us_per_frame;
    status->rom_size = handle->rom_size;
    strlcpy(status->path, handle->path, sizeof(status->path));
    portEXIT_CRITICAL(&handle->lock);
    return ESP_OK;
}

esp_err_t aether_nes_del(aether_nes_handle_t handle)
{
    ESP_RETURN_ON_ERROR(aether_nes_stop_wait(handle, 3000), TAG,
                        "NES task did not stop");
    aether_nes_release_frame(handle);
    free(handle);
    return ESP_OK;
}

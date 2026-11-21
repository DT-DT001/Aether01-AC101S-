/* Run the real worker's bounded ROM loader against real temporary files. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs/runtime.h"
#include "../../components/aether_nes/aether_nes.c"

static bool inserted = true, allocation_failure;
size_t strlcpy(char *out,const char *in,size_t capacity)
{size_t n=strlen(in);if(capacity){size_t copy=n<capacity-1?n:capacity-1;memcpy(out,in,copy);out[copy]=0;}return n;}
bool aether_nes_port_mapper_supported(unsigned mapper) { return mapper == 0 || mapper == 2 || mapper == 4; }
void *heap_caps_malloc(size_t bytes, unsigned caps)
{ (void)caps; return allocation_failure ? NULL : malloc(bytes); }
bool aether_storage_card_inserted(aether_storage_handle_t storage)
{ (void)storage; return inserted; }

static void check(const char *path, const uint8_t *data, size_t length,
                  esp_err_t expected, bool stop)
{
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(data, 1, length, file) == length);
    assert(!fclose(file));
    struct aether_nes_t handle = {.media_lease = {.tf=(void*)1,.held=true}};
    snprintf(handle.path, sizeof(handle.path), "%s", path);
    atomic_init(&handle.stop_requested, stop);
    assert(load_rom(&handle) == expected);
    if (expected == ESP_OK) {
        assert(handle.rom_data && handle.rom_size == length);
        assert(!memcmp(handle.rom_data, data, length));
        free(handle.rom_data);
    } else {
        assert(!handle.rom_data && !handle.rom_size);
        assert(handle.detail[0]);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char path[192];
    snprintf(path, sizeof(path), "%s/header.nes", argv[1]);
    uint8_t *data = calloc(1, 16 + 16384 + 512);
    assert(data);
    memcpy(data, "NES\x1a", 4);
    data[4] = 1;
    check(path, data, 16 + 16384, ESP_OK, false);
    memcpy(data+7,"DiskDude!",9);
    check(path,data,16+16384,ESP_OK,false);
    data[15]='?'; /* Near-match must never bypass PAL/NES2 checks. */
    check(path,data,16+16384,ESP_ERR_NOT_SUPPORTED,false);
    memset(data+7,0,9);
    data[6]=0xF0;data[7]=0xF0;
    check(path,data,16+16384,ESP_ERR_NOT_SUPPORTED,false);
    data[6]=data[7]=0;
    data[6] = 4; /* Trainer length participates in the size check. */
    check(path, data, 16 + 16384, ESP_ERR_INVALID_SIZE, false);
    check(path, data, 16 + 16384 + 512, ESP_OK, false);
    data[6] = 0;
    data[7] = 8;
    check(path, data, 16 + 16384, ESP_ERR_NOT_SUPPORTED, false);
    data[7] = 0;
    data[9] = 1;
    check(path, data, 16 + 16384, ESP_ERR_NOT_SUPPORTED, false);
    data[9] = 0;
    data[10] = 2;
    check(path, data, 16 + 16384, ESP_ERR_NOT_SUPPORTED, false);
    data[10] = 3; /* Dual-system image may run using the NTSC timing. */
    check(path, data, 16 + 16384, ESP_OK, false);
    data[10] = 0;
    data[4] = 0;
    check(path, data, 16 + 16384, ESP_ERR_NOT_SUPPORTED, false);
    data[4] = 1;
    check(path, data, 16, ESP_ERR_INVALID_SIZE, false);
    data[0] = 'X';
    check(path, data, 16 + 16384, ESP_ERR_INVALID_RESPONSE, false);
    data[0] = 'N';
    check(path, data, 16 + 16384, ESP_ERR_INVALID_STATE, true);
    inserted = false;
    check(path, data, 16 + 16384, ESP_ERR_INVALID_STATE, false);
    inserted = true;
    allocation_failure = true;
    check(path, data, 16 + 16384, ESP_ERR_NO_MEM, false);
    free(data);
    assert(!remove(path));
    puts("NES production ROM loader: NTSC/PAL/NES2/trainer/corruption/stop/unplug/allocation passed");
    return 0;
}

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <noftypes.h>
#include <bitmap.h>
#include <nes.h>
#include <nes_rom.h>
#include <nes_ppu.h>

void *__real_malloc(size_t bytes);
void __real_free(void *pointer);
static struct { void *pointer; bool live; } allocations[32];
static unsigned calls, fail_at, live;
static unsigned persistence_errors;
static unsigned char rom[16 + 16384];

void *__wrap_malloc(size_t bytes)
{
    ++calls;
    if (calls == fail_at) return NULL;
    assert(calls < 32);
    void *pointer = __real_malloc(bytes);
    assert(pointer);
    allocations[calls].pointer = pointer;
    allocations[calls].live = true;
    ++live;
    return pointer;
}

void __wrap_free(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 1; i < 32; ++i) {
        if (allocations[i].pointer == pointer && allocations[i].live) {
            allocations[i].live = false;
            --live;
            __real_free(pointer);
            return;
        }
    }
    assert(!"invalid or duplicate free");
}

static void reset(unsigned fail)
{
    assert(live == 0);
    calls = 0;
    fail_at = fail;
    memset(allocations, 0, sizeof(allocations));
}

char *osd_getromdata(void) { return (char *)rom; }
int osd_storage_available(void) { return 1; }
void osd_persistence_error(const char *detail) { assert(detail && *detail); ++persistence_errors; }
void osd_fullname(char *out, const char *path) { snprintf(out, PATH_MAX, "%s", path); }
char *osd_newextension(char *path, char *extension)
{
    char *dot = strrchr(path, '.');
    assert(dot);
    strcpy(dot, extension);
    return path;
}
bool mmc_peek(int mapper) { return mapper == 0; }
void gui_sendmsg(int color, char *format, ...) { (void)color; (void)format; }
int log_printf(const char *format, ...) { (void)format; return 0; }
nes_t *nes_getcontextptr(void) { static nes_t machine; return &machine; }
void ppu_setdefaultpal(ppu_t *ppu) { (void)ppu; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    char path[PATH_MAX], save_path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/game.nes", argv[1]) > 0);
    assert(snprintf(save_path, sizeof(save_path), "%s/game.sav", argv[1]) > 0);
    memcpy(rom, "NES\x1a", 4);
    rom[4] = 1;
    rom[6] = 2;
    FILE *save = fopen(save_path, "wb");
    assert(save);
    for (unsigned i = 0; i < 8192; ++i) fputc(0x3c, save);
    assert(!fclose(save));
    for (unsigned failure = 1; failure <= 3; ++failure) {
        reset(failure);
        assert(rom_load(path) == NULL);
        assert(live == 0);
        save = fopen(save_path, "rb");
        assert(save && fgetc(save) == 0x3c);
        assert(!fseek(save, 8191, SEEK_SET) && fgetc(save) == 0x3c);
        fclose(save);
    }
    reset(0);
    rominfo_t *info = rom_load(path);
    assert(info && !strcmp(info->filename, path));
    assert(info->rom == rom + 16 && info->sram && info->vram);
    assert(info->sram[0] == 0x3c && info->sram[8191] == 0x3c);
    memset(info->sram, 0x5a, 8192);
    rom_free(&info);
    assert(!info && !live);
    save = fopen(save_path, "rb");
    assert(save);
    for (unsigned i = 0; i < 8192; ++i) assert(fgetc(save) == 0x5a);
    assert(fgetc(save) == EOF);
    fclose(save);
    reset(0);
    info = rom_load(path);
    assert(info && info->sram[0] == 0x5a && info->sram[8191] == 0x5a);
    rom_free(&info);
    assert(!info && !live);
    assert(!persistence_errors);
    save=fopen(save_path,"wb");assert(save&&fwrite("short",1,5,save)==5&&!fclose(save));
    reset(0);info=rom_load(path);assert(info&&!(info->flags&ROM_FLAG_BATTERY)&&persistence_errors==1);
    rom_free(&info);assert(!info&&!live);
    save=fopen(save_path,"rb");assert(save);char partial[6]={0};assert(fread(partial,1,6,save)==5&&!strcmp(partial,"short"));assert(!fclose(save));
    for (unsigned failure = 1; failure <= 2; ++failure) {
        reset(failure);
        assert(bmp_create(256, 224, 8) == NULL);
        assert(!live);
    }
    reset(0);
    bitmap_t *bitmap = bmp_create(256, 224, 8);
    assert(bitmap && bitmap->line[1] == bitmap->line[0] + bitmap->pitch);
    bmp_clear(bitmap, 0x1f);
    assert(bitmap->line[223][255] == 0x1f);
    bmp_destroy(&bitmap);
    assert(!bitmap && !live);
    puts("NES ROM/bitmap: 4 groups passed (allocation rollback, filename/SRAM persistence, reload, bitmap rollback/guards)");
    return 0;
}

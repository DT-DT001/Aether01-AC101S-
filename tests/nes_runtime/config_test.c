/* A corrupt removable-media configuration must never corrupt emulator memory,
 * leak partial state or replace the original with partially parsed data. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/aether_nes/vendor/nofrendo/config.c"

static unsigned allocation, fail_at;
void *__real_malloc(size_t bytes);
void *__wrap_malloc(size_t bytes)
{
    if (++allocation == fail_at) return NULL;
    return __real_malloc(bytes);
}
int log_printf(const char *format, ...) { (void)format; return 0; }
int osd_storage_available(void) { return 1; }
void osd_persistence_error(const char *detail) { (void)detail; assert(!"unexpected save error"); }

static void write_file(const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, length, file) == length && !fclose(file));
}
static void unchanged(const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    for (size_t i = 0; i < length; ++i) assert(fgetc(file) == ((const unsigned char *)bytes)[i]);
    assert(fgetc(file) == EOF && !fclose(file));
}
static void reject(const char *path, const void *bytes, size_t length)
{
    write_file(path, bytes, length);
    assert(config.open()); /* Existing API uses false for success. */
    assert(myVars == NULL && !mySaveNeeded);
    config.close();
    unchanged(path, bytes, length);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char path[256];
    snprintf(path, sizeof(path), "%s/nofrendo-test.cfg", argv[1]);
    config.filename = path;
    const char valid[] = "[NES]\r\nanswer=42\nname=\xe4\xb8\xad\xe6\x96\x87\nlast=tail";
    write_file(path, valid, sizeof(valid) - 1);
    for (unsigned i = 0; i < 100; ++i) {
        assert(!config.open());
        assert(config.read_int("nes", "answer", 0) == 42);
        assert(!strcmp(config.read_string("NES", "last", NULL), "tail"));
        assert(!strcmp(config.read_string("NES", "name", NULL), "\xe4\xb8\xad\xe6\x96\x87"));
        config.close();
        assert(!myVars);
    }
    unchanged(path, valid, sizeof(valid) - 1);
    assert(!config.open());config.write_int("nes","answer",43);config.close();
    assert(!config.open() && config.read_int("nes","answer",0)==43);config.close();
    write_file(path,valid,sizeof(valid)-1);
    const char nul_line[] = "[NES]\na=1\n\0corrupt\n";
    reject(path, nul_line, sizeof(nul_line) - 1);
    char long_line[CONFIG_MAX_LINE + 2];
    memset(long_line, 'x', sizeof(long_line));
    long_line[1] = '=';
    reject(path, long_line, sizeof(long_line));
    FILE *file = fopen(path, "wb");
    assert(file);
    for (unsigned i = 0; i <= CONFIG_MAX_VARIABLES; ++i) fprintf(file, "key%03u=%u\n", i, i);
    assert(!fclose(file));
    assert(config.open() && !myVars && !mySaveNeeded);
    config.close();
    file = fopen(path, "wb");
    assert(file);
    for (unsigned i = 0; i < 70U * 1024U / 16U; ++i) fputs("#12345678901234\n", file);
    assert(!fclose(file));
    assert(config.open() && !myVars && !mySaveNeeded);
    config.close();
    /* Inject each allocation failure through all parse/tree construction steps. */
    write_file(path, valid, sizeof(valid) - 1);
    for (unsigned i = 1; i <= 18; ++i) {
        allocation = 0;
        fail_at = i;
        (void)config.open();
        fail_at = 0;
        config.close();
        assert(!myVars && !mySaveNeeded);
        unchanged(path, valid, sizeof(valid) - 1);
    }
    assert(!remove(path));
    puts("NES production config: UTF-8/EOF/relaunch/NUL/long-line/tree-limit/allocation failures passed");
    return 0;
}

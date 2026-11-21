#include "mjpeg_stream.h"
#include <assert.h>
#include <string.h>

static unsigned yields;
static bool cancel;
static bool cancelled(void *context) { (void)context; return cancel; }
static void yield(void *context) { (void)context; ++yields; }

static FILE *stream(const uint8_t *data, size_t size)
{
    FILE *file = tmpfile();
    assert(file && fwrite(data, 1, size, file) == size && !fseek(file, 0, SEEK_SET));
    return file;
}

static void put32(FILE *f, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) fputc((value >> (i * 8)) & 255, f);
}
static long begin_chunk(FILE *f, const char id[4])
{
    long start = ftell(f); assert(start >= 0);
    assert(fwrite(id, 1, 4, f) == 4); put32(f, 0); return start;
}
static void end_chunk(FILE *f, long start)
{
    long end = ftell(f); assert(end >= start + 8);
    uint32_t size = (uint32_t)(end - start - 8);
    assert(!fseek(f, start + 4, SEEK_SET)); put32(f, size);
    assert(!fseek(f, end, SEEK_SET)); if (size & 1) fputc(0, f);
}
static void chunk(FILE *f, const char id[4], const uint8_t *data, size_t size)
{
    long start = begin_chunk(f, id); assert(fwrite(data, 1, size, f) == size); end_chunk(f, start);
}

int main(void)
{
    uint8_t block[AETHER_MJPEG_READ_BYTES], output[128];
    /* APP1 embeds EOI and SOI. Entropy embeds a stuffed FF and restart marker. */
    const uint8_t jpeg[] = {0xff,0xd8, 0xff,0xe1,0,8, 0xff,0xd9,0xff,0xd8,1,2,
                            0xff,0xda,0,2, 1,0xff,0,0xd9, 0xff,0xd0,2, 0xff,0xd9};
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite("AVI", 1, 3, file) == 3);
    assert(fwrite(jpeg, 1, sizeof(jpeg), file) == sizeof(jpeg));
    assert(fwrite(jpeg, 1, sizeof(jpeg), file) == sizeof(jpeg));
    rewind(file);
    aether_mjpeg_reader_t reader = {.file=file,.buffer=block,.cancelled=cancelled,.yield=yield};
    size_t size = 0;
    for (unsigned i = 0; i < 2; ++i) {
        assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_OK);
        assert(size == sizeof(jpeg) && !memcmp(output, jpeg, size));
    }
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_NOT_FOUND);
    fclose(file);
    file = tmpfile();
    assert(file);
    for (unsigned i = 0; i < AETHER_MJPEG_READ_BYTES - 1; ++i) fputc(0, file);
    assert(fwrite(jpeg, 1, sizeof(jpeg), file) == sizeof(jpeg));
    rewind(file);
    reader = (aether_mjpeg_reader_t){.file=file,.buffer=block};
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_OK);
    assert(size == sizeof(jpeg));
    fclose(file);
    const uint8_t bad_length[] = {0xff,0xd8,0xff,0xe0,0,1};
    file = stream(bad_length, sizeof(bad_length));
    reader = (aether_mjpeg_reader_t){.file=file,.buffer=block};
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_INVALID_SIZE);
    fclose(file);
    file = stream(jpeg, sizeof(jpeg) - 1);
    reader = (aether_mjpeg_reader_t){.file=file,.buffer=block};
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_INVALID_SIZE);
    fclose(file);
    file = stream(jpeg, sizeof(jpeg));
    reader = (aether_mjpeg_reader_t){.file=file,.buffer=block};
    memset(output, 0xa5, sizeof(output));
    assert(aether_mjpeg_read_frame(&reader, output, 10, &size) == ESP_ERR_INVALID_SIZE);
    for (unsigned i = 10; i < sizeof(output); ++i) assert(output[i] == 0xa5);
    assert(aether_mjpeg_read_frame(&reader, output, 1, &size) == ESP_ERR_INVALID_ARG);
    fclose(file);
    file = stream(jpeg, sizeof(jpeg));
    reader = (aether_mjpeg_reader_t){.file=file,.buffer=block,.cancelled=cancelled};
    cancel = true;
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_INVALID_STATE);
    cancel = false;
    fclose(file);
    file = tmpfile();
    assert(file);
    for (unsigned i = 0; i < AETHER_MJPEG_READ_BYTES * 32 + 8; ++i) fputc(0, file);
    rewind(file);
    reader = (aether_mjpeg_reader_t){.file=file,.buffer=block,.yield=yield};
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_NOT_FOUND);
    assert(yields == 1);
    fclose(file);
    puts("MJPEG stream: 6 groups passed (segments, consecutive frames, block boundaries, malformed/capacity, cancellation, yielding)");

    file = tmpfile(); assert(file);
    long riff = begin_chunk(file, "RIFF"); fwrite("AVI ", 1, 4, file);
    long hdrl = begin_chunk(file, "LIST"); fwrite("hdrl", 1, 4, file);
    long avih = begin_chunk(file, "avih"); put32(file, 40000); end_chunk(file, avih);
    chunk(file, "JUNK", jpeg, sizeof(jpeg)); /* Header thumbnail must not be displayed. */
    end_chunk(file, hdrl);
    long movi = begin_chunk(file, "LIST"); fwrite("movi", 1, 4, file);
    chunk(file, "01wb", jpeg, sizeof(jpeg)); /* Audio may contain marker-like bytes. */
    chunk(file, "JUNK", (const uint8_t *)"odd", 3);
    chunk(file, "00dc", jpeg, sizeof(jpeg));
    chunk(file, "02dc", jpeg, sizeof(jpeg)); /* Do not mix a second video stream. */
    long rec = begin_chunk(file, "LIST"); fwrite("rec ", 1, 4, file);
    chunk(file, "00dc", jpeg, sizeof(jpeg)); end_chunk(file, rec); end_chunk(file, movi);
    chunk(file, "idx1", jpeg, sizeof(jpeg)); end_chunk(file, riff); rewind(file);
    reader = (aether_mjpeg_reader_t){.file = file, .buffer = block};
    for (unsigned i = 0; i < 2; ++i) {
        assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_OK);
        assert(size == sizeof(jpeg) && !memcmp(output, jpeg, size));
    }
    assert(reader.avi && reader.frame_period_us == 40000);
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_NOT_FOUND);
    fclose(file);
    puts("PASS RIFF AVI traversal excludes thumbnails/audio/index, respects odd padding and nested rec lists");

    const uint8_t avi_overflow[] = {'R','I','F','F',20,0,0,0,'A','V','I',' ', '0','0','d','c',255,255,255,255};
    file = stream(avi_overflow, sizeof(avi_overflow));
    reader = (aether_mjpeg_reader_t){.file = file, .buffer = block};
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_INVALID_SIZE); fclose(file);
    const uint8_t wrong_riff[] = {'R','I','F','F',4,0,0,0,'W','A','V','E'};
    file = stream(wrong_riff, sizeof(wrong_riff));
    reader = (aether_mjpeg_reader_t){.file = file, .buffer = block};
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_NOT_SUPPORTED); fclose(file);
    const uint8_t progressive[] = {0xFF,0xD8,0xFF,0xC2,0,2,0xFF,0xD9};
    file = stream(progressive, sizeof(progressive));
    reader = (aether_mjpeg_reader_t){.file = file, .buffer = block};
    assert(aether_mjpeg_read_frame(&reader, output, sizeof(output), &size) == ESP_ERR_NOT_SUPPORTED); fclose(file);
    puts("PASS invalid RIFF/chunk bounds and unsupported progressive JPEG rejected before hardware decode");
    return 0;
}

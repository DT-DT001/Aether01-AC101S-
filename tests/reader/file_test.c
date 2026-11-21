#define _POSIX_C_SOURCE 200809L
#include "app_reader_file.h"
#include "app_reader_layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int reader_test_fail_allocation;
static char path[] = "/tmp/aether-reader-XXXXXX";

static void put_file(const char *data, size_t length)
{
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(data, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void release(app_reader_chunk_t *chunk)
{
    free(chunk->text); free(chunk->source_offsets);
}

static void check_converted(const char *raw, size_t length, const char *expected,
                               app_reader_encoding_t encoding)
{
    put_file(raw, length);
    for (size_t limit = 8; limit <= 24; ++limit) {
        char actual[1024] = {0}; size_t used = 0; uint64_t offset = 0;
        while (offset < length) {
            app_reader_chunk_t chunk;
            assert(app_reader_file_read(path, offset, limit, &chunk) == ESP_OK);
            const size_t count = strlen(chunk.text);
            assert(chunk.encoding == encoding && chunk.source_offsets && !chunk.invalid_count);
            assert(chunk.offset >= offset && chunk.next_offset > offset && chunk.next_offset <= length);
            assert(chunk.source_offsets[0] == chunk.offset && chunk.source_offsets[count] == chunk.next_offset);
            for (size_t i = 0; i < count; ++i) assert(chunk.source_offsets[i] <= chunk.source_offsets[i+1]);
            assert(used + count < sizeof(actual)); memcpy(actual + used, chunk.text, count + 1); used += count;
            offset = chunk.next_offset; release(&chunk);
        }
        assert(!strcmp(actual, expected));
    }
}

static uint16_t width(uint32_t cp, uint32_t next, void *context)
{ (void)next; return cp >= 0x80 ? *(unsigned *)context : 8; }
static bool cancel_after_three(void *context) { return ++*(unsigned *)context >= 3; }

static void check_mapped_pagination(void)
{
    const char *raw = "\xB5\xDA\xD2\xBB\xD5\xC2\xD6\xD0\xCE\xC4\xD0\xA1\xCB\xB5\r\nABC\xCC\xEC\xB5\xD8\xD0\xFE\xBB\xC6\n";
    const char *utf8 = "第一章中文小说\r\nABC天地玄黄\n";
    size_t raw_size = strlen(raw) * 1000, utf8_size = strlen(utf8) * 1000;
    char *input = malloc(raw_size), *expected = malloc(utf8_size+1), *actual = malloc(utf8_size+1);
    assert(input && expected && actual);
    for (unsigned i=0;i<1000;++i) { memcpy(input+i*strlen(raw),raw,strlen(raw)); memcpy(expected+i*strlen(utf8),utf8,strlen(utf8)); }
    expected[utf8_size]=0; put_file(input,raw_size);
    for (unsigned font=16;font<=24;font+=8) {
        uint64_t offset=0, bookmark=0; size_t used=0; unsigned pages=0;
        while(offset<raw_size) {
            app_reader_chunk_t chunk;
            assert(app_reader_file_read_ex(path,offset,2048,true,2,&chunk)==ESP_OK);
            char rendered[4096];
            app_reader_page_t page=app_reader_layout_page(chunk.text,strlen(chunk.text),rendered,sizeof(rendered),424,260,font,4,width,&font);
            assert(page.consumed && used+page.consumed<=utf8_size);
            memcpy(actual+used,chunk.text,page.consumed); used+=page.consumed;
            uint64_t next=chunk.source_offsets[page.consumed]; assert(next>offset);
            if (++pages==7) bookmark=offset;
            offset=next; release(&chunk);
        }
        actual[used]=0; assert(used==utf8_size && !strcmp(actual,expected) && pages>10);
        app_reader_chunk_t resumed;
        assert(app_reader_file_read_ex(path,bookmark,2048,false,2,&resumed)==ESP_OK && resumed.offset==bookmark);
        assert(resumed.next_offset>bookmark && !resumed.invalid_count); release(&resumed);
    }
    free(input); free(expected); free(actual);
    puts("PASS 1000-paragraph GBK pagination at 16/24px reconstructs exact text with source-byte bookmark/reopen boundaries");
    input=malloc(100000); assert(input);
    for(size_t i=0;i<100000;i+=2) { input[i]=(char)0xD6; input[i+1]=(char)0xD0; }
    put_file(input,100000); free(input);
    unsigned checks=0; app_reader_chunk_t canceled;
    assert(app_reader_file_read_cancelable(path,90001,2048,false,2,cancel_after_three,&checks,&canceled)==ESP_ERR_INVALID_STATE);
    assert(checks==3 && !canceled.text && !canceled.source_offsets);
    checks=0;
    assert(app_reader_file_read_cancelable(path,0,32768,true,2,cancel_after_three,&checks,&canceled)==ESP_ERR_INVALID_STATE);
    assert(checks==3 && !canceled.text && !canceled.source_offsets);
    puts("PASS long unbroken GBK seek alignment and conversion cancel promptly and release ownership");
}

int main(void)
{
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
    const char *content = "\xEF\xBB\xBF" "一二三四五六甲乙\xF0\xA0\x80\x80" "ABC中文小说";
    put_file(content, strlen(content));
    for (size_t limit = 1; limit < 25; ++limit) {
        uint64_t offset = 0;
        char recovered[256] = {0};
        size_t used = 0;
        while (offset < strlen(content)) {
            app_reader_chunk_t chunk;
            assert(app_reader_file_read(path, offset, limit, &chunk) == ESP_OK);
            assert(chunk.text && chunk.next_offset > offset && chunk.offset >= offset);
            assert(chunk.file_size == strlen(content));
            assert(chunk.next_offset - chunk.offset == strlen(chunk.text));
            assert(used + strlen(chunk.text) < sizeof(recovered));
            strcpy(recovered + used, chunk.text);
            used += strlen(chunk.text);
            offset = chunk.next_offset;
            free(chunk.text);
        }
        assert(strcmp(recovered, content + 3) == 0);
    }
    puts("PASS real-file UTF-8/BOM chunk reconstruction at limits 1..24");
    app_reader_chunk_t chunk;
    assert(app_reader_file_read(path, 4, 8, &chunk) == ESP_OK);
    assert(chunk.offset == 6 && !strncmp(chunk.text, "二", 3));
    free(chunk.text);
    assert(app_reader_file_read(path, UINT64_MAX, 8, &chunk) == ESP_OK);
    assert(chunk.offset == strlen(content) && !chunk.text[0]);
    free(chunk.text);
    puts("PASS seeks inside a codepoint and beyond EOF");

    char preface[600]; memset(preface,'A',512); strcpy(preface+512,"中文小说正文");
    put_file(preface,strlen(preface));
    for (size_t offset=512;offset<strlen(preface);++offset) {
        assert(app_reader_file_read(path,offset,16,&chunk)==ESP_OK && chunk.encoding==APP_READER_UTF8 && !chunk.source_offsets);
        size_t aligned=offset;
        while(aligned<strlen(preface) && ((unsigned char)preface[aligned]&0xC0)==0x80)++aligned;
        assert(chunk.offset==aligned && !strncmp(chunk.text,preface+aligned,strlen(chunk.text)));
        release(&chunk);
    }
    puts("PASS UTF8 seeks inside every CJK byte after an ASCII-only preface never mis-detect GBK");

    put_file("A\xE4\xB8", 3);
    assert(app_reader_file_read(path, 0, 8, &chunk) == ESP_OK);
    assert(strcmp(chunk.text, "A??") == 0 && chunk.next_offset == 3);
    free(chunk.text);
    put_file("", 0);
    assert(app_reader_file_read(path, 0, 8, &chunk) == ESP_OK);
    assert(!chunk.file_size && chunk.text && !chunk.text[0]);
    free(chunk.text);
    puts("PASS malformed final bytes and empty file");

    put_file("a\0b", 3);
    assert(app_reader_file_read(path, 0, 8, &chunk) == ESP_ERR_NOT_SUPPORTED && !chunk.text);
    const char gbk[] = "\xD6\xD0\xCE\xC4\xD0\xA1\xCB\xB5\r\n\xB5\xDA\xD2\xBB\xD5\xC2" "ABC";
    check_converted(gbk, sizeof(gbk)-1, "中文小说\r\n第一章ABC", APP_READER_GBK);
    const char le[] = "\xFF\xFE\x2D\x4E\x87\x65\x0D\x00\x0A\x00\x40\xD8\x00\xDC\x41\x00";
    const char be[] = "\xFE\xFF\x4E\x2D\x65\x87\x00\x0D\x00\x0A\xD8\x40\xDC\x00\x00\x41";
    check_converted(le, sizeof(le)-1, "中文\r\n\xF0\xA0\x80\x80" "A", APP_READER_UTF16_LE);
    check_converted(be, sizeof(be)-1, "中文\r\n\xF0\xA0\x80\x80" "A", APP_READER_UTF16_BE);
    put_file(gbk, sizeof(gbk)-1);
    assert(app_reader_file_read(path, 1, 16, &chunk) == ESP_OK && chunk.offset == 2 && !strncmp(chunk.text,"文",3));
    release(&chunk);
    put_file(le, sizeof(le)-1);
    assert(app_reader_file_read(path, 11, 16, &chunk) == ESP_OK && chunk.offset == 14 && !strcmp(chunk.text,"A"));
    release(&chunk);
    put_file("\xFF\xFE\x00\xD8\x41\x00", 6);
    assert(app_reader_file_read(path, 0, 16, &chunk) == ESP_OK && !strcmp(chunk.text,"?A") && chunk.invalid_count == 1);
    release(&chunk);
    put_file("\xD6\xFF", 2);
    assert(app_reader_file_read(path, 0, 16, &chunk) == ESP_OK && !strcmp(chunk.text,"??") && chunk.invalid_count == 2);
    release(&chunk);
    puts("PASS real-IDF CP936 GBK and BOM UTF16LE/BE streaming, original-byte maps, seeks/surrogates and invalid diagnostics");
    put_file("\xC2\xA9",2); /* Valid UTF-8 and valid GBK: caller must be able to override detection. */
    assert(app_reader_file_read_ex(path,0,8,false,1,&chunk)==ESP_OK && !strcmp(chunk.text,"\xC2\xA9") && !chunk.source_offsets);
    release(&chunk);
    assert(app_reader_file_read_ex(path,0,8,false,2,&chunk)==ESP_OK && strcmp(chunk.text,"\xC2\xA9") && chunk.source_offsets[3]==2);
    release(&chunk);
    put_file(le+2,sizeof(le)-3);
    assert(app_reader_file_read_ex(path,0,32,false,3,&chunk)==ESP_OK && !strcmp(chunk.text,"中文\r\n\xF0\xA0\x80\x80" "A"));
    release(&chunk);
    check_mapped_pagination();
    put_file(gbk,sizeof(gbk)-1); reader_test_fail_allocation=2;
    assert(app_reader_file_read(path,0,16,&chunk)==ESP_ERR_NO_MEM && !chunk.text && !chunk.source_offsets);
    reader_test_fail_allocation=0;
    puts("PASS conversion-map allocation failure releases both owned buffers");
    put_file("valid", 5);
    reader_test_fail_allocation = 1;
    assert(app_reader_file_read(path, 0, 8, &chunk) == ESP_ERR_NO_MEM && !chunk.text);
    reader_test_fail_allocation = 0;
    assert(unlink(path) == 0);
    assert(app_reader_file_read(path, 0, 8, &chunk) == ESP_ERR_NOT_FOUND && !chunk.text);
    assert(app_reader_file_read(NULL, 0, 8, &chunk) == ESP_ERR_INVALID_ARG && !chunk.text);
    puts("PASS binary NUL rejection, allocation failure, missing file cleanup");
    return 0;
}

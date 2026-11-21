"""Compile the installed IDF's actual aligned/unaligned SD read/write paths with fault injection.

No SDK source changes. Only controller transactions/heap calls are doubled.
"""
from pathlib import Path
import os
import subprocess
import sys

out = Path(sys.argv[1])
idf = Path(os.environ.get('IDF_PATH', '/mnt/c/APP/ESP/.espressif/v6.1/esp-idf'))
source = idf / 'components/sdmmc/sdmmc_cmd.c'
text = source.read_text()


def function(name):
    start = text.index(name)
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define MALLOC_CAP_DMA 1
#define SOC_SDMMC_PSRAM_DMA_CAPABLE 0
#define MIN(a,b) ((a)<(b)?(a):(b))
static void test_log(const char *tag,const char *format,...) { (void)tag; (void)format; }
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGD(...) test_log(__VA_ARGS__)
static const char *TAG="sdk_dma_test";
typedef struct {
 struct { size_t unaligned_multi_block_rw_max_chunk_size; void *dma_aligned_buffer;
          int slot; bool (*check_buffer_alignment)(int,const void*,size_t); } host;
 struct { size_t sector_size; } csd;
} sdmmc_card_t;
static unsigned allocations,transactions,fail_transaction;
static unsigned char reserved[4096] __attribute__((aligned(64)));
static unsigned char direct[32768] __attribute__((aligned(64)));
static bool direct_pointer(const void *p) { return (uintptr_t)p>=(uintptr_t)direct && (uintptr_t)p<(uintptr_t)direct+sizeof(direct); }
static bool esp_ptr_external_ram(const void *p) { return !direct_pointer(p) && p!=reserved; }
static void *heap_caps_malloc(size_t n,unsigned caps) { assert(n>=512 && caps==MALLOC_CAP_DMA); ++allocations; return NULL; }
static size_t heap_caps_get_allocated_size(void *p) { assert(p==reserved); return sizeof(reserved); }
static bool unaligned(int slot,const void *p,size_t n) { (void)slot;(void)p;(void)n;return false; }
static bool aligned(int slot,const void *p,size_t n) { (void)slot;return direct_pointer(p) && !((uintptr_t)p%64) && !(n%64); }
static esp_err_t sdmmc_read_sectors_dma(sdmmc_card_t *card,void *dst,size_t start,size_t count,size_t length)
{
 assert(card && count && (dst==reserved ? count<=8 && length==sizeof(reserved) : direct_pointer(dst) && count<=64 && length==count*512));
 ++transactions; if(transactions==fail_transaction) return ESP_FAIL;
 for(size_t i=0;i<count*512;++i) ((uint8_t*)dst)[i]=(uint8_t)((start*512+i)^0xa7);
 return ESP_OK;
}
static esp_err_t sdmmc_write_sectors_dma(sdmmc_card_t *card,const void *src,size_t start,size_t count,size_t length)
{
 assert(card && count && (src==reserved ? count<=8 && length==sizeof(reserved) : direct_pointer(src) && count<=64 && length==count*512));
 ++transactions; if(transactions==fail_transaction) return ESP_FAIL;
 for(size_t i=0;i<count*512;++i) assert(((const uint8_t*)src)[i]==(uint8_t)((start*512+i)^0xa7));
 return ESP_OK;
}
'''
body = ''.join(function(name) for name in (
    'static inline size_t get_chunk_size(',
    'static esp_err_t allocate_dma_buf(',
    'esp_err_t sdmmc_read_sectors(',
    'esp_err_t sdmmc_write_sectors('))
suffix = r'''
int main(void)
{
 sdmmc_card_t card={.host={.unaligned_multi_block_rw_max_chunk_size=8,
   .dma_aligned_buffer=reserved,.check_buffer_alignment=unaligned},.csd={.sector_size=512}};
 for(unsigned offset=0;offset<32;++offset) for(unsigned count=0;count<=129;++count) {
   size_t size=count*512; uint8_t *allocation=malloc(size+offset+32); assert(allocation);
   memset(allocation,0x5a,size+offset+32); transactions=0;
   assert(sdmmc_read_sectors(&card,allocation+offset,13,count)==ESP_OK);
   assert(!allocations && transactions==(count+7)/8);
   for(size_t i=0;i<size;++i) assert(allocation[offset+i]==(uint8_t)((13*512+i)^0xa7));
   for(unsigned i=0;i<offset;++i) assert(allocation[i]==0x5a);
   for(unsigned i=0;i<32;++i) assert(allocation[offset+size+i]==0x5a);
   free(allocation);
 }
 uint8_t data[32*512]; transactions=0; fail_transaction=3;
 assert(sdmmc_read_sectors(&card,data,0,32)==ESP_FAIL && transactions==3 && !allocations);
 for(unsigned offset=0;offset<32;++offset) for(unsigned count=0;count<=129;++count) {
   size_t size=count*512; uint8_t *allocation=malloc(size+offset+32); assert(allocation);
   memset(allocation,0x5a,size+offset+32); transactions=0; fail_transaction=0;
   for(size_t i=0;i<size;++i) allocation[offset+i]=(uint8_t)((13*512+i)^0xa7);
   assert(sdmmc_write_sectors(&card,allocation+offset,13,count)==ESP_OK);
   assert(!allocations && transactions==(count+7)/8);
   for(unsigned i=0;i<offset;++i) assert(allocation[i]==0x5a);
   for(unsigned i=0;i<32;++i) assert(allocation[offset+size+i]==0x5a);
   free(allocation);
 }
 card.host.check_buffer_alignment=aligned;
 for(unsigned count=16;count<=32;count*=2) {
   memset(direct,0,count*512); transactions=0;
   assert(sdmmc_read_sectors(&card,direct,13,count)==ESP_OK && transactions==1 && !allocations);
   for(size_t i=0;i<count*512;++i) assert(direct[i]==(uint8_t)((13*512+i)^0xa7));
   transactions=0; assert(sdmmc_write_sectors(&card,direct,13,count)==ESP_OK && transactions==1 && !allocations);
 }
 transactions=0;fail_transaction=1;
 assert(sdmmc_read_sectors(&card,direct,13,32)==ESP_FAIL && transactions==1);
 transactions=0;assert(sdmmc_write_sectors(&card,direct,13,32)==ESP_FAIL && transactions==1);
 card.host.check_buffer_alignment=unaligned;
 puts("PASS installed IDF SD write path:4160 unaligned writes preserve bounds with zero allocations; aligned8/16KiB read/write each use one DMA transaction; faults propagate without retries");
 card.host.dma_aligned_buffer=NULL; fail_transaction=0; transactions=0;
 assert(sdmmc_read_sectors(&card,data,0,8)==ESP_ERR_NO_MEM && allocations==4 && !transactions);
 puts("PASS installed IDF SD path: 4160 unaligned cached reads, correct 8-sector chunks/tails, zero DMA allocations despite heap failure; real card errors propagate");
 return 0;
}
'''
generated = out / 'sdk_dma.c'
generated.write_text(prefix + body + suffix)
binary = out / 'sdk_dma'
subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
                '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
                str(generated), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)

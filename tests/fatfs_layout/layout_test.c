#include "ff.h"
#include "diskio_impl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Original sparse card geometry:32GB, one primary basic-data partition,
 * deterministic files. No user media, mounted card or real disk is accessed. */
#define PHYSICAL_SECTORS (30436U*2048U)
#define PART_START 2048U
#define PART_COUNT (PHYSICAL_SECTORS-4096U)
#define FAT_SECTORS 8192U
#define DATA_START (PART_START+32U+2U*FAT_SECTORS)
#define MAP_LIMIT 4096U
struct sector {uint32_t lba;uint8_t bytes[512];};
static struct sector map[MAP_LIMIT];
static unsigned used,reads,writes,trim_calls;
static uint32_t capacity=PHYSICAL_SECTORS,last_read,last_write,trim_start,trim_end;
static bool allow_write,ioctl_fail;
static unsigned allocations;
static const BYTE basic_guid[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
void *ff_memalloc(size_t n){void *p=malloc(n);if(p)++allocations;return p;}
void ff_memfree(void *p){if(p){assert(allocations);--allocations;free(p);}}
int ff_mutex_create(int n){(void)n;return 1;}
void ff_mutex_delete(int n){(void)n;}
int ff_mutex_take(int n){(void)n;return 1;}
void ff_mutex_give(int n){(void)n;}
static void p16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void p32(uint8_t *p,uint32_t n){p16(p,(uint16_t)n);p16(p+2,(uint16_t)(n>>16));}
static void p64(uint8_t *p,uint64_t n){p32(p,(uint32_t)n);p32(p+4,(uint32_t)(n>>32));}
static uint8_t *at(uint32_t lba)
{for(unsigned i=0;i<used;++i)if(map[i].lba==lba)return map[i].bytes;assert(used<MAP_LIMIT);map[used].lba=lba;memset(map[used].bytes,0,512);return map[used++].bytes;}
static const uint8_t *find(uint32_t lba)
{for(unsigned i=0;i<used;++i)if(map[i].lba==lba)return map[i].bytes;return NULL;}
static DSTATUS ready(BYTE p){assert(p==0);return 0;}
static DRESULT read32(BYTE p,BYTE *out,uint32_t lba,UINT count)
{
    assert(p==0);++reads;last_read=lba;
    if(lba>=capacity||count>capacity-lba)return RES_PARERR;
    for(UINT i=0;i<count;++i){const BYTE *s=find(lba+i);if(s)memcpy(out+(size_t)i*512,s,512);else memset(out+(size_t)i*512,0,512);}return RES_OK;
}
static DRESULT write32(BYTE p,const BYTE *in,uint32_t lba,UINT count)
{
    assert(p==0);++writes;last_write=lba;
    if(!allow_write)return RES_WRPRT;
    if(lba>=capacity||count>capacity-lba)return RES_PARERR;
    for(UINT i=0;i<count;++i)memcpy(at(lba+i),in+(size_t)i*512,512);
    return RES_OK;
}
static DRESULT ioctl32(BYTE p,BYTE cmd,void *out)
{
    assert(p==0);if(ioctl_fail)return RES_ERROR;
    switch(cmd){case GET_SECTOR_COUNT:*(DWORD*)out=capacity;return RES_OK;
    case GET_SECTOR_SIZE:*(WORD*)out=512;return RES_OK;
    case GET_BLOCK_SIZE:*(DWORD*)out=8;return RES_OK;
    case CTRL_SYNC:return RES_OK;
    case CTRL_TRIM:++trim_calls;trim_start=((DWORD*)out)[0];trim_end=((DWORD*)out)[1];return RES_OK;}
    return RES_PARERR;
}
static void reset(void)
{used=reads=writes=trim_calls=0;capacity=PHYSICAL_SECTORS;allow_write=ioctl_fail=false;}
static uint32_t crc(const uint8_t *p,size_t n)
{uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320U&-(int32_t)(c&1));}return ~c;}
static uint8_t pattern(size_t n){return (uint8_t)((n*17U+43U)%251U);}
static void fat32(void)
{
    uint8_t *b=at(PART_START);b[0]=0xeb;b[1]=0x58;b[2]=0x90;memcpy(b+3,"AETHER01",8);
    p16(b+11,512);b[13]=64;p16(b+14,32);b[16]=2;p32(b+28,PART_START);p32(b+32,PART_COUNT);
    p32(b+36,FAT_SECTORS);p32(b+44,2);p16(b+48,1);p16(b+50,6);memcpy(b+82,"FAT32   ",8);p16(b+510,0xaa55);
    memcpy(at(PART_START+6),b,512);
    b=at(PART_START+1);p32(b,0x41615252);p32(b+484,0x61417272);p32(b+488,UINT32_MAX);p32(b+492,UINT32_MAX);p32(b+508,0xaa550000);
    for(unsigned i=0;i<2;++i){b=at(PART_START+32+i*FAT_SECTORS);p32(b,0x0ffffff8);for(unsigned j=1;j<=4;++j)p32(b+j*4,0x0fffffff);}
    b=at(DATA_START);memcpy(b,"HELLO   TXT",11);b[11]=AM_ARC;p16(b+26,3);p32(b+28,2000);
    /* One UTF16 LFN entry plus its deterministic8.3 alias. */
    const char alias[11]="BOOKNO~1TXT";unsigned sum=0;
    for(unsigned i=0;i<11;++i)sum=(((sum&1)?128:0)+(sum>>1)+(uint8_t)alias[i])&255U;
    uint8_t *lfn=b+32;lfn[0]=0x41;lfn[11]=0x0f;lfn[13]=(uint8_t)sum;
    const uint16_t name[13]={0x6d4b,0x8bd5,0x5c0f,0x8bf4,'.','t','x','t',0,0xffff,0xffff,0xffff,0xffff};
    const unsigned offsets[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    for(unsigned i=0;i<13;++i)p16(lfn+offsets[i],name[i]);
    b+=64;memcpy(b,alias,11);b[11]=AM_ARC;p16(b+26,4);p32(b+28,17);
    for(unsigned s=0;s<4;++s){b=at(DATA_START+64+s);for(unsigned i=0;i<512;++i)b[i]=pattern(s*512U+i);}
    memcpy(at(DATA_START+128),"Chinese-book-data",17);
}
static void gpt_header(uint32_t lba,uint32_t other,uint32_t table,uint32_t table_crc)
{
    uint8_t *h=at(lba);memcpy(h,"EFI PART",8);p32(h+8,0x00010000);p32(h+12,92);
    p64(h+24,lba);p64(h+32,other);p64(h+40,34);p64(h+48,PHYSICAL_SECTORS-34U);
    for(unsigned i=0;i<16;++i)h[56+i]=(uint8_t)(i+1);
    p64(h+72,table);p32(h+80,128);p32(h+84,128);p32(h+88,table_crc);p32(h+16,crc(h,92));
}
static void fixture(bool gpt)
{
    reset();fat32();uint8_t *m=at(0);p16(m+510,0xaa55);m[450]=gpt?0xee:0x0c;
    p32(m+454,gpt?1:PART_START);p32(m+458,gpt?PHYSICAL_SECTORS-1:PART_COUNT);
    if(gpt){uint8_t table[16384]={0};memcpy(table,basic_guid,16);for(unsigned i=0;i<16;++i)table[16+i]=(uint8_t)(i+91);
        p64(table+32,PART_START);p64(table+40,PART_START+PART_COUNT-1);
        p16(table+56,'A');p16(table+58,'e');p16(table+60,'t');
        memcpy(at(2),table,512);memcpy(at(PHYSICAL_SECTORS-33),table,512);
        const uint32_t checksum=crc(table,sizeof(table));gpt_header(1,PHYSICAL_SECTORS-1,2,checksum);
        gpt_header(PHYSICAL_SECTORS-1,1,PHYSICAL_SECTORS-33,checksum);}
}
static void read_files(FATFS *fs)
{
    FF_DIR directory={0};FILINFO info;assert(f_opendir(&directory,"0:/")==FR_OK);
    bool ascii=false,chinese=false;
    for(;;){assert(f_readdir(&directory,&info)==FR_OK);if(!info.fname[0])break;
        if(!strcmp(info.fname,"HELLO.TXT"))ascii=true;
        if(!strcmp(info.fname,"测试小说.txt"))chinese=true;}
    assert(ascii&&chinese&&f_closedir(&directory)==FR_OK);
    FIL file={0};UINT count;uint8_t data[2100];assert(f_open(&file,"0:/HELLO.TXT",FA_READ)==FR_OK);
    assert(f_size(&file)==2000&&f_read(&file,data,sizeof(data),&count)==FR_OK&&count==2000);
    for(unsigned i=0;i<count;++i)assert(data[i]==pattern(i));
    assert(f_lseek(&file,1931)==FR_OK&&f_read(&file,data,100,&count)==FR_OK&&count==69);
    for(unsigned i=0;i<count;++i)assert(data[i]==pattern(1931+i));
    assert(f_close(&file)==FR_OK);
    assert(f_open(&file,"0:/测试小说.txt",FA_READ)==FR_OK&&f_read(&file,data,sizeof(data),&count)==FR_OK&&count==17);
    assert(!memcmp(data,"Chinese-book-data",17)&&f_close(&file)==FR_OK);
    assert(fs->fs_type==FS_FAT32&&writes==0);
}
static void test_layouts(void)
{
    for(unsigned gpt=0;gpt<2;++gpt){FATFS fs;memset(&fs,0,sizeof(fs));fixture(gpt);
        FRESULT e=f_mount(&fs,"0:",1);if(e!=FR_OK)fprintf(stderr,"layout%u failed%d\n",gpt,e);
        assert(e==FR_OK);read_files(&fs);assert(f_mount(NULL,"0:",0)==FR_OK&&!allocations);}
    puts("PASS actual owned FatFs reads32GB GPT and MBR FAT32 sparse original media, UTF8 Chinese/ASCII directories, content and seek with zero disk writes");
    FATFS fs;fixture(true);at(1)[16]^=1;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_NO_FILESYSTEM);
    assert(f_mount(NULL,"0:",0)==FR_OK&&!allocations&&writes==0);
    fixture(true);at(2)[0]^=1;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_NO_FILESYSTEM);
    assert(f_mount(NULL,"0:",0)==FR_OK&&!allocations&&writes==0);
    puts("PASS actual official GPT scanner rejects corrupt header CRC or non-basic-data GUID without writes or allocation leaks");
}
static void test_bridge(void)
{
    uint8_t bytes[512];unsigned n=reads;
    assert(ff_disk_read(0,bytes,UINT64_C(0x100000000),1)==RES_PARERR&&reads==n);
    assert(ff_disk_read(0,bytes,UINT32_MAX,2)==RES_PARERR&&reads==n);
    assert(ff_disk_read(0,bytes,0,0)==RES_PARERR&&reads==n);
    n=writes;assert(ff_disk_write(0,bytes,UINT64_MAX,1)==RES_PARERR&&writes==n);
    assert(ff_disk_write(0,bytes,UINT32_MAX,2)==RES_PARERR&&writes==n);
    struct {LBA_t value;uint32_t guard;} result={UINT64_MAX,0x11223344};
    assert(ff_disk_ioctl(0,GET_SECTOR_COUNT,&result.value)==RES_OK&&result.value==capacity&&result.guard==0x11223344);
    ioctl_fail=true;result.value=UINT64_MAX;assert(ff_disk_ioctl(0,GET_SECTOR_COUNT,&result.value)==RES_ERROR&&result.value==UINT64_MAX);ioctl_fail=false;
    LBA_t trim[2]={7,9};n=trim_calls;assert(ff_disk_ioctl(0,CTRL_TRIM,trim)==RES_OK&&trim_calls==n+1&&trim_start==7&&trim_end==9);
    n=trim_calls;trim[0]=11;assert(ff_disk_ioctl(0,CTRL_TRIM,trim)==RES_PARERR&&trim_calls==n);
    trim[0]=0;trim[1]=UINT64_C(0x100000000);assert(ff_disk_ioctl(0,CTRL_TRIM,trim)==RES_PARERR&&trim_calls==n);
    trim[1]=UINT32_MAX;assert(ff_disk_ioctl(0,CTRL_TRIM,trim)==RES_PARERR&&trim_calls==n);
    WORD sector_size=0;DWORD block_size=0;
    assert(ff_disk_ioctl(0,GET_SECTOR_SIZE,&sector_size)==RES_OK&&sector_size==512);
    assert(ff_disk_ioctl(0,GET_BLOCK_SIZE,&block_size)==RES_OK&&block_size==8);
    puts("PASS real diskio bridge preserves32bit SD/USB driver ABI, initializes all64 capacity bits, converts TRIM pairs and rejects truncation/wrap without invoking drivers");
}
static void test_exfat(void)
{
    reset();capacity=262144;allow_write=true;
    MKFS_PARM format={.fmt=FM_EXFAT|FM_SFD,.au_size=4096};uint8_t work[4096];
    FRESULT e=f_mkfs("0:",&format,work,sizeof(work));if(e)fprintf(stderr,"exfat format%d\n",e);assert(e==FR_OK);
    FATFS fs;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_OK&&fs.fs_type==FS_EXFAT);
    FIL file={0};UINT count;const char text[]="Original exFAT fixture content";
    assert(f_open(&file,"0:/EXFAT.TXT",FA_WRITE|FA_CREATE_ALWAYS)==FR_OK);
    assert(f_write(&file,text,sizeof(text),&count)==FR_OK&&count==sizeof(text)&&f_close(&file)==FR_OK);
    assert(f_mount(NULL,"0:",0)==FR_OK&&!allocations);
    allow_write=false;writes=0;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_OK&&fs.fs_type==FS_EXFAT);
    assert(f_open(&file,"0:/EXFAT.TXT",FA_READ)==FR_OK);
    char data[80]={0};assert(f_read(&file,data,sizeof(data),&count)==FR_OK&&count==sizeof(text)&&!memcmp(data,text,sizeof(text)));
    assert(f_lseek(&file,9)==FR_OK&&f_read(&file,data,5,&count)==FR_OK&&count==5&&!memcmp(data,text+9,5));
    assert(f_close(&file)==FR_OK&&f_mount(NULL,"0:",0)==FR_OK&&!allocations&&writes==0);
    puts("PASS enabled exFAT parser reads/seeks an original RAM-only fixture without writes; real cards and large-file app support remain unqualified");
}
static void test_write_roundtrip(bool exfat)
{
    if(!exfat)fixture(false); /* Original MBR/FAT32 card; exFAT uses prior RAM fixture. */
    allow_write=true;
    FATFS fs;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_OK);
    assert(fs.fs_type==(exfat?FS_EXFAT:FS_FAT32));
    assert(f_mkdir("0:/录音")==FR_OK);
    FIL file={0};UINT n;
    static uint8_t data[42000],readback[42000];
    for(unsigned i=0;i<sizeof(data);++i)data[i]=pattern(i+81);
    assert(f_open(&file,"0:/录音/写入测试.wav",FA_CREATE_ALWAYS|FA_WRITE)==FR_OK);
    assert(f_write(&file,data,40000,&n)==FR_OK&&n==40000&&f_sync(&file)==FR_OK);
    assert(f_close(&file)==FR_OK);
    assert(f_open(&file,"0:/录音/写入测试.wav",FA_OPEN_APPEND|FA_WRITE)==FR_OK);
    assert(f_write(&file,data+40000,2000,&n)==FR_OK&&n==2000&&f_close(&file)==FR_OK);
    assert(f_mount(NULL,"0:",0)==FR_OK&&!allocations);
    const unsigned saved_writes=writes;
    allow_write=false;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_OK);
    assert(f_open(&file,"0:/录音/写入测试.wav",FA_READ)==FR_OK&&f_size(&file)==sizeof(data));
    assert(f_read(&file,readback,sizeof(readback),&n)==FR_OK&&n==sizeof(readback)&&!memcmp(data,readback,sizeof(data)));
    assert(f_lseek(&file,32717)==FR_OK&&f_read(&file,readback,317,&n)==FR_OK&&n==317&&!memcmp(readback,data+32717,317));
    assert(f_close(&file)==FR_OK&&f_mount(NULL,"0:",0)==FR_OK&&!allocations&&writes==saved_writes);
    allow_write=true;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_OK);
    assert(f_unlink("0:/录音/写入测试.wav")==FR_OK&&f_unlink("0:/录音")==FR_OK);
    assert(f_mount(NULL,"0:",0)==FR_OK&&!allocations);
    allow_write=false;memset(&fs,0,sizeof(fs));assert(f_mount(&fs,"0:",1)==FR_OK);
    FILINFO info;assert(f_stat("0:/录音",&info)==FR_NO_FILE);
    assert(f_mount(NULL,"0:",0)==FR_OK&&!allocations);
    puts(exfat?"PASS actual exFAT RAM fixture mkdir/Chinese filename/write/sync/close/append/remount/read/seek/delete/remount roundtrip preserves full PCM-like bytes":
        "PASS actual FAT32 RAM fixture mkdir/Chinese filename/write/sync/close/append/remount/read/seek/delete/remount crosses cluster boundaries without leaks");
}
int main(void)
{
    _Static_assert(sizeof(LBA_t)==8&&sizeof(FSIZE_t)==8,"Use owned GPT-enabled ABI");
    const ff_diskio_impl_t driver={ready,ready,read32,write32,ioctl32};ff_diskio_register(0,&driver);
    test_layouts();test_bridge();test_exfat();test_write_roundtrip(true);test_write_roundtrip(false);ff_diskio_unregister(0);assert(!allocations);
    return 0;
}

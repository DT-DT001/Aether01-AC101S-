#include "aether_storage_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t mbr[512],vbr[512];
static unsigned reads;
static uint32_t expected_lba;
static int read_error;
static int read_sector(void *ctx,uint32_t lba,uint8_t *out)
{
    assert(ctx==(void*)1);++reads;
    if(read_error)return read_error;
    if(lba==0)memcpy(out,mbr,512);else{assert(lba==expected_lba);memcpy(out,vbr,512);}return 0;
}
static void p16(uint8_t *p,uint16_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void p32(uint8_t *p,uint32_t x){p16(p,(uint16_t)x);p16(p+2,(uint16_t)(x>>16));}
static void boot(uint8_t *b,uint32_t sectors)
{
    memset(b,0,512);b[0]=0xeb;b[1]=0x58;b[2]=0x90;p16(b+11,512);b[13]=64;p16(b+14,32);b[16]=2;
    p32(b+32,sectors);p32(b+36,8192);p32(b+44,2);memcpy(b+82,"FAT32   ",8);p16(b+510,0xaa55);
}
static aether_storage_diagnostics_t probe(void)
{aether_storage_diagnostics_t d;reads=0;aether_storage_probe(read_sector,(void*)1,67108864,&d);return d;}
int main(void)
{
    boot(mbr,67108864);aether_storage_diagnostics_t d=probe();
    assert(d.layout==AETHER_STORAGE_LAYOUT_SUPERFLOPPY&&d.filesystem==AETHER_STORAGE_FS_FAT32&&d.bpb_valid&&!d.retry_suspended&&reads==1);
    assert(d.capacity_bytes==UINT64_C(34359738368)&&d.sector_bytes==512&&d.sectors_per_cluster==64&&d.fat_count==2&&d.clusters==1048319);
    memset(mbr,0,512);p16(mbr+510,0xaa55);mbr[446+4]=0x0c;p32(mbr+446+8,2048);p32(mbr+446+12,67106816);
    boot(vbr,67106816);expected_lba=2048;d=probe();
    assert(d.layout==AETHER_STORAGE_LAYOUT_MBR&&d.partition_lba==2048&&d.bpb_valid&&reads==2);
    puts("PASS32GiB FAT32 superfloppy and MBR metadata parse without32bit capacity truncation or card writes");
    memset(mbr,0,512);mbr[0]=0xe8;p16(mbr+11,512);mbr[13]=1;p16(mbr+14,1);mbr[16]=2;
    p16(mbr+17,224);p16(mbr+19,2880);p16(mbr+22,9);d=probe();
    assert(d.filesystem==AETHER_STORAGE_FS_FAT12_16&&d.bpb_valid&&d.layout==AETHER_STORAGE_LAYOUT_SUPERFLOPPY);
    puts("PASS legacy FAT12/16 BPB without system label/signature matches SDK recognition rather than falsely latching an unsupported format");
    memset(mbr,0,512);p16(mbr+510,0xaa55);mbr[450]=0xee;
    mbr[446+4]=0xee;d=probe();assert(d.layout==AETHER_STORAGE_LAYOUT_GPT&&d.retry_suspended&&reads==1);
    memset(mbr,0,512);memcpy(mbr+3,"EXFAT   ",8);d=probe();assert(d.filesystem==AETHER_STORAGE_FS_EXFAT&&d.retry_suspended);
    memcpy(mbr+3,"NTFS    ",8);d=probe();assert(d.filesystem==AETHER_STORAGE_FS_NTFS&&d.retry_suspended);
    puts("PASS GPT protective layout, exFAT and NTFS are diagnosed explicitly without formatting or unsupported mounts");
    boot(mbr,67108864);p16(mbr+11,4096);d=probe();assert(d.filesystem==AETHER_STORAGE_FS_FAT32&&!d.bpb_valid&&d.retry_suspended&&d.sector_bytes==4096);
    boot(mbr,67108864);mbr[13]=0;d=probe();assert(!d.bpb_valid);
    boot(mbr,67108864);mbr[13]=3;d=probe();assert(!d.bpb_valid);
    boot(mbr,67108864);mbr[16]=0;d=probe();assert(!d.bpb_valid);
    boot(mbr,67108864);p16(mbr+14,0);d=probe();assert(!d.bpb_valid);
    boot(mbr,67108864);p32(mbr+44,UINT32_MAX);d=probe();assert(!d.bpb_valid);
    boot(mbr,67108864);p32(mbr+36,1);d=probe();assert(!d.bpb_valid);
    boot(mbr,67108864);p16(mbr+42,1);d=probe();assert(!d.bpb_valid);
    memset(mbr,0,512);d=probe();assert(d.filesystem==AETHER_STORAGE_FS_INVALID&&d.retry_suspended);
    puts("PASS invalid FAT32 sector/cluster/FAT/root/version geometry is bounded and distinct from a capacity limit");
    memset(mbr,0,512);p16(mbr+510,0xaa55);mbr[450]=0x0c;p32(mbr+454,67108860);p32(mbr+458,128);d=probe();assert(reads==1&&d.retry_suspended);
    read_error=0x108;d=probe();assert(d.read_error==0x108&&!d.retry_suspended&&!d.sector_read);
    puts("PASS out-of-range partition offsets never trigger reads; transport faults remain retryable and never masquerade as format errors");
}

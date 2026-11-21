#include "aether_storage_probe.h"
#include <stddef.h>
#include <string.h>

static uint16_t le16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return (uint32_t)le16(p)|(uint32_t)le16(p+2)<<16;}
static bool power2(uint32_t value){return value && !(value&(value-1));}
static bool boot_record(const uint8_t *b,aether_storage_diagnostics_t *out)
{
    if(!memcmp(b+3,"EXFAT   ",8)){out->filesystem=AETHER_STORAGE_FS_EXFAT;out->retry_suspended=true;return true;}
    if(!memcmp(b+3,"NTFS    ",8)){out->filesystem=AETHER_STORAGE_FS_NTFS;out->retry_suspended=true;return true;}
    if(b[0]!=0xeb && b[0]!=0xe9 && b[0]!=0xe8)return false;
    const bool fat32=le16(b+510)==0xaa55&&!memcmp(b+82,"FAT32   ",8);
    /* Match FatFs recognition of legacy FAT VBRs whose system label/signature
     * can be absent. This is diagnosis only, never a formatter or writer. */
    const uint16_t initial_sector=le16(b+11);
    const bool fat16=power2(initial_sector)&&initial_sector>=512&&initial_sector<=4096&&
        power2(b[13])&&le16(b+14)&&(b[16]==1||b[16]==2)&&le16(b+17)&&
        (le16(b+19)>=128||le32(b+32)>=65536)&&le16(b+22);
    if(!fat32&&!fat16)return false;
    out->filesystem=fat32?AETHER_STORAGE_FS_FAT32:AETHER_STORAGE_FS_FAT12_16;
    out->sector_bytes=le16(b+11);out->sectors_per_cluster=b[13];out->reserved_sectors=le16(b+14);
    out->fat_count=b[16];out->total_sectors=le16(b+19)?le16(b+19):le32(b+32);
    out->fat_sectors=le16(b+22)?le16(b+22):le32(b+36);
    const uint32_t roots=le16(b+17);
    const uint64_t root_sectors=((uint64_t)roots*32+511)/512;
    const uint64_t overhead=out->reserved_sectors+(uint64_t)out->fat_count*out->fat_sectors+root_sectors;
    bool valid=out->sector_bytes==512 && power2(out->sectors_per_cluster) && out->sectors_per_cluster<=128 &&
        out->reserved_sectors && (out->fat_count==1||out->fat_count==2) && out->fat_sectors &&
        out->total_sectors>overhead && (!out->partition_sectors||out->total_sectors<=out->partition_sectors);
    if(valid){
        out->clusters=(uint32_t)((out->total_sectors-overhead)/out->sectors_per_cluster);
        if(fat32)valid=!roots&&!le16(b+22)&&!le16(b+42)&&out->clusters>65525U&&out->clusters<=0x0ffffff5U&&
            le32(b+44)>=2&&le32(b+44)<(uint64_t)out->clusters+2&&
            ((uint64_t)out->clusters+2)*4<=(uint64_t)out->fat_sectors*512;
        else valid=roots && out->clusters>0 && out->clusters<=65525U;
    }
    out->bpb_valid=valid;
    if(!valid)out->retry_suspended=true;
    return true;
}
void aether_storage_probe(aether_storage_sector_read_t read,void *context,
                         uint64_t sectors,aether_storage_diagnostics_t *out)
{
    memset(out,0,sizeof(*out));out->attempted=true;out->capacity_bytes=sectors*UINT64_C(512);
    uint8_t b[512];
    if(!read || !sectors){out->read_error=-1;return;}
    out->read_error=read(context,0,b);if(out->read_error)return;
    out->sector_read=true;out->partition_sectors=sectors>UINT32_MAX?UINT32_MAX:(uint32_t)sectors;
    if(boot_record(b,out)){out->layout=AETHER_STORAGE_LAYOUT_SUPERFLOPPY;return;}
    out->partition_sectors=0;
    if(le16(b+510)!=0xaa55){out->filesystem=AETHER_STORAGE_FS_INVALID;out->retry_suspended=true;return;}
    out->layout=AETHER_STORAGE_LAYOUT_MBR;
    struct {uint32_t lba,sectors;uint8_t type;} partitions[4];
    for(unsigned i=0;i<4;++i){const uint8_t *p=b+446+i*16;partitions[i].type=p[4];partitions[i].lba=le32(p+8);partitions[i].sectors=le32(p+12);}
    for(unsigned i=0;i<4;++i){
        if(partitions[i].type==0xee){out->layout=AETHER_STORAGE_LAYOUT_GPT;out->retry_suspended=true;return;}
    }
    bool any=false;
    for(unsigned i=0;i<4;++i){
        const uint64_t lba=partitions[i].lba,count=partitions[i].sectors;
        if(!partitions[i].type||!lba||!count)continue;
        any=true;
        if(lba>=sectors||count>sectors-lba)continue;
        out->read_error=read(context,(uint32_t)lba,b);if(out->read_error)return;
        out->partition_lba=(uint32_t)lba;out->partition_sectors=(uint32_t)count;
        if(boot_record(b,out))return;
    }
    out->filesystem=AETHER_STORAGE_FS_INVALID;out->retry_suspended=true;
    if(!any)out->layout=AETHER_STORAGE_LAYOUT_UNKNOWN;
}
const char *aether_storage_layout_name(aether_storage_layout_t value)
{switch(value){case AETHER_STORAGE_LAYOUT_SUPERFLOPPY:return "superfloppy";case AETHER_STORAGE_LAYOUT_MBR:return "MBR";case AETHER_STORAGE_LAYOUT_GPT:return "GPT";default:return "unknown";}}
const char *aether_storage_fs_name(aether_storage_fs_t value)
{switch(value){case AETHER_STORAGE_FS_FAT12_16:return "FAT12/16";case AETHER_STORAGE_FS_FAT32:return "FAT32";case AETHER_STORAGE_FS_EXFAT:return "exFAT";case AETHER_STORAGE_FS_NTFS:return "NTFS";case AETHER_STORAGE_FS_INVALID:return "unrecognized";default:return "unknown";}}

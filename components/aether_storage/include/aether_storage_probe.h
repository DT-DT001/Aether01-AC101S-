#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AETHER_STORAGE_LAYOUT_UNKNOWN, AETHER_STORAGE_LAYOUT_SUPERFLOPPY,
    AETHER_STORAGE_LAYOUT_MBR, AETHER_STORAGE_LAYOUT_GPT,
} aether_storage_layout_t;
typedef enum {
    AETHER_STORAGE_FS_UNKNOWN, AETHER_STORAGE_FS_FAT12_16, AETHER_STORAGE_FS_FAT32,
    AETHER_STORAGE_FS_EXFAT, AETHER_STORAGE_FS_NTFS, AETHER_STORAGE_FS_INVALID,
} aether_storage_fs_t;
/* Read-only metadata. No sector content, directory names or user data retained. */
typedef struct {
    bool attempted, sector_read, bpb_valid, retry_suspended;
    aether_storage_layout_t layout;
    aether_storage_fs_t filesystem;
    uint64_t capacity_bytes;
    uint32_t partition_lba, partition_sectors, total_sectors, fat_sectors, clusters;
    uint16_t sector_bytes, reserved_sectors;
    uint8_t sectors_per_cluster, fat_count;
    int read_error;
    uint32_t failure_count, retry_delay_ms;
} aether_storage_diagnostics_t;
/* Callback returns0 only after filling exactly one512-byte sector. */
typedef int (*aether_storage_sector_read_t)(void *context,uint32_t lba,uint8_t *sector);
void aether_storage_probe(aether_storage_sector_read_t read,void *context,
                         uint64_t sectors,aether_storage_diagnostics_t *out);
const char *aether_storage_layout_name(aether_storage_layout_t layout);
const char *aether_storage_fs_name(aether_storage_fs_t filesystem);

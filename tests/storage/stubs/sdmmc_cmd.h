#pragma once
#include <stdint.h>
#include <stddef.h>
#include "driver/sdmmc_host.h"
typedef struct { struct { uint32_t capacity, sector_size; } csd; sdmmc_host_t host; int real_freq_khz; } sdmmc_card_t;
int sdmmc_card_init(const sdmmc_host_t *, sdmmc_card_t *);
int sdmmc_get_status(sdmmc_card_t *);
int sdmmc_read_sectors(sdmmc_card_t *,void *,size_t,size_t);
int sdmmc_write_sectors(sdmmc_card_t *,const void *,size_t,size_t);

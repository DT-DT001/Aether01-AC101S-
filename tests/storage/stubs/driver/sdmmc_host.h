#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { int max_freq_khz, unaligned_multi_block_rw_max_chunk_size; unsigned flags; void *dma_aligned_buffer; int slot; int (*init)(void); int (*deinit)(void); int (*deinit_p)(int); bool (*check_buffer_alignment)(int,const void*,size_t); } sdmmc_host_t;
typedef struct { int width, clk, cmd, d0, d1, d2, d3, cd, wp; unsigned flags; } sdmmc_slot_config_t;
int test_sd_host_init(void);
bool test_sd_buffer_alignment(int,const void*,size_t);
int test_sd_host_deinit(void);
int test_sd_host_deinit_p(int);
int sdmmc_host_init_slot(int, const sdmmc_slot_config_t *);
#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){.init=test_sd_host_init,.deinit=test_sd_host_deinit,.deinit_p=test_sd_host_deinit_p,.check_buffer_alignment=test_sd_buffer_alignment})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})
#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_SLOT_NO_CD -1
#define SDMMC_SLOT_NO_WP -1
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1U
#define SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF 2U

#define SDMMC_HOST_FLAG_DEINIT_ARG 4U

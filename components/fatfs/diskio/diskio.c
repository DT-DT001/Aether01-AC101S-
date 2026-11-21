/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2016        */
/* ESP-IDF port Copyright 2016-2025 Espressif Systems (Shanghai) PTE LTD */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various existing       */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include "diskio_impl.h"
#include "private_include/diskio_private.h"
#include "ffconf.h"
#include "ff.h"

static ff_diskio_impl_t * s_impls[FF_VOLUMES] = { NULL };

#if FF_MULTI_PARTITION		/* Multiple partition configuration */
PARTITION VolToPart[FF_VOLUMES] = {
    {0, 0},    /* Logical drive 0 ==> Physical drive 0, auto detection */
#if FF_VOLUMES > 1
    {1, 0},    /* Logical drive 1 ==> Physical drive 1, auto detection */
#endif
#if FF_VOLUMES > 2
    {2, 0},     /* Logical drive 2 ==> Physical drive 2, auto detection */
#endif
#if FF_VOLUMES > 3
    {3, 0},     /* Logical drive 3 ==> Physical drive 3, auto detection */
#endif
#if FF_VOLUMES > 4
    {4, 0},     /* Logical drive 4 ==> Physical drive 4, auto detection */
#endif
#if FF_VOLUMES > 5
    {5, 0},     /* Logical drive 5 ==> Physical drive 5, auto detection */
#endif
#if FF_VOLUMES > 6
    {6, 0},     /* Logical drive 6 ==> Physical drive 6, auto detection */
#endif
#if FF_VOLUMES > 7
    {7, 0},     /* Logical drive 7 ==> Physical drive 7, auto detection */
#endif
#if FF_VOLUMES > 8
    {8, 0},     /* Logical drive 8 ==> Physical drive 8, auto detection */
#endif
#if FF_VOLUMES > 9
    {9, 0},     /* Logical drive 9 ==> Physical drive 9, auto detection */
#endif
};
#endif

esp_err_t ff_diskio_get_drive(BYTE* out_pdrv)
{
    BYTE i;
    for(i=0; i<FF_VOLUMES; i++) {
        if (!s_impls[i]) {
            *out_pdrv = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void ff_diskio_register(BYTE pdrv, const ff_diskio_impl_t* discio_impl)
{
    assert(pdrv < FF_VOLUMES);

    if (s_impls[pdrv]) {
        ff_diskio_impl_t* im = s_impls[pdrv];
        s_impls[pdrv] = NULL;
        free(im);
    }

    if (!discio_impl) {
        return;
    }

    ff_diskio_impl_t * impl = (ff_diskio_impl_t *)malloc(sizeof(ff_diskio_impl_t));
    assert(impl != NULL);
    memcpy(impl, discio_impl, sizeof(ff_diskio_impl_t));
    s_impls[pdrv] = impl;
}

DSTATUS ff_disk_initialize (BYTE pdrv)
{
    return s_impls[pdrv]->init(pdrv);
}
DSTATUS ff_disk_status (BYTE pdrv)
{
    return s_impls[pdrv]->status(pdrv);
}
/* Existing IDF/MSC diskio callbacks retain their uint32_t-sector ABI.
 * GPT parsing needs FatFs LBA_t=64, but must never silently truncate a sector
 * outside those drivers' range or leave half of an LBA_t count uninitialized. */
static bool sector_range_fits_driver(LBA_t sector, UINT count)
{
    return count && sector <= UINT32_MAX && (LBA_t)(count - 1U) <= UINT32_MAX - sector;
}
DRESULT ff_disk_read (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    if (!sector_range_fits_driver(sector, count)) return RES_PARERR;
    return s_impls[pdrv]->read(pdrv, buff, (uint32_t)sector, count);
}
DRESULT ff_disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    if (!sector_range_fits_driver(sector, count)) return RES_PARERR;
    return s_impls[pdrv]->write(pdrv, buff, (uint32_t)sector, count);
}
DRESULT ff_disk_ioctl (BYTE pdrv, BYTE cmd, void* buff)
{
#if FF_LBA64
    if (cmd == GET_SECTOR_COUNT) {
        if (!buff) return RES_PARERR;
        DWORD sectors32 = 0;
        DRESULT result = s_impls[pdrv]->ioctl(pdrv, cmd, &sectors32);
        if (result == RES_OK) *((LBA_t*)buff) = (LBA_t)sectors32;
        return result;
    }
    if (cmd == CTRL_TRIM) {
        if (!buff) return RES_PARERR;
        const LBA_t *range = buff;
        if (range[0] > range[1] || range[1] > UINT32_MAX ||
                range[1] - range[0] + 1U > UINT32_MAX) return RES_PARERR;
        DWORD range32[2] = {(DWORD)range[0], (DWORD)range[1]};
        return s_impls[pdrv]->ioctl(pdrv, cmd, range32);
    }
#endif
    return s_impls[pdrv]->ioctl(pdrv, cmd, buff);
}

DWORD get_fattime(void)
{
    time_t t = time(NULL);
    struct tm tmr;
    localtime_r(&t, &tmr);
    int year = tmr.tm_year < 80 ? 0 : tmr.tm_year - 80;
    return    ((DWORD)(year) << 25)
            | ((DWORD)(tmr.tm_mon + 1) << 21)
            | ((DWORD)tmr.tm_mday << 16)
            | (WORD)(tmr.tm_hour << 11)
            | (WORD)(tmr.tm_min << 5)
            | (WORD)(tmr.tm_sec >> 1);
}

bool ff_diskio_is_registered(BYTE ldrv)
{
    if (ldrv < FF_VOLUMES && s_impls[ldrv]) {
        return true;
    }
    return false;
}

inline DRESULT ff_diskio_get_sector_size(BYTE ldrv, UINT* out_bytes_per_sector)
{
    return ff_disk_ioctl(ldrv, GET_SECTOR_SIZE, (void*) out_bytes_per_sector);
}

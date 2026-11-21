#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ff.h"

/* The host libc is 64 bit. Model the actual target ABI explicitly. */
#define off_t int32_t
#define ssize_t int32_t
#undef SSIZE_MAX
#define SSIZE_MAX INT32_MAX
#define stat target_stat
struct target_stat { int32_t st_size; mode_t st_mode; time_t st_mtim, st_atim, st_ctim; int st_blksize; };
#undef st_mtime
#undef st_atime
#undef st_ctime
#define st_mtime st_mtim
#define st_atime st_atim
#define st_ctime st_ctim
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
#define CONFIG_FATFS_LINK_LOCK 1
#define CONFIG_FATFS_VFS_FSTAT_BLKSIZE 0
#define F_WRITE_MALLOC_ZEROING_BUF_SIZE_LIMIT 512

typedef union { struct { uint16_t mday:5, mon:4, year:7; }; uint16_t as_int; } fat_date_t;
typedef union { struct { uint16_t sec:5, min:6, hour:5; }; uint16_t as_int; } fat_time_t;
typedef struct vfs_fat_dir_t {
    struct { FILINFO cached_fileinfo; char *cached_file_path; } cached_data;
    SLIST_ENTRY(vfs_fat_dir_t) open_dirs_entry;
} vfs_fat_dir_t;
SLIST_HEAD(vfs_fat_open_dirs, vfs_fat_dir_t);
typedef struct {
    char fat_drive[8], tmp_path_buf[FILENAME_MAX+3], tmp_path_buf2[FILENAME_MAX+3];
    int lock;
    FATFS fs;
    size_t max_files;
    uint32_t flags[2];
    struct vfs_fat_open_dirs open_dirs;
    FIL files[2];
} vfs_fat_ctx_t;

static int lock_depth, seek_calls, write_calls, read_calls, open_calls, close_calls, stat_calls;
static FSIZE_t disk_size;
static FRESULT close_result, seek_result;
static FATFS fake_fs;
static void _lock_acquire(int *lock) { (void)lock; assert(lock_depth++ == 0); }
static void _lock_release(int *lock) { (void)lock; assert(--lock_depth == 0); }
void *ff_memalloc(size_t n) { return calloc(1, n); }
void ff_memfree(void *p) { free(p); }
FRESULT f_open(FIL *f, const TCHAR *path, BYTE mode)
{
    (void)path; open_calls++;
    memset(f, 0, sizeof(*f)); f->obj.fs = &fake_fs;
    f->obj.objsize = (mode & FA_CREATE_ALWAYS) ? 0 : disk_size;
    f->buf = malloc(32); assert(f->buf);
    return FR_OK;
}
FRESULT f_close(FIL *f)
{
    close_calls++; free(f->buf); f->buf = NULL; f->obj.fs = NULL;
    return close_result;
}
FRESULT f_lseek(FIL *f, FSIZE_t pos)
{
    seek_calls++;
    if (seek_result != FR_OK) return seek_result;
    f->fptr = pos;
    /* Deliberately model a writable file: an unsafe seek really expands it. */
    if (pos > f->obj.objsize) f->obj.objsize = pos;
    return FR_OK;
}
FRESULT f_write(FIL *f, const void *src, UINT n, UINT *written)
{
    (void)src; write_calls++; *written = n; f->fptr += n;
    if (f->fptr > f->obj.objsize) f->obj.objsize = f->fptr;
    return FR_OK;
}
FRESULT f_read(FIL *f, void *dst, UINT n, UINT *read)
{
    (void)dst; read_calls++;
    FSIZE_t left = f->obj.objsize - f->fptr;
    *read = left < n ? (UINT)left : n; f->fptr += *read;
    return FR_OK;
}
FRESULT f_stat(const TCHAR *p, FILINFO *info)
{
    (void)p; stat_calls++; memset(info, 0, sizeof(*info)); info->fsize = disk_size; return FR_OK;
}
FRESULT f_truncate(FIL *f) { f->obj.objsize = f->fptr; return FR_OK; }

#include "production_vfs.c"

static vfs_fat_ctx_t ctx;
static void reset(FSIZE_t size, FSIZE_t pos)
{
    assert(lock_depth == 0);
    memset(&ctx, 0, sizeof(ctx)); ctx.max_files = 2; ctx.fs.csize = 8;
    ctx.files[0].obj.fs = &fake_fs; ctx.files[0].obj.objsize = size; ctx.files[0].fptr = pos;
    SLIST_INIT(&ctx.open_dirs);
    disk_size = size; close_result = seek_result = FR_OK;
    seek_calls = write_calls = read_calls = open_calls = close_calls = stat_calls = 0;
    errno = 0;
}
static void unchanged(FSIZE_t size, FSIZE_t pos)
{
    assert(ctx.files[0].obj.objsize == size && ctx.files[0].fptr == pos);
    assert(seek_calls == 0 && write_calls == 0 && read_calls == 0 && lock_depth == 0);
}
int main(void)
{
    const FSIZE_t max = INT32_MAX;
    assert(vfs_fat_max_offset() == max);
    reset(max + 1, 0); memset(&ctx.files[0], 0, sizeof(FIL));
    assert(vfs_fat_open(&ctx, "/big", O_RDONLY, 0) == -1 && errno == EOVERFLOW);
    assert(close_calls == 1 && ctx.files[0].obj.fs == NULL && ctx.files[0].buf == NULL);
    disk_size = 123;
    assert(vfs_fat_open(&ctx, "/small", O_RDONLY, 0) == 0);
    assert(f_close(&ctx.files[0]) == FR_OK);
    reset(max + 1, 0); memset(&ctx.files[0], 0, sizeof(FIL)); close_result = FR_DISK_ERR;
    assert(vfs_fat_open(&ctx, "/big", O_RDWR, 0) == -1 && errno == EOVERFLOW);
    assert(close_calls == 1 && ctx.files[0].obj.fs == NULL && ctx.files[0].buf == NULL);
    close_result = FR_OK;
    assert(vfs_fat_open(&ctx, "/big", O_WRONLY | O_CREAT | O_TRUNC, 0) == 0);
    assert(ctx.files[0].obj.objsize == 0 && f_close(&ctx.files[0]) == FR_OK);
    puts("PASS oversized open releases descriptor and per-file buffer before fastseek");

    reset(123, 10);
    assert(vfs_fat_lseek(&ctx, 0, -1, SEEK_SET) == -1 && errno == EINVAL); unchanged(123, 10);
    assert(vfs_fat_lseek(&ctx, 0, INT32_MIN, SEEK_CUR) == -1 && errno == EINVAL); unchanged(123, 10);
    assert(vfs_fat_lseek(&ctx, 0, -124, SEEK_END) == -1 && errno == EINVAL); unchanged(123, 10);
    assert(vfs_fat_lseek(&ctx, 0, INT32_MAX, SEEK_CUR) == -1 && errno == EOVERFLOW); unchanged(123, 10);
    assert(vfs_fat_lseek(&ctx, 0, INT32_MAX, SEEK_END) == -1 && errno == EOVERFLOW); unchanged(123, 10);
    assert(vfs_fat_lseek(&ctx, 0, 0, 999) == -1 && errno == EINVAL); unchanged(123, 10);
    reset(max + 1, 0);
    assert(vfs_fat_lseek(&ctx, 0, 0, SEEK_END) == -1 && errno == EOVERFLOW); unchanged(max + 1, 0);
    puts("PASS seek rejects negative, OFF_MIN, invalid modes and arithmetic overflow before FatFs");

    reset(123, 10);
    assert(vfs_fat_lseek(&ctx, 0, -3, SEEK_CUR) == 7 && ctx.files[0].fptr == 7);
    assert(vfs_fat_lseek(&ctx, 0, -3, SEEK_END) == 120);
    assert(vfs_fat_lseek(&ctx, 0, 33, SEEK_SET) == 33);
    reset(max, max - 2);
    assert(vfs_fat_lseek(&ctx, 0, 2, SEEK_CUR) == INT32_MAX);
    assert(lock_depth == 0);
    puts("PASS ordinary signed relative seeks preserve behavior");

    reset(max, max - 2);
    assert(vfs_fat_write(&ctx, 0, "abc", 3) == -1 && errno == EOVERFLOW); unchanged(max, max - 2);
    ctx.flags[0] = O_APPEND;
    assert(vfs_fat_write(&ctx, 0, "x", 1) == -1 && errno == EOVERFLOW); unchanged(max, max - 2);
    assert(vfs_fat_pwrite(&ctx, 0, "abc", 3, INT32_MAX - 2) == -1 && errno == EOVERFLOW); unchanged(max, max - 2);
    assert(vfs_fat_pwrite(&ctx, 0, "x", 1, -1) == -1 && errno == EINVAL); unchanged(max, max - 2);
    assert(vfs_fat_pread(&ctx, 0, NULL, 1, INT32_MIN) == -1 && errno == EINVAL); unchanged(max, max - 2);
    ctx.flags[0] = 0;
    assert(vfs_fat_write(&ctx, 0, "ab", 2) == 2 && ctx.files[0].fptr == max);
    reset(max + 1, 0);
    assert(vfs_fat_read(&ctx, 0, NULL, 1) == -1 && errno == EOVERFLOW); unchanged(max + 1, 0);
    reset(100, max + 1);
    assert(vfs_fat_pread(&ctx, 0, NULL, 1, 0) == -1 && errno == EOVERFLOW); unchanged(100, max + 1);
    puts("PASS regular, append and positional writes never seek or write an overflowing end");

    reset(100, 7);
    size_t too_large = (size_t)INT32_MAX + 1;
    assert(vfs_fat_read(&ctx, 0, NULL, too_large) == -1 && errno == EOVERFLOW); unchanged(100, 7);
    assert(vfs_fat_pread(&ctx, 0, NULL, too_large, 0) == -1 && errno == EOVERFLOW); unchanged(100, 7);
    assert(vfs_fat_write(&ctx, 0, NULL, too_large) == -1 && errno == EOVERFLOW); unchanged(100, 7);
    assert(vfs_fat_pwrite(&ctx, 0, NULL, too_large, 0) == -1 && errno == EOVERFLOW); unchanged(100, 7);
    assert(vfs_fat_pread(&ctx, 0, NULL, 1, 101) == 0); unchanged(100, 7);
    assert(vfs_fat_pwrite(&ctx, 0, NULL, 0, 101) == 0); unchanged(100, 7);
    assert(vfs_fat_write(&ctx, 0, NULL, 0) == 0); unchanged(100, 7);
    puts("PASS transfer return bounds, EOF pread and zero writes do not mutate files");

    reset(100, 7);
    assert(vfs_fat_read(&ctx, 0, NULL, 3) == 3 && ctx.files[0].fptr == 10);
    assert(vfs_fat_pread(&ctx, 0, NULL, 3, 20) == 3 && ctx.files[0].fptr == 10);
    assert(vfs_fat_pwrite(&ctx, 0, "abc", 3, 20) == 3 && ctx.files[0].fptr == 10);
    ctx.flags[0] = O_APPEND;
    assert(vfs_fat_write(&ctx, 0, "abc", 3) == 3 && ctx.files[0].obj.objsize == 103);
    assert(lock_depth == 0);
    puts("PASS small file read, append and positional transfers");

    struct stat st;
    reset(max + 1, 0);
    assert(vfs_fat_fstat(&ctx, 0, &st) == -1 && errno == EOVERFLOW && st.st_size == 0);
    assert(vfs_fat_stat(&ctx, "/big", &st) == -1 && errno == EOVERFLOW && st.st_size == 0);
    vfs_fat_dir_t dir = {0}; char cached[] = "/cached";
    dir.cached_data.cached_file_path = cached; dir.cached_data.cached_fileinfo.fsize = max + 1;
    SLIST_INSERT_HEAD(&ctx.open_dirs, &dir, open_dirs_entry); stat_calls = 0;
    assert(vfs_fat_stat(&ctx, "/cached", &st) == -1 && errno == EOVERFLOW);
    assert(stat_calls == 0 && cached[0] == '\0' && lock_depth == 0);
    reset(123, 0);
    assert(vfs_fat_fstat(&ctx, 0, &st) == 0 && st.st_size == 123);
    assert(vfs_fat_stat(&ctx, "/small", &st) == 0 && st.st_size == 123);
    char small_cached[] = "/cached";
    memset(&dir, 0, sizeof(dir)); dir.cached_data.cached_file_path = small_cached;
    dir.cached_data.cached_fileinfo.fsize = 456; SLIST_INSERT_HEAD(&ctx.open_dirs, &dir, open_dirs_entry);
    stat_calls = 0;
    assert(vfs_fat_stat(&ctx, "/cached", &st) == 0 && st.st_size == 456 && stat_calls == 0);
    puts("PASS fstat and direct/cached stat preserve overflow errors and small sizes");

    reset((FSIZE_t)UINT32_MAX + 17, 0);
    assert(vfs_fat_link(&ctx, "/big", "/copy") == -1 && errno == EOVERFLOW);
    assert(open_calls == 1 && close_calls == 1 && seek_calls == 0 && write_calls == 0);
    assert(lock_depth == 0);
    puts("PASS copying an oversized file refuses before creating a partial destination");

    reset(max + 1, 0);
    assert(vfs_fat_truncate(&ctx, "/big", 4) == -1 && errno == EOVERFLOW); unchanged(max + 1, 0);
    assert(close_calls == 1);
    assert(vfs_fat_ftruncate(&ctx, 0, 4) == -1 && errno == EOVERFLOW); unchanged(max + 1, 0);
    reset(100, 7);
    assert(vfs_fat_ftruncate(&ctx, 0, 4) == 0 && ctx.files[0].obj.objsize == 4);
    puts("PASS truncate guards oversized handles without changing normal truncation");
    return 0;
}

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <osd.h>

/* FAT rename cannot replace an existing destination. Keep a checked, closed
 * temporary file and a recoverable old backup instead of unlinking the only
 * valid save. These sidecars are deliberately never promoted on failed writes. */
static int sidecars(const char *path, char *temporary, char *backup)
{
    if (!path || strlen(path) + sizeof(".s31tmp") > OSD_SAVE_PATH_CAPACITY) {
        osd_persistence_error("存档路径过长");
        return -1;
    }
    const size_t length = strlen(path);
    memcpy(temporary, path, length);
    memcpy(temporary + length, ".s31tmp", sizeof(".s31tmp"));
    memcpy(backup, path, length);
    memcpy(backup + length, ".s31bak", sizeof(".s31bak"));
    return 0;
}

static int exists(const char *path)
{
    struct stat info;
    if (!stat(path, &info)) {
        if (S_ISREG(info.st_mode)) return 1;
        errno = EISDIR;
        return -1;
    }
    return errno == ENOENT ? 0 : -1;
}

int osd_restore_save(const char *path)
{
    char temporary[OSD_SAVE_PATH_CAPACITY], backup[OSD_SAVE_PATH_CAPACITY];
    if (!osd_storage_available()) {
        osd_persistence_error("存储已断开，未读取或写入存档");
        return -1;
    }
    if (sidecars(path, temporary, backup)) return -1;
    const int current = exists(path);
    if (current > 0) return 0;
    const int previous = current == 0 ? exists(backup) : -1;
    if (current < 0 || previous < 0 ||
        (previous && (!osd_storage_available() || rename(backup, path)))) {
        osd_persistence_error("存档备份恢复失败，已保留备份");
        return -1;
    }
    return 0;
}

int osd_save_file(const char *path, int (*write_contents)(FILE *, void *), void *context)
{
    char temporary[OSD_SAVE_PATH_CAPACITY], backup[OSD_SAVE_PATH_CAPACITY];
    if (sidecars(path, temporary, backup) || osd_restore_save(path)) return -1;
    if (!osd_storage_available()) goto disconnected;
    FILE *file = fopen(temporary, "wb");
    if (!file) goto failed;
    int error = write_contents(file, context) || ferror(file);
    if (!osd_storage_available()) error = -1;
    if (!error && fflush(file)) error = -1;
    if (!error && (!osd_storage_available() || fsync(fileno(file)))) error = -1;
    if (fclose(file)) error = -1;
    if (error || !osd_storage_available()) goto failed;

    const int previous = exists(path);
    if (previous < 0) goto failed;
    if (previous) {
        if (!osd_storage_available()) goto disconnected;
        if (remove(backup) && errno != ENOENT) goto failed;
        if (!osd_storage_available()) goto disconnected;
        if (rename(path, backup)) goto failed;
    }
    if (!osd_storage_available()) goto disconnected;
    if (rename(temporary, path)) {
        /* Do not overwrite an unexpectedly present destination. A failed
         * rollback retains the backup for recovery on the next valid mount. */
        if (previous && osd_storage_available() && exists(path) == 0)
            (void)rename(backup, path);
        goto failed;
    }
    if (previous && (!osd_storage_available() || remove(backup))) {
        osd_persistence_error("新存档已提交，旧备份清理未完成");
        return -1;
    }
    return 0;

disconnected:
    osd_persistence_error("存储已断开，未提交存档；旧存档或备份已保留");
    return -1;
failed:
    /* Cleanup only a disposable temporary file; never delete the old save. */
    if (osd_storage_available()) (void)remove(temporary);
    osd_persistence_error("存档写入或同步失败；旧存档或备份已保留");
    return -1;
}

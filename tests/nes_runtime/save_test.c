/* Actual transactional helper with fault-observable libc calls. No real card. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <osd.h>

static unsigned opens, stats, renames, flushes, syncs, closes, reports;
static unsigned rename_failure, rollback_failure;
static bool available=true, fail_open, fail_flush, fail_sync, fail_close, short_write;
static bool drop_after_write, drop_after_backup, fail_backup_cleanup, fail_stat;
static char path[PATH_MAX+1], temporary[PATH_MAX+1], backup[PATH_MAX+1];
static const char old_data[]="old valid SRAM bytes", new_data[]="new complete SRAM bytes";

int osd_storage_available(void) { return available; }
void osd_persistence_error(const char *detail) { assert(detail && *detail); ++reports; }
static FILE *fake_open(const char *name,const char *mode)
{++opens;if(fail_open){errno=EIO;return NULL;}return fopen(name,mode);}
static int fake_stat(const char *name,struct stat *info)
{++stats;if(fail_stat){errno=EIO;return -1;}return stat(name,info);}
static int fake_flush(FILE *file)
{++flushes;int r=fflush(file);if(fail_flush){errno=EIO;return -1;}return r;}
static int fake_sync(int fd)
{++syncs;if(fail_sync){errno=EIO;return -1;}return fsync(fd);}
static int fake_close(FILE *file)
{++closes;int r=fclose(file);if(fail_close){errno=EIO;return -1;}return r;}
static int fake_rename(const char *from,const char *to)
{
    ++renames;
    if(renames==rename_failure||renames==rollback_failure){errno=EIO;return -1;}
    struct stat info;
    /* Match FAT rename: overwriting an existing destination is forbidden. */
    if(!stat(to,&info)){errno=EEXIST;return -1;}
    int r=rename(from,to);
    if(!r&&drop_after_backup&&!strcmp(to,backup))available=false;
    return r;
}
static int fake_remove(const char *name)
{
    struct stat info;
    if(fail_backup_cleanup&&!strcmp(name,backup)&&!stat(name,&info)){errno=EIO;return -1;}
    return remove(name);
}
#define fopen fake_open
#define stat(...) fake_stat(__VA_ARGS__)
#define fflush fake_flush
#define fsync fake_sync
#define fclose fake_close
#define rename fake_rename
#define remove fake_remove
#include "../../components/aether_nes/aether_nes_save.c"
#undef fopen
#undef stat
#undef fflush
#undef fsync
#undef fclose
#undef rename
#undef remove

static void write_real(const char *name,const char *bytes)
{FILE*f=fopen(name,"wb");assert(f);assert(fwrite(bytes,1,strlen(bytes),f)==strlen(bytes));assert(!fclose(f));}
static void contents(const char *name,const char *bytes)
{FILE*f=fopen(name,"rb");assert(f);for(size_t i=0;i<strlen(bytes);++i)assert(fgetc(f)==(unsigned char)bytes[i]);assert(fgetc(f)==EOF);assert(!fclose(f));}
static void absent(const char *name)
{struct stat s;assert(stat(name,&s)==-1&&errno==ENOENT);}
static void reset(void)
{
    available=true;fail_open=fail_flush=fail_sync=fail_close=short_write=false;
    drop_after_write=drop_after_backup=fail_backup_cleanup=fail_stat=false;
    opens=stats=renames=flushes=syncs=closes=reports=rename_failure=rollback_failure=0;
    remove(path);remove(temporary);remove(backup);write_real(path,old_data);
}
static int writer(FILE *file,void *context)
{
    assert(context==(void*)new_data);
    const size_t length=strlen(new_data), n=short_write?length/2:length;
    const int result=fwrite(new_data,1,n,file)==length?0:-1;
    if(drop_after_write)available=false;
    return result;
}
static int save(void){return osd_save_file(path,writer,(void*)new_data);}
int main(int argc,char **argv)
{
    assert(argc==2);
    assert(snprintf(path,sizeof(path),"%s/transaction.sav",argv[1])>0);
    assert(snprintf(temporary,sizeof(temporary),"%s.s31tmp",path)>0);
    assert(snprintf(backup,sizeof(backup),"%s.s31bak",path)>0);
    reset();assert(save()==0&&!reports&&opens==1&&flushes==1&&syncs==1&&closes==1&&renames==2);
    contents(path,new_data);absent(temporary);absent(backup);
    puts("PASS actual NES save commits checked closed/synced bytes through FAT-compatible backup/rename without unlinking the old save");
    reset();available=false;assert(save()==-1&&reports==1&&!opens&&!stats&&!renames);contents(path,old_data);
    for(unsigned failure=0;failure<5;++failure){
        reset();
        if(failure==0)fail_open=true;
        if(failure==1)short_write=true;
        if(failure==2)fail_flush=true;
        if(failure==3)fail_sync=true;
        if(failure==4)fail_close=true;
        assert(save()==-1&&reports==1&&!renames);contents(path,old_data);absent(backup);absent(temporary);
        assert(closes==(failure?1U:0U));
    }
    puts("PASS missing-media/open/short-write/flush/sync/close failures preserve old save and close all opened streams before reporting failure");
    reset();rename_failure=1;assert(save()==-1&&reports==1);contents(path,old_data);absent(backup);
    reset();rename_failure=2;assert(save()==-1&&reports==1&&renames==3);contents(path,old_data);absent(backup);
    reset();rename_failure=2;rollback_failure=3;assert(save()==-1&&reports==1);absent(path);contents(backup,old_data);
    rename_failure=rollback_failure=0;assert(!osd_restore_save(path));contents(path,old_data);absent(backup);
    puts("PASS each rename/rollback failure retains a valid canonical or backup save; loading restores the old backup after interrupted commit");
    reset();drop_after_write=true;assert(save()==-1&&reports==1&&!renames&&closes==1);contents(path,old_data);
    reset();drop_after_backup=true;assert(save()==-1&&reports==1&&renames==1);absent(path);contents(backup,old_data);contents(temporary,new_data);
    available=true;drop_after_backup=false;assert(!osd_restore_save(path));contents(path,old_data);assert(!save());contents(path,new_data);
    puts("PASS hot-unplug before commit never starts another rename, retains the old backup and never promotes an uncommitted temporary file");
    reset();fail_backup_cleanup=true;assert(save()==-1&&reports==1);contents(path,new_data);contents(backup,old_data);
    fail_backup_cleanup=false;assert(!osd_restore_save(path));contents(path,new_data);assert(!save());absent(backup);
    reset();fail_stat=true;assert(save()==-1&&reports==1&&!opens);contents(path,old_data);
    reset();assert(!remove(path)&&!mkdir(path,0700));assert(save()==-1&&reports==1&&!opens);assert(!rmdir(path));
    char long_path[PATH_MAX+2];memset(long_path,'a',sizeof(long_path)-1);long_path[sizeof(long_path)-1]=0;
    reset();assert(osd_save_file(long_path,writer,(void*)new_data)==-1&&reports==1&&!opens&&!stats);
    remove(path);remove(temporary);remove(backup);
    puts("PASS cleanup/stat/path errors are reported without replacing directories or overflowing paths; successful new save remains authoritative with a stale backup");
}

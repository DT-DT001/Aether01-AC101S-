from pathlib import Path
import re
root=Path(__file__).resolve().parents[2]
out=root/'build/host_usb_storage_driver';out.mkdir(parents=True,exist_ok=True)
def function(text,name):
    match=re.search(r'^(?:static )?(?:esp_err_t|void|BYTE)\s+'+re.escape(name)+r'\s*\(',text,re.M)
    assert match,name
    at=match.start();begin=at
    brace=text.index('{',at);depth=0
    for end in range(brace,len(text)):
        if text[end]=='{':depth+=1
        elif text[end]=='}':
            depth-=1
            if not depth:return text[begin:end+1]+'\n'
    raise AssertionError(name)
fat=(root/'components/fatfs/vfs/vfs_fat.c').read_text(encoding='utf-8')
bdl=(root/'components/fatfs/vfs/vfs_fat_bdl.c').read_text(encoding='utf-8')
msc=(root/'components/aether_usb_storage/vendor/usb_host_msc/src/msc_host_vfs.c').read_text(encoding='utf-8')
parts=[function(fat,'esp_vfs_fat_unregister_path'),function(bdl,'esp_vfs_fat_bdl_unmount'),function(msc,'dealloc_msc_vfs'),function(msc,'msc_host_vfs_register'),function(msc,'msc_host_vfs_unregister')]
(out/'actual_vfs.inc').write_text('\n'.join(parts),encoding='utf-8')

sd=(root/'components/fatfs/vfs/vfs_fat_sdmmc.c').read_text(encoding='utf-8')
disk=(root/'components/fatfs/diskio/diskio_sdmmc.c').read_text(encoding='utf-8')
parts=[function(disk,n) for n in ['ff_diskio_get_pdrv_card','ff_diskio_get_pdrv_cnt_card','ff_diskio_clear_pdrv_card']]
parts += [function(sd,n) for n in ['esp_vfs_fat_mount_internal','esp_vfs_fat_save_ctx','esp_vfs_fat_mount_initialized','unmount_card_core','esp_vfs_fat_sdcard_unmount']]
(out/'actual_sd.inc').write_text('\n'.join(parts),encoding='utf-8')

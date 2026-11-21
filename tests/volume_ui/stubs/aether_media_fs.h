#pragma once
#include "app_io.h"
#include <string.h>
static inline bool aether_media_fs_path_ready(aether_storage_handle_t tf, const char *path)
{ return path && !strncmp(path,"/sdcard",7) && aether_storage_is_mounted(tf) && !app_io_usb_storage_blocked(NULL); }
static inline const char *aether_media_fs_default_root(aether_storage_handle_t tf)
{ (void)tf; return "/sdcard"; }

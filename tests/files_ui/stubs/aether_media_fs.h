#define aether_media_fs_path_ready tf_media_path_ready
#define aether_media_fs_generation tf_media_generation
#include "../../support/tf_media_fs.h"
#undef aether_media_fs_generation
#undef aether_media_fs_path_ready
uint32_t aether_media_fs_generation(aether_storage_handle_t,const char*);
bool aether_media_fs_path_ready(aether_storage_handle_t,const char*);

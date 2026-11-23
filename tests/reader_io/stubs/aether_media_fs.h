#define aether_media_fs_default_root tf_media_default_root
#define aether_media_fs_path_ready tf_media_path_ready
#define aether_media_fs_generation tf_media_generation
#define aether_media_fs_safe_path tf_media_safe_path
#define aether_media_fs_acquire tf_media_acquire
#include "../../support/tf_media_fs.h"
#undef aether_media_fs_default_root
#undef aether_media_fs_path_ready
#undef aether_media_fs_generation
#undef aether_media_fs_safe_path
#undef aether_media_fs_acquire
const char *aether_media_fs_default_root(aether_storage_handle_t);
bool aether_media_fs_path_ready(aether_storage_handle_t, const char *);
uint32_t aether_media_fs_generation(aether_storage_handle_t, const char *);
bool aether_media_fs_safe_path(const char *);
esp_err_t aether_media_fs_acquire(aether_storage_handle_t, const char *, aether_media_fs_lease_t *);

#define AETHER_TEST_MEDIA_READY aether_storage_card_inserted
#define aether_media_fs_path_ready tf_media_path_ready
#define aether_media_fs_safe_path tf_media_safe_path
#include "../../support/tf_media_fs.h"
#undef aether_media_fs_path_ready
#undef aether_media_fs_safe_path
/* Player fixtures are ordinary host temporary files, not a mounted VFS. */
static inline bool aether_media_fs_path_ready(aether_storage_handle_t tf,const char *p){(void)p;return aether_storage_card_inserted(tf);}
static inline bool aether_media_fs_safe_path(const char *path)
{
    static const char prefix[] = "/tmp/aether_music_resume_";
    return tf_media_safe_path(path) || (path && !strncmp(path, prefix, sizeof(prefix) - 1) &&
           path[sizeof(prefix) - 1] && !strpbrk(path + sizeof(prefix) - 1, "/\\"));
}

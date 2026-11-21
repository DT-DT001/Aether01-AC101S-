#define AETHER_TEST_MEDIA_READY aether_storage_card_inserted
#define aether_media_fs_path_ready tf_media_path_ready
#include "../../support/tf_media_fs.h"
#undef aether_media_fs_path_ready
/* Player fixtures are ordinary host temporary files, not a mounted VFS. */
static inline bool aether_media_fs_path_ready(aether_storage_handle_t tf,const char *p){(void)p;return aether_storage_card_inserted(tf);}

#include "aether_media_fs.h"
#include <string.h>

static bool under(const char *path, const char *root)
{
    if (!path) return false;
    const size_t n = strlen(root);
    return !strncmp(path, root, n) && (!path[n] || path[n] == '/');
}
bool aether_media_fs_safe_path(const char *path)
{
    const char *root = under(path, AETHER_STORAGE_MOUNT_POINT) ? AETHER_STORAGE_MOUNT_POINT :
        under(path, AETHER_USB_STORAGE_MOUNT_POINT) ? AETHER_USB_STORAGE_MOUNT_POINT : NULL;
    if (!root) return false;
    for (const char *part = path + strlen(root); *part;) {
        if (*part++ != '/') return false;
        const char *end = strchr(part, '/');
        const size_t n = end ? (size_t)(end - part) : strlen(part);
        if (!n || (n == 1 && *part == '.') || (n == 2 && part[0] == '.' && part[1] == '.') ||
            memchr(part, '\\', n)) return false;
        part += n;
    }
    return true;
}
bool aether_media_fs_path_ready(aether_storage_handle_t tf, const char *path)
{
    if (!aether_media_fs_safe_path(path)) return false;
    if (under(path, AETHER_STORAGE_MOUNT_POINT)) return tf && aether_storage_is_mounted(tf);
    aether_usb_storage_status_t status = {0};
    return aether_usb_storage_get_status(aether_usb_storage_default(), &status) == ESP_OK &&
        status.mounted && !status.draining;
}
uint32_t aether_media_fs_generation(aether_storage_handle_t tf, const char *path)
{
    (void)tf;
    if (!aether_media_fs_safe_path(path)) return 0;
    if (under(path, AETHER_STORAGE_MOUNT_POINT)) return tf ? aether_storage_generation(tf) : 0;
    if (!under(path, AETHER_USB_STORAGE_MOUNT_POINT)) return 0;
    aether_usb_storage_status_t status = {0};
    return aether_usb_storage_get_status(aether_usb_storage_default(), &status) == ESP_OK ? status.generation : 0;
}
const char *aether_media_fs_default_root(aether_storage_handle_t tf)
{
    if (tf && aether_storage_is_mounted(tf)) return AETHER_STORAGE_MOUNT_POINT;
    if (aether_media_fs_path_ready(tf, AETHER_USB_STORAGE_MOUNT_POINT)) return AETHER_USB_STORAGE_MOUNT_POINT;
    return AETHER_STORAGE_MOUNT_POINT;
}
esp_err_t aether_media_fs_acquire(aether_storage_handle_t tf, const char *path,
                                  aether_media_fs_lease_t *lease)
{
    if (!lease || lease->held || !aether_media_fs_safe_path(path)) return ESP_ERR_INVALID_ARG;
    esp_err_t error;
    if (under(path, AETHER_STORAGE_MOUNT_POINT)) {
        if (!tf) return ESP_ERR_INVALID_STATE;
        error = aether_storage_acquire(tf, 0);
        if (error == ESP_OK) lease->tf = tf;
    } else {
        error = aether_usb_storage_acquire(aether_usb_storage_default(), &lease->usb);
        if (error == ESP_OK) lease->tf = NULL;
    }
    lease->held = error == ESP_OK;
    return error;
}
bool aether_media_fs_available(const aether_media_fs_lease_t *lease)
{
    return lease && lease->held && (lease->tf ? aether_storage_card_inserted(lease->tf) :
        aether_usb_storage_is_available(&lease->usb));
}
void aether_media_fs_release(aether_media_fs_lease_t *lease)
{
    if (!lease || !lease->held) return;
    if (lease->tf) aether_storage_release(lease->tf);
    else aether_usb_storage_release(&lease->usb);
    memset(lease, 0, sizeof(*lease));
}

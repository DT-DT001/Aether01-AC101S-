#include <fcntl.h>
#include <time.h>
#include <unistd.h>

/* Real 1×1 grayscale baseline JPEG and 24-bit BMP. */
static const uint8_t fixture_jpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09,
    0x09, 0x08, 0x0a, 0x0c, 0x14, 0x0d, 0x0c, 0x0b, 0x0b, 0x0c, 0x19, 0x12,
    0x13, 0x0f, 0x14, 0x1d, 0x1a, 0x1f, 0x1e, 0x1d, 0x1a, 0x1c, 0x1c, 0x20,
    0x24, 0x2e, 0x27, 0x20, 0x22, 0x2c, 0x23, 0x1c, 0x1c, 0x28, 0x37, 0x29,
    0x2c, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1f, 0x27, 0x39, 0x3d, 0x38, 0x32,
    0x3c, 0x2e, 0x33, 0x34, 0x32, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00,
    0x3b, 0xff, 0xd9,
};

static const uint8_t fixture_bmp[58] = {
    [0] = 'B', [1] = 'M', [2] = 58, [10] = 54, [14] = 40,
    [18] = 1, [22] = 1, [26] = 1, [28] = 24, [34] = 4, [56] = 255,
};

static void fixture_path(char path[256], const char *root, const char *name)
{
    const int length = snprintf(path, 256, "%s/%s", root, name);
    assert(length > 0 && length < 256);
}

static void write_photo_fixture(const char *root, const char *name, const void *data, size_t size, time_t modified)
{
    char path[256];
    fixture_path(path, root, name);
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(data, 1, size, file) == size && !fclose(file));
    const struct timespec stamp[2] = {{.tv_sec = modified}, {.tv_sec = modified}};
    assert(!utimensat(AT_FDCWD, path, stamp, 0));
}

static app_io_result_t gallery_result(app_io_handle_t io)
{
    return (app_io_result_t){
        .operation = APP_IO_CATALOG,
        .generation = atomic_load(&io->latest_generation),
        .request_id = atomic_load(&io->latest_id),
    };
}

static app_io_handle_t gallery_scan_io;
static void cancel_scan_on_yield(void)
{
    app_io_cancel(gallery_scan_io, atomic_load(&gallery_scan_io->latest_generation) + 1U);
    delay_hook = NULL;
}

static void test_gallery_catalog(app_io_handle_t io)
{
    char tf[] = "/tmp/aether-gallery-tf-XXXXXX";
    assert(mkdtemp(tf) && mkdtemp(gallery_usb_root));
    char camera[256], album[256];
    fixture_path(camera, tf, "DCIM");
    fixture_path(album, camera, "旅行");
    assert(!mkdir(camera, 0700) && !mkdir(album, 0700));
    write_photo_fixture(album, "Photo.JPG", fixture_jpeg, sizeof(fixture_jpeg), 1000);
    write_photo_fixture(tf, "tail.bmp", fixture_bmp, sizeof(fixture_bmp), 1001);
    write_photo_fixture(gallery_usb_root, "tail.bmp", fixture_bmp, sizeof(fixture_bmp), 2000);
    write_photo_fixture(gallery_usb_root, "short.bmp", fixture_bmp, 54, 3000);
    write_photo_fixture(album, "renamed.jpg", fixture_bmp, sizeof(fixture_bmp), 3000);
    write_photo_fixture(album, "still-not-a-photo.mp4", fixture_jpeg, sizeof(fixture_jpeg), 3000);
    uint8_t progressive[sizeof(fixture_jpeg)];
    memcpy(progressive, fixture_jpeg, sizeof(progressive));
    progressive[90] = 0xc2;
    write_photo_fixture(album, "progressive.jpeg", progressive, sizeof(progressive), 3000);

    gallery_path_t *dirs = calloc(GALLERY_MAX_DIRS, sizeof(*dirs));
    assert(dirs);
    size_t capacity = 0;
    app_io_result_t r = gallery_result(io);
    assert(aether_media_fs_acquire(io->storage, tf, &io->media_lease) == ESP_OK);
    assert(scan_gallery_root(io, tf, dirs, &r, &capacity, 2) == ESP_OK);
    aether_media_fs_release(&io->media_lease);
    assert(r.count == 2 && r.total_count == 2 && !r.truncated);
    char expected[256];
    fixture_path(expected, album, "Photo.JPG");
    assert(!strcmp(r.entries[0].path, expected));
    assert(aether_media_fs_acquire(io->storage, gallery_usb_root, &io->media_lease) == ESP_OK);
    assert(scan_gallery_root(io, gallery_usb_root, dirs, &r, &capacity, 2) == ESP_OK);
    aether_media_fs_release(&io->media_lease);
    qsort(r.entries, r.count, sizeof(*r.entries), newest_photo);
    fixture_path(expected, gallery_usb_root, "tail.bmp");
    assert(r.count == 2 && r.total_count == 3 && r.truncated && !strcmp(r.entries[0].path, expected));
    fixture_path(expected, tf, "tail.bmp");
    assert(!strcmp(r.entries[1].path, expected));
    app_io_result_free(&r);
    free(dirs);

    const app_io_request_t request = {.operation = APP_IO_CATALOG, .filter = APP_IO_IMAGES};
    r = gallery_result(io);
    assert(perform_request(io, &request, &r) == ESP_ERR_NOT_FOUND && !r.entries);
    gallery_usb_ready = true;
    assert(perform_request(io, &request, &r) == ESP_OK);
    assert(r.count == 1 && r.next_offset == 1 && !r.truncated && !io->media_lease.held);
    app_io_result_free(&r);
    r = gallery_result(io);
    gallery_advance_generation_on_release = true;
    assert(perform_request(io, &request, &r) == ESP_ERR_INVALID_STATE && !io->media_lease.held);
    gallery_advance_generation_on_release = false;
    app_io_result_free(&r);
    r = gallery_result(io);
    assert(perform_request(io, &request, &r) == ESP_OK && r.count == 1);
    app_io_result_free(&r);

    char filename[20];
    for (unsigned i = 0; i < 40; ++i) {
        snprintf(filename, sizeof(filename), "ignore-%02u.txt", i);
        write_photo_fixture(gallery_usb_root, filename, "", 0, 4000);
    }
    r = gallery_result(io);
    gallery_scan_io = io;
    delay_hook = cancel_scan_on_yield;
    assert(perform_request(io, &request, &r) == ESP_ERR_INVALID_STATE && !io->media_lease.held && !delay_hook);
    assert(!request_current(io, &r));
    app_io_result_free(&r);
    gallery_scan_io = NULL;
    gallery_usb_ready = false;

    for (unsigned i = 0; i < 40; ++i) {
        snprintf(filename, sizeof(filename), "ignore-%02u.txt", i);
        fixture_path(expected, gallery_usb_root, filename);
        assert(!unlink(expected));
    }
    const char *const album_files[] = {"Photo.JPG", "renamed.jpg", "still-not-a-photo.mp4", "progressive.jpeg"};
    for (size_t i = 0; i < sizeof(album_files) / sizeof(album_files[0]); ++i) {
        fixture_path(expected, album, album_files[i]);
        assert(!unlink(expected));
    }
    fixture_path(expected, tf, "tail.bmp");
    assert(!unlink(expected));
    fixture_path(expected, gallery_usb_root, "tail.bmp");
    assert(!unlink(expected));
    fixture_path(expected, gallery_usb_root, "short.bmp");
    assert(!unlink(expected));
    assert(!rmdir(album) && !rmdir(camera) && !rmdir(tf) && !rmdir(gallery_usb_root));
    puts("PASS gallery I/O recursively filters real 1×1 JPEG/BMP, keeps newest across both roots, discards an old USB generation and cancels during a scan");
}

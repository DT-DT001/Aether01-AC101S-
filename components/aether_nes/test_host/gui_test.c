/* Include production code so the test can exercise its private drawing helpers. */
#include "../vendor/nofrendo/gui.c"
#include <assert.h>

int main(void)
{
    enum { WIDTH = 256, HEIGHT = 224 };
    uint8 pixels[WIDTH * HEIGHT + 16];
    memset(pixels, 0xa5, sizeof(pixels));
    bitmap_t *surface = malloc(sizeof(*surface) + HEIGHT * sizeof(uint8 *));
    assert(surface);
    surface->width = WIDTH;
    surface->height = HEIGHT;
    for (unsigned i = 0; i < HEIGHT; ++i) surface->line[i] = pixels + 8 + i * WIDTH;
    gui_surface = surface;
    char utf8[] = "ROM \xe4\xb8\xad\xe6\x96\x87\n\x01.nes";
    assert(gui_textlen(utf8, &small) > 0);
    gui_textout(utf8, 250, 220, &small, 0);
    gui_textout(utf8, -20, -3, &small, 0);
    char long_message[1024];
    memset(long_message, 'X', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = 0;
    gui_sendmsg(GUI_GREEN, "%s", long_message);
    assert(strlen(msg.text) == sizeof(msg.text) - 1);
    gui_textbar(msg.text, 2, HEIGHT - 10, &small, GUI_GREEN, GUI_DKGRAY, BUTTON_UP);
    for (unsigned i = 0; i < 8; ++i) {
        assert(pixels[i] == 0xa5);
        assert(pixels[WIDTH * HEIGHT + 8 + i] == 0xa5);
    }
    free(surface);
    puts("NES overlay: UTF-8/control characters, long-message formatting and clipped drawing passed");
    return 0;
}

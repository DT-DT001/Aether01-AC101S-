/* Original, generated NROM fixture: no third-party ROM or copyrighted game.
 * It changes the universal backdrop each vblank, increments zero-page RAM and
 * programs a square wave. All CPU/PPU/APU/mapper/event/video code is production. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bitmap.h>
#include <event.h>
#include <nes.h>
#include <nesinput.h>
#include <nofconfig.h>
#include <nofrendo.h>
#include <osd.h>

static uint8_t rom[16 + 16384 + 8192];
static char rom_name[512], config_name[512];
static bitmap_t *surface;
static void (*process_audio)(void *, int);
static unsigned frames, displayed, audio_samples, changed_frames, target_frames;
static unsigned idle_calls, paused_waits;
static uint32_t previous_hash;
static bool varied_audio, exercise_controls;
static int16_t first_sample;
static unsigned saved_ram_counter;
static ppu_t ppu_snapshot;

static int video_init(int width, int height) { (void)width; (void)height; return 0; }
static void video_shutdown(void) { bmp_destroy(&surface); }
static int video_mode(int width, int height) { (void)width; (void)height; return 0; }
static void video_palette(rgb_t *palette) { assert(palette); }
static void video_clear(uint8 color) { (void)color; }
static bitmap_t *video_lock(void)
{ if (!surface) surface = bmp_create(256, 240, 8); return surface; }
static void video_unlock(int dirties, rect_t *rectangles) { (void)dirties; (void)rectangles; }
static void video_blit(bitmap_t *bitmap, int dirties, rect_t *rectangles)
{
    (void)dirties; (void)rectangles;
    assert(bitmap && bitmap->width == 256 && bitmap->height == 240);
    uint32_t hash = 2166136261U;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 256; ++x) hash = (hash ^ bitmap->line[y][x]) * 16777619U;
    if (displayed && hash != previous_hash) ++changed_frames;
    previous_hash = hash;
    ++displayed;
}
static viddriver_t video_driver = { "Host fixture", video_init, video_shutdown,
    video_mode, video_palette, video_clear, video_lock, video_unlock, video_blit, false };

char *osd_getromdata(void) { return (char *)rom; }
int osd_storage_available(void) { return 1; }
void osd_persistence_error(const char *detail) { (void)detail; assert(!"unexpected save error"); }
void osd_getvideoinfo(vidinfo_t *info)
{ *info = (vidinfo_t){.default_width = 256, .default_height = 240, .driver = &video_driver}; }
void osd_getsoundinfo(sndinfo_t *info) { info->sample_rate = 48000; info->bps = 16; }
void osd_setsound(void (*process)(void *, int)) { process_audio = process; }
void osd_emulated_frame(int drawn)
{
    (void)drawn;
    int16_t samples[800];
    assert(process_audio);
    process_audio(samples, 800);
    for (unsigned i = 0; i < 800; ++i) {
        if (audio_samples && samples[i] != first_sample) varied_audio = true;
        if (!audio_samples) first_sample = samples[i];
        ++audio_samples;
    }
    ++frames;
}
void osd_frame_begin(void) {}
void osd_frame_end(void) {}
void osd_waitframe(void)
{
    assert(++idle_calls < 10000);
    if (nes_getcontextptr()->pause) {
        ++paused_waits;
        if (paused_waits == 3) event_get(event_togglepause)(INP_STATE_MAKE);
    }
    atomic_fetch_add(&nofrendo_ticks, 1);
}
void osd_getinput(void)
{
    if (frames == 1) {
        /* Real PPU controller ports: high strobe repeatedly reads live A,
         * falling edge starts eight button bits, then standard pads stay high. */
        event_get(event_joypad1_a)(INP_STATE_MAKE);
        ppu_writehigh(PPU_JOY0, 1);
        for (unsigned i = 0; i < 64; ++i) {
            assert(ppu_readhigh(PPU_JOY0) == 0x41);
            assert(ppu_readhigh(PPU_JOY1) == 0x40);
        }
        event_get(event_joypad1_a)(INP_STATE_BREAK);
        assert(ppu_readhigh(PPU_JOY0) == 0x40);
        event_get(event_joypad1_a)(INP_STATE_MAKE);
        ppu_writehigh(PPU_JOY0, 0);
        assert(ppu_readhigh(PPU_JOY0) == 0x41);
        for (unsigned i = 1; i < 8; ++i) assert(ppu_readhigh(PPU_JOY0) == 0x40);
        for (unsigned i = 0; i < 1000; ++i) assert(ppu_readhigh(PPU_JOY0) == 0x41);
        event_get(event_joypad1_a)(INP_STATE_BREAK);
        input_strobe();
        ppu_getcontext(&ppu_snapshot);
        for (unsigned page = 8; page < 16; ++page) {
            const uintptr_t backing = (uintptr_t)ppu_snapshot.page[page] + page * 1024U;
            assert(backing >= (uintptr_t)ppu_snapshot.nametab);
            assert(backing - (uintptr_t)ppu_snapshot.nametab < sizeof(ppu_snapshot.nametab));
        }
    }
    if (exercise_controls && frames == 8) event_get(event_togglepause)(INP_STATE_MAKE);
    if (exercise_controls && frames == 20) event_get(event_hard_reset)(INP_STATE_MAKE);
    if (frames >= target_frames) {
        saved_ram_counter = nes_getcontextptr()->cpu->mem_page[0][0];
        /* Leave an input pressed on purpose; the next session must reset it. */
        event_get(event_joypad1_a)(INP_STATE_MAKE);
        main_quit();
    }
}
int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{ (void)func; (void)funcsize; (void)counter; (void)countersize; assert(frequency == 60); return 0; }
int osd_init(void) { return 0; }
void osd_shutdown(void) { process_audio = NULL; }
int osd_main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    config.filename = config_name;
    /* An old held button must not leak between independent games. */
    input_strobe();
    assert(!(input_get(INP_JOYPAD0) & 1));
    return main_loop(rom_name, system_nes);
}
void osd_getmouse(int *x, int *y, int *button) { *x = *y = *button = 0; }
void osd_fullname(char *full, const char *name) { snprintf(full, PATH_MAX, "%s", name); }
char *osd_newextension(char *name, char *extension)
{
    char *dot = strrchr(name, '.');
    if (!dot) dot = name + strlen(name);
    snprintf(dot, PATH_MAX - (size_t)(dot - name), "%s", extension);
    return name;
}
int osd_makesnapname(char *filename, int length) { (void)filename; (void)length; return -1; }

static void build_fixture(void)
{
    memcpy(rom, "NES\x1a", 4);
    rom[4] = 1; rom[5] = 1;
    /* This simple program is authored specifically for this regression. */
    const uint8_t code[] = {
        0x78, 0xd8, 0xa2, 0xff, 0x9a, /* SEI; CLD; LDX #$ff; TXS */
        0xa9, 0x00, 0x8d, 0x00, 0x20, 0x8d, 0x01, 0x20,
        0xa9, 0x01, 0x8d, 0x15, 0x40,
        0xa9, 0x9f, 0x8d, 0x00, 0x40,
        0xa9, 0xfd, 0x8d, 0x02, 0x40,
        0xa9, 0x08, 0x8d, 0x03, 0x40,
        0xa9, 0x08, 0x8d, 0x01, 0x20,
        /* $8026: poll PPU vblank, count it, write changing backdrop palette. */
        0xad, 0x02, 0x20, 0x10, 0xfb,
        0xe6, 0x00,
        0xa9, 0x3f, 0x8d, 0x06, 0x20,
        0xa9, 0x00, 0x8d, 0x06, 0x20,
        0xa5, 0x00, 0x29, 0x3f, 0x8d, 0x07, 0x20,
        0x4c, 0x26, 0x80
    };
    memcpy(rom + 16, code, sizeof(code));
    /* NMI/RESET/IRQ all point to the start; IRQ is masked by SEI. */
    for (unsigned vector = 0x3ffa; vector <= 0x3ffe; vector += 2) {
        rom[16 + vector] = 0;
        rom[16 + vector + 1] = 0x80;
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(rom_name, sizeof(rom_name), "%s/original-fixture.nes", argv[1]);
    snprintf(config_name, sizeof(config_name), "%s/original-fixture.cfg", argv[1]);
    build_fixture();
    for (unsigned session = 0; session < 101; ++session) {
        frames = displayed = audio_samples = changed_frames = idle_calls = paused_waits = 0;
        varied_audio = false;
        exercise_controls = session == 0;
        target_frames = exercise_controls ? 64 : 3;
        assert(nofrendo_main(0, NULL) == 0);
        assert(frames == target_frames && displayed == target_frames);
        assert(audio_samples == target_frames * 800 && varied_audio);
        assert(saved_ram_counter > 0);
        if (exercise_controls) assert(changed_frames > 10 && paused_waits == 3);
        assert(!surface);
    }
    /* Exact historical dirty header: the genuine production core must keep
     * using mapper 0 instead of interpreting ASCII 'D' as mapper bits. */
    memcpy(rom+7,"DiskDude!",9);
    frames = displayed = audio_samples = idle_calls = paused_waits = 0;
    varied_audio = false;
    assert(nofrendo_main(0,NULL)==0 && frames==target_frames && varied_audio && !surface);
    memset(rom+7,0,9);
    /* Unsupported mapper initialization must clean up fully and permit the
     * following valid ROM to start, rather than retaining a half machine. */
    frames = displayed = audio_samples = 0;
    rom[6] = rom[7] = 0xf0;
    assert(nofrendo_main(0, NULL) != 0 && !frames && !surface);
    rom[6] = rom[7] = 0;
    idle_calls = paused_waits = 0;
    assert(nofrendo_main(0, NULL) == 0 && frames == target_frames && !surface);
    remove(config_name);
    puts("NES full production core: original NROM CPU/PPU/APU/mapper, NTSC DiskDude, pause/reset/audio/video and 101 relaunches passed");
    return 0;
}

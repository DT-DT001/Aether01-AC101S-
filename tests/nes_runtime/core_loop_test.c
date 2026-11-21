/* Exercise the production emulation loop, with only CPU/PPU and platform I/O
 * replaced. A missing idle yield or a runaway loop fails deterministically. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <nes.h>
#include <nofrendo.h>

_Atomic(uint32_t) nofrendo_ticks;
static apu_t apu;
static mmc_t mmc;
static mapintf_t mapper;
static unsigned waits, frames, blits, input_calls, scanlines, gui_ticks;
static unsigned expected_frames, initial_ticks;
static unsigned drawn_mask;
static bool pause_case;
static unsigned ticks_per_audio_frame, current_skips, maximum_skips, stop_at_frame;
static bool pause_during_run;

static void fake_process(void *buffer, int samples) { (void)buffer; (void)samples; }
void osd_setsound(void (*process)(void *, int)) { assert(process == fake_process); }
void osd_waitframe(void)
{
    ++waits;
    assert(waits < 10);
    if (ticks_per_audio_frame && nes_getcontextptr()->pause) {
        atomic_fetch_add(&nofrendo_ticks,120); /* Two seconds of paused time. */
        nes_togglepause();
        return;
    }
    if (pause_case) {
        assert(frames == 0);
        if (waits == 1) atomic_fetch_add(&nofrendo_ticks, 5);
        if (waits == 2) {
            nes_togglepause();
            atomic_fetch_add(&nofrendo_ticks, 1);
        }
    } else if (waits == 3) {
        atomic_fetch_add(&nofrendo_ticks, initial_ticks);
    }
}
void osd_emulated_frame(int drawn)
{
    assert(frames < expected_frames);
    if (drawn && frames < 32) drawn_mask |= 1U << frames;
    current_skips = drawn ? 0 : current_skips + 1;
    if (current_skips > maximum_skips) maximum_skips = current_skips;
    ++frames;
    /* Actual production loop; the PCM output clock is replaced by a sink
     * consuming one NTSC frame in one (or two overloaded) 60Hz tick periods. */
    atomic_fetch_add(&nofrendo_ticks,ticks_per_audio_frame);
    if (frames == expected_frames) nes_poweroff();
}
void osd_frame_begin(void) {}
void osd_frame_end(void) {}
void osd_getinput(void)
{
    ++input_calls;
    if (stop_at_frame && frames == stop_at_frame) nes_poweroff();
    if (pause_during_run && frames == 100) { pause_during_run=false; nes_togglepause(); }
}
int nes6502_execute(int cycles) { return cycles; }
void nes6502_irq(void) {}
uint8 ppu_read(uint32 address) { (void)address; return 0; }
uint8 ppu_readhigh(uint32 address) { (void)address; return 0; }
uint8 apu_read(uint32 address) { (void)address; return 0; }
void ppu_write(uint32 address, uint8 value) { (void)address; (void)value; }
void ppu_writehigh(uint32 address, uint8 value) { (void)address; (void)value; }
void apu_write(uint32 address, uint8 value) { (void)address; (void)value; }
void ppu_scanline(bitmap_t *bitmap, int scanline, bool draw)
{
    (void)bitmap; (void)draw;
    assert(scanline == (int)(scanlines % 262));
    ++scanlines;
}
void ppu_endscanline(int scanline) { (void)scanline; }
void ppu_checknmi(void) {}
bitmap_t *vid_getbuffer(void) { return NULL; }
void vid_flush(void) { ++blits; }
void gui_frame(bool draw) { (void)draw; }
void gui_tick(int ticks) { gui_ticks += (unsigned)ticks; }

static void run(unsigned tick_base, unsigned ticks, bool autoframeskip,
                bool paused, unsigned expected, unsigned expected_mask)
{
    nes_t *machine = nes_getcontextptr();
    memset(machine, 0, sizeof(*machine));
    machine->apu = &apu;
    machine->mmc = &mmc;
    machine->autoframeskip = autoframeskip;
    machine->pause = paused;
    apu.process = fake_process;
    mmc.intf = &mapper;
    waits = frames = blits = input_calls = scanlines = gui_ticks = drawn_mask = 0;
    expected_frames = expected;
    initial_ticks = ticks;
    pause_case = paused;
    ticks_per_audio_frame = current_skips = maximum_skips = stop_at_frame = 0;
    pause_during_run = false;
    atomic_store(&nofrendo_ticks, tick_base);
    nes_emulate();
    assert(frames == expected);
    assert(drawn_mask == expected_mask);
    assert(scanlines == expected * 262);
    assert(input_calls == expected); /* Also serviced on skipped frames. */
    assert(blits == (paused || !autoframeskip ? expected : 1));
    assert(waits == (paused ? 2U : 3U));
}

static void run_backpressure(unsigned sink_ticks, unsigned tick_base, bool pause, unsigned stop_frame)
{
    nes_t *machine=nes_getcontextptr();
    memset(machine,0,sizeof(*machine));
    machine->apu=&apu;machine->mmc=&mmc;machine->autoframeskip=true;
    apu.process=fake_process;mmc.intf=&mapper;
    waits=frames=blits=input_calls=scanlines=gui_ticks=drawn_mask=0;
    current_skips=maximum_skips=0;
    expected_frames=3600;initial_ticks=3;pause_case=false;
    ticks_per_audio_frame=sink_ticks;pause_during_run=pause;stop_at_frame=stop_frame;
    atomic_store(&nofrendo_ticks,tick_base);
    nes_emulate();
    const unsigned completed=stop_frame?stop_frame:expected_frames;
    assert(frames==completed && input_calls==completed && scanlines==completed*262);
    assert(maximum_skips<=2 && blits>=completed/3);
    if(sink_ticks==1 && !pause) {
        /* Keeping the I2S-paced clock must not introduce an extra idle tick
         * after each forced display and reduce emulation from 60 to 45Hz. */
        assert(waits==3 && blits==completed-2);
        assert((uint32_t)(atomic_load(&nofrendo_ticks)-tick_base)==completed+3);
    }
}

int main(void)
{
    run(0, 1, true, false, 1, 1); /* Idle does not simulate ahead of the timer. */
    run(0, 3, true, false, 3, 4); /* All emulated frames get an audio hook. */
    run(0, 3, false, false, 3, 7); /* Disabling skip still respects timer. */
    run(UINT32_MAX - 1U, 3, true, false, 3, 4); /* Tick wrap. */
    run(0, 1000, true, false, 3, 4); /* Discard obsolete catch-up debt after bounded skips. */
    assert(gui_ticks == 12);
    run(0, 0, true, true, 1, 1); /* Paused ticks never become a resume burst. */
    run_backpressure(1,0,false,0); /* A constant debt must not starve display. */
    run_backpressure(1,UINT32_MAX-20U,false,0); /* Same with timer rollover. */
    run_backpressure(2,0,false,0); /* Sustained slow core/output still presents. */
    run_backpressure(1,0,true,0); /* Paused wall time does not become long skip debt. */
    run_backpressure(2,0,false,200); /* Stop/input works on an undisplayed frame. */
    puts("NES production core loop: idle/skip/audio hook/pause/tick-wrap + 3600-frame I2S backpressure/slow-frame/no-video-starvation/stop passed");
    return 0;
}

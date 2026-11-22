# Full-screen media UI verification

Run `python3 tests/media_ui/build_run.py` with the existing WSL/Linux GCC.
The runner compiles production `main/apps/app_media.c` and `main/app_frames.c`,
links the real project LVGL and supplied Chinese fonts, and reuses compatible
native reader-suite object caches. No tools are installed or flashed.

The real 480x480 RGB565 display and pointer gestures verify:

- Video CONTAIN fills the available screen proportionally. A 320x180 source
  renders in a fitted 480x270 image with actual black pixels above and below.
  A continuous 66-frame stream with advancing position reproduces the old
  draw-event activity bug and verifies that redraws never reset idle timing.
  After hiding, a real frame flushes 131,520 pixels at y=103..376, including the
  two-pixel AA margin, instead of all 230,400 screen pixels. Semi-transparent
  controls hide after three idle seconds and a real tap restores them. A swipe
  does not count as that tap; pause/error retain controls. A press crossing the
  status timer cannot reveal and then immediately hide the controls again.
- Real volume dragging, persist-on-release and mute use system volume. Controls
  do not bubble into the background toggle. Holding progress for 3.5 seconds
  during a moving stream cannot auto-hide or seek before actual release.
- Known MP4 progress uses actual status duration/position. The final drag target
  survives three simulated BUSY results. The synchronous seek reset clears real
  pending frames before an immediate paused target preview, which remains visible.
  FINISHED and unknown MJPEG durations disable seeking instead of faking progress.
- Rejected seek submission retains a distinct API error notice; an accepted seek
  that later fails preserves the complete backend error and a working retry.
  This checks UI wiring, not actual decoder-reset behavior on S31.
- Prepare retry repeats the preparation barrier; startup/format errors remain
  readable in full and preserve back/stop/retry. MJPEG native/fixed rate controls
  retain their actual API wiring.
- Video and NES each scroll through a 96-entry catalog using 12 reused rows.
  A real pointer tap on the last entry submits its source path to preparation
  without an extra directory scan.
- NES receives a producer-expanded480x420 RGB565 frame with anti-aliasing off.
  The real LVGL image remains480x420 and needs no software scaling. During RUNNING
  and PAUSED, only the viewport is visible: no joypad, translucent panels, title
  or empty return-button background. Loading/error states preserve a real return
  action and readable failure detail. Cached nonzero HID input updates activity
  only while awake; a sleeping page is not woken. Stop/release and touch-restoration
  intent remain wired; the owner port separately tests the invisible exit hold.
- One hundred full-screen video/NES/home rebuilds retain LVGL allocator integrity
  and release the real frame buffers after deleting their image objects.

Screenshots are `build/host_media_ui/fullscreen-*.png`, generated directly from
mock-display pixels. Their sample video/game graphics and rates are synthetic.
Backend decoders, asynchronous preparation and page-common construction are
mocked; the real control/page callbacks, sliders, image scaling, rendering and
frame copying are exercised. This does not measure physical FPS, SMP/DMA timing,
codec compatibility, real multi-touch hit testing or full device acceptance.
The reused LVGL objects are not ASan-instrumented; `lv_mem_test()` checks each
rebuild, while independent frame/backend/runtime suites cover their own logic.

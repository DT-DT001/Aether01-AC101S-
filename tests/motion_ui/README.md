# Motion page verification

Run `python3 tests/motion_ui/build_run.py` with the installed WSL/Linux GCC.
The suite compiles the production `main/apps/app_motion.c` and
`main/app_motion_filter.c`, links real LVGL and the supplied Chinese fonts,
and reuses the reader suite's compatible native object cache. No tools are
installed and no device is opened.

Tests feed the actual QMI8658A sample structure and timestamps into the page:

- The rendered 3D model's RGB565 pixel hash changes with roll, pitch and yaw.
  Numeric labels and the real zero-button event follow those updates.
- Still, tilted, turning, moving, shaking, impact and freefall labels follow
  sensor samples rather than a synthetic animation state.
- Repeated/older timestamps, stale errors and NaN/Infinity do not advance the
  displayed pose. Sleep releases the IMU interest once, does not read telemetry
  while asleep, and restores interest on waking.
- Labels stay within 480x480; every rendered character has an actual supplied
  or fallback glyph. The zero button clears the footer. One hundred motion/home
  rebuilds exercise cleanup with `lv_mem_test()`.

The page-common constructor, timer ownership, display power and telemetry
hardware are mocked. Their subscription cleanup follows the actual common-page
contract. LVGL, draw callbacks, quaternion filtering, text and buttons are real.
This does not measure board FPS, I2C timing, calibrated sensor accuracy or SMP
behavior. The reused LVGL objects are not sanitizer-instrumented; the independent
filter suite covers its arithmetic with ASan/UBSan.

The runner exports `build/host_motion_ui/stand.png`, `tilt.png` and `rotate.png`
from actual mock-display pixels for visual review. The omitted common battery
badge is covered by its own UI suite.

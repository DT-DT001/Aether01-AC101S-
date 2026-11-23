# Music catalog refresh ownership

Run `python3 tests/music_refresh/build_run.py` after the native reader UI cache
exists. Production `app_music.c` runs with actual LVGL and the existing volume
UI's read-only hardware doubles. Only IO completion is replaced by a controlled
asynchronous fixture. AddressSanitizer/UndefinedBehaviorSanitizer instrument the
production page and harness; cached LVGL/font objects are reused.

The test sends repeated clicks to the real Refresh control while its catalog
request is pending. No second request can overwrite that completion. A failed
scan and an empty successful scan both allow a later retry, and existing music
playback/stream volume remain unchanged. This verifies callback ownership and
UI admission; it does not measure physical SD/USB scan throughput.

The test also navigates through 96 catalog entries with keyboard events and
taps the final recycled row using a real LVGL pointer. It verifies the track
index, the viewport-sized row pool and absence of additional catalog requests.
Ten alternating fast drags verify that every visible caption follows its row,
long Chinese filenames are ellipsized, caption storage is reused and playback
keeps the same track.

Prepared catalog delivery does not copy/rebuild the backend playlist on LVGL.
While refresh owns the completion, old row indices cannot select a different
song from the worker's new list. Page cleanup publishes a full session release
and no longer preserves background playback.

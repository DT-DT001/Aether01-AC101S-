# Recorder page verification

Run `python3 tests/recorder_ui/build_run.py` after the existing native reader UI
cache is available. This compiles the complete production recorder page against
real LVGL and the project's full Chinese 16/24 fonts. Cached objects are read
only; output goes to `build/host_recorder_ui`.

Real pointer input checks media preparation before recording, duplicate start
prevention, IO/start errors and retry, pause/resume, sample duration and input
peak display, asynchronous save, saved-file replay, backend Chinese error detail
and the Recordings browser route. A USB-microphone selector waits for the applied input, disables during
capture and preserves an unavailable USB MIC selection after removal until the
user explicitly selects board MIC (preventing an asynchronous start from silently
recording the wrong source). The selector uses cached audio-service status;
it never changes codec/USB hardware on a GUI callback.
Tests also check stale preparation callbacks after navigation, asynchronous
stop on leaving the page, remote gate transitions, missing backend, card
removal/reinsertion, control geometry and LVGL heap integrity.

Rendered RGB565 pages are exported as `recorder-idle.png`,
`recorder-recording.png`, `recorder-saved.png` and `recorder-error.png`.
The backend is mocked here: these tests do not certify microphone wiring,
recorded sound quality, SD write throughput or long-duration recording.

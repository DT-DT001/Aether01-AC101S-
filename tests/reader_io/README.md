# Reader IO and bookmark queue regression

`sh tests/reader_io/run.sh` compiles the real `main/app_io.c` against bounded
FreeRTOS-queue/NVS/storage mocks with ASan/UBSan. It checks:

- storage path validation;
- queued SAVE visibility on immediate LOAD;
- save saturation and latest-value retention/coalescing;
- ordering between the normal queue and overflow slot, including sequence wrap;
- stale UI-result cancellation without losing queued saves;
- persistence/init errors reported without stopping unrelated UI or erasing NVS.
- retained failed commits with retry backoff and RAM-first reopening;
- deferral while music/video/NES is running, then latest-position commit on pause;
- growth across forty books with pending positions;
- v1 bookmarks with nonzero old padding load as automatic encoding, while v2
  persists manual encoding without moving the path/offset/font/theme fields.

The production file-chunk code and source font assets are checked separately
by `tests/reader/run.sh`, which invokes this suite too. Real flash wear,
partition retention across upgrades, power-cut recovery, and hardware hot-plug
still require device acceptance.

USB exit confirmation uses a separate worker command with the displayed USB
request ID. Tests reject stale, pending and already-consumed tokens and check
that strict host ownership rejection is unchanged. Physical host cache flushing
and USB enumeration are outside this host harness.

Owning-page exit is coalesced through `app_io_release_media`. The regression
injects a failed close, verifies retry/admission and frame retention, and then
queues a new-page request in the queue-wait window: work cannot bypass release
after dequeue. Music playlist copying now happens on this IO worker, with stale
page generations rejected before installation; LVGL only renders its catalog.

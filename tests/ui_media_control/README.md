# GUI remote-control queue regression

Run `sh tests/ui_media_control/run.sh`. No LVGL object cache is required or
modified. The production queue bridge is compiled with pthread-backed bounded
FreeRTOS queue and critical-section mocks under ASan/UBSan. The production
`app_ui_request_io_ex` and the IO-completion/remote-dispatch portion of the GUI
service are extracted verbatim into the focused test translation unit. The
actual `service_user_key` function is also included. Caption objects use small
GUI-thread mocks; timer logic is retained, including cached power/release
status and resource-release notice creation, update and deletion.

The suite covers media IO classification, old-command invalidation, rejection
during a pending handover, protection through the launch completion callback,
chained media requests, synchronous submission failure, a full 16-item queue,
and a producer that copies an old epoch before a handover but sends only after
the handover has finished. Dispatcher and timer calls assert GUI-thread
ownership; enqueue performs no LVGL operations. Additional service checks cover
release pending and retry errors through an IO completion, caption reuse and
cleanup, continued gates during queued IO, shutdown/recovery/failed status
snapshots, and one-shot generation-bound user-key navigation. Ending USB
ownership reopens the shared gate on the next actual service tick.

# Video facade regression

Run `sh tests/video_facade/run.sh`. The production facade is exercised with
controlled mock backends, pthread synchronization, ASan and UBSan.

- Case-insensitive MP4/MOV routing, native MP4 cadence and MJPEG FPS override.
- Active-backend exclusion and pause routing.
- One timeout budget shared by the command lock and both backend drains,
  including lock timeout and zero-time polls.
- Partial destructor success followed by failure and retry, without a second
  deletion of the already-freed backend; constructor cleanup.
- An asynchronous stop while either backend's status check is blocked, and
  while either backend is accepting playback, must cancel the new start.

The status-check case failed before the stop flag was cleared at the beginning
of the serialized start. This suite does not replace the real MJPEG/MP4 decoder
or device playback tests.

# Media command arbitration regression

Run `sh tests/media_control/run.sh`. The native binary compiles production
`main/app_media_control.c` with the real audio, video, MP4, MJPEG, NES and
Bluetooth public headers under ASan/UBSan. Only hardware-facing storage, codec,
touch and error typedefs, and backend snapshot/command endpoints are replaced.

Tests cover Flash deferral during STARTING/running and failed snapshots,
paused/idle allowance, commands routed to the current owner, music restart with
the retained path/volume, and music NEXT/PREV remaining blocked throughout
video/NES startup, playback and pause. They also check NES pause/resume dispatch,
all combinations of multiple owners, missing handles, unavailable status, and
backend command failures without a second command or an old-music fallback.

The dispatcher runs on the GUI service after handovers. This test verifies its
policy; actual UI queue order, worker cleanup, Bluetooth transport and target
throughput are covered separately and are not simulated here.

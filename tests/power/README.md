# Display power regression

For an independent check against the actual project LVGL 9 implementation,
run `python3 tests/power/run_lvgl_semantics.py` after the native reader UI has
been built by `tests/reader_ui/build_run.py`. The runner reuses those existing
LVGL object files and does not rebuild LVGL or install tools. It demonstrates
that `lv_display_enable_invalidation()` increments/decrements a counter:
two disables followed by one enable leave invalidation disabled; the second
balancing enable restores it. It also checks 1,000 balanced cycles and separate
counters for separate displays. This test does not use the power-test LVGL stub.

Run `sh tests/power/run.sh` using an existing Linux/WSL C compiler. The test includes the actual `main/app_power.c`; stubs provide LVGL, board I/O, NVS and task scheduling. No compiler or SDK is installed.

Groups cover sleep/wake and settings failure recovery, screen-off touch rejection and timer wrap. Media activity defers Flash commits while brightness still applies; pausing permits the pending commit. The LVGL stub models the actual reference counter:1,000 sleep/retry/wake cycles balance invalidation correctly. Full-worker PMIC scripts cover awake short-press sleep, asleep immediate press-wake, release/short ordering and combined latches, repeated failed acknowledgments, idle timeout while held, unavailable LVGL locks, an already queued explicit UI wake, pending explicit sleep and LONG suppression. A physical sleeping screen retains wake priority even with an unapplied wake request. Hardware long-press shutdown registers are unchanged. Touch IRQ fallback is explicitly unable to wake the panel, including while NES owns raw touch; the separate BOOT key still wakes. An explicit key wake consumes a held gesture before new GUI input. These tests do not establish physical electrical timing or SMP execution.

The native power runner also exercises the software shutdown path with
ASan/UBSan. A successful PRESS starts a 3,000 ms timer; RELEASE, SHORT,
coalesced press/release and any read/acknowledgement error cancel it. LONG alone
does not start a timer. The TG29 exposes latched edges rather than a readable
current PWRON level, so this is a conservative confirmed-edge history, not a
direct electrical measurement. An aged PRESS|LONG snapshot or a LONG received
less than 1,000 ms after the observed PRESS does not start/continue the software
timer. Full-worker tests check threshold release,
one attempt per press, operation without the LVGL lock, and keeping gates
closed after an acknowledged power-off command. Sensor quiesce, sensor sleep
verification, USB ownership errors, actual NVS write errors and overrun of the
absolute preparation budget retain power and expose cached status. Under the
current requested policy, a general BEGIN timeout still proceeds to mandatory
sensor quiesce/sleep, while actual USB/ownership errors reject preparation.
Noncritical FINALIZE gets at most 500 ms; its timeout
and a preference-save timeout set `cleanup_timed_out` and proceed to power-off
without waking either sensor. After a BEGIN timeout, a missing cleanup-gate
state is also skipped. This can discard unsaved TF/settings data.
The sensor barrier and its timeouts are never skipped. PMIC command ACK is
not proof that the physical rails switched off.

Run `sh tests/power/run_sensors.sh` for real QMI8658A/DPS368 driver tests with
a fake register bank: retained-power boot, verified Power Down/standby,
idempotent restore, lost acknowledgements, writes acknowledged without effect,
read errors and failed restoration. These tests do not measure sleep current.

Run `sh tests/power/run_telemetry.sh` for a pthread test of the actual telemetry
worker. It blocks a real sampling call, proves quiesce waits for that batch,
checks that no sensor/RTC I2C starts after acknowledgement, tests resume and
timeout retention, and checks 20 ms active IMU scheduling and unchanged error
backoff. This checks task semantics with host scheduling, not S31 latency.

`sh tests/tg29/run.sh` separately verifies that software off writes only the
REG27 command, retains control bits, never writes hardware key timing REG26
or hard-off control REG22, and reports PMIC lock/read/write/restart failures.
The 8-second hardware protection is independent and cannot guarantee file
integrity if the user keeps holding the key after software cleanup fails.


## Button-only wake and OTG shutdown integration

Touch GPIO state no longer supplies a panel-wake source. Full-worker cases
assert that quick touch/activity cannot reverse manual sleep, raw touch IRQ
cannot wake after LVGL lock recovery, and TG29 power/BOOT key transitions
remain available before the LVGL lock. The touch suite separately verifies a
held contact across key wake is consumed until release. Evidence:
[power regression](../../tmp/button-only-power-host.log) and
[touch regression](../../tmp/button-only-touch-host.log).

TG29 CC acknowledgments are independent of key-event success and acknowledge
only observed REG4C[6:5]; key REG49 and fault latches remain untouched. USB Host
shutdown uses a bounded stop barrier before the final PMIC off command. Busy
Host transfers, failed VBUS-off or uninstall retain physical-bus ownership;
unknown cleanup must not permit a new Host/MSC session. The Host component
and bridge regressions qualify these software gates, not actual USB power or
CC-state electrical timing. The8-second hardware power-off fallback is not
reprogrammed.


The [final candidate manifest](../../build/nes-hid-otg-manifest.json) records
[passing post-review IDF6.1 compilation](../../tmp/nes-hid-otg-final-build.log)
and fixed artifacts under
`build/releases/nes-hid-otg/`. No flashing or GitHub publication was performed;
hardware CC/VBUS ordering, physical-key behavior and long-duration performance
remain device-acceptance work rather than results of these native tests.

Aborted shutdown retains a failed cancellation context. The power worker retries
with100ms hook budgets every200ms and publishes `shutdown_recovering` until IO
resume acknowledgment and sensor restoration complete. Another shutdown cannot
overwrite pending recovery. The native suite exercises this full-worker cadence;
a second executable extracts the actual main CANCEL block and injects storage
timeout, resume-mutex contention and delayed worker/GUI acknowledgment, checking
that storage admission and controls are restored once without restarting media.
Evidence: `tmp/shutdown-recovery-power-host.log` (ASan/UBSan).

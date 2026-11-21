# USB HID Host regression

Run `sh tests/usb_host/run.sh` with GCC and ASan/UBSan. The production report
parser and Host worker/controller run against lifecycle-aware USB doubles.
The suite verifies report-ID state, keyboard arrays/NKRO, modifiers and mapped
keys, gamepad signed axes/hats/buttons,10,000 malformed descriptors, default-OFF
isolation from MSC, CC role gating, failed boost readback, asynchronous transfer
cancellation, no early free, exclusive reservation and100 shutdown/restart cycles.

The official `espressif/usb`1.6.0 API is separately compiled by the real S31 build;
the host doubles do not emulate UTMI electrical signaling or PMIC CC behavior.
No physical keyboard/gamepad or VBUS-source acceptance is implied by these tests.

Additional regressions hold the switch ON and CCsink true while PMIC-off or
mux-restore fails: no role query, Host reinstall or boost-ON may occur until
cleanup succeeds. Two composite devices/four interfaces verify per-device
hot-unplug releases; four-button adapters and one-second safe exit are covered.
Host control metadata stays within the512-byte INTERNAL compile-time budget;
the8 interface/parser models are allocated separately in PSRAM. Host-ABI sizes
printed by the test differ from the real S31 ABI.


Composite regressions include an exact63-byte USB-IF keyboard descriptor with
LED Output fields, standard boot mouse, and Logitech driver-layout-inspired
consumer arrays and HID++ vendor reports. A keyboard and mouse remain usable with two
valid unmapped consumer/vendor siblings. Separate tests preserve actual malformed, parse-limit,
GET_DESCRIPTOR stall, short-transfer, cancellation/disconnect/timeout and submit
errors. SET_PROTOCOL is Boot-subclass-only and only STALL permits fallback.
Rounded EP0 padding is bounded and excluded from parsing. A real error's complete
identity/stage/length/status tuple survives later READY/ignored/input and a
different global bus error; a new session clears it.


The 25-group multi-class/input suite additionally covers full generic keyboard
bitmaps/modifiers, report-ID state and relative mouse X/Y/wheel/buttons; mouse
deltas do not repeat during keyboard reports. Production callbacks are exercised
through hot-unplug and one RESET on teardown. Three class doubles verify that
one USB library starts all clients before root-port ON, requests stop once,
cuts physical VBUS promptly while clients drain, and only uninstalls the library
after all clients stop. Partial startup rolls back successful starts only.
These doubles do not validate real UAC/MSC transfers or actual device descriptors.

The new four-way-hat/nullable-axis case checks logical0..3 and1..4 cardinal
mapping with null release, signed -128 outside logical -127..127, normal endpoint
travel, centered axes and independently held gamepad buttons.

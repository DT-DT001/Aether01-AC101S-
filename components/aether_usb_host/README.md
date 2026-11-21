# Aether01 USB multi-class host

ESP32-S31, existing ESP-IDF6.1, pinned official `espressif/usb`1.6.0. This is a
USB Host client using the official UTMI PHY driver, with no SDK installation or
USJ register overrides. The measured OTG mux route is1; a complete shutdown
restores route0. Host and MSC use the shared atomic USB-bus reservation.

The settings switch defaults OFF and is not persisted ON. Default OFF neither
claims USB, polls CC, changes its mux/PHY nor powers VBUS. Its existing worker
sleeps until notified. ON reserves the bus, polls a verified PMIC sink-attached
callback and requires three20ms positive samples. It installs Host with the root
port unpowered, checks CC again, confirms PMIC boost enable, then enables the
logical root port. Only the TG29 ATTACH_SRC result may authorize source power;
CC attach-wait/try/unknown states do not. Hardware CC/VBUS ordering and actual
Type-C behavior still require device acceptance; unknown/error is fail-closed.

OFF, detach or role loss disables the root port and cancels endpoint traffic.
VBUS-off is attempted even if pending callbacks or uninstall stall. Transfers
are never freed while pending, and the bus reservation survives failed teardown
until Host uninstall, verified PMIC-off and download-route restoration complete.
Waiting for CC also invokes the off callback when disabled, withdrawing its CC
source policy even if boost was never enabled. No worker thread calls LVGL.

The client handles up to4 opened USB devices and8 HID interfaces, alt0, one
interrupt-IN endpoint/interface and report descriptors up to4096 bytes (subject
to the controller EP0 maximum). It sends SET_PROTOCOL only to Boot-subclass
keyboard/mouse interfaces. A SET_PROTOCOL STALL falls back to fetching the report
descriptor; disconnect, cancellation, timeout and submit errors still fail.
Supported input includes Generic Desktop Keyboard, Mouse/Pointer, Joystick and
Gamepad applications. Relative mouse X/Y/wheel and eight buttons are delivered
to the application as generic input; absolute tablets/digitizers are not mapped.
Report IDs,
keyboard arrays/NKRO bitmaps, signed/unsigned X/Y axes, four/eight-way hats with null
position, D-pad usages and numbered buttons are decoded with bounded state.
Keyboard arrows/WASD = directions; Z=A, X=B, Enter=START, either Shift=SELECT,
P=pause, R=reset and Escape=exit. Generic gamepad buttons1/2=A/B. Reports with at most4 buttons map3/4 to
SELECT/START; larger button sets use9/10. Numbered11/12/13 do not implicitly
pause/reset/exit (they can be ordinary stick clicks). Standard System Menu or
Consumer AC Home variable usages exit; alternatively holdSELECT+START+UP for1second.
Keyboard Escape/P/R remain exit/pause/reset. XInput, proprietary console protocols,
vendor-specific initialization, output/rumble and arbitrary button remapping are
not implemented. Standard composite devices can expose several supported classes
on one USB device without a hub. Hubs are disabled in this project's default
configuration; the root port uses the S31 high-speed-capable default. Enabling
hubs later requires separate topology testing, including the official driver's
full/low-speed child limitations. No hub or forced full-speed mode is enabled
by this component.

`tests/usb_host/run.sh` exercises the production parser and owner controller under
ASan/UBSan: keyboard/gamepad/IDs/NKRO/hat/axis/rollover/malformed reports, default
OFF/MSC exclusion, source-role refusal, partial boost failure, busy cleanup,
EP0 packet rounding, in-flight lifetime and100 power/session cycles. These
checks do not establish physical USB enumeration, CC-role timing, actual power
current or compatibility with an untested controller.

Host control metadata is explicitly INTERNAL with a compile-time budget of
512bytes; the8 bounded interface/report models use a separate PSRAM allocation.
The host ABI test prints their current sizes; the real S31 build validates its
own ABI. A pending
PMIC-off or mux-restore failure is a hard cleanup gate even while the switch
stays ON and CC still sees a sink. Only verified cleanup permits a new session.
Successful unplug/cleanup returns to WAIT_SINK rather than a stale STOPPING UI.


Composite keyboard receivers commonly expose consumer/media, mouse and vendor
interfaces alongside the keyboard. A structurally valid descriptor without either a generic or NES
input mapping is closed quietly and counted in ignored_interfaces for the
current Host session. The parser separately reports malformed descriptors and
bounded-decoder limits; actual USB/EP0 errors are never reclassified as ignored.
The public HID diagnostic tuple records VID/PID, interface/subclass/protocol,
stage, report lengths, transfer status and parser error. A real hid_error latches
that whole tuple until a new Host session; later READY, ignored interfaces or
successful input cannot mix identities or hide the original error. The live
hid_interfaces count remains independent.

Regression receiver fixtures use the USB-IF boot-keyboard/mouse layouts and
consumer-array/HID++ layouts from the Logitech-authored Linux v6.12
[hid-logitech-dj driver](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-logitech-dj.c).
Those driver-generated layouts are not a dump of the user's receiver.


The same core can start up to three static class clients through
aether_usb_host_class.h. It installs the USB library once, starts each client
with the root port unpowered, and only then permits root-port power. OFF first
requests all started classes to stop, disables the logical root port and drains
HID traffic. It waits for every class's cache-only poll_stopped result before
uninstalling the USB library. Failed partial startup stops only classes whose
start completed successfully. A pending class never prevents the outer power
controller from attempting physical VBUS-off.

aether_usb_input.h reports complete 256-usage keyboard bitmaps, complete mouse
button snapshots and relative mouse deltas. Keyboard state is combined across
interfaces/report IDs; a detached interface releases only its own keys/buttons.
Relative deltas are never replayed by an unrelated report. Host teardown emits
RESET so the application can release all held GUI keys/buttons. The callback
must copy into a bounded nonblocking queue; it must not call LVGL or file/USB
operations. The existing NES bit-mask callback remains independent.

Other class components share this core: aether_usb_audio uses the pinned
official UAC1 driver (USB Audio Class 1.0, not UAC2), and aether_usb_storage uses
the owned pinned MSC driver with FAT12/16/32/exFAT at /usb (no NTFS). Storage mounts
only the first selected disk. Neither class starts a second USB library event
pump. Native TF-card USB-device export and Host mode remain mutually exclusive.

The latest host regression includes 25 groups covering the composite receiver
fix, generic keyboard/mouse dispatch, class startup/rollback/draining, and the
earlier lifecycle/power fault cases. The Linux test ABI currently reports
240bytes of control metadata and21184bytes of interface/parser storage; these
are not promises about the S31 ABI. Real hardware tests are still required for
receiver enumeration, mouse input, UAC audio and external FAT disks.

Current gamepad compatibility includes zero- or one-based four-way hats as
cardinal quarter-turns, eight-way hats with diagonals, nullable signed axes and
independent report-ID/device state. Out-of-logical-range axis values stay neutral
rather than becoming unintended full-travel directions. Generic button numbering
uses the documented A/B/Select/Start convention; it cannot identify every
vendor's labels or private protocol. XInput/proprietary modes and universal
controller compatibility are not claimed. Physical acceptance remains necessary.

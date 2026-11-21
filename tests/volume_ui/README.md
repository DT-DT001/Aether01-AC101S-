# Actual pointer volume UI regression

Run `python3 tests/volume_ui/build_run.py` after `reader_ui` populates the real
LVGL/font object cache. The complete production music page and the verbatim
settings section plus shared helpers from `app_system.c` are compiled against
mock hardware services. Input uses LVGL's real pointer processing, not injected
slider events.

For center and ±20px vertical offsets, the test drags system volume from 17 to
70, then uses the music page to change that same global volume from 10 to 70.
It holds 700ms across each page's actual timer, releases and waits another 1.5
seconds. It checks slider, global backend request and persistent-request values.
Each gesture makes exactly one persistent request on release. Music's per-stream
gain remains 100 and its setter is never called. Opening settings, music and
settings again shares the global value without writing preferences; external
volume changes also synchronize through the music timer. It does not write or certify device NVS; backend persistence
and hardware output are separately tested.

Before enlarging the music slider, center gestures passed but the +20px gesture
missed its hit area and left it unpressed. Both volume controls now expose 46px
high touch targets. No timer-overwrite or 70-to-10 scaling defect was reproduced.

The same real-LVGL harness compiles the production USB settings/detail page.
It verifies default OFF, actual mode during pending changes, navigation without
implicit disconnect, safe-eject confirmation cancellation and submission,
stale request-token rejection, direct exit after protocol-confirmed eject,
and100 dialog open/cancel lifecycles. Confirmation is a user acknowledgment
that the computer has completed safe removal, never inferred from USB suspend.
It also covers shutdown overlay/gating and cancellation. Captures are synthetic
host output, not physical USB/SD behavior; see the device acceptance manifest.


The USB-speed release adds actual-page coverage for cached link
rate (HS480Mb/s, FS12Mb/s and unknown), actual SD clock and initialization
fallback status. Requested40MHz is not substituted for an unknown or rounded
SDK clock, and page status/timer processing does not issue USB-stack or SD
hardware queries. Evidence: [real-LVGL speed UI regressions](../../tmp/usb-speed-ui-host.log).
These are synthetic host states, not new link negotiation or throughput
measurements. The user subsequently authorized flashing, and the 16KiB release
passes COM36 flash hashes and bounded boot checks; see the local
[deployment record](../../build/usb-speed16-com36-manifest.json). The captured
tail only shows raw USB initialization starting. Completed export, throughput,
sustained integrity and radio-memory headroom remain unverified.


The final16KiB candidate [IDF6.1 build passes](../../tmp/usb-speed16-final-build.log).
The fixed [manifest](../../build/usb-speed16-candidate-manifest.json) records
`build/releases/usb-speed16-candidate/`, generated-config/ELF confirmation of
the16,384-byte MSC buffer and a net12,288-byte internal-RAM increase with IRAM
unchanged. The user initially requested no flashing and later authorized it;
COM36 flash/boot checks now pass, as recorded in the local
[deployment record](../../build/usb-speed16-com36-manifest.json). These checks
do not establish completed USB export, throughput, sustained integrity or
wireless memory headroom. `build/` and `tmp/` evidence remains local and is
excluded from the source repository.


## CC-gated OTG settings candidate

The real settings-page harness also compiles the OTG switch and cached status.
It verifies initial OFF, waiting for a verified sink, active HID-interface count,
cleanup-pending switch state and rejection while MSC or shutdown owns the
operation. GUI changes publish an application command; they do not query CC,
configure a PHY or source VBUS in the callback. Entering/leaving settings does
not persist ON or implicitly disconnect an active host. The switch and virtual
USB disk remain mutually exclusive through the backend's atomic bus owner,
not only through a disabled visual control. Evidence:
[OTG settings UI regression](../../tmp/otg-settings-ui-final-host.log).

The screen-off help text now identifies physical-key wake; touch wake is
removed. These synthetic states do not prove real HID enumeration, CC/VBUS
ordering, controller mappings or power behavior. The current NES/HID/OTG
candidate has not been flashed or pushed. Its [final post-review IDF6.1 build
passes](../../tmp/nes-hid-otg-final-build.log), with six staged artifacts in
`build/releases/nes-hid-otg/`; the [manifest](../../build/nes-hid-otg-manifest.json)
records the matching7,075,824-byte application and289 frozen source/configuration
hashes. Final settings screenshots were visually checked. The deployed16KiB USB-release evidence above is historical
and does not qualify this new Host path.

The OTG power follow-up covers STARTING, Host-without-power-proof, verified5V
with zero enumerated HID interfaces, a connected HID interface, and expired
measurement snapshots. VBUS/TS values come from the mocked worker cache; invalid
or expired data is labelled unavailable rather than displayed as0V. The real
settings page displays sampled CC/REG19/REG50 and work-temperature fault latches.
`settings-otg-power.ppm` is a synthetic UI fixture, not a board voltage reading.

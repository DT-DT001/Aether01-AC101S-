# NES keyboard mapping

`sh tests/nes_controls/run.sh` compiles the production cache/NVS service with
ASan/UBSan and pthread-backed critical sections. It validates all eleven default
actions and aliases, primary/secondary assignment, unbinding and duplicate-key
rejection, immediate HID mapping without NVS work, deferred writes during media,
whole-map save/reboot/default restore, corrupt/version/length fallback, commit
failure retries, changes during an older commit, tick rollover and concurrent
mapping/settings updates. Task-creation failure retains usable defaults.

`sh tests/usb_host/run.sh` additionally decodes real boot/NKRO keyboard and pad
reports through the actual Host aggregation. A custom key replaces the old
keyboard shortcut completely without erasing gamepad state, interleaved report
IDs or independent key/device releases. The optional callback preserves legacy
behavior for other Host users when unset.

The new `nes_keys` NVS namespace holds a versioned whole-map record. A physical
key can be assigned to one action/slot only. RAM changes are immediate, while
Flash writes run on an event-driven priority2 service after250ms quiet time and
only when the main media persistence guard permits it. Failures remain visible
and retry without discarding the active mapping. The application owner can
include snapshot.pending in bounded shutdown persistence handling.

No real USB receiver, MCU or Flash is used by these host tests.

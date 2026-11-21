# TG29 board power and battery driver

The component uses the TG29 register definitions, not the bundled AXP717
implementation. Existing rail policy retains the MCU's DCDC2 and the shared
I2C supply; PWRON event acknowledgement is restricted to its observed key bits.

`tg29_get_battery_state` is a pure-read API. It holds the existing recursive
PMIC mutex across its reads and publishes a local snapshot only after every
required read succeeds. A bus, mutex or argument failure clears the output.
It does not enable battery detection, the gauge, ADC, charging or any rail, and
does not acknowledge interrupts. The mutex serializes driver callers; it does
not freeze asynchronous charging hardware while different status registers
are being read.

The contract is based on TG29 Datasheet V0.9, 2026-06-29:

| Register | Used fields |
| --- | --- |
| 00 | bit 3 reported battery presence; bit 5 VBUSGOOD |
| 01 | bits 6:5 current direction; bits 2:0 charging state |
| 68 | bit 0 battery-detection enable |
| 0B | bit 2 gauge enable |
| A4 | full eight-bit state of charge, valid only from 0 through 100 |
| C0 | bit 0 VBAT ADC enable |
| C4, C5 | high six bits then low eight bits, 1 mV per LSB |

`vbus_present` means usable input (VBUSGOOD), not merely a physically inserted
cable. Detection disabled causes the PMIC to report battery-present regardless
of the actual cell, so `presence_valid` is false in that case. The manual also
states that detection runs with usable external input and does not provide a
documented completion/ready flag. Detection enabled is therefore reported
presence validity, not proof that a fresh presence test has just completed.

Raw charge state and direction are retained while unrelated status bits are
ignored. Direction 3 and states 6/7 are reserved and invalidate
`charge_state_valid`. Charging additionally requires trusted battery presence,
VBUSGOOD, direction 1 and state 0..3. Done requires the same presence/input/
state-valid guards and state 4; standby direction 0 with state 4 is legitimate.
In particular the observed status byte 0x94 must still decode as done.

SOC and voltage are sampled only with valid reported battery presence and
their respective enable bits set. A valid SOC of 0 is not treated as missing;
128/255 are not masked into a false valid percentage. VBAT is read high first
to latch the matching low byte. Raw 14-bit millivolts are retained. Values 0 and
0x3FFF are rejected as a conservative software plausibility check, not as
documented hardware invalid/saturation codes. No invented cell-chemistry range
is applied to other values. The manual's ADC summary lists 14 bits but also an
endpoint of 1FFF/8.191 V; the driver follows the explicit C4 high-six/C5 low-eight
register layout without importing AXP717 thresholds or validity flags.

The API performs no sampling timer or caching. Application background code
must decide its polling interval, retain the error/timestamp, and reject stale
snapshots before displaying them. Native regression is `sh tests/tg29/run.sh`;
actual battery wiring, detection behavior and measurement accuracy still
require board testing.

## CC detection and switched USB source power

The same TG29 V0.9 manual defines the following controls (pages29,31,32,38,
40 and50). These are not AXP717 definitions.

| Register | Driver behavior |
| --- | --- |
| 0B[3] | Explicitly enable and verify Type-C CC detection; its EFUSE default is not assumed. The RWAC watchdog command in bit0 is not replayed. |
| E3[5:0] | OFF selects SINK; allowed detection selects DRP with sink preference and default USB current advertisement. Unrelated high bits are retained. |
| E7[3:0] | State6 ATTACH_SRC is the only local-source/remote-sink permission. State3 ATTACH_SNK, WAIT/TRY, accessory, disabled and reserved states never authorize boost. |
| 13[0] | Verified VBUS source-path gate, required by the public56-page TG29 manual p24. All other bits are preserved. |
| 19[4] | Verified manual boost control; disabling does not depend on a valid CC state. Charger, LED, backup and watchdog settings are retained. |
| 1E | Select5.126V and500mA current limit; retain the existing battery undervoltage cutoff. Do not select the unlimited-current option. |
| 4C[6:5] | `tg29_get_cc_events` reads and acknowledges only observed CC insert/remove IRQs, leaving key/fault latches untouched. |

`tg29_configure_cc_detection` does not enable boost. Each new handle starts
with software source permission false. An OFF request revokes that permission
before waiting for the PMIC mutex, so a stale DRP register or an I2C/lock failure
cannot authorize a later ON call. ON permission is published only after the
whole CC configuration verifies. The application must retain its own user
preference and call OFF during shutdown and USB-role teardown.

`tg29_set_otg_enabled(true)` checks current detection enable, CC mode and stable
source attachment, rejects VBUSGOOD already present before our boost starts,
sets the protected output configuration, then rechecks the role immediately
before and after enabling. A detected role change or communication failure
attempts to switch boost off and returns the failure. VBUSGOOD can be the
local output once boost was already enabled. `boost_enabled` is the checked
control bit; it is not a physical voltage measurement or a guarantee that TS,
battery voltage and the PMIC's internal protections permit output.

`tg29_configure_manual_otg` authorizes a separate source session after checked
OFF. CC detection stays enabled in default-current DRP. In manual mode
`otg_role_ready` accepts `UNATTACH_SNK`, `UNATTACH_SRC` and `ATTACH_SRC`.
`attached_as_source` still denotes a detected local source attachment.
Before energizing VBUS, the driver checks the role and VBUSGOOD while the ADC
settles for at least 1000 ms. Two samples spaced by at least 10 ms must be at
most 800 mV. Raw zero can corroborate VBUSGOOD low during preflight and remains
invalid as a reported voltage. Fresh ADC, CC, VBUSGOOD and cancellation checks
precede the RBFET write. The driver retains a temporarily enabled VBUS ADC
channel until checked OFF. An OFF request cancels a pending preflight, and
the worker releases the PMIC mutex between samples.

Manual mode requires `tg29_verify_otg_output_ex` to confirm 4.75–5.50 V before
enabling USB host power. The control-only fallback is available for automatic
mode. A manual source session is intended for an explicitly selected unpowered
load. With CC missing, the board cannot detect attachment or removal of that
load through CC. Register tests cover the preflight and cleanup policy. The
actual ADC settling time and voltage at the connector require board testing.

The manual lists BOOST prerequisites separately from CC and provides no raw
Rd/debounce bit or detailed CC state-transition timing. The driver therefore
does not treat ATTACHWAIT_SRC or TRY_SRC as a stable attachment. Actual
ATTACH_SRC timing before source VBUS, Type-C cable/adapter behavior, connector
wiring, output voltage and overcurrent behavior require board acceptance.
A software I2C command cannot guarantee physical shutdown during a lost bus;
OFF errors must remain visible and be retried by the owner.

## Charging-current ceiling

Fast charging uses REG62[5:0], and precharge uses REG61[3:0], in64mA steps.
The documented default ICC code0x10 means1024mA; the last captured COM36 boot
also showed REG62=0x10. That is an observed historical setting, not a fresh
measurement or the instantaneous battery current.

`tg29_limit_charge_current(...,800,...)` floors the ceiling to768mA (code0x0c),
because832mA would exceed the user's800mA maximum. Fast and precharge settings
already below the ceiling are not rewritten or increased. Invalid high ICC
codes are replaced by the safe ceiling. The manual is inconsistent about
ICC code48 (it gives a general <=48 formula but marks48..63 reserved in the
explicit table); the getter conservatively flags48..63 invalid. The limit API
repairs these codes to a valid requested ceiling.

Writes preserve upper bits, verify every changed field, and do not enable
charging or alter CV, TS, JEITA, safety timers, hardware shutdown timing or
power rails. On a partial failure `changed` can be true while the output state
is cleared and an error is returned. The application must not report a safe
limit after an error. `tg29_disable_charging` is available for that failure
policy: it clears and verifies only REG19[1], preserving the boost bit and all
other controls. It never enables charging later on its own.

`sh tests/tg29/run.sh` covers all1024 ICC/precharge code pairs, all64 CC
state/mode combinations, detection enable and watchdog-command preservation,
source policy OFF during mutex/bus failures, external/own VBUS, dropped writes,
readback failures, role changes during enable, precise CC IRQ acknowledgment,
and all256 charger-disable control bytes. The previous rail, battery, key and
software/hardware-off preservation cases run in the same ASan/UBSan suite.

## Physical OTG output and this board's fixed TS resistor

The public [56-page TG29 datasheet](../../Files/TG29资料包/datasheet/TG29_56pages_LCSC_18d01f63.pdf)
p24 sec6.9 explicitly adds REG13[0] RBFET enable when outputting to VBUS.
The project-supplied V0.9 copy omits that prerequisite. The public file SHA256
is `6af222a39f77a8ed43d50db5c573047cdfcb7f3a18932e8091aa12f2fcbf94d0`.
The remaining prerequisites are battery above the retained REG1E[3:2] cutoff,
VBUS below VBAT+VSLEEP before boost starts, REG19[4] set, and TS inside REG56/57.
Page25 and the block diagram place RBFET between VMID and VBUS: enabling the
converter alone does not open the source path. REG13[0] is checked ON before
REG19[4], with fresh CC/policy/gate checks between and after. All other REG13
bits remain unchanged. REG00[5] is an input-source indication (p16), not output
PG. Initial/pre-gate external-good checks remain; once our gate opens, a rising
GOOD cannot independently distinguish our path from an external source.

`tg29_set_otg_fixed_ts_10k_policy` is an explicit board-only opt-in, false for a
new handle. This board's authoritative POWER schematic has R311 fixed10k to
GND and a known idle REG50 low nibble0x2. Only with that opt-in, fixed-input
bit4 still set and the current source OFF does OTG temporarily select50uA
always-on (low nibble0xE). The expected500mV must fit the existing work window;
REG50 bit4, real-NTC mode, active bias owned elsewhere, charger controls and
REG56/57 protection thresholds are retained. Failures preserve cleanup ownership.
Both RBFET and BOOST are independently cleared and verified before TS/ADC
restoration; failure of either still attempts the other and retains cleanup
ownership for retry. Checked output-off precedes TS restoration. An opted-in warm reset recognizes
only the known fixed-input0x1E pattern and returns it to the explicitly declared
idle0x12 baseline; no unknown board/NTC setting is inferred or rewritten.

The owner calls `tg29_verify_otg_output` after the control command and before
powering the USB root port. It temporarily enables an otherwise disabled VBUS
ADC channel, retains its ownership until checked OFF, and requires at least
1000ms settling plus two10ms-spaced readings in4.75..5.50V with fresh CC/source
permission, RBFET and BOOST-control checks throughout. These timing/window values are
software policy requiring hardware acceptance: the manual exposes no ADC-ready
bit or conversion-time guarantee. A register acknowledgment cannot substitute
for the readings. No delay holds the PMIC mutex. GUI OFF may immediately call
`tg29_revoke_otg_source_permission`, an atomic-only cancellation gate; the worker
still performs checked hardware OFF/restoration. A cancellation epoch prevents
a later ON from reviving an older verification attempt.

The cheap power snapshot includes REG13 gate control, ADC control and VBUS
high-before-low reads.
`snapshot_valid` requires every snapshot read to succeed; per-channel validity
also requires an enabled channel and a non-endpoint conversion. It does not
prove settling. Verification errors retain the last complete sample as failure
evidence, possibly preceding cleanup; read/lock failures invalidate it. Non-OK
verification never proves output. Slower `tg29_get_otg_diagnostics` reads TS
configuration, thresholds, fault/IRQ latches and the already-selected TS ADC.
It never ACKs faults, switches the shared ADC selector or silently enables an
ADC channel. An unavailable/unselected/endpoint sample is invalid, not a
measured zero. Fault/restore and timing tests qualify software behavior only;
actual VBUS/TS voltages, adapter continuity and HID enumeration remain device tests.

## Consistent failure probes and external-input ADC baseline

`tg29_verify_otg_output_ex` preserves `verify_error` and `cleanup_error` as
separate results. Only a normal voltage deadline with source permission and CC
still valid attempts a complete diagnostic snapshot before checked OFF. Its
power, REG50, TS and fault fields share that pre-cleanup phase. Every probe
transaction checks cancellation, and the final CC/control check must still
pass. A failed or cancelled probe clears the whole diagnostic packet. Cancel,
role loss and read/lock errors skip extra diagnostic collection. Callers must
not combine retained pre-OFF power with later restored idle TS settings.
The original verify API remains a compatibility wrapper whose return value
still prioritizes cleanup failure. Neither API changes protection thresholds.

`tg29_probe_vbus_input(...,2000,...)` is a board-bringup-only baseline while a
known external USB input is present, before GUI/Host admission is possible.
It requires source permission OFF, BOOST OFF and VBUSGOOD throughout; it never
writes BOOST, CC, TS, charging, the ADC selector or protection registers. It
only enables an otherwise disabled VBUS ADC channel and restores that owned
bit after the requested100..2500ms continuous observation. Up to five raw
milestones at100/250/500/1000/2000ms record actual elapsed time. The final power
sample is the latest read; `first_valid_elapsed_ms` is only the first range-valid
conversion after100ms, not a plausible5V guarantee. This distinguishes delayed
readiness from a persistent out-of-range code without assuming a conversion
time absent from the manual. Existing ADC enable remains unchanged. Probe and
cleanup errors are separate; failed restoration retains
ownership so checked-OFF cleanup can retry. OFF/cancel epoch, source activation
and PC removal abort collection; no external input returns immediately.
Raw C0/C6/C7 and unscaled14-bit data are retained even for zero/full-scale or
out-of-window readings. These baseline readings do not assume an actual5V
output or establish the cause of a later OTG timeout.

## Untrusted VBUS ADC sessions

An actual PC-input baseline on this board reported C0=0x07, C6/C7=0x3F/0xFC
(raw0x3FFC) while BOOST was OFF. That exceeds the V0.9 Table6-6 documented
VBUS range0..0x1FFF (0..8.191V), and must not be displayed as measured16.38V.
The manual does not identify0x3FFC as a specific saturation/disabled code; the
raw value is retained without inventing that meaning. VBUS and selected TS
measurement validity now also requires the documented range, not merely a
nonzero/non-all-ones14-bit value.

The board/application chooses ADC trust once at boot from a plausible known
external-input baseline. A missing/unavailable/implausible baseline is untrusted.
Trusted sessions keep the strict measured-voltage verification path. In an
untrusted session, `tg29_settle_otg_control(...,100,...)` checks fresh stable
CC6, detection, source/cancellation epoch, RBFET and BOOST control for100ms, without
enabling an ADC or claiming voltage proof. It permits the caller to attempt
normal Host enumeration while `output_verified` remains false; ongoing software
checks still reject lost role, control or source permission. Failures perform
checked OFF/restoration and retain the original/control error separately from
cleanup failure. PMIC undervoltage, temperature, current-limit and charger
settings are unchanged. Enumeration success still does not calibrate the ADC
or constitute a voltage measurement.

The strict path's1000ms minimum settling is an empirical floor above the actual
816ms first-conversion observation, with a2000ms owner budget and two consecutive
in-range samples. It prevents acceptance of an early stale PC-input value in
the host model; it is not a hardware ready flag or a universal TG29 conversion
time. The untrusted control-only path remains100ms. Diagnostic boot reads also
include05 (BC1.2 classification),0B (separate BC/CC enables) and13 without writing
them. No BC1.2, reserved CC/BOOST or input-current configuration is changed.
This gate fix follows explicit TG29 primary evidence; actual outgoing5V/HID and
incoming charging behavior still require board acceptance.

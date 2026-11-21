# TG29 rail policy and PWRON key regression

Run `sh tests/tg29/run.sh` with the existing Linux/WSL C compiler. The test compiles the actual production TG29 driver with a fake I2C register bank and recursive mutex, using ASan/UBSan.

Manual output explicitly disables both CC detection and the CC port after
checked output OFF; historical E7 states are diagnostic only. Tests retain
the full role matrix for automatic output, verify more than60s at5.134V across
stale manual E7 values0/1/3/4/6, repeat manual ON/OFF, and reject unexpected
detector/port policy changes. External VBUSGOOD, settled low-input ADC, source
permission, TS, RBFET/BOOST, measured output and cancellation remain enforced.
OFF installs SINK before re-enabling detection. The software model does not
prove physical protection against plugging a second supply into an already
driven connector: manual output must be stopped before attaching a computer
or charger.

Runtime external-input leases use independent tokens and ADC ownership. Tests
cover charge-complete VBUSGOOD, ADC-only acquisition, an already enabled ADC,
source ON exclusion, ordinary source OFF preserving the input owner, stale
tokens, input/role/BOOST/RBFET/ADC loss, every snapshot read failure, and ADC
enable/readback/rollback/release failures. Failed cleanup retains the token and
blocks source until retry succeeds; release changes only its owned C0 bit and
preserves other users' ADC settings. ADC endpoint/readiness validity remains
separate from the consumer's voltage-range and settling policy. The bringup
probe cannot take over a runtime input owner, and runtime admission rejects an
active probe or an older source cleanup owner.
Absent VBUSGOOD with no owner/source/BOOST/RBFET conflict returns NOT_FOUND
even when the battery-only CC FSM retains a disabled or stale state. This only
hands admission to the separate manual policy and strict low-voltage preflight;
it does not enable output. Known external input still requires a safe sink role.

The register model checks the board contract: DCDC2 retained without voltage writes; ALDO2 prepared at 3.3 V before optional loads are isolated and retained across initialization and failure cleanup; five board voltages encoded and read back; runtime rail readback; argument rejection; and no writes to charger, protection, fault-clear or unsupported CPUSLDO registers. ALDO2-off profiles, runtime disable and non-3.3 V requests are rejected. A fake bus-clamp scenario loses communication if the already-powered shared sensor supply is cut. Failed preparation must not enable an unknown voltage; an unreadable LDO0 bank is left unchanged while other cleanup proceeds. Fault injection covers dropped writes, one-shot bus failures and diagnostic read failures.

Four additional groups cover the PWRON wake API from the TG29 manual's REG41/REG49 contract: enabling press/release/short/long key interrupts while preserving upper enable bits; startup acknowledgement of key latches only; real RW1C semantics with a new edge arriving during acknowledgement; and read/write/drop failures. REG49 writes are constrained to observed low-four key bits, preserving VBUS/BAT events and every other IRQ bank. A read error returns zero events; an acknowledgement error retains the observed event snapshot for the caller. Hardware long-press shutdown and charging/protection settings are never changed.

Battery tests forbid every write during `tg29_get_battery_state`, require the
recursive mutex at every I2C read, and check that the caller's output remains
cleared until the complete snapshot is ready. They check the exact control/SOC/
high-VBAT/low-VBAT read order and inject failure at each of the eight reads,
plus lock timeout and NULL arguments. A recursive caller retains its original
lock level. Full-byte SOC cases include 0, 100, 101, 128 and 255. Raw VBAT cases
cover zero, full 14-bit range and values above 5 V without inventing a chemistry
clamp; only zero/full endpoints fail the conservative validity check. All 32
direction/state combinations, unrelated high bits, actual 0x94 done, battery-only
discharge, USB without a battery, disabled detection with forced reported
presence, disabled gauge and disabled ADC are covered. Disabled measurements
are not read or enabled. This verifies register interpretation and transaction
semantics, not detection completion, battery percentage calibration, voltage
accuracy or a physical charging current.

This verifies software register transactions and error cleanup. It cannot confirm PMIC output voltages, output shorts, charging current, silicon temperature, startup timing or hardware stability.

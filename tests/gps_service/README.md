# GPS service lifecycle regression

Run `sh tests/gps_service/run.sh` under the existing WSL/Linux compiler. It
compiles production `main/app_gps.c` and the production NMEA parser with
ASan/UBSan. Virtual FreeRTOS callbacks run the real worker until it reaches its
fully cleaned idle notification wait; no device, USB, serial port or Flash is used.

Coverage includes lazy/default OFF, cache-only GUI commands, verified-power
ordering, delayed power/cancellation, real RMC and PPS publication, borrowed
power, forced OFF/fault cancellation, retained UART/lease/GPIO cleanup failures,
bounded stop timeout, partial UART/IRQ/worker-start rollback, rapid requests,
shutdown without automatic restart and OFF interleaved with UART setup.

External-input cases run the same production worker and parser with a separate
ADC-only input-lease mock. They cover charging and charge-complete VBUS,
4750/5500 mV boundaries, 1-second settling and distinct 20-ms samples, invalid or
out-of-range measurements, unplugging during startup and reception, a bridge
BOOST/source anomaly, admission errors, retained ADC-cleanup ownership, partial
UART rollback, and explicit-only source re-evaluation. Every external case
asserts that GPS never acquires/releases manual-source power or enables Host.
The default mock returns NOT_FOUND so the original OTG cases remain unchanged.

Two recovery regressions intentionally do not call `fresh()` or reinitialize
the worker between sessions: source startup TIMEOUT followed by an explicit
new source session, and external-input unplug ERROR followed by replug and an
explicit new input session. Both must release all ownership, retain the same
task/parser, obtain a distinct token, reach FIXED using real RMC plus PPS, and
stop cleanly. Fresh-source admission clears the prior manual failure as the
real bridge does; input sample counters restart on each newly acquired token.

GPIO reset deliberately emulates the installed IDF's implicit pull-up, requiring
production cleanup to restore a floating RX. UART TX40 stays an unmapped,
no-pull input and ON_OFF37 is open-drain, never push-pull HIGH.

This suite tests deterministic task interleavings, not all SMP schedules,
real PPS timing, UART signal integrity, module internal pull-ups, real TG29
VBUS sensing or satellite reception. The OTG bridge and parser have separate
production-source suites; on-device GNSS acceptance remains required.

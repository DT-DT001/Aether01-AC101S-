# Board display sleep regression

Run `sh tests/board_display/run.sh` with the existing host C compiler.
The actual production `aether_board_display.c` and `co5300_sleep.c` run
under ASan/UBSan with GPIO, PMIC and panel-I/O spies. Tests cover 100 ordinary
sleep/wake cycles, QSPI DCS command framing, 120ms settle intervals, retained
CLDO3/AVDD_EN/reset/touch, idempotence, partial-command failures, cold recovery,
failed-recovery rollback/retry and cached brightness restoration.

The hardware panel, PMIC rail voltages, module timing, actual SPI/DMA completion
and LVGL reference counting are not modeled here; these require separate power
tests and device acceptance.

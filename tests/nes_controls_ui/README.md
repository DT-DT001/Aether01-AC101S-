# Actual LVGL NES mapping page

Run `python3 tests/nes_controls_ui/build_run.py` using existing Linux/WSL tools.
The fixture links the production mapping service and actual mapping page against
LVGL9.5 plus the full16/24px Chinese fonts already built by reader_ui tests.
Production handlers receive real dropdown VALUE_CHANGED events. ASan/UBSan and
LVGL heap validation cover all11 actions/22 selectors, full key-choice lists,
key application/unbinding, conflict rejection/reversion while a popup is open,
custom Exit key, media-deferred and saved status, restore defaults, reboot
reload and100 page create/cleanup cycles. PNG/PPM renders are retained under
build/host_nes_controls_ui. NVS, task scheduling and USB hardware are faked;
actual receiver/gameplay acceptance remains a separate device test.

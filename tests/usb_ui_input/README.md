# USB keyboard and mouse UI adapter

Run `python3 tests/usb_ui_input/build_run.py` after the native reader UI cache is
available. This compiles the production `main/app_usb_input.c` against the real
LVGL implementation, with AddressSanitizer/UndefinedBehaviorSanitizer on the
adapter and harness; cached LVGL objects are reused without sanitizer changes.

Tests exercise actual focus groups, Enter clicks, Tab/Shift+Tab navigation,
textarea US ASCII/Shift/Caps/punctuation/edit keys and repeat, relative mouse
motion and queued fast clicks, click-to-focus and actual wheel scrolling.
They verify no wheel replay, cancellation of an Enter gesture on RESET, held
input consumption through sleep/queued sleep/NES gating/disable, and bounded
publication from a concurrent producer with overflow recovery and LVGL heap
checks. USB callbacks never call LVGL.

Modal tests use separate real LVGL focus groups: Tab/Shift+Tab/Enter stay within
the dialog, newly created background controls remain excluded, replaced modal
rows join on refresh, and a nested dialog keeps its own group. One hundred
hide/reopen cycles, automatic nested deletion, stale saved focus and held Enter
across deletion are covered. Exhausting the bounded eight-modal registry leaves
the keypad blocked while the mouse can still close the dialog; deleting its
parent restores normal input.

This covers application input processing, not physical USB enumeration or a
particular receiver's report descriptors. Keyboard text uses the standard US
layout; additional layouts/IME, consumer media keys and mouse right-button
menus are not provided by this adapter.

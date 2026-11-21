# Native launcher verification

Run `python3 tests/home_ui/build_run.py` from the project root after the reader
UI suite has populated its real LVGL and font object cache. The test recompiles
the production `main/ui/app_home.c`, the 15 icon assets enabled in the firmware (the retained compass icon is excluded), and the two
additional Montserrat heading sizes. Hardware state is mocked.

Checks cover 14 real routes and the absence of the removed compass entry, four-column geometry, scrolling to the last
row, thirteen 48px image descriptors and the recorder's vector microphone,
RTC/TF/audio state changes and cleanup. Actual
RGB565 frames are exported as `build/host_home_ui/home-top.png` and
`home-bottom.png`. This does not certify device refresh rate or radio operation.

Icon originals, built-in generation prompts and hashes are preserved under
`main/assets/icons/`. `tools/generate_icon_assets.py` uses the project's official
LVGL image converter to regenerate the 48px ARGB8888 C resources. Regeneration
needs Pillow, pypng and lz4; firmware builds consume the checked-in C and require
none of those Python packages. The 16 pixel arrays total 147,456 bytes before
linker removal of unused reserved icons.

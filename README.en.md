# Aether01 AC101S firmware

This standalone firmware demo targets the AC101S edition of Aether01, a handheld ESP32-S31 device with 64 MiB flash, 32 MiB PSRAM, a 480 × 480 AMOLED touchscreen, and TF storage. It uses ESP-IDF 6.1 and LVGL 9.5. Device coverage is still developing and the demo may contain bugs.

[中文说明](README.md) · [AC101S hardware](https://github.com/DT-DT001/Aether01-Hardware-AC101S) · [I/O and power map](docs/hardware/io-power-map.md)

The firmware includes music playback with a live five-band EQ, video, recording, a TXT reader, NES emulation, USB OTG, Wi-Fi, Bluetooth controls, and ESP-NOW push-to-talk. Build from this checkout using an ESP-IDF 6.1 environment with ESP32-S31 support.

```sh
idf.py --preview set-target esp32s31
idf.py --preview build
```

The AC101S application image is `build/aether01_ac101s.bin`. Flash arguments are generated in `build/flash_args`.

Original firmware by DarkThunder is licensed under [Apache-2.0](LICENSE). Third-party licences apply to their respective components and assets. [NOTICE](NOTICE) · [Third-party notices](THIRD_PARTY_NOTICES.md)

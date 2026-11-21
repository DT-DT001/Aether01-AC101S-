# Third-party software and assets

The root [Apache-2.0 licence](LICENSE) applies to the original Aether01 firmware code by DarkThunder. Third-party files and their modifications keep the licence and notices applicable to those files. The [hardware and enclosure repository](https://github.com/DT-DT001/Aether01-Hardware-AC101S) carries its own design licences and credits.

| Component | Source and licensing |
| --- | --- |
| Nofrendo NES core | Matthew Conte and other upstream contributors. See the [NES provenance and licence breakdown](components/aether_nes/LICENSES.md) and the [GNU Library General Public License, version 2](LICENSES/LGPL-2.0-only.txt). |
| minimp3 | CC0-1.0. See the [upstream notice](components/aether_audio_player/vendor/minimp3/UPSTREAM.txt) and [licence](components/aether_audio_player/vendor/minimp3/LICENSE). |
| minimp4 | CC0-1.0. See the [upstream and modification record](components/aether_mp4_player/vendor/SOURCE.md). |
| FatFs and ESP-IDF glue | ChaN's FatFs notices remain in its source files. Espressif glue and the local overlay retain their applicable licences. See [provenance](components/fatfs/PROVENANCE.json) and [licence](components/fatfs/LICENSE). |
| USB Host audio and mass storage | Apache-2.0 vendor components with local changes. See the [UAC provenance](components/aether_usb_audio/vendor/usb_host_uac/PROVENANCE.json) and [MSC provenance](components/aether_usb_storage/vendor/usb_host_msc/PROVENANCE.json), and the included licence files. |
| UI bitmap fonts | Derived from Huayuan Gothic by Lapomme under the SIL Open Font License 1.1. See the [font provenance and licence](main/assets/FONTS.md). |

ESP-IDF, LVGL and other managed dependencies are resolved by the build environment and its component manager. Their own licences apply. In particular, the [ESP Audio Codec licence](components/aether_mp4_player/LICENSE.esp_audio_codec) has an Espressif-product use restriction. This repository publishes source. It does not distribute those downloaded binary codec libraries or a firmware image.

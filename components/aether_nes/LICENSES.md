# Aether01 NES component provenance

`vendor/nofrendo/**` is Nofrendo, copyright 1998–2000 Matthew Conte,
including ESP32/RISC-V fixes carried by Espressif's `esp32-nesemu` lineage.
The core is covered by the GNU Library General Public License version 2 only
([full license text](../../LICENSES/LGPL-2.0-only.txt)). Preserve the original
file-level notices, including the public-domain notice in `nofconfig.h`.

The exact source snapshot copied into this project was:

- Repository: <https://github.com/tangstoney/esp32p4-nesemu>
- Commit: `7baf1171ff83aca5b0a526cd3143560660601731`
- Copied path: `main/nofrendo/**`
- Earlier ESP32 port: <https://github.com/espressif/esp32-nesemu>

The Aether01 integration files outside `vendor/` (`aether_nes*.c/.h` and the
component CMake file) are board-specific glue written for this project. Local
patches to the vendored core are limited to RISC-V pointer-width-safe timer
arithmetic, deterministic shutdown, correct trainer-pointer advancement, and
ownership of the single OSD-loaded ROM allocation.
Additional local patches keep frame debt/input responsive and route SRAM and
configuration persistence through the project-owned checked removable-media
save/recovery adapter. The 2026-11-14 public source snapshot omits the SNSS
instant-state library and its unused event/mapper hooks. Cartridge battery SRAM
continues to use `.sav` files through the checked adapter. Original source
license notices remain authoritative.

No game ROM is included. ROM copyright and permission are separate from the
emulator; users must provide ROMs they are legally entitled to use.

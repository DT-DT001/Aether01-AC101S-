file(GLOB NOFRENDO_SOURCES
    "vendor/nofrendo/*.c"
    "vendor/nofrendo/cpu/*.c"
    "vendor/nofrendo/nes/*.c"
    "vendor/nofrendo/sndhrdw/*.c"
    "vendor/nofrendo/mappers/*.c")

idf_component_register(
    SRCS "aether_nes.c" "aether_nes_port.c" "aether_nes_save.c" ${NOFRENDO_SOURCES}
    INCLUDE_DIRS "include"
                 "vendor/nofrendo"
                 "vendor/nofrendo/cpu"
                 "vendor/nofrendo/nes"
                 "vendor/nofrendo/sndhrdw"
                 "vendor/nofrendo/mappers"
    REQUIRES aether_media_fs aether_storage ac101s_audio esp_lcd_touch esp_timer)

set_source_files_properties(vendor/nofrendo/nes/nes_ppu.c PROPERTIES
    COMPILE_FLAGS "-Wno-error=implicit-fallthrough -Wno-implicit-fallthrough -Wno-error=maybe-uninitialized -Wno-maybe-uninitialized")
set_source_files_properties(vendor/nofrendo/vid_drv.c PROPERTIES
    COMPILE_FLAGS "-Wno-error=implicit-fallthrough -Wno-implicit-fallthrough")
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=char-subscripts
    -Wno-error=attributes
    -Wno-error=format-truncation
    -Wno-error=int-to-pointer-cast
    -Wno-error=pointer-to-int-cast)

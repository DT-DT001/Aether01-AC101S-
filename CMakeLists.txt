cmake_minimum_required(VERSION 3.16)

# Retain the removed board option's driver sources without building its component.
list(APPEND EXCLUDE_COMPONENTS mmc5603)
# S31 vector work buffers must exclude LP while other tasks retain RTC heap.
option(AETHER_H264_HP_ALLOCATOR "Confine S31 H264 allocations to HP/PSRAM" ON)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aether01_ac101s)

# External library BSS relies on the board's existing cache-on PSRAM XIP mode.
if(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY AND
   (NOT CONFIG_SPIRAM_FETCH_INSTRUCTIONS OR NOT CONFIG_SPIRAM_RODATA))
    message(FATAL_ERROR "External library BSS requires this board's code/rodata PSRAM XIP")
endif()

include(components/aether_board/cmake/s31_extram_link.cmake)
aether_fix_s31_extram_link()

if(CONFIG_ESP_H264_S31_USE_ASM AND AETHER_H264_HP_ALLOCATOR)
    include(components/aether_mp4_player/cmake/s31_h264_memory.cmake)
    aether_enable_s31_h264_hp_alloc()
endif()

# IDF 6.1 Bluedroid copies fixed-width Bluetooth names with strncpy().
# GCC 15 diagnoses the intentional full-width copies at -O2. Keep these
# warnings visible and restrict the exception to the upstream BT component.
if(CONFIG_BT_BLUEDROID_ENABLED AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    idf_component_get_property(aether_idf_bt_library bt COMPONENT_LIB)
    target_compile_options(${aether_idf_bt_library} PRIVATE -Wno-error=stringop-truncation)
endif()

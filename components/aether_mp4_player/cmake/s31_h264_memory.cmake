# Optional project-owned allocator isolation. Include and call this only after
# project(aether01), when IDF has created the GLOBAL imported tinyh264 target.
# Including this file alone does not change any target or configuration.
set(_AETHER_S31_H264_MEMORY_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

function(aether_enable_s31_h264_hp_alloc)
    if(NOT CONFIG_IDF_TARGET_ESP32S31 OR NOT CONFIG_ESP_H264_S31_USE_ASM)
        message(FATAL_ERROR "The H264 HP allocator candidate requires ESP32-S31 ASM")
    endif()
    if(CONFIG_ESP_H264_DUAL_TASK OR CONFIG_FREERTOS_UNICORE)
        message(FATAL_ERROR "S31 H264 PIE requires core1 and the single-task decoder")
    endif()
    if(TARGET aether_s31_h264_archive)
        return()
    endif()
    if(NOT TARGET tinyh264 OR NOT CMAKE_OBJCOPY OR NOT CMAKE_NM)
        message(FATAL_ERROR "IDF tinyh264 and its existing objcopy/nm are required")
    endif()
    get_target_property(_imported tinyh264 IMPORTED)
    get_target_property(_original tinyh264 IMPORTED_LOCATION)
    if(NOT _imported OR NOT _original MATCHES "/sw/libs/esp32s31/libtinyh264[.]a$")
        message(FATAL_ERROR "Unexpected tinyh264 target or source profile: ${_original}")
    endif()
    set(_expected "a8b3d56882971c78ebee585a22e336ee64e35279f9553b71aa186e5ee1bf3643")
    file(SHA256 "${_original}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR "Unsupported S31 H264 archive; expected the pinned official 1.4.1 binary")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_original}")
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_python PYTHON)
    idf_component_get_property(_mp4_lib aether_mp4_player COMPONENT_LIB)
    get_filename_component(_component_dir "${_AETHER_S31_H264_MEMORY_COMPONENT_DIR}" ABSOLUTE)
    set(_prepare "${_component_dir}/tools/prepare_s31_h264.py")
    # The basename preserves the upstream linker.lf archive/IRAM mapping.
    set(_generated "${_build_dir}/aether_s31_h264/libtinyh264.a")
    add_custom_command(OUTPUT "${_generated}"
        BYPRODUCTS "${_generated}.provenance.json"
        COMMAND "${_python}" "${_prepare}" --source "${_original}"
            --output "${_generated}" --build-dir "${_build_dir}"
            --objcopy "${CMAKE_OBJCOPY}" --nm "${CMAKE_NM}"
        DEPENDS "${_original}" "${_prepare}"
        COMMENT "Isolating pinned S31 H264 allocations from LP SRAM"
        VERBATIM)
    add_custom_target(aether_s31_h264_archive DEPENDS "${_generated}")
    # IDF ldgen and the ELF both consume this imported target. CMake follows
    # its dependencies transitively, so generation precedes either consumer.
    add_dependencies(tinyh264 aether_s31_h264_archive)
    set_property(TARGET tinyh264 PROPERTY IMPORTED_LOCATION "${_generated}")
    target_sources(${_mp4_lib} PRIVATE "${_component_dir}/h264_hp_alloc.c")
    # The imported tiny archive can follow IDF's component --start-group.
    # Pull this one shim object before that group is searched, even when none
    # of its renamed users has been read yet. All four entry points share it.
    target_link_options(${_mp4_lib} INTERFACE
        "LINKER:--wrap=esp_h264_calloc_prefer"
        "LINKER:--undefined=aether_h264_hp_malloc_prefer")
    set_property(TARGET ${_mp4_lib} PROPERTY AETHER_S31_H264_HP_ALLOCATOR TRUE)
endfunction()

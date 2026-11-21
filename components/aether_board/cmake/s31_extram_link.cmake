# Project-owned workaround for IDF 6.1's S31 XIP/BSS dummy assignment order.
# Include/call after project(); neither SDK nor ldgen output is modified.
set(_AETHER_S31_EXTRAM_LINK_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

function(aether_fix_s31_extram_link)
    if(NOT CONFIG_IDF_TARGET_ESP32S31 OR
       NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY OR
       NOT CONFIG_SPIRAM_XIP_FROM_PSRAM)
        return()
    endif()
    if(TARGET aether_s31_extram_link_script)
        return()
    endif()
    idf_component_get_property(_system_lib esp_system COMPONENT_LIB)
    idf_build_get_property(_elf EXECUTABLE)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_idf_dir IDF_PATH)
    idf_build_get_property(_python PYTHON)
    if(NOT TARGET ${_system_lib} OR NOT TARGET ${_elf})
        message(FATAL_ERROR "S31 external BSS linker fix must run after project()")
    endif()
    get_target_property(_options ${_system_lib} INTERFACE_LINK_OPTIONS)
    set(_count 0)
    set(_replacement_options)
    set(_original "${_build_dir}/esp-idf/esp_system/ld/sections.ld")
    set(_patched "${_build_dir}/aether_s31_extram/sections.ld")
    foreach(_option IN LISTS _options)
        if(_option STREQUAL "SHELL:-T sections.ld")
            math(EXPR _count "${_count}+1")
            list(APPEND _replacement_options "SHELL:-T \"${_patched}\"")
        else()
            list(APPEND _replacement_options "${_option}")
        endif()
    endforeach()
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "Expected exactly one esp_system sections.ld link option, found ${_count}")
    endif()
    get_filename_component(_component_dir "${_AETHER_S31_EXTRAM_LINK_COMPONENT_DIR}" ABSOLUTE)
    set(_prepare "${_component_dir}/tools/prepare_s31_extram_link.py")
    set(_template "${_idf_dir}/components/esp_system/ld/ld.ext_ram.sections")
    add_custom_command(OUTPUT "${_patched}"
        BYPRODUCTS "${_patched}.provenance.json"
        COMMAND "${_python}" "${_prepare}" --source "${_original}"
            --template "${_template}" --output "${_patched}" --build-dir "${_build_dir}"
        DEPENDS "${_original}" "${_template}" "${_prepare}"
        COMMENT "Preserving S31 XIP/BSS dummy alignment before assignment"
        VERBATIM)
    add_custom_target(aether_s31_extram_link_script DEPENDS "${_patched}")
    # ldgen depends on esp_system and produces _original. Depend on the copy
    # only from the final ELF, never from esp_system (which would form a cycle).
    add_dependencies(${_elf} aether_s31_extram_link_script)
    set_property(TARGET ${_elf} APPEND PROPERTY LINK_DEPENDS "${_patched}")
    set_property(TARGET ${_system_lib} PROPERTY INTERFACE_LINK_OPTIONS "${_replacement_options}")
endfunction()

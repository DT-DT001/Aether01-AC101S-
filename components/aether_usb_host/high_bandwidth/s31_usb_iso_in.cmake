# Include after project() to patch the resolved USB 1.6.0 target.
set(_AETHER_S31_USB_ISO_IN_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(aether_enable_s31_usb_iso_in)
    idf_build_get_property(_target IDF_TARGET)
    if(NOT _target STREQUAL "esp32s31")
        return()
    endif()

    idf_component_get_property(_usb_dir espressif__usb COMPONENT_DIR)
    idf_component_get_property(_usb_lib espressif__usb COMPONENT_LIB)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_python PYTHON)

    set(_source "${_usb_dir}/src/hcd_dwc.c")
    set(_script "${_AETHER_S31_USB_ISO_IN_DIR}/prepare_s31_usb_iso_in.py")
    set(_generated "${_build_dir}/aether_s31_usb_hcd/src/hcd_dwc.c")
    get_target_property(_sources "${_usb_lib}" SOURCES)
    list(FIND _sources "${_source}" _old_index)
    if(_old_index EQUAL -1)
        message(FATAL_ERROR "S31 ISO IN overlay could not find the pinned USB HCD source")
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}" "${_script}")
    execute_process(COMMAND "${_python}" "${_script}" --source "${_source}" --output "${_generated}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "S31 ISO IN overlay could not prepare the USB HCD: ${_result}")
    endif()

    # Reuse all of Espressif's target options and private include paths.
    # Keep the source basename for the IDF linker's archive-member mapping.
    list(REMOVE_AT _sources "${_old_index}")
    list(INSERT _sources "${_old_index}" "${_generated}")
    set_property(TARGET "${_usb_lib}" PROPERTY SOURCES "${_sources}")
endfunction()

# Include after project() to patch the resolved UVC 2.6.0 target.
set(_AETHER_S31_UVC_RX_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(aether_enable_s31_uvc_rx)
    idf_build_get_property(_target IDF_TARGET)
    if(NOT _target STREQUAL "esp32s31")
        return()
    endif()

    idf_component_get_property(_uvc_dir espressif__usb_host_uvc COMPONENT_DIR)
    idf_component_get_property(_uvc_lib espressif__usb_host_uvc COMPONENT_LIB)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_python PYTHON)
    set(_script "${_AETHER_S31_UVC_RX_DIR}/prepare_s31_uvc_rx.py")
    get_target_property(_sources "${_uvc_lib}" SOURCES)

    foreach(_name IN ITEMS uvc_isoc.c uvc_descriptor_parsing.c uvc_frame.c)
        set(_source "${_uvc_dir}/${_name}")
        set(_generated "${_build_dir}/aether_s31_uvc/${_name}")
        list(FIND _sources "${_source}" _index)
        if(_index EQUAL -1)
            message(FATAL_ERROR "S31 UVC receive overlay could not find ${_name}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}" "${_script}")
        execute_process(COMMAND "${_python}" "${_script}" --source "${_source}" --output "${_generated}"
            RESULT_VARIABLE _result)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "S31 UVC receive overlay could not prepare ${_name}: ${_result}")
        endif()
        list(REMOVE_AT _sources "${_index}")
        list(INSERT _sources "${_index}" "${_generated}")
    endforeach()

    set_property(TARGET "${_uvc_lib}" PROPERTY SOURCES "${_sources}")
    target_include_directories("${_uvc_lib}" PRIVATE "${_AETHER_S31_UVC_RX_DIR}"
        "${_AETHER_S31_UVC_RX_DIR}/include")
endfunction()

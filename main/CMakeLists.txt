set(AETHER_ICON_SOURCES
    "assets/icons/icon_barometer.c"
    "assets/icons/icon_bluetooth.c"
    "assets/icons/icon_clock.c"
    "assets/icons/icon_files.c"
    "assets/icons/icon_game.c"
    "assets/icons/icon_headphones.c"
    "assets/icons/icon_motion.c"
    "assets/icons/icon_music.c"
    "assets/icons/icon_nes.c"
    "assets/icons/icon_network.c"
    "assets/icons/icon_reader.c"
    "assets/icons/icon_settings.c"
    "assets/icons/icon_status.c"
    "assets/icons/icon_video.c"
    "assets/icons/icon_wifi.c"
)
idf_component_register(
    SRCS "main.c"
         "app_audio_system.c"
         "app_audio_diagnostic.c"
         "app_radio_diagnostic.c"
         "app_media_control.c"
         "app_power.c"
         "app_otg.c"
         "app_gps.c"
         "app_nes_controls.c"
         "app_intercom.c"
         "app_pc_remote.c"
         "app_air_mouse.c"
         "app_arcade.c"
         "app_displacement.c"
         "app_displacement_estimator.c"
         "app_utilities.c"
         "app_alarm_audio.c"
         "app_weather_data.c"
         "app_usb_input.c"
         "app_telemetry.c"
         "app_io.c"
         "app_frames.c"
         "app_display_metrics.c"
         "app_lvgl_memory.c"
         "app_display_sync.c"
         "app_touch.c"
         "app_text.c"
         "app_lyrics.c"
         "app_reader_layout.c"
         "app_reader_file.c"
         "app_tools.c"
         "app_tools_math.c"
         "app_motion_filter.c"
         "ui/app_ui.c"
         "ui/app_ui_modal.c"
         "ui/app_ui_gyro_calibration.c"
         "ui/app_text_input.c"
         "app_pinyin.c"
         "app_pinyin_extended.c"
         "app_pinyin_swipe.c"
         "ui/app_ui_media_control.c"
         "ui/app_home.c"
         "ui/app_battery.c"
         "apps/app_system.c"
         "apps/app_utilities.c"
         "apps/app_controls.c"
         "apps/app_sensors.c"
         "apps/app_motion.c"
         "apps/app_usb.c"
         "apps/app_media.c"
         "apps/app_camera.c"
         "apps/app_gallery.c"
         "apps/app_music.c"
         "apps/app_recorder.c"
         "apps/app_reader.c"
         "apps/app_files.c"
         "apps/app_game.c"
         "apps/app_bluetooth.c"
         "apps/app_pc_remote.c"
         "apps/app_displacement.c"
         "apps/app_gps.c"
         "apps/app_intercom.c"
         "apps/app_wifi.c"
         "assets/font_16.c"
         "assets/font_24.c"
         ${AETHER_ICON_SOURCES}
    INCLUDE_DIRS "." "ui"
    EMBED_FILES "assets/boot_logo.rgb565"
    REQUIRES aether_media_fs aether_usb_storage aether_usb_audio aether_board aether_storage aether_usb_msc aether_usb_host aether_audio_player aether_recorder
             aether_video_player aether_bluetooth aether_wifi aether_intercom_radio aether_intercom_audio aether_nes co5300_display
             aether_usb_camera aether_camera_record aether_image esp_driver_jpeg
             aether_gnss esp_driver_uart esp_lvgl_port esp_psram esp_timer esp_app_format spi_flash lvgl nvs_flash fatfs esp_netif lwip
             esp_http_client mbedtls esp_partition)
# Pull the project custom memory provider before the later LVGL archive is read.
# All nine public allocator entry points live in this single object.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND CONFIG_LV_USE_CUSTOM_MALLOC)
    target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:--undefined=lv_malloc_core")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND CONFIG_LV_USE_CUSTOM_MALLOC AND NOT CONFIG_SPIRAM_USE_MALLOC)
    message(FATAL_ERROR "Custom LVGL allocations require the PSRAM heap")
endif()
if(AETHER_CODEC_SELFTEST AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_CODEC_SELFTEST=1)
endif()
if(AETHER_AUDIO_DIAGNOSTIC AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_AUDIO_DIAGNOSTIC=1)
endif()
if(AETHER_DISPLAY_PROFILING AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_DISPLAY_PROFILING=1)
endif()
if(AETHER_RADIO_SELFTEST AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_RADIO_SELFTEST=1)
endif()
if(AETHER_GPS_DIAGNOSTIC AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_sources(${COMPONENT_LIB} PRIVATE "app_gps_diagnostic.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_GPS_DIAGNOSTIC=1)
    if(AETHER_GPS_DIAGNOSTIC_PASSIVE)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_GPS_DIAGNOSTIC_PASSIVE=1)
    endif()
endif()
if(AETHER_UI_SELFTEST AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_sources(${COMPONENT_LIB} PRIVATE "app_ui_selftest.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_UI_SELFTEST=1)
endif()
if(AETHER_VIDEO_DIAGNOSTIC AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_VIDEO_DIAGNOSTIC=1)
endif()
if(AETHER_VIDEO_AUTOPROBE AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_sources(${COMPONENT_LIB} PRIVATE "app_video_probe.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_VIDEO_AUTOPROBE=1)
    if(DEFINED AETHER_VIDEO_PROBE_ZERO_SECONDS)
        if(NOT AETHER_VIDEO_PROBE_ZERO_SECONDS MATCHES "^(20|30|60|90|120)$")
            message(FATAL_ERROR "Video probe zero-play seconds must be20/30/60/90/120")
        endif()
        target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_VIDEO_PROBE_ZERO_SECONDS=${AETHER_VIDEO_PROBE_ZERO_SECONDS})
    endif()
endif()

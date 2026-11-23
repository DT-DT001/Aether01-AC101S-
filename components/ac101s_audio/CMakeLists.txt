idf_component_register(SRCS "ac101s_audio.c" "ac101s_codec.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_gpio freertos esp_timer)

if(AETHER_UI_SELFTEST AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC AETHER_AUDIO_DIAG=1)
endif()

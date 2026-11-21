idf_component_register(SRCS "ac101s_audio.c" "ac101s_codec.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_gpio freertos esp_timer)

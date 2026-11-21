idf_component_register(SRCS "aether_usb_audio.c" "pcm_convert.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES usb_host_uac freertos heap esp_timer)

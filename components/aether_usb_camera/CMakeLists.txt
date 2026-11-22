idf_component_register(
    SRCS "aether_usb_camera.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer usb
)

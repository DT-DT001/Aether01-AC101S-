idf_component_register(SRCS "aether_usb_storage.c" INCLUDE_DIRS "include"
    REQUIRES aether_usb_host fatfs esp_timer
    PRIV_REQUIRES usb_host_msc)

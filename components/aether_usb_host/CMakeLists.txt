idf_component_register(SRCS "aether_usb_host.c" "hid_report.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES usb esp_driver_gpio board_config esp_timer
    REQUIRES aether_usb_bus)

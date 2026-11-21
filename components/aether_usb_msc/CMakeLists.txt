idf_component_register(SRCS "aether_usb_msc.c"
    INCLUDE_DIRS "include"
    REQUIRES aether_storage aether_usb_bus
    PRIV_REQUIRES esp_tinyusb esp_hw_support esp_driver_gpio board_config)

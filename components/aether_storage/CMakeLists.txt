idf_component_register(SRCS "aether_storage.c" "storage_probe.c"
                       INCLUDE_DIRS "include"
                       REQUIRES aether_board board_config fatfs esp_driver_gpio
                                esp_driver_sdmmc)

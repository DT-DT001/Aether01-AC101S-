idf_component_register(SRCS "aether_board.c" "aether_board_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_config tg29 qmi8658a dps368 pcf85063
                                cst9220 co5300_display ac101s_audio
                                esp_driver_gpio esp_driver_i2c esp_lcd)

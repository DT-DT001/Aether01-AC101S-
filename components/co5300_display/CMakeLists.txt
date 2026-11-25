idf_component_register(SRCS "co5300_display.c" "co5300_sleep.c" "co5300_te.c" "co5300_stream.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_spi esp_driver_gpio esp_lcd esp_lcd_co5300 esp_timer)

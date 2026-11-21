idf_component_register(SRCS "aether_intercom_radio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES aether_wifi esp_wifi esp_timer esp_hw_support nvs_flash mbedtls freertos)

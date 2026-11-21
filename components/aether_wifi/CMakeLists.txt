idf_component_register(SRCS "aether_wifi.c" INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer freertos)

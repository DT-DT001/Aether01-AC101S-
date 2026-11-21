idf_component_register(SRCS "aether_bluetooth.c" "bt_pcm.c" "bt_hid.c" "bt_hid_reports.c"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
    REQUIRES bt ac101s_audio esp_timer PRIV_REQUIRES nvs_flash)

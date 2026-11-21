idf_component_register(
    SRCS "aether_mjpeg_player.c" "mjpeg_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES aether_media_fs aether_storage esp_driver_jpeg esp_timer)

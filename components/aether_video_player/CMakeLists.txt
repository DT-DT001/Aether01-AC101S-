idf_component_register(SRCS "aether_video_player.c" INCLUDE_DIRS "include"
    REQUIRES aether_mjpeg_player aether_mp4_player ac101s_audio
    PRIV_REQUIRES esp_timer freertos)

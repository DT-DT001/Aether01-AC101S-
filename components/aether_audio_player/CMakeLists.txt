idf_component_register(SRCS "aether_audio_player.c" "wav_stream.c" "mp3_stream.c" "music_eq.c"
                       INCLUDE_DIRS "include"
                       REQUIRES aether_media_fs aether_storage ac101s_audio aether_mp4_player nvs_flash esp_timer)

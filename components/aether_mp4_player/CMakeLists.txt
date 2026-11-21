idf_component_register(
    SRCS "aether_mp4_player.c" "mp4_demux.c" "mp4_aac.c" "mp4_audio.c" "codec_check.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES aether_media_fs aether_mjpeg_player aether_storage ac101s_audio
    PRIV_REQUIRES esp_h264 esp_audio_codec esp_timer freertos)

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND CONFIG_ESP_H264_S31_USE_ASM)
    # S31 PIE can access HP SRAM/PSRAM, but not LP SRAM. The decoder's private
    # INTERNAL-first heap requests must not silently land in the RTC heap.
    if(CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP AND NOT AETHER_H264_HP_ALLOCATOR)
        message(FATAL_ERROR "S31 optimized H264 with RTC heap requires its HP/PSRAM allocator shim")
    endif()
    if(CONFIG_ESP_H264_DUAL_TASK OR CONFIG_FREERTOS_UNICORE)
        message(FATAL_ERROR "S31 optimized H264 requires core1 and the single-task decoder")
    endif()
endif()

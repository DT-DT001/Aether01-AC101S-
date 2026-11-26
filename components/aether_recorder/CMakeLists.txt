idf_component_register(SRCS "aether_recorder.c" "aether_recorder_denoise.c"
                       INCLUDE_DIRS "include"
                       REQUIRES aether_storage ac101s_audio
                       PRIV_REQUIRES esp_partition
                       LDFRAGMENTS "nsnet3_memory.lf")

if(CONFIG_SR_NSN_NSNET3 AND CONFIG_MODEL_IN_FLASH AND NOT CONFIG_FREERTOS_UNICORE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AETHER_RECORDER_HAS_NSNET3)
endif()

idf_component_register(
    SRCS "audio_player.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_pipeline freertos
    PRIV_REQUIRES esp_timer
)

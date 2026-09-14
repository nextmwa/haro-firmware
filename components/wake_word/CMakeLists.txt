idf_component_register(
    SRCS "wake_word.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-sr audio_pipeline
    PRIV_REQUIRES esp_timer
)

idf_component_register(
    SRCS "audio_pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_codec_dev driver esp_driver_i2c esp_driver_i2s
)

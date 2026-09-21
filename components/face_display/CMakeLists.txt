idf_component_register(
    SRCS "face_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd driver esp_driver_i2c font5x7
)

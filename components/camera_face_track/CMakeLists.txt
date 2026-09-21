idf_component_register(
    SRCS "camera_face_track.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32-camera audio_pipeline server_client driver
)

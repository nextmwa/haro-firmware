if(IDF_TARGET STREQUAL "linux")
    set(main_reqs unity protocol orchestrator wifi_profiles font5x7)
else()
    set(main_reqs haro_config wifi_provisioning audio_pipeline buttons wake_word server_client face_display status_led servo camera_face_track orchestrator protocol esp_wifi esp_timer lwip)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${main_reqs}
)

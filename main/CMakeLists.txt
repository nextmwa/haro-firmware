if(IDF_TARGET STREQUAL "linux")
    set(main_reqs unity protocol orchestrator)
else()
    set(main_reqs haro_config wifi_provisioning audio_pipeline wake_word server_client face_display orchestrator protocol)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${main_reqs}
)

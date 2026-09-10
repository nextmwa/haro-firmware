if(IDF_TARGET STREQUAL "linux")
    set(main_reqs unity protocol)
else()
    set(main_reqs audio_pipeline wake_word)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${main_reqs}
)

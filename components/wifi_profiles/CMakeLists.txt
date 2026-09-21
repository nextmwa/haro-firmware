if(IDF_TARGET STREQUAL "linux")
    set(srcs "wifi_profiles.c" "test/test_wifi_profiles.c")
    set(reqs unity)
else()
    set(srcs "wifi_profiles.c")
    set(reqs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
    WHOLE_ARCHIVE
)

if(IDF_TARGET STREQUAL "linux")
    set(srcs "font5x7.c" "test/test_font5x7.c")
    set(reqs unity)
else()
    set(srcs "font5x7.c")
    set(reqs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
    WHOLE_ARCHIVE
)

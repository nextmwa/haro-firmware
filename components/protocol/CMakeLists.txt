if(IDF_TARGET STREQUAL "linux")
    set(srcs "protocol.c" "test/test_protocol.c")
    set(reqs cjson unity)
else()
    set(srcs "protocol.c")
    set(reqs cjson)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
    WHOLE_ARCHIVE
)

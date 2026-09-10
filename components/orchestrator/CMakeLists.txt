if(IDF_TARGET STREQUAL "linux")
    set(srcs "orchestrator.c" "test/test_orchestrator.c")
    set(reqs unity protocol)
else()
    set(srcs "orchestrator.c")
    set(reqs protocol)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
    WHOLE_ARCHIVE
)

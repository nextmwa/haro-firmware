if(CONFIG_IDF_TARGET_LINUX)
    set(srcs "protocol.c" "test/test_protocol.c")
    set(reqs unity)
    set(extra_includes "${IDF_PATH}/components/unity/include" "${IDF_PATH}/components/unity/unity/src")
else()
    set(srcs "protocol.c")
    set(reqs)
    set(extra_includes)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" ${extra_includes}
    REQUIRES ${reqs}
)

# For Linux target, configure cJSON from homebrew
if(CONFIG_IDF_TARGET_LINUX)
    # Try pkg-config first
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CJSON libcjson)
        if(CJSON_FOUND)
            target_include_directories(${COMPONENT_LIB} PUBLIC ${CJSON_INCLUDE_DIRS})
            target_link_directories(${COMPONENT_LIB} PUBLIC ${CJSON_LIBRARY_DIRS})
            target_link_libraries(${COMPONENT_LIB} PUBLIC ${CJSON_LIBRARIES})
        else()
            # Fallback to hardcoded homebrew paths (note: cJSON.h is in cjson/ subdirectory)
            target_include_directories(${COMPONENT_LIB} PUBLIC /opt/homebrew/Cellar/cjson/1.7.18/include/cjson)
            target_link_directories(${COMPONENT_LIB} PUBLIC /opt/homebrew/Cellar/cjson/1.7.18/lib)
            target_link_libraries(${COMPONENT_LIB} PUBLIC cjson)
        endif()
    else()
        # Fallback to hardcoded homebrew paths if pkg-config not available
        target_include_directories(${COMPONENT_LIB} PUBLIC /opt/homebrew/Cellar/cjson/1.7.18/include/cjson)
        target_link_directories(${COMPONENT_LIB} PUBLIC /opt/homebrew/Cellar/cjson/1.7.18/lib)
        target_link_libraries(${COMPONENT_LIB} PUBLIC cjson)
    endif()

    # Ensure UNITY_INCLUDE_CONFIG_H is defined for test compilation
    target_compile_definitions(${COMPONENT_LIB} PUBLIC UNITY_INCLUDE_CONFIG_H)
endif()

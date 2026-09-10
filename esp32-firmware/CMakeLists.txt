# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Check sdkconfig or sdkconfig.defaults for the target to set COMPONENTS
set(_target_check_file "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig")
    set(_target_check_file "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
    set(_target_check_file "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
endif()

if(_target_check_file)
    file(READ "${_target_check_file}" _sdkconfig_content)
    if(_sdkconfig_content MATCHES "CONFIG_IDF_TARGET=\"linux\"" OR _sdkconfig_content MATCHES "CONFIG_IDF_TARGET=linux")
        set(COMPONENTS main protocol)
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(haro_firmware)

cmake_minimum_required(VERSION 3.16)

if(DEFINED DEVICE_NUMBER)
    add_compile_definitions(DEVICE_NUMBER=${DEVICE_NUMBER})
    message(STATUS "Configuring for DEVICE_NUMBER=${DEVICE_NUMBER}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(translator_c3)

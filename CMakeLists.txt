cmake_minimum_required(VERSION 3.21)

project(CBFMacroBot VERSION 1.0.0 LANGUAGES CXX)

# Geode v5 requires C++23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(${PROJECT_NAME} SHARED)

# FORCE LINK: Look up the robot's SDK directory location directly
if(NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Please define GEODE_SDK environment variable")
else()
    add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
endif()

# Fallback setup macro for direct subdirectory inclusion
setup_geode_mod(${PROJECT_NAME})

# All sources live under src/.
file(GLOB_RECURSE PROJECT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# On Windows we want a console-less DLL so the mod loads cleanly.
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS"
    )
endif()

# Optimize for size on release builds.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O2 -fdata-sections -ffunction-sections>
        $<$<CXX_COMPILER_ID:MSVC>:/O2 /Gy /Gw>
    )
    if(WIN32)
        target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wl,--gc-sections>
        )
    endif()
endif()
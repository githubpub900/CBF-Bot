cmake_minimum_required(VERSION 3.21)

# Project name must match the add_library target below.
project(CBFMacroBot VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(${PROJECT_NAME} SHARED)

# Geode v4.0.0 macro setup command
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

# Optimize for size on release builds. Macro files are tiny by design, but
# we keep the binary small too so the .geode package stays lean.
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

cmake_minimum_required(VERSION 3.20)
project(SoundpadTS3Diag LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Soundpad-TS3-Diag currently targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TS3_SDK_DIR "${CMAKE_SOURCE_DIR}/vendor/ts3client-pluginsdk" CACHE PATH "Path to TeamSpeak 3 Client Plugin SDK")

if(NOT EXISTS "${TS3_SDK_DIR}/include/ts3_functions.h")
    message(FATAL_ERROR
        "TeamSpeak 3 Client Plugin SDK not found at ${TS3_SDK_DIR}. "
        "Run scripts/bootstrap.ps1 first.")
endif()

add_library(soundpad_ts3_diag SHARED
    src/ts3_plugin.cpp
)

target_include_directories(soundpad_ts3_diag PRIVATE
    "${TS3_SDK_DIR}/include"
)

target_compile_definitions(soundpad_ts3_diag PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(soundpad_ts3_diag PRIVATE /W4 /permissive-)
endif()

set_target_properties(soundpad_ts3_diag PROPERTIES
    PREFIX ""
    OUTPUT_NAME "soundpad_ts3_diag_win64"
)

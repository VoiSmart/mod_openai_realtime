cmake_minimum_required(VERSION 3.18)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MOD_OPENAI_REALTIME_VERSION LIMIT_COUNT 1)
if(NOT MOD_OPENAI_REALTIME_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version in MAJOR.MINOR.PATCH format")
endif()

project(mod_openai_audio_stream
        VERSION "${MOD_OPENAI_REALTIME_VERSION}"
        DESCRIPTION "Audio streaming module for FreeSWITCH."
        HOMEPAGE_URL "https://github.com/VoiSmart/mod_openai_realtime")

include(GNUInstallDirs)

option(ENABLE_LOCAL "Enable local compile/debug specific" OFF)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(ENABLE_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "ENABLE_SANITIZERS requires Clang or GCC")
    endif()

    message(STATUS "AddressSanitizer and UndefinedBehaviorSanitizer enabled")
    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    add_link_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
endif()

if(ENABLE_LOCAL)
    set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_check_modules(SpeexDSP REQUIRED IMPORTED_TARGET speexdsp)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
if(NOT FS_MOD_DIR)
    message(FATAL_ERROR "FreeSWITCH pkg-config metadata does not define modulesdir")
endif()
message(STATUS "FreeSWITCH modules dir: ${FS_MOD_DIR}")

set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "Disable ixwebsocket installation")
option(USE_TLS "Use TLS for secure WebSocket connections" ON)
add_subdirectory(libs/IXWebSocket)

add_library(mod_openai_audio_stream SHARED
    mod_openai_audio_stream.c
    mod_openai_audio_stream.h
    openai_audio_streamer_glue.h
    openai_audio_streamer_glue.cpp
    playback_queue.cpp
    playback_queue.h
    stream_protocol.cpp
    stream_protocol.h
    base64.cpp
)

set_target_properties(mod_openai_audio_stream PROPERTIES
    PREFIX ""
    POSITION_INDEPENDENT_CODE ON
    C_EXTENSIONS ON
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)

target_compile_features(mod_openai_audio_stream PRIVATE c_std_99)

target_include_directories(mod_openai_audio_stream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/IXWebSocket
)

target_link_libraries(mod_openai_audio_stream PRIVATE
    PkgConfig::FreeSWITCH
    PkgConfig::SpeexDSP
    ixwebsocket
    Threads::Threads
)

install(TARGETS ${PROJECT_NAME}
        COMPONENT ${PROJECT_NAME}
        DESTINATION ${FS_MOD_DIR})

cmake_minimum_required(VERSION 3.18)
project(mod_openai_audio_stream
        VERSION 1.0.0
        DESCRIPTION "Audio streaming module for FreeSWITCH."
        HOMEPAGE_URL "https://github.com/VoiSmart/mod_openai_realtime") 

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")

option(ENABLE_LOCAL "Enable local compile/debug specific" OFF)
if(ENABLE_LOCAL)
    set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
find_package(SpeexDSP REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
message(STATUS "FreeSWITCH modules dir: ${FS_MOD_DIR}")

set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "Disable ixwebsocket installation" FORCE)
set(USE_TLS ON CACHE BOOL "Use TLS for secure WebSocket connections" FORCE)
add_subdirectory(libs/IXWebSocket)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    # Add linker flags to strip symbols and reduce size of static library
    set_target_properties(ixwebsocket PROPERTIES LINK_FLAGS_RELEASE "-s -w") #-static-libgcc -static-libstdc++
endif()

add_library(mod_openai_audio_stream SHARED
    mod_openai_audio_stream.c
    mod_openai_audio_stream.h
    openai_audio_streamer_glue.h
    openai_audio_streamer_glue.cpp
    base64.cpp
)

set_property(TARGET mod_openai_audio_stream PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(mod_openai_audio_stream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs/IXWebSocket)

target_link_libraries(mod_openai_audio_stream PRIVATE PkgConfig::FreeSWITCH pthread)
target_link_libraries (mod_openai_audio_stream PRIVATE ixwebsocket)

install(TARGETS ${PROJECT_NAME}
        COMPONENT ${PROJECT_NAME}
        DESTINATION ${FS_MOD_DIR})


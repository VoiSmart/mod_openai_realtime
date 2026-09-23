cmake_minimum_required(VERSION 3.18)
project(mod_openai_audio_stream_tests LANGUAGES C CXX)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MODULE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

enable_testing()
add_subdirectory(unit)
add_test(NAME python COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p "test_*.py")
set_tests_properties(python PROPERTIES
    ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/integration"
    TIMEOUT 10)

option(BUILD_INTEGRATION_TESTS "Build the FreeSWITCH lifecycle test probe" OFF)
if(BUILD_INTEGRATION_TESTS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FREESWITCH REQUIRED IMPORTED_TARGET freeswitch)
    add_library(lifecycle_probe SHARED integration/lifecycle_probe.c)
    target_link_libraries(lifecycle_probe PRIVATE PkgConfig::FREESWITCH ${CMAKE_DL_LIBS})
    set_target_properties(lifecycle_probe PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
endif()

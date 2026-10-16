# A submission token grants report submission only; never use an API/admin token.
set(backtrace_default_url "https://submit.backtrace.io/r5rlauncher/657dc750570e574383ec9d78548fed4c5d5061be3033d7adc064a4c6c28e058d/minidump")
if(NOT "$ENV{RESKATE_BACKTRACE_URL}" STREQUAL "")
    set(backtrace_default_url "$ENV{RESKATE_BACKTRACE_URL}")
endif()
set(DINGOSDK_BACKTRACE_URL "${backtrace_default_url}" CACHE STRING "Backtrace HTTPS minidump submission URL (empty disables uploads)")
if(DINGOSDK_BACKTRACE_URL AND NOT DINGOSDK_BACKTRACE_URL MATCHES "^https://[^ \r\n\t]+$")
    message(FATAL_ERROR "DINGOSDK_BACKTRACE_URL must be an HTTPS minidump submission URL")
endif()
set(backtrace_url_escaped "${DINGOSDK_BACKTRACE_URL}")
string(REPLACE "\\" "\\\\" backtrace_url_escaped "${backtrace_url_escaped}")
string(REPLACE "\"" "\\\"" backtrace_url_escaped "${backtrace_url_escaped}")
find_package(Git QUIET)
set(backtrace_version "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=12
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE backtrace_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(backtrace_revision)
        set(backtrace_version "${backtrace_revision}")
    endif()
endif()
configure_file(cmake/templates/backtrace_config.h.in generated/backtrace_config.h @ONLY)

add_library(dingosdk_backtrace_client STATIC Engine/Core/Debug/client.cpp)
add_library(dingosdk_backtrace_upload STATIC Engine/Core/Debug/upload.cpp
    Engine/Core/Debug/native_dump.cpp Engine/Core/Debug/multipart.cpp)
target_link_libraries(dingosdk_backtrace_upload PRIVATE dingosdk_json winhttp bcrypt)
target_sources(dingosdk_launcher PRIVATE Launcher/crash_reporter.cpp)
target_link_libraries(dingosdk_launcher PRIVATE dingosdk_backtrace_upload dingosdk_json dbghelp)
target_link_libraries(dingosdk_logging PRIVATE dingosdk_backtrace_client)
# The launcher hosts crash capture for both executables; no third binary is shipped.
add_dependencies(dingosdk_runtime dingosdk_launcher)

# Release dumps need matching symbols; keep optimization and omit incremental linking.
# PDBALTPATH embeds only the PDB's file name, not the build machine's folder.
foreach(target dingosdk_runtime dingosdk_launcher dingosdk_server)
    target_compile_options(${target} PRIVATE /Zi)
    target_link_options(${target} PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF
        "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${target}>")
endforeach()
get_property(backtrace_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS backtrace_targets)
    get_target_property(kind ${target} TYPE)
    if(kind STREQUAL "STATIC_LIBRARY")
        target_compile_options(${target} PRIVATE /Zi)
    endif()
endforeach()

option(DINGOSDK_BUILD_BACKTRACE_TESTS "Build crash capture and upload regression tests" OFF)
if(DINGOSDK_BUILD_BACKTRACE_TESTS)
    enable_testing()
    add_executable(dingosdk_backtrace_tests Engine/Core/Debug/Test/backtrace_tests.cpp)
    target_link_libraries(dingosdk_backtrace_tests PRIVATE dingosdk_logging dingosdk_backtrace_upload dingosdk_json dbghelp)
    add_dependencies(dingosdk_backtrace_tests dingosdk_launcher)
    add_test(NAME backtrace COMMAND dingosdk_backtrace_tests "${CMAKE_CURRENT_BINARY_DIR}/backtrace-fixtures")
    set_tests_properties(backtrace PROPERTIES TIMEOUT 90)
endif()

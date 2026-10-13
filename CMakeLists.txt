cmake_minimum_required(VERSION 3.16)
project(logguard VERSION 0.4.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Layered design:
#   platform/   OS mapping (mmap) + crash handlers (POSIX signals / Win VEH)
#   core/       lock-free flight ring, rules engine, watchdog, lazy format
#   export/     MCAP container writer + optional runtime-loaded zstd codec
#   capi/       stable C ABI wrapping the C++ core
#
# No hard third-party dependencies. zstd is loaded at runtime via dlopen when
# available and enabled; builds and runs on systems without libzstd-dev.
# ---------------------------------------------------------------------------

option(LOGGUARD_ENABLE_ZSTD "Compress incident packages via runtime libzstd" ON)
option(LOGGUARD_BUILD_TOOLS "Build lgtool CLI" ON)
option(LOGGUARD_BUILD_EXAMPLES "Build examples" ON)
option(LOGGUARD_BUILD_TESTS "Build/run shell end-to-end tests" ON)
option(LOGGUARD_BUILD_BENCH "Build microbenchmarks" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    set(LOGGUARD_PLATFORM_SOURCES
        src/platform/clock_windows.cpp
        src/platform/thread_id_windows.cpp
        src/platform/file_util_windows.cpp
        src/platform/mapping_windows.cpp
        src/platform/crash_windows.cpp
        src/platform/stack_walk_windows.cpp)
else()
    set(LOGGUARD_PLATFORM_SOURCES
        src/platform/clock_posix.cpp
        src/platform/thread_id_posix.cpp
        src/platform/file_util_posix.cpp
        src/platform/mapping_posix.cpp
        src/platform/crash_posix.cpp
        src/platform/stack_walk_posix.cpp)
endif()

add_library(logguard
    ${LOGGUARD_PLATFORM_SOURCES}
    src/core/ring.cpp
    src/core/json_util.cpp
    src/core/ring_audit.cpp
    src/core/ring_repair.cpp
    src/capture/recorder.cpp
    src/capture/recorder_config.cpp
    src/capture/recorder_registry.cpp
    src/capture/burst_guard.cpp
    src/diagnosis/rule.cpp
    src/diagnosis/rule_engine.cpp
    src/diagnosis/fingerprint.cpp
    src/diagnosis/signature_diff.cpp
    src/diagnosis/sequence_rule.cpp
    src/diagnosis/symbolize.cpp
    src/diagnosis/session_stats.cpp
    src/diagnosis/system_snapshot.cpp
    src/export/mcap_writer.cpp
    src/export/zstd_codec.cpp
    src/export/slot_render.cpp
    src/export/exporter.cpp
    src/export/redaction.cpp
    src/export/mcap_verify.cpp
    src/export/query.cpp
    src/export/retention.cpp
    src/capi/logguard.cpp)

target_include_directories(logguard PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/export
    ${CMAKE_CURRENT_SOURCE_DIR}/src/capi)

find_package(Threads REQUIRED)
target_link_libraries(logguard PUBLIC Threads::Threads)

if(LOGGUARD_ENABLE_ZSTD)
    target_compile_definitions(logguard PRIVATE LOGGUARD_ENABLE_ZSTD=1)
    # dlopen/dlsym
    include(CheckLibraryExists)
    check_library_exists(dl dlopen "" HAVE_LIBDL)
    if(HAVE_LIBDL)
        target_link_libraries(logguard PRIVATE dl)
    endif()
endif()

if(MSVC)
    target_compile_options(logguard PRIVATE /W4)
else()
    target_compile_options(logguard PRIVATE -Wall -Wextra)
endif()

if(LOGGUARD_BUILD_TOOLS)
    add_executable(lgtool tools/lgtool.cpp)
    target_link_libraries(lgtool PRIVATE logguard)
    if(HAVE_LIBDL)
        target_link_libraries(lgtool PRIVATE dl)
    endif()
    target_include_directories(lgtool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/export)
endif()

if(LOGGUARD_BUILD_EXAMPLES)
    add_executable(crash_demo examples/crash_demo.cpp)
    target_link_libraries(crash_demo PRIVATE logguard)
    add_executable(basic examples/basic.cpp)
    target_link_libraries(basic PRIVATE logguard)
    add_executable(c_abi examples/c_abi.c)
    target_link_libraries(c_abi PRIVATE logguard)
    add_executable(c_multi examples/c_multi.c)
    target_link_libraries(c_multi PRIVATE logguard)
endif()

if(LOGGUARD_BUILD_BENCH)
    add_executable(bench_hotpath benchmarks/bench_hotpath.cpp)
    target_link_libraries(bench_hotpath PRIVATE logguard Threads::Threads)

    add_executable(bench_diagnosis benchmarks/bench_diagnosis.cpp)
    target_include_directories(bench_diagnosis PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(bench_diagnosis PRIVATE logguard Threads::Threads)
endif()

if(LOGGUARD_BUILD_TESTS)
    enable_testing()

    # Pure-logic unit tests (no ring/filesystem); cross-platform.
    add_executable(unit_tests tests/unit/test_diagnosis.cpp)
    target_include_directories(unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_tests PRIVATE logguard)
    add_test(NAME unit COMMAND unit_tests)

    add_executable(unit_redaction tests/unit/test_redaction.cpp)
    target_include_directories(unit_redaction PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_redaction PRIVATE logguard)
    add_test(NAME unit_redaction COMMAND unit_redaction)

    add_executable(unit_recorder tests/unit/test_recorder.cpp)
    target_include_directories(unit_recorder PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_recorder PRIVATE logguard Threads::Threads)
    add_test(NAME unit_recorder COMMAND unit_recorder)

    add_executable(unit_sequence tests/unit/test_sequence.cpp)
    target_include_directories(unit_sequence PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_sequence PRIVATE logguard Threads::Threads)
    if(HAVE_LIBDL)
        target_link_libraries(unit_sequence PRIVATE dl)
    endif()
    add_test(NAME unit_sequence COMMAND unit_sequence)

    add_executable(unit_mcap tests/unit/test_mcap_verify.cpp)
    target_include_directories(unit_mcap PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_mcap PRIVATE logguard Threads::Threads)
    if(HAVE_LIBDL)
        target_link_libraries(unit_mcap PRIVATE dl)
    endif()
    add_test(NAME unit_mcap COMMAND unit_mcap)

    add_executable(unit_ring tests/unit/test_ring_audit.cpp)
    target_include_directories(unit_ring PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_ring PRIVATE logguard Threads::Threads)
    add_test(NAME unit_ring COMMAND unit_ring)

    add_executable(unit_repair tests/unit/test_ring_repair.cpp)
    target_include_directories(unit_repair PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_repair PRIVATE logguard Threads::Threads)
    add_test(NAME unit_repair COMMAND unit_repair)

    add_executable(unit_stats tests/unit/test_session_stats.cpp)
    target_include_directories(unit_stats PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_stats PRIVATE logguard Threads::Threads)
    add_test(NAME unit_stats COMMAND unit_stats)

    add_executable(unit_burst tests/unit/test_burst.cpp)
    target_include_directories(unit_burst PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_burst PRIVATE logguard Threads::Threads)
    add_test(NAME unit_burst COMMAND unit_burst)

    add_executable(unit_query tests/unit/test_query.cpp)
    target_include_directories(unit_query PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_query PRIVATE logguard Threads::Threads)
    add_test(NAME unit_query COMMAND unit_query)

    add_executable(unit_retention tests/unit/test_retention.cpp)
    target_include_directories(unit_retention PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_retention PRIVATE logguard Threads::Threads)
    add_test(NAME unit_retention COMMAND unit_retention)

    add_executable(unit_snapshot tests/unit/test_system_snapshot.cpp)
    target_include_directories(unit_snapshot PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(unit_snapshot PRIVATE logguard Threads::Threads)
    add_test(NAME unit_snapshot COMMAND unit_snapshot)

    # Fault-injection end-to-end tests are POSIX-only (real SIGSEGV/SIGKILL).
    if(UNIX AND NOT APPLE)
        add_test(NAME e2e
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(e2e PROPERTIES
            ENVIRONMENT "BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()

# Install rules: shared/static lib + public headers + CLI
include(GNUInstallDirs)
install(TARGETS logguard
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(LOGGUARD_BUILD_TOOLS)
    install(TARGETS lgtool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

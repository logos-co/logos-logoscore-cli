# ── Unit tests (no external runtime needed) ──────────────────────────────────

# Testable library: config, connection_file, output, commands (with mock client)
set(TEST_LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/connection_file.cpp
    ${CMAKE_SOURCE_DIR}/src/client/output.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/status_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/load_module_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/unload_module_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/reload_module_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/list_modules_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/module_info_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/call_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/watch_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/stats_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/stop_command.cpp
)

add_library(logoscore_testlib STATIC ${TEST_LIB_SOURCES})
target_include_directories(logoscore_testlib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${LOGOS_LIBLOGOS_ROOT}/include
)
target_link_libraries(logoscore_testlib PUBLIC
    CLI11::CLI11
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)

# Unit tests
add_executable(unit_tests
    test_config.cpp
    test_connection_file.cpp
    test_output.cpp
    test_commands.cpp
    test_mode_detection.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(unit_tests PRIVATE
    logoscore_testlib
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

gtest_discover_tests(unit_tests)

# ── CLI integration tests (require compiled logoscore binary) ─────────────────

add_executable(cli_tests
    test_cli.cpp
)

target_link_libraries(cli_tests PRIVATE
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

target_include_directories(cli_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

gtest_discover_tests(cli_tests)

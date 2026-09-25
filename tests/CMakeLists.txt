# ── Unit tests (no external runtime needed) ──────────────────────────────────

# Testable library: config, daemon_state, output, commands (with mock client)
set(TEST_LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/paths.cpp
    ${CMAKE_SOURCE_DIR}/src/yaml_json.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/daemon_state.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/access_policy_arg.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/port_allocator.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/token_store.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/log_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/package_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/client/client_state.cpp
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
    ${CMAKE_SOURCE_DIR}/src/client/commands/issue_token_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/revoke_token_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/list_tokens_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/package_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/catalog_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/config_command.cpp
)

add_library(logosctl_testlib STATIC ${TEST_LIB_SOURCES})
target_include_directories(logosctl_testlib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${LOGOS_LIBLOGOS_ROOT}/include
)
target_link_libraries(logosctl_testlib PUBLIC
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    fmt::fmt
    yaml-cpp::yaml-cpp
    spdlog::spdlog
)
if(WIN32)
    # port_allocator.cpp probes ports with Winsock, as logosctl does.
    target_link_libraries(logosctl_testlib PUBLIC ws2_32)
endif()

# Unit tests
add_executable(unit_tests
    test_config.cpp
    test_log_sink.cpp
    test_access_policy_arg.cpp
    test_daemon_state.cpp
    test_paths.cpp
    test_output.cpp
    test_commands.cpp
    test_mode_detection.cpp
    test_token_store.cpp
    test_port_allocator.cpp
    test_process_util.cpp
    test_local_endpoint.cpp
    test_package_bootstrap.cpp
    test_rpc_deadlines.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(unit_tests PRIVATE
    logosctl_testlib
    GTest::gtest_main
    # test_local_endpoint's oracle: the transport the daemon binds with.
    logos-protocol::logos_qt_remote_plain_transport
)

gtest_discover_tests(unit_tests)

# PlainRpcClient against the shared plain runtime the daemon links.
add_executable(plain_rpc_tests
    test_plain_rpc.cpp
    ${CMAKE_SOURCE_DIR}/src/plain_rpc.cpp
)
target_include_directories(plain_rpc_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(plain_rpc_tests PRIVATE
    logos-protocol::logos_protocol_plain_shared
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
gtest_discover_tests(plain_rpc_tests)

# test_process_util.cpp's child on Windows, which has no fork().
if(WIN32)
    add_executable(logosctl_test_child test_child.cpp)
    add_dependencies(unit_tests logosctl_test_child)
endif()

# ── CLI integration tests (require compiled logosctl binary) ─────────────────

add_executable(cli_tests
    test_cli.cpp
)

target_link_libraries(cli_tests PRIVATE
    GTest::gtest_main
)

target_include_directories(cli_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

gtest_discover_tests(cli_tests)

# ── Daemon-backed integration tests (own target, own CTest entry) ────────────
#
# Kept separate from cli_tests and registered as a SINGLE CTest test
# (not gtest_discover_tests): the suite shares one daemon across all
# LoadedModuleTest cases via SetUpTestSuite(). Per-case discovery would
# make CTest spawn one process per test, defeating that sharing and
# starting a fresh daemon for every case. The Nix check runs the binary
# directly (also one process), so sharing holds there too.

add_executable(integration_tests
    test_integration.cpp
)

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
)

target_include_directories(integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME integration_tests COMMAND integration_tests)

# logoscore's copies of the suites are not ported to Windows; they go with the tool.
if(WIN32)
    return()
endif()

# The same suite against logoscore. Both binaries ship, so both are tested;
# this target goes away with the tool.
add_executable(cli_tests_logoscore
    test_cli_logoscore.cpp
)

target_link_libraries(cli_tests_logoscore PRIVATE
    GTest::gtest_main
)

target_include_directories(cli_tests_logoscore PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

gtest_discover_tests(cli_tests_logoscore)

# Same again for logoscore. Deleted with the tool.
add_executable(integration_tests_logoscore
    test_integration_logoscore.cpp
)

target_link_libraries(integration_tests_logoscore PRIVATE
    GTest::gtest_main
)

target_include_directories(integration_tests_logoscore PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)


# CLI integration tests

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

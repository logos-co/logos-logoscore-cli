cmake_minimum_required(VERSION 3.14)
project(LogoscoreCLI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt packages
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)

set(CMAKE_AUTOMOC ON)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── logos-liblogos dependency ─────────────────────────────────────────────────
# LOGOS_LIBLOGOS_ROOT must point to the installed logos-liblogos tree
# (containing lib/liblogos_core.{so,dylib} and include/logos_core.h)
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT OR LOGOS_LIBLOGOS_ROOT STREQUAL "")
    message(FATAL_ERROR "LOGOS_LIBLOGOS_ROOT is not set. Pass -DLOGOS_LIBLOGOS_ROOT=<path>")
endif()
get_filename_component(LOGOS_LIBLOGOS_ROOT "${LOGOS_LIBLOGOS_ROOT}" ABSOLUTE)

message(STATUS "Using logos-liblogos at ${LOGOS_LIBLOGOS_ROOT}")

find_library(LOGOS_CORE_LIBRARY
    NAMES logos_core
    PATHS "${LOGOS_LIBLOGOS_ROOT}/lib"
    NO_DEFAULT_PATH
)

if(NOT LOGOS_CORE_LIBRARY)
    message(FATAL_ERROR "liblogos_core not found in ${LOGOS_LIBLOGOS_ROOT}/lib")
endif()

add_library(logos_core SHARED IMPORTED)
set_target_properties(logos_core PROPERTIES
    IMPORTED_LOCATION "${LOGOS_CORE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_LIBLOGOS_ROOT}/include;${LOGOS_LIBLOGOS_ROOT}/include/cpp"
)

# ── logos-module-client dependency ────────────────────────────────────────────
# LOGOS_MODULE_CLIENT_ROOT must point to the installed logos-module-client tree
# (containing lib/liblogos_module_client.{so,dylib} and include/)
if(NOT DEFINED LOGOS_MODULE_CLIENT_ROOT OR LOGOS_MODULE_CLIENT_ROOT STREQUAL "")
    message(FATAL_ERROR "LOGOS_MODULE_CLIENT_ROOT is not set. Pass -DLOGOS_MODULE_CLIENT_ROOT=<path>")
endif()
get_filename_component(LOGOS_MODULE_CLIENT_ROOT "${LOGOS_MODULE_CLIENT_ROOT}" ABSOLUTE)

message(STATUS "Using logos-module-client at ${LOGOS_MODULE_CLIENT_ROOT}")

find_library(LOGOS_MODULE_CLIENT_LIBRARY
    NAMES logos_module_client
    PATHS "${LOGOS_MODULE_CLIENT_ROOT}/lib"
    NO_DEFAULT_PATH
)

if(NOT LOGOS_MODULE_CLIENT_LIBRARY)
    message(FATAL_ERROR "liblogos_module_client not found in ${LOGOS_MODULE_CLIENT_ROOT}/lib")
endif()

add_library(logos_module_client SHARED IMPORTED)
set_target_properties(logos_module_client PROPERTIES
    IMPORTED_LOCATION "${LOGOS_MODULE_CLIENT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_CLIENT_ROOT}/include"
)

# ── logoscore executable ──────────────────────────────────────────────────────
set(CLI_SOURCES
    src/main.cpp
    src/config.cpp

    # Daemon path
    src/daemon/daemon.cpp
    src/daemon/connection_file.cpp

    # Client path
    src/client/client.cpp
    src/client/output.cpp
    src/client/commands/command.cpp
    src/client/commands/status_command.cpp
    src/client/commands/load_module_command.cpp
    src/client/commands/unload_module_command.cpp
    src/client/commands/reload_module_command.cpp
    src/client/commands/list_modules_command.cpp
    src/client/commands/module_info_command.cpp
    src/client/commands/call_command.cpp
    src/client/commands/watch_command.cpp
    src/client/commands/stats_command.cpp
    src/client/commands/stop_command.cpp

    # Core service (in-process daemon module)
    src/core_service/core_service_impl.cpp
    src/core_service/core_service_dispatch.cpp

    # SDK provider implementation (needed for in-process core_service)
    ${LOGOS_LIBLOGOS_ROOT}/include/cpp/logos_provider_object.cpp

    # Inline path (legacy)
    src/inline/command_line_parser.cpp
    src/inline/call_executor.cpp
)

add_executable(logoscore ${CLI_SOURCES})

target_link_libraries(logoscore PRIVATE
    logos_core
    logos_module_client
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)

target_include_directories(logoscore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LOGOS_LIBLOGOS_ROOT}/include
    ${LOGOS_LIBLOGOS_ROOT}/include/cpp
    ${LOGOS_MODULE_CLIENT_ROOT}/include
)

# RPATH so the binary finds liblogos_core at runtime
if(APPLE)
    set(_rpath "@loader_path/../lib")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _rpath "${_qt_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _rpath)
    set_target_properties(logoscore PROPERTIES
        INSTALL_RPATH "${_rpath}"
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logoscore PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found via find_package, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
else()
    message(STATUS "Using system GoogleTest")
endif()

enable_testing()
include(GoogleTest)

add_subdirectory(tests)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS logoscore
    RUNTIME DESTINATION bin
)

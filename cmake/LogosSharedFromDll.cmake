# Windows: take the shared C++ runtime from liblogos_core.dll, not from the
# static archives.
#
# logosctl.exe / logoscore.exe link liblogos_core.dll AND, through
# logos-qt-sdk::logos_qt_sdk, the liblogos_qt_sdk.a / liblogos_protocol.a static
# archives. On ELF and Mach-O that is harmless:
# liblogos_core exports the symbols and the loader interposes them, so there is
# one LogosAPI, one TokenManager, one of everything. PE has no interposition, so
# each image binds its own static copy and gets its own function-local statics.
# Here the symptom is not subtle at all -- the link simply fails:
#   multiple definition of `TokenManager::instance()'
#   ... liblogos_core.dll.a(liblogos_core_dll_d000294.o): first defined here
# (also LogosAPIClient::invokeRemoteMethod, LogosProviderObject::* and
# logos::reapStaleSockets). Same cause as Basecamp's silent token failure: once
# liblogos_core.dll became the single provider, a consumer that ALSO links the
# archives has two definitions of everything it exports.
#
# liblogos_core.dll exports those symbols explicitly (see
# logos-liblogos/src/CMakeLists.txt) and the consumers declare them
# __declspec(dllimport) via LOGOS_SHARED_USE_DLL. That alone is NOT enough,
# because ld picks archive members by object file for reasons that have nothing
# to do with our symbols: measured here, main_ui's AppsModel.cpp.obj referenced
# std::string's move constructor, ld satisfied it out of liblogos_qt_sdk.a's
# logos_api.cpp.obj, and that one object dragged in LogosAPI, LogosAPIClient and
# TokenManager behind it -- each then colliding with the DLL's export
# ("multiple definition of `LogosAPI::LogosAPI'"). No export list can prevent
# that; the archive simply must not be a candidate.
#
# So the archives are replaced by an EMPTY one. Everything else the imported
# targets carry -- include directories, Qt/OpenSSL/Boost/nlohmann link
# interfaces, compile features -- is untouched, which is why this is done by
# repointing IMPORTED_LOCATION rather than by dropping the
# target_link_libraries() call and re-listing the usage requirements by hand.
#
# Off Windows this file is never included: ELF/Mach-O already resolve to the one
# provider, and there is nothing to fix.

function(logos_use_shared_runtime_from_dll)
    if(NOT WIN32)
        return()
    endif()

    # One empty archive per build tree, shared by every target that needs it.
    set(_stub "${CMAKE_BINARY_DIR}/logos_shared_from_dll_stub.a")
    if(NOT EXISTS "${_stub}")
        execute_process(
            COMMAND "${CMAKE_AR}" crs "${_stub}"
            RESULT_VARIABLE _ar_rc
            OUTPUT_QUIET ERROR_VARIABLE _ar_err)
        if(NOT _ar_rc EQUAL 0 OR NOT EXISTS "${_stub}")
            message(FATAL_ERROR
                "Could not create the empty archive that stands in for the "
                "logos static libraries on Windows: ${_ar_err}")
        endif()
    endif()

    foreach(_tgt IN LISTS ARGN)
        if(TARGET ${_tgt})
            set_target_properties(${_tgt} PROPERTIES IMPORTED_LOCATION "${_stub}")
            message(STATUS "Windows: ${_tgt} provided by liblogos_core.dll, static archive suppressed")
        endif()
    endforeach()
endfunction()

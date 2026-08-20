# Take the shared C++ runtime from liblogos_core, not from the static archives.
#
# logosctl / logoscore link liblogos_core AND, through
# logos-qt-host::logos_qt_host, the liblogos_qt_host.a / liblogos_protocol.a
# static archives. Every image that links an archive gets its own copy of the
# code in it, and therefore its own copy of every function-local static inside
# it: TokenManager::instance, the per-identity StoreRegistry, the host-services
# grant.
#
# On Windows the symptom is not subtle at all -- the link simply fails:
#   multiple definition of `TokenManager::instance()'
#   ... liblogos_core.dll.a(liblogos_core_dll_d000294.o): first defined here
# (also LogosAPIClient::invokeRemoteMethod, LogosProviderObject::* and
# logos::reapStaleSockets). Once liblogos_core.dll became the single provider, a
# consumer that ALSO links the archives has two definitions of everything it
# exports.
#
# THIS IS NOT A WINDOWS-ONLY PROBLEM, and the loud Windows failure hid that.
# An earlier version of this file returned early off Windows, under a comment
# asserting ELF and Mach-O "already resolve to the one provider, and there is
# nothing to fix". That is false for Mach-O, whose two-level namespace gives no
# interposition. It merely fails QUIETLY there instead of at link time: the
# image binds its own copy and the split-brain shows up at runtime as refused
# calls. Measured in logos-basecamp: ONE reference to LogosAPI::forIdentity
# pulled logos_api.cpp.o into the executable and produced 31 "ModuleProxy:
# rejecting unauthorized call" lines against a baseline of 0.
#
# ELF does interpose, so it genuinely collapses duplicates -- but the archives
# are emptied there too, so the invariant is ONE rule on three platforms.
#
# Declaring the symbols __declspec(dllimport) (LOGOS_SHARED_USE_DLL, Windows
# only) is NOT sufficient on its own: ld picks archive members by object file
# for reasons that have nothing to do with our symbols, so one incidental
# reference can drag LogosAPI, LogosAPIClient and TokenManager in behind it. No
# export list can prevent that; the archive simply must not be a candidate.
#
# So the archives are replaced by an EMPTY one. Everything else the imported
# targets carry -- include directories, Qt/OpenSSL/nlohmann link interfaces,
# compile features -- is untouched, which is why this repoints IMPORTED_LOCATION
# rather than dropping the target_link_libraries() call.

function(logos_use_shared_runtime_from_dll)
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
        if(NOT TARGET ${_tgt})
            message(FATAL_ERROR
                "logos_use_shared_runtime_from_dll: ${_tgt} is not a target. It must be "
                "an IMPORTED target whose archive can be replaced by the empty stand-in; "
                "skipping it would leave a static copy of the shared runtime in this "
                "image alongside liblogos_core.dll's exported one.")
        endif()
        set_target_properties(${_tgt} PROPERTIES IMPORTED_LOCATION "${_stub}")
        message(STATUS "${_tgt} provided by liblogos_core, static archive suppressed")
    endforeach()
endfunction()

#pragma once

#include <string>

namespace paths {

// Returns the directory containing the running executable, using platform
// APIs (no Qt). Returns an empty string if the platform call fails.
std::string executableDir();

// Absolute path of the running binary. Used by --detach to re-exec itself:
// macOS forbids continuing to run a forked process that has already
// initialised CoreFoundation, so the child must exec() rather than carry on.
std::string executablePath();

// Returns "{executableDir}/../modules" if that directory exists, or empty.
std::string bundledModulesDir();

// The package modules (package_manager, package_downloader) bundled for
// logosctl, kept in their own directory rather than alongside
// capability_module. logoscore does not add this to its search path, so its
// module list stays exactly what it reports today.
std::string bundledPackageModulesDir();

} // namespace paths

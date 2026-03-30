#pragma once

#include <string>

namespace paths {

// Returns the directory containing the running executable, using platform
// APIs (no Qt). Returns an empty string if the platform call fails.
std::string executableDir();

// Returns "{executableDir}/../modules" if that directory exists, or empty.
std::string bundledModulesDir();

} // namespace paths

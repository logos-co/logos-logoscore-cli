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

// The path to re-exec this process with, given the argv[0] we were invoked
// under. NOT the same question as executablePath().
//
// A portable Linux bundle installs `bin/logosctl` as a launcher script whose
// job is to run the real ELF through a bundled dynamic loader; the ELF sits
// beside it as `bin/.logosctl.elf` with a PT_INTERP that does not exist on the
// host. executablePath() resolves to that ELF, and exec'ing it directly fails
// with ENOENT -- the kernel reporting the missing *interpreter*, not the
// missing binary -- so the re-exec died with status 127 and no output at all.
//
// argv[0] is what the caller actually ran, which is the launcher, so prefer it:
// by path when it has a slash, via PATH when it is a bare name. Fall back to
// executablePath() only when neither yields something executable (argv[0] can
// be absent or a lie).
std::string relaunchPath(const char* argv0);

// Returns "{executableDir}/../modules" if that directory exists, or empty.
std::string bundledModulesDir();

// The package modules (package_manager, package_downloader) bundled for
// logosctl, kept in their own directory rather than alongside
// capability_module. logoscore does not add this to its search path, so its
// module list stays exactly what it reports today.
std::string bundledPackageModulesDir();

} // namespace paths

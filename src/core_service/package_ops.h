#ifndef PACKAGE_OPS_H
#define PACKAGE_OPS_H

#include <logos_json.h>
#include <string>
#include <vector>

class LogosAPI;

// Daemon-side orchestration for the mutating package operations.
//
// Why these live in the daemon rather than the client: the package_manager
// module gates install/upgrade/uninstall behind a listener-ack protocol that
// requires ackPendingAction within 3 seconds of the `before*` event. Driving
// that from a short-lived client process would mean holding an event
// subscription open, interleaving it with outbound calls, and winning a
// three-second race across the RPC boundary. In-process the ack is immediate
// and cannot lose the race, and every client command stays thin and stateless.
//
// This is the CLI's counterpart to basecamp's PackageCoordinator.
namespace package_ops {

enum class Op { Install, Upgrade, Remove };

struct Options {
    // install/upgrade: resolve and act on the dependency closure. Off means
    // "only the packages I named", which fails if a dependency is missing.
    bool withDeps = true;
    // remove: take the package's dependents with it. Off means "only this
    // package", which fails if anything still depends on it.
    bool withDependents = true;

    std::string version;      // pin an exact version
    std::string rootHash;     // disambiguate two releases sharing a version
    std::string catalog;      // restrict to one catalog (url or name)

    // Local artifacts to install instead of resolving from a catalog
    // (--file / --dir, already expanded to concrete .lgx paths).
    std::vector<std::string> localFiles;
};

// What the operation would do, without doing any of it. Shape:
//   { status, op, changes: [{name, action, fromVersion, toVersion, repository}],
//     affected_loaded: [names], errors: [...] }
//
// `action` is one of install | installed | reinstall | upgrade, the same
// classification basecamp's confirmation dialog shows. `affected_loaded` is
// the set of currently-loaded modules the operation will stop and restart.
LogosMap plan(LogosAPI* api, Op op,
              const std::vector<std::string>& names,
              const Options& opts);

// Execute. Returns the plan plus what actually happened:
//   { status, op, changes, affected_loaded, installed: [...], removed: [...],
//     reloaded: [...], failed_step?, error? }
//
// Newly installed packages are deliberately NOT loaded — installing puts
// files on disk, loading is a separate explicit act. Only modules that were
// already running before the operation are restarted afterwards.
LogosMap apply(LogosAPI* api, Op op,
               const std::vector<std::string>& names,
               const Options& opts);

// Fetch a .lgx without installing it, and put it where the caller asked.
//
// The download itself lands wherever package_downloader chose -- it takes no
// destination, so that is $TMPDIR on the daemon's host. This moves the result
// to `destDir`, or to the session's cache directory when `destDir` is empty,
// which is what makes both `package download -o` and the config's
// `dirs.cache` mean anything.
//
// `destDir` is a path on the DAEMON's filesystem. The client resolves a
// relative -o against its own working directory before sending, so a local
// daemon -- the usual case -- does exactly what the user typed; against a
// remote daemon the path is remote, and a bad one fails loudly here rather
// than silently writing somewhere else.
//
// Shape: { status, result: { name, version, path, repository, ... } }
LogosMap download(LogosAPI* api, const std::string& name,
                  const Options& opts, const std::string& destDir);

} // namespace package_ops

#endif // PACKAGE_OPS_H

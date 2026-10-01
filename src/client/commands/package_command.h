#ifndef PACKAGE_COMMAND_H
#define PACKAGE_COMMAND_H

#include "command.h"

namespace package_confirmation {
enum class Choice { All, Mandatory, Cancel };
Choice parse(const std::string& input);
// Suffix for a change row: " (optional)", or " (optional, required by X)" for a
// package that comes only with selected optional packages.
std::string optionalNote(const nlohmann::json& change);
}

// The `package` group: install, upgrade, remove, ls, show, deps, search,
// download.
//
// The mutating three are plan-print-prompt-apply against core_service; the
// queries are thin proxies onto the bundled package_manager /
// package_downloader modules. All of them need a running daemon, because that
// is where the package modules live.
class PackageCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "package"; }
    std::string description() const override {
        return "Install, remove and inspect packages";
    }

    // One row of `package deps` human output. Public because reaching it
    // through deps() needs a live daemon, and the rendering is worth a test of
    // its own.
    static std::string formatDependencyRow(const LogosMap& n);

private:
    int mutate(const std::string& op, const std::vector<std::string>& args);
    int list(const std::vector<std::string>& args);
    int show(const std::vector<std::string>& args);
    int deps(const std::vector<std::string>& args);
    int search(const std::vector<std::string>& args);
    int download(const std::vector<std::string>& args);
};

#endif // PACKAGE_COMMAND_H

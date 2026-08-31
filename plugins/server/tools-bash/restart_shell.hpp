#pragma once

#include "minimal/commands/command.hpp"
#include "minimal/plugins/service_registry.hpp"

namespace minimal::tools_bash {

// /restart-shell, registered by tools-bash (the state it resets lives here).
//
// Resolves the shell-state store THROUGH the service registry rather than
// reaching the plugin-internal static store directly. That is not ceremony:
// the service path is the track-L pattern (docs/EXTENSIBILITY.md §2.2), and
// driving it keeps the same lookup a third-party consumer would use honest --
// attribution, collision detection and the absent-provider degrade all run.
//
// Degrade: a null Find result (registry absent, or the service not provided --
// a stale build predating register_services) answers a kError result. It never
// throws and never blocks: a broken command must not take a turn with it.
class RestartShellCommand : public minimal::commands::HarnessCommand {
public:
    // Borrowed; may be null (a host that offers no service seam). The pointer
    // is captured at registration and consulted at EXECUTE time -- plugin load
    // order is file order, so resolution must be lazy.
    explicit RestartShellCommand(const minimal::plugins::ServiceRegistry* services);

    std::string_view Name() const override;
    std::string_view Summary() const override;
    std::string_view Usage() const override;

    minimal::commands::CommandResult Execute(minimal::commands::CommandContext& context,
                                             const minimal::commands::CommandArgs&) override;

private:
    const minimal::plugins::ServiceRegistry* services_;
};

}  // namespace minimal::tools_bash

// Exercises minimal_plugin_register_project_commands: the ONLY fixture that
// exports it. Registers "global-only" through the ordinary startup hook and
// "shared" through BOTH hooks (with different summaries), so a test can prove
// the project registry's "shared" shadows the global one of the same name --
// the whole point of the two being separate registries rather than one.
#include "minimal/plugins/plugin_abi.h"

namespace {

class FixtureCommand : public minimal::commands::HarnessCommand {
public:
    FixtureCommand(std::string name, std::string summary)
        : name_(std::move(name)), summary_(std::move(summary)) {}
    std::string_view Name() const override { return name_; }
    std::string_view Summary() const override { return summary_; }
    std::string_view Usage() const override { return name_; }
    bool ReadOnly() const override { return true; }
    minimal::commands::CommandResult Execute(minimal::commands::CommandContext&,
                                             const minimal::commands::CommandArgs&) override {
        return minimal::commands::CommandResult::Ok(summary_);
    }

private:
    std::string name_;
    std::string summary_;
};

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-project-commands"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_commands(minimal::commands::CommandRegistry& registry,
                                      const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureCommand>("global-only", "from the global registry"));
    registry.Register(std::make_unique<FixtureCommand>("shared", "from the global registry"));
}

void minimal_plugin_register_project_commands(
    minimal::commands::CommandRegistry& registry, const std::filesystem::path& project_root,
    const minimal::plugins::PluginRegistrationContext&) {
    // Named after the root so a test can tell WHICH root's registry it is
    // looking at without any other bookkeeping.
    registry.Register(std::make_unique<FixtureCommand>(
        "project-only", "from the project registry at " + project_root.string()));
    registry.Register(
        std::make_unique<FixtureCommand>("shared", "from the project registry"));
}
}

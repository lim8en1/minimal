#pragma once

// Everything a /-command needs, without a session.
//
// CommandContext takes a SessionControl INTERFACE rather than agent::Session
// precisely so this is possible: a command needs a preset, a loop and an
// executor, not a worker thread, a gRPC stream and a SQLite transcript. The loop
// here is REAL, though -- /clear and /compact act on it, and a fake loop would
// only assert that the fake was called.

#include <memory>
#include <optional>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../../plugins/server/commands-core/core_commands.hpp"
#include "fakes.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_dispatcher.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "temp_dir.hpp"

namespace minimal::testing {

// Answers whatever it was told to. Empty means "no answer", which is a different
// outcome from "No" and the dispatcher must treat it as one.
class FakeConfirmationBroker : public commands::ConfirmationBroker {
public:
    std::string answer = "Yes";
    int calls = 0;
    commands::ConfirmationRequest last;

    commands::ConfirmationResponse RequestConfirmation(
        const commands::ConfirmationRequest& request) override {
        ++calls;
        last = request;
        return commands::ConfirmationResponse{answer};
    }
};

class ThrowingCommand : public commands::HarnessCommand {
public:
    std::string_view Name() const override { return "boom"; }
    std::string_view Summary() const override { return "throws"; }
    std::string_view Usage() const override { return "/boom"; }
    commands::CommandResult Execute(commands::CommandContext&,
                                    const commands::CommandArgs&) override {
        throw std::runtime_error("deliberate");
    }
};

// Two presets over two models on ONE provider, which is what lets the precedence
// test distinguish "the preset's model" from "the one the user picked".
class CommandFixture : public commands::SessionControl {
public:
    CommandFixture() {
        Write("providers.json",
              R"({"providers":{"main":{"backend":"fake","url":"http://x/v1",
                   "models":{"fast":{"context_window_tokens":1000},
                             "slow":{"context_window_tokens":2000}}}}})");
        Write("settings.json", R"({"default_model":"main/fast"})");
        Write("permissions.json", "{}");
        Write("prompts/compression.md", "SUMMARIZE.\n");
        Write("agents/build.md", "---\nname: build\nmodel: main/fast\n---\nbody\n");
        Write("agents/other.md", "---\nname: other\nmodel: main/fast\n---\nother body\n");

        skills_ = std::make_unique<config::SkillRegistry>(config::SkillRegistry::Load(dir_.path()));
        presets_ = std::make_unique<agent::AgentPresetRegistry>(
            dir_.path(), *skills_,
            config::LoadSettingsConfig(dir_.path() / "settings.json"));
        preset_ = presets_->Resolve("build");

        executor_ = std::make_unique<tools::ToolExecutor>(tools_, checker_, AllowAllPolicy(),
                                                        tools::McpPermissionPolicy{},
                                                        tools::PermissionRunMode::Normal);
        agent::SessionContext context{common::DefaultLogger()};
        context.session_id = "s1";
        loop_ = std::make_unique<agent::AgentLoop>(
            provider_, *executor_, tools_, store_, preset_, agent::LoopConfig{}, std::move(context),
            [] { return false; }, [](const llm::StreamEvent&) { return true; },
            // A real-shaped compactor: /compact must actually fold history, not
            // just be dispatched. Returns a summary without a provider call.
            [](std::span<const llm::Message>, llm::LlmProvider&,
               const agent::CompactionRequest&) {
                return agent::CompactionResult{UserText("[summary]"), 42};
            });

        for (auto& command : commands_core::MakeCoreCommands()) {
            registry.Register(std::move(command));
        }
        registry.Register(commands_core::MakeHelpCommand(registry));
    }

    // Gives the loop a history to clear or compact.
    void SeedHistory() {
        provider_.PushResponse(TextResponse("hello"));
        loop_->RunTurn(UserText("hi"));
    }

    commands::CommandResult Run(const std::string& name, std::vector<std::string> positional = {},
                                bool busy = false) {
        commands::CommandArgs args;
        args.positional = std::move(positional);
        commands::CommandContext context(this, *presets_, confirmations, common::DefaultLogger());
        return commands::Dispatch(registry, name, args, context, busy);
    }

    // What the durable store holds, which /clear must never touch.
    std::size_t stored_messages() const { return store_.messages.size(); }

    // --- commands::SessionControl ------------------------------------------
    const std::string& session_id() const override { return session_id_; }
    // The fixture's config dir doubles as the project root: no project-local
    // scope beyond what a test writes into it deliberately.
    std::filesystem::path project_root() const override { return dir_.path(); }
    agent::AgentLoop& loop() override { return *loop_; }
    tools::ToolExecutor& executor() override { return *executor_; }
    const agent::AgentPreset& preset() const override { return preset_; }
    agent::AgentPreset preset_snapshot() const override { return preset_; }
    void ApplyPreset(agent::AgentPreset preset) override {
        preset_ = std::move(preset);
        loop_->SetPreset(preset_);
    }
    const std::optional<common::ModelRef>& model_override() const override { return override_; }
    void set_model_override(std::optional<common::ModelRef> ref) override {
        override_ = std::move(ref);
    }
    // Records rather than runs: a prompt-shorthand test asserts the text and
    // shape of the submission, and running a real turn here would need a
    // scripted provider response the command itself knows nothing about.
    bool SubmitTurn(std::string text) override {
        submitted_turns_.push_back(std::move(text));
        return true;
    }
    const std::vector<std::string>& submitted_turns() const { return submitted_turns_; }

    // Records the per-turn overrides alongside the text: a command's
    // frontmatter `model:`/`agent:` must reach the submission, and the
    // session's sticky state is untouched by construction here.
    bool SubmitTurnWithOverrides(std::string text, const std::optional<common::ModelRef>& model,
                                 const std::string& agent_preset) override {
        submitted_models_.push_back(model ? model->str() : "");
        submitted_agents_.push_back(agent_preset);
        return SubmitTurn(std::move(text));
    }
    const std::vector<std::string>& submitted_override_models() const {
        return submitted_models_;
    }
    const std::vector<std::string>& submitted_override_agents() const {
        return submitted_agents_;
    }

    // Replaces providers.json wholesale -- for tests that exercise discover
    // entries beside the static fixture. Call ReloadPresets() afterwards.
    void WriteProviders(const std::string& contents) {
        Write("providers.json", contents);
    }

    // Rebuilds the preset registry from the config dir and re-resolves the
    // session's preset -- for tests that edit providers.json after
    // construction (the refresh tests edit it to add a discover-entry).
    void ReloadPresets() {
        presets_ = std::make_unique<agent::AgentPresetRegistry>(
            dir_.path(), *skills_, config::LoadSettingsConfig(dir_.path() / "settings.json"));
        preset_ = presets_->Resolve("build");
    }

    // Discovery injection, exactly as main.cpp does after plugin load.
    void SetDiscoveryHook(config::DiscoveryHook hook) {
        presets_->SetDiscoveryHook(std::move(hook));
    }

    commands::CommandRegistry registry;
    FakeConfirmationBroker confirmations;

    // Accessors for tests that build their OWN CommandContext (the sessionless
    // dispatch tests pass a null session ref into the same registries).
    const agent::AgentPresetRegistry& registry_presets_for_test() const { return *presets_; }
    commands::ConfirmationBroker& registry_confirmations_for_test() { return confirmations; }

private:
    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir_.path() / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    TempDir dir_;
    std::string session_id_ = "s1";
    // Declared before loop_, so it OUTLIVES it: the loop holds a reference.
    std::unique_ptr<tools::ToolExecutor> executor_;
    FakeLlmProvider provider_;
    tools::ToolRegistry tools_;
    AlwaysAllowPermissionChecker checker_;
    InMemoryHistoryStore store_;
    std::unique_ptr<config::SkillRegistry> skills_;
    std::unique_ptr<agent::AgentPresetRegistry> presets_;
    agent::AgentPreset preset_;
    std::unique_ptr<agent::AgentLoop> loop_;
    std::optional<common::ModelRef> override_;
    std::vector<std::string> submitted_turns_;
    std::vector<std::string> submitted_models_;
    std::vector<std::string> submitted_agents_;
};

}  // namespace minimal::testing

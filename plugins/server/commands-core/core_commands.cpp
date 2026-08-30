#include "core_commands.hpp"

#include <sstream>
#include <string>

#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::commands_core {
namespace {

using minimal::commands::Command;
using minimal::commands::CommandArgs;
using minimal::commands::CommandContext;
using minimal::commands::CommandResult;
using minimal::commands::HarnessCommand;
using minimal::commands::LLMCommand;

// The registry is reached through the context rather than captured at
// construction, so /help lists what is registered NOW -- including commands from
// plugins that loaded after this one.
class HelpCommand : public HarnessCommand {
public:
    explicit HelpCommand(const minimal::commands::CommandRegistry& registry)
        : registry_(registry) {}

    std::string_view Name() const override { return "help"; }
    std::string_view Summary() const override { return "List commands, or explain one"; }
    std::string_view Usage() const override { return "/help [command]"; }
    bool ReadOnly() const override { return true; }

    CommandResult Execute(CommandContext&, const CommandArgs& args) override {
        if (!args.positional.empty()) {
            const std::string& wanted = args.positional.front();
            const Command* command = registry_.Find(wanted);
            if (command == nullptr) {
                return CommandResult::Error("unknown command: /" + wanted);
            }
            std::ostringstream out;
            out << command->Usage() << "\n  " << command->Summary();
            if (command->RequiresConfirmation()) out << "\n  (asks for confirmation first)";
            return CommandResult::Ok(out.str());
        }

        std::ostringstream out;
        for (const Command* command : registry_.All()) {
            out << "/" << command->Name() << "  -  " << command->Summary() << "\n";
        }
        return CommandResult::Ok(out.str());
    }

private:
    const minimal::commands::CommandRegistry& registry_;
};

// Reads providers.json's resolved contents, marking the one in use. No LLM call,
// no session state touched -- so it answers while a turn is running.
class ProvidersCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "providers"; }
    std::string_view Summary() const override { return "List configured models"; }
    std::string_view Usage() const override { return "/providers"; }
    bool ReadOnly() const override { return true; }

    CommandResult Execute(CommandContext& context, const CommandArgs&) override {
        const auto refs = context.providers().ModelRefs();
        if (refs.empty()) {
            return CommandResult::Error(
                "no models configured; edit providers.json (see providers.example.json)");
        }
        // Compared as a REFERENCE, not as a connection plus a model id: two
        // providers can serve the same model at the same url with different
        // limits, and matching on those would mark both.
        const std::string active = context.session().preset().model_ref.str();

        std::ostringstream out;
        for (const auto& ref : refs) {
            const auto parsed = minimal::common::ParseModelRef(ref);
            const auto* model = parsed ? context.providers().Resolve(*parsed) : nullptr;
            out << (ref == active ? "* " : "  ") << ref;
            if (model != nullptr && model->context_window_tokens) {
                out << "  (" << *model->context_window_tokens << " ctx)";
            }
            out << "\n";
        }
        return CommandResult::Ok(out.str());
    }
};

class CompactCommand : public LLMCommand {
public:
    std::string_view Name() const override { return "compact"; }
    std::string_view Summary() const override {
        return "Summarize the older half of the conversation";
    }
    std::string_view Usage() const override { return "/compact"; }

    CommandResult Execute(CommandContext& context, const CommandArgs&) override {
        const auto folded = context.session().loop().CompactNow();
        if (!folded) {
            // The reason is already in the log at the point it was decided --
            // repeating a guess here would be a second, possibly wrong, story.
            return CommandResult::Error(
                "nothing to compact (too little history, no compaction prompt, or the summarizer "
                "already failed this turn -- see the log)");
        }
        return CommandResult::Ok("compacted the oldest " + std::to_string(*folded) +
                                 " messages into one summary");
    }
};

class ClearCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "clear"; }
    std::string_view Summary() const override {
        return "Forget the conversation so far (the transcript is kept)";
    }
    std::string_view Usage() const override { return "/clear"; }
    // Destroys context the user cannot get back into the model, so it asks --
    // even though the durable record survives.
    bool RequiresConfirmation() const override { return true; }

    CommandResult Execute(CommandContext& context, const CommandArgs&) override {
        const auto dropped = context.session().loop().history().size();
        context.session().loop().ClearHistory();
        // The store is untouched, the same rule compaction follows: what the
        // model sees and what scrollback shows are different things.
        return CommandResult::Ok("cleared " + std::to_string(dropped) +
                                 " messages from context; the transcript is unchanged");
    }
};

class AgentCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "agent"; }
    std::string_view Summary() const override { return "Switch agent preset"; }
    std::string_view Usage() const override { return "/agent <name>"; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            // The searchable picker belongs to the TUI, which can draw one. Until
            // then the shorthand is the whole interface, and saying so is better
            // than a list nobody asked for.
            return CommandResult::Error("usage: /agent <name>");
        }
        const std::string& wanted = args.positional.front();

        minimal::agent::AgentPreset resolved;
        try {
            resolved = context.presets().Resolve(wanted);
        } catch (const std::exception& error) {
            return CommandResult::Error(error.what());
        }

        // THE PRECEDENCE RULE. A preset supplies a default model; a model the
        // user chose is a decision and survives the switch. Without this,
        // /agent would silently undo /model.
        if (const auto& override_ref = context.session().model_override()) {
            const auto* model = context.providers().Resolve(*override_ref);
            if (model != nullptr) minimal::agent::ApplyModel(resolved, *model);
        }

        context.session().ApplyPreset(std::move(resolved));
        // The full reference, not the bare model id: two references can name
        // the same model at the same url with different limits, so the id alone
        // does not say which one is now in force.
        return CommandResult::Ok("agent: " + wanted + " (model " +
                                 context.session().preset().model_ref.str() + ")");
    }
};

class ModelCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "model"; }
    std::string_view Summary() const override { return "Switch model, overriding the preset's"; }
    std::string_view Usage() const override { return "/model <provider>/<model>"; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Error("usage: /model <provider>/<model> (see /providers)");
        }
        const std::string& text = args.positional.front();
        const auto ref = minimal::common::ParseModelRef(text);
        if (!ref) {
            return CommandResult::Error("'" + text +
                                        "' must be <provider>/<model>, e.g. local/qwen4");
        }
        const auto* model = context.providers().Resolve(*ref);
        if (model == nullptr) {
            return CommandResult::Error("unknown model '" + text + "' (see /providers)");
        }

        // Recorded on the SESSION, not folded into the preset, so a later /agent
        // re-applies it rather than resolving it away.
        context.session().set_model_override(*ref);

        auto preset = context.session().preset();
        minimal::agent::ApplyModel(preset, *model);
        // ApplyPreset rebuilds the provider if this crossed a connection.
        context.session().ApplyPreset(std::move(preset));
        return CommandResult::Ok("model: " + text);
    }
};

class PermissionModeCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "permission-mode"; }
    std::string_view Summary() const override { return "Change how tool permissions are decided"; }
    std::string_view Usage() const override {
        return "/permission-mode <normal|bypass|allow-only>";
    }
    // Widening permissions mid-session is exactly the thing worth asking about.
    bool RequiresConfirmation() const override { return true; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Error("usage: /permission-mode <normal|bypass|allow-only>");
        }
        const std::string& wanted = args.positional.front();
        minimal::tools::PermissionRunMode mode{};
        if (wanted == "normal") {
            mode = minimal::tools::PermissionRunMode::Normal;
        } else if (wanted == "bypass") {
            mode = minimal::tools::PermissionRunMode::Bypass;
        } else if (wanted == "allow-only") {
            mode = minimal::tools::PermissionRunMode::AllowOnly;
        } else {
            return CommandResult::Error("unknown mode '" + wanted +
                                        "' (want normal, bypass or allow-only)");
        }
        context.session().executor().SetRunMode(mode);
        return CommandResult::Ok("permission mode: " + wanted);
    }
};

}  // namespace

std::vector<std::unique_ptr<Command>> MakeCoreCommands() {
    // /help needs the registry to list it, but it is being built to go INTO that
    // registry -- so the registry reference is bound at register time by the
    // plugin entry point, not here. Everything else is self-contained.
    std::vector<std::unique_ptr<Command>> out;
    out.push_back(std::make_unique<ProvidersCommand>());
    out.push_back(std::make_unique<CompactCommand>());
    out.push_back(std::make_unique<ClearCommand>());
    out.push_back(std::make_unique<AgentCommand>());
    out.push_back(std::make_unique<ModelCommand>());
    out.push_back(std::make_unique<PermissionModeCommand>());
    return out;
}

std::unique_ptr<Command> MakeHelpCommand(const minimal::commands::CommandRegistry& registry) {
    return std::make_unique<HelpCommand>(registry);
}

}  // namespace minimal::commands_core

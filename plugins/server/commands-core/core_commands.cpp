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

// Bare `/agent` opens the picker (display=View, view_hint="agents"), same
// shape as SessionsCommand below: the command itself carries no data, the
// client fetches the preset list itself over ListAgentPresets. With a name it
// switches, exactly as before. ReadOnly() stays false (the default) even
// though the no-arg branch touches nothing: ReadOnly() describes the command
// as a whole, and the by-name branch still mutates session state via
// ApplyPreset -- a command that can mutate must queue at a turn boundary
// every time, not just on the branches that happen to need it, or a listing
// call could run concurrently with a same-named switch mid-turn.
class AgentCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "agent"; }
    std::string_view Summary() const override { return "Switch agent preset, or open the picker"; }
    std::string_view Usage() const override { return "/agent [name]"; }
    minimal::commands::CommandDisplay Display() const override {
        return minimal::commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "agents"; }

    // Only the bare "/agent" (list) opens the picker; "/agent <name>" is a
    // direct switch and has nothing to show -- without this override the
    // dispatcher's static Display()/ViewHint() above (correct for the
    // catalog, where /agent AS A WHOLE is view-capable) applied to EVERY
    // invocation, so a successful switch re-opened the picker it was just
    // answered from.
    minimal::commands::CommandDisplay DisplayFor(const CommandArgs& args) const override {
        return args.positional.empty() ? minimal::commands::CommandDisplay::kView
                                        : minimal::commands::CommandDisplay::kStatus;
    }
    std::string_view ViewHintFor(const CommandArgs& args) const override {
        return args.positional.empty() ? std::string_view("agents") : std::string_view{};
    }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Ok("agent list requested");
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
            const auto model = context.providers().Resolve(*override_ref);
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

// Bare /model opens the client's model picker (display=View, view_hint
// "models"), the same shape as AgentCommand's bare form: the command itself
// carries no data, the client fetches the model list itself over ListModels.
// "/model <provider>/<model>" is a direct switch, unchanged. ReadOnly() stays
// false (the default) even though the no-arg branch touches nothing, for the
// same reason AgentCommand's does: the by-ref branch mutates session state and
// a command that can mutate must queue at a turn boundary every time.
class ModelCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "model"; }
    std::string_view Summary() const override {
        return "Switch model, or open the model picker";
    }
    std::string_view Usage() const override { return "/model [provider/model]"; }

    minimal::commands::CommandDisplay Display() const override {
        return minimal::commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "models"; }

    // Only the bare "/model" opens the picker; "/model <ref>" is a direct
    // switch with nothing to show -- without this the dispatcher's static
    // Display() applied to EVERY invocation and a successful switch re-opened
    // the picker it was just answered from (the exact bug AgentCommand's
    // DisplayFor override exists for).
    minimal::commands::CommandDisplay DisplayFor(const CommandArgs& args) const override {
        return args.positional.empty() ? minimal::commands::CommandDisplay::kView
                                       : minimal::commands::CommandDisplay::kStatus;
    }
    std::string_view ViewHintFor(const CommandArgs& args) const override {
        return args.positional.empty() ? std::string_view("models") : std::string_view{};
    }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Ok("model list requested");
        }
        const std::string& text = args.positional.front();
        // SplitModelRef, not ParseModelRef: a discovered model's id may
        // itself carry slashes ("openrouter/anthropic/claude-opus-5").
        const auto ref = minimal::common::SplitModelRef(text);
        if (!ref) {
            return CommandResult::Error("'" + text +
                                        "' must be <provider>/<model>, e.g. local/qwen4 "
                                        "(or run /model with no arguments)");
        }
        const auto model = context.providers().Resolve(*ref);
        if (model == nullptr) {
            return CommandResult::Error("unknown model '" + text +
                                        "' (run /model with no arguments for the list)");
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

// The result is the interface: display=View with view_hint="sessions" tells the
// client to open its session picker, which fetches its own list over
// ListSessions -- the command itself carries no data to show. ReadOnly because
// it touches nothing, so it answers beside a live turn. The message exists for
// a client with no picker (headless): display only governs WHERE a result
// surfaces, never whether it is answered.
class SessionsCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "sessions"; }
    std::string_view Summary() const override { return "Open the session picker"; }
    std::string_view Usage() const override { return "/sessions"; }
    bool ReadOnly() const override { return true; }
    minimal::commands::CommandDisplay Display() const override {
        return minimal::commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "sessions"; }

    CommandResult Execute(CommandContext&, const CommandArgs&) override {
        return CommandResult::Ok("session list requested");
    }
};

}  // namespace

std::vector<std::unique_ptr<Command>> MakeCoreCommands() {
    // /help needs the registry to list it, but it is being built to go INTO that
    // registry -- so the registry reference is bound at register time by the
    // plugin entry point, not here. Everything else is self-contained.
    std::vector<std::unique_ptr<Command>> out;
    out.push_back(std::make_unique<CompactCommand>());
    out.push_back(std::make_unique<ClearCommand>());
    out.push_back(std::make_unique<AgentCommand>());
    out.push_back(std::make_unique<ModelCommand>());
    out.push_back(std::make_unique<PermissionModeCommand>());
    out.push_back(std::make_unique<SessionsCommand>());
    return out;
}

std::unique_ptr<Command> MakeHelpCommand(const minimal::commands::CommandRegistry& registry) {
    return std::make_unique<HelpCommand>(registry);
}

}  // namespace minimal::commands_core

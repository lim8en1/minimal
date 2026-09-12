#include "core_commands.hpp"

#include <sstream>
#include <string>
#include <system_error>

#include <filesystem>
#include <fstream>

#include "minimal/commands/command_context.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/file_lock.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/tools/atomic_write.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::commands_core {
namespace {

using minimal::commands::Command;
using minimal::commands::CommandArgs;
using minimal::commands::CommandContext;
using minimal::commands::CommandResult;
using minimal::commands::HarnessCommand;
using minimal::commands::LLMCommand;
using minimal::commands::Sessionless;

// The registry is reached through the context rather than captured at
// construction, so /help lists what is registered NOW -- including commands from
// plugins that loaded after this one.
//
// Sessionless<HarnessCommand>: /help reads only the registry, so it executes
// with a null session ref -- that is what lets it answer on the TUI's
// sessionless tabs (a tab with no session yet) instead of forcing one into
// existence just to list commands.
class HelpCommand : public Sessionless<HarnessCommand> {
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
        for (const auto& entry : registry_.All()) {
            out << "/" << entry.surface << "  -  " << entry.command->Summary() << "\n";
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
            // The session's root anchors project-local discovery: /agent into a
            // project preset works exactly like the session's own creation did.
            // The session's existing model_override rides straight into Resolve()
            // (not just into the precedence re-apply below) so switching to a
            // preset with a bad `model:` -- or a bad settings.default_model --
            // cannot fail the switch when the session already has a working
            // override in force; see AgentPresetRegistry::Resolve's model_override
            // doc.
            resolved = context.presets().Resolve(wanted, context.session().project_root(),
                                                 std::nullopt, context.session().model_override());
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

// The subagent-model sibling of ModelCommand: sets what model THIS session's
// spawned subagents run on. Bare /subagent-model opens the client's picker
// (display=View, view_hint "subagent-models"), which prepends a "Same as
// parent" row -- the default, expressed by the "inherit" sentinel here: the
// session's override is cleared and spawned children go back to running the
// parent's own model (unless something more specific outranks it: the spawn
// call's own model argument, or the child preset's declared model).
// "/subagent-model <provider>/<model>" pins the choice for this session's
// children; "/subagent-model inherit" clears it. The same two-form display
// rule as ModelCommand: only the bare form opens the picker.
class SubagentModelCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "subagent-model"; }
    std::string_view Summary() const override {
        return "Set the model for spawned subagents, or open its picker";
    }
    std::string_view Usage() const override { return "/subagent-model [provider/model|inherit]"; }

    minimal::commands::CommandDisplay Display() const override {
        return minimal::commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "subagent-models"; }

    minimal::commands::CommandDisplay DisplayFor(const CommandArgs& args) const override {
        return args.positional.empty() ? minimal::commands::CommandDisplay::kView
                                       : minimal::commands::CommandDisplay::kStatus;
    }
    std::string_view ViewHintFor(const CommandArgs& args) const override {
        return args.positional.empty() ? std::string_view("subagent-models")
                                       : std::string_view{};
    }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Ok("subagent model list requested");
        }
        const std::string& text = args.positional.front();
        if (text == "inherit") {
            // Back to the default: children inherit the parent's model.
            context.session().set_subagent_model_override(std::nullopt);
            return CommandResult::Ok("subagent model: inherit (same as parent)");
        }
        // SplitModelRef, not ParseModelRef: a discovered model's id may itself
        // carry slashes ("openrouter/anthropic/claude-opus-5").
        const auto ref = minimal::common::SplitModelRef(text);
        if (!ref) {
            return CommandResult::Error("'" + text +
                                        "' must be <provider>/<model> or 'inherit' "
                                        "(or run /subagent-model with no arguments)");
        }
        const auto model = context.providers().Resolve(*ref);
        if (model == nullptr) {
            return CommandResult::Error("unknown model '" + text +
                                        "' (run /subagent-model with no arguments for the "
                                        "list)");
        }
        // Recorded on the SESSION as this session's choice for ITS children;
        // consumed at spawn time, never folded into any preset.
        context.session().set_subagent_model_override(*ref);
        return CommandResult::Ok("subagent model: " + text);
    }
};

// The result is the interface: display=View with view_hint="sessions" tells the
// client to open its session picker, which fetches its own list over
// ListSessions -- the command itself carries no data to show. ReadOnly because
// it touches nothing, so it answers beside a live turn. The message exists for
// a client with no picker (headless): display only governs WHERE a result
// surfaces, never whether it is answered.
//
// Sessionless<HarnessCommand>: the point of /sessions is exactly that it needs
// no session -- the demonstrated "0-message junk session" bug was the TUI
// creating one just to run this command on the empty welcome app. With the
// Sessionless<> mixin it dispatches sessionless instead.
class SessionsCommand : public Sessionless<HarnessCommand> {
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

// Bare /mcp opens the client's MCP picker (display=View, view_hint "mcp"),
// the same shape as /agent and /model: the command itself carries no data,
// the client fetches the server list over ListMcpServers. "/mcp <name>
// enable|disable" toggles LIVE via McpClientManager::SetEnabled -- no server
// restart, unlike editing mcps.json directly.
//
// Sessionless AND ReadOnly, like /connect: an MCP server's enabled state is
// process-global (McpClientManager), not session state, so it never touches
// the conversation path -- the exact meaning ReadOnly() carries here, not
// "mutates nothing".
class McpCommand : public Sessionless<HarnessCommand> {
public:
    std::string_view Name() const override { return "mcp"; }
    std::string_view Summary() const override {
        return "View MCP servers, or enable/disable one live";
    }
    std::string_view Usage() const override { return "/mcp [name enable|disable]"; }
    bool ReadOnly() const override { return true; }
    minimal::commands::CommandDisplay Display() const override {
        return minimal::commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "mcp"; }

    minimal::commands::CommandDisplay DisplayFor(const CommandArgs& args) const override {
        return args.positional.empty() ? minimal::commands::CommandDisplay::kView
                                        : minimal::commands::CommandDisplay::kStatus;
    }
    std::string_view ViewHintFor(const CommandArgs& args) const override {
        return args.positional.empty() ? std::string_view("mcp") : std::string_view{};
    }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Ok("mcp list requested");
        }
        if (args.positional.size() != 2 ||
            (args.positional[1] != "enable" && args.positional[1] != "disable")) {
            return CommandResult::Error("usage: /mcp <name> enable|disable");
        }
        if (context.mcp_manager() == nullptr) {
            return CommandResult::Error("MCP is not available (no mcps.json configured)");
        }
        const std::string& name = args.positional[0];
        const bool enabled = args.positional[1] == "enable";
        std::string error;
        if (!context.mcp_manager()->SetEnabled(name, enabled, error)) {
            return CommandResult::Error(error);
        }
        return CommandResult::Ok("mcp: " + name + " " + (enabled ? "enabled" : "disabled"));
    }
};

}  // namespace

// /connect <provider> <api-key> -- stores a credential in the data dir's
// auth.json ({"<provider>": {"key": "..."}}), the same shape (and, for the
// opencode-compat path, the same file) ResolveApiKey's credentials-file link
// reads. Core never writes config; a command plugin writing a CREDENTIALS file
// outside the seeded config dir is the sanctioned /connect shape.
//
// Sessionless AND ReadOnly: storing a credential is exactly the thing a user
// does BEFORE any session exists (its whole purpose is bootstrapping a
// provider connection), and ReadOnly here means what the dispatcher means by
// it -- never touches the conversation path -- not "writes no files": the
// auth.json merge runs inline on the RPC thread under its own cross-process
// lock, the same shape any read-only catalog fetch on the RPC thread uses.
// Without the ReadOnly answer, DispatchSessionless refused the command with
// the session-required wording even though RequiresSession() was false --
// the command advertised itself in autocomplete and then could not run
// anywhere but a sessioned tab, backwards for a pre-session command.
class ConnectCommand : public Sessionless<HarnessCommand> {
public:
    std::string_view Name() const override { return "connect"; }
    std::string_view Summary() const override {
        return "Store a provider API key (used when providers.json names none)";
    }
    std::string_view Usage() const override { return "/connect <provider> <api-key>"; }
    bool ReadOnly() const override { return true; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        (void)context;
        if (args.positional.size() != 2) {
            return CommandResult::Error("usage: /connect <provider> <api-key>");
        }
        const std::string& provider = args.positional[0];
        const std::string& key = args.positional[1];
        if (provider.empty() || key.empty() ||
            provider.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123"
                                       "456789-.") != std::string::npos) {
            return CommandResult::Error("invalid provider name or key");
        }

        const auto path = common::DataDir() / "auth.json";
        // The whole read-modify-write runs under a cross-process exclusive
        // lock: two sessions running /connect concurrently would otherwise
        // race (both read, both merge, last writer wins) and one provider's
        // entry would silently vanish. ExclusiveFileLock throws when the lock
        // is held -- the command surface has no exception handler, so the
        // refusal is converted to a command error below.
        try {
            const common::ExclusiveFileLock lock(path);
            return SaveCredential(path, provider, key);
        } catch (const std::exception& e) {
            return CommandResult::Error("could not lock " + path.string() +
                                        " for update: " + e.what());
        }
    }

private:
    static CommandResult SaveCredential(const std::filesystem::path& path,
                                        const std::string& provider, const std::string& key) {
        nlohmann::json doc = nlohmann::json::object();
        {   // merge into whatever is already stored
            std::ifstream in(path);
            if (in) {
                try {
                    doc = nlohmann::json::parse(in);
                    if (!doc.is_object()) doc = nlohmann::json::object();
                } catch (const std::exception&) {
                    return CommandResult::Error(path.string() +
                                                " is malformed; fix or remove it first");
                }
            }
        }
        // A whole-object replace would silently discard whatever else lives
        // under this provider's entry -- opencode's own auth.json uses an
        // OAuth shape here ({"type":"oauth","refresh":...,"access":...}),
        // which LookupKeyInAuthJson already tolerates by probing {"key",
        // "api_key"}. Setting just the one field preserves the rest of
        // whatever shape was already there.
        if (!doc[provider].is_object()) doc[provider] = nlohmann::json::object();
        doc[provider]["key"] = key;
        std::filesystem::create_directories(path.parent_path());
        const tools::WriteOutcome written = tools::write_file(
            path, doc.dump() + "\n",
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        if (!written.ok) {
            return CommandResult::Error("could not write " + path.string() + ": " +
                                        written.error);
        }
        // 0600, enforced EXPLICITLY and only after the write: this file holds
        // a live API key, but write_file preserves an existing file's mode
        // (the atomic rename swaps inodes wholesale, so create_perms only
        // applies to a fresh file) -- a pre-existing auth.json created with
        // wider permissions would stay wide no matter what was passed above.
        // Failing closed: reporting an error is better than silently leaving
        // a credential group/other-readable.
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) {
            return CommandResult::Error("stored the credential but could not restrict " +
                                        path.string() + " to owner-only: " + ec.message());
        }
        return CommandResult::Ok("stored credential for '" + provider + "' in " + path.string() +
                                 " -- restart the server for it to take effect");
    }
};

std::vector<std::unique_ptr<Command>> MakeCoreCommands() {
    // /help needs the registry to list it, but it is being built to go INTO that
    // registry -- so the registry reference is bound at register time by the
    // plugin entry point, not here. Everything else is self-contained.
    std::vector<std::unique_ptr<Command>> out;
    out.push_back(std::make_unique<CompactCommand>());
    out.push_back(std::make_unique<ClearCommand>());
    out.push_back(std::make_unique<AgentCommand>());
    out.push_back(std::make_unique<ModelCommand>());
    out.push_back(std::make_unique<SubagentModelCommand>());
    out.push_back(std::make_unique<SessionsCommand>());
    out.push_back(std::make_unique<ConnectCommand>());
    out.push_back(std::make_unique<McpCommand>());
    return out;
}

std::unique_ptr<Command> MakeHelpCommand(const minimal::commands::CommandRegistry& registry) {
    return std::make_unique<HelpCommand>(registry);
}

}  // namespace minimal::commands_core

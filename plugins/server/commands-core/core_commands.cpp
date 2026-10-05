#include "core_commands.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "minimal/commands/command_context.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/file_lock.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/config/permission_codec.hpp"
#include "minimal/config/project_trust.hpp"
#include "minimal/config/providers_config.hpp"
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

    // Over the EFFECTIVE catalog: this session's project commands included,
    // with the same shadowing rule dispatch applies -- so /help never lists a
    // command that would not run, nor misses one that would. Sessionless (no
    // project scope yet), project_commands() is null and this is the global
    // registry alone.
    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        const minimal::commands::CommandRegistry* project = context.project_commands();
        if (!args.positional.empty()) {
            const std::string& wanted = args.positional.front();
            const Command* command = minimal::commands::FindEffective(registry_, project, wanted);
            if (command == nullptr) {
                return CommandResult::Error("unknown command: /" + wanted);
            }
            std::ostringstream out;
            out << command->Usage() << "\n  " << command->Summary();
            if (command->RequiresConfirmation()) out << "\n  (asks for confirmation first)";
            return CommandResult::Ok(out.str());
        }

        std::ostringstream out;
        for (const auto& entry : minimal::commands::EffectiveCatalog(registry_, project)) {
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
    std::string_view Usage() const override { return "/compact [prompt]"; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.size() > 1) {
            return CommandResult::Error("usage: /compact [prompt]");
        }
        // A named prompt is resolved by the registry -- same bare-filename rule
        // and same prompts/ directory a preset frontmatter `compression_prompt:`
        // value uses -- so a miss or a traversal is a clean command error, not a
        // half-configured compaction. The override rides THIS invocation only.
        std::string prompt;
        if (!args.positional.empty()) {
            try {
                prompt = context.presets().ResolveCompressionPrompt(args.positional.front());
            } catch (const std::exception& error) {
                return CommandResult::Error(error.what());
            }
            if (prompt.empty()) {
                // An empty body would short-circuit compact_messages into
                // disabling compaction; saying so here is the honest answer.
                return CommandResult::Error(
                    "compression prompt '" + args.positional.front() + "' is empty");
            }
        }
        const auto folded = context.session().CompactNow(prompt);
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
        const auto dropped = context.session().ClearHistory();
        // The store is untouched, the same rule compaction follows: what the
        // model sees and what scrollback shows are different things.
        return CommandResult::Ok("cleared " + std::to_string(dropped) +
                                 " messages from context; the transcript is unchanged");
    }
};

// The "bare form opens a picker, argument form acts" shape shared by /agent,
// /model and /subagent-model (the mcp plugin's /mcp follows it too). The
// catalog sees the command AS A WHOLE as view-capable (Display()/
// ViewHint()); each INVOCATION opens the picker
// only when it has no arguments -- the argument form is a direct action with
// nothing to show, and before DisplayFor existed a successful switch
// re-opened the picker it was just answered from. `PickerView()` is the one
// place the client view id is written.
template <typename Base>
class PickerCommand : public Base {
public:
    virtual std::string_view PickerView() const = 0;

    minimal::commands::CommandDisplay Display() const final {
        return minimal::commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const final { return PickerView(); }
    minimal::commands::CommandDisplay DisplayFor(const CommandArgs& args) const final {
        return args.positional.empty() ? minimal::commands::CommandDisplay::kView
                                       : minimal::commands::CommandDisplay::kStatus;
    }
    std::string_view ViewHintFor(const CommandArgs& args) const final {
        return args.positional.empty() ? PickerView() : std::string_view{};
    }
    // The bare form only asks the client to open its picker -- it touches no
    // session state, so it answers beside a live turn instead of queueing
    // behind it. The argument form keeps the command's own ReadOnly().
    bool ReadOnlyFor(const CommandArgs& args) const final {
        return args.positional.empty() || this->ReadOnly();
    }
};

// Switches THIS session to `ref`: the one sequence /model and /think share.
// The ref is recorded on the SESSION (set_model_override), not folded into
// the preset, so a later /agent re-applies it rather than resolving it away;
// ApplyPreset rebuilds the provider when the switch crossed a connection.
// `shown` is how the user named the model, for the miss message.
CommandResult SwitchModel(CommandContext& context, const minimal::common::ModelRef& ref,
                          const std::string& shown, std::string ok_message) {
    const auto model = context.providers().Resolve(ref);
    if (model == nullptr) {
        return CommandResult::Error("unknown model '" + shown +
                                    "' (run /model with no arguments for the list)");
    }
    context.session().set_model_override(ref);
    auto preset = context.session().preset();
    minimal::agent::ApplyModel(preset, *model);
    context.session().ApplyPreset(std::move(preset));
    return CommandResult::Ok(std::move(ok_message));
}

// Bare `/agent` opens the picker (display=View, view_hint="agents"), same
// shape as SessionsCommand below: the command itself carries no data, the
// client fetches the preset list itself over ListAgentPresets. With a name it
// switches, exactly as before. ReadOnly() stays false (the default): it
// describes the command as a whole, and the by-name branch mutates session
// state via ApplyPreset. The bare form answers beside a live turn anyway, via
// PickerCommand's ReadOnlyFor -- it touches nothing.
class AgentCommand : public PickerCommand<HarnessCommand> {
public:
    std::string_view Name() const override { return "agent"; }
    std::string_view Summary() const override { return "Switch agent preset, or open the picker"; }
    std::string_view Usage() const override { return "/agent [name]"; }
    std::string_view PickerView() const override { return "agents"; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Ok("agent list requested");
        }
        if (args.positional.size() > 1) return CommandResult::Error("usage: /agent [name]");
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
                                                 nullptr, context.session().model_override());
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
// false for the same reason AgentCommand's does; the bare form still answers
// beside a live turn (PickerCommand::ReadOnlyFor).
class ModelCommand : public PickerCommand<HarnessCommand> {
public:
    std::string_view Name() const override { return "model"; }
    std::string_view Summary() const override {
        return "Switch model, or open the model picker";
    }
    std::string_view Usage() const override { return "/model [provider/model]"; }
    std::string_view PickerView() const override { return "models"; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Ok("model list requested");
        }
        if (args.positional.size() > 1) {
            return CommandResult::Error("usage: /model [provider/model]");
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
        return SwitchModel(context, *ref, text, "model: " + text);
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
class SubagentModelCommand : public PickerCommand<HarnessCommand> {
public:
    std::string_view Name() const override { return "subagent-model"; }
    std::string_view Summary() const override {
        return "Set the model for spawned subagents, or open its picker";
    }
    std::string_view Usage() const override { return "/subagent-model [provider/model|inherit]"; }
    std::string_view PickerView() const override { return "subagent-models"; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.empty()) {
            return CommandResult::Ok("subagent model list requested");
        }
        if (args.positional.size() > 1) {
            return CommandResult::Error("usage: /subagent-model [provider/model|inherit]");
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

// The effort-variant switcher (docs/CONFIGURATION.md's "reasoning-effort
// variants"): `<model>@<name>` siblings are declared in providers.json or
// synthesized by discovery, and /think flips the CURRENT session between them
// without re-picking a model. Reads the session's current model ref, strips any
// existing @suffix at the LAST '@' (SplitVariantSuffix -- the same rule Resolve
// applies), and rebuilds the ref against the base. "/think off" drops the
// suffix entirely (back to the base model); "/think <name>" resolves
// <provider>/<base>@<name>, which is a MISS for an undeclared name -- reported
// with the declared list, the DescribeModels habit, never a guess.
//
// Persistence: when a /model choice is in force the variant replaces it, the
// ModelCommand path (set_model_override + ApplyModel + ApplyPreset), and
// survives /agent; otherwise only the running preset's model changes, and a
// later /agent resolves its own model as usual (see SwitchTo). There is
// deliberately no picker form: the bare /think answer is the list, and a
// session's variants are a short hand-written set -- a client picker would be
// surface ahead of need.
class ThinkCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "think"; }
    std::string_view Summary() const override {
        return "Switch reasoning-effort variant of the current model";
    }
    std::string_view Usage() const override { return "/think [variant|off]"; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        if (args.positional.size() > 1) return CommandResult::Error("usage: /think [variant|off]");
        const config::ModelRef current = context.session().preset_snapshot().model_ref;
        if (current.provider.empty() || current.model.empty()) {
            return CommandResult::Error("no model is active for this session yet");
        }

        const auto [base_id, current_variant] =
            config::SplitVariantSuffix(current.model);
        const std::vector<std::string> declared =
            context.providers().VariantsOf(config::ModelRef{current.provider, base_id});

        if (args.positional.empty()) {
            if (declared.empty()) {
                return CommandResult::Ok(
                    "model '" + current.str() +
                    "' declares no reasoning variants; declare them with a \"variants\" "
                    "block in providers.json (or discovery may supply them)");
            }
            std::string list;
            for (const auto& name : declared) {
                list += std::string(name == current_variant ? " *" : "  ") + name;
                list += name == current_variant ? " (current)" : "";
                list += "\n";
            }
            return CommandResult::Ok("variants of " + current.provider + "/" + base_id +
                                     ":\n" + list + "/think <variant> to switch, /think off "
                                     "for the base model");
        }

        const std::string& wanted = args.positional.front();
        // "off" is the hardcoded reset UNLESS the model's own providers.json
        // "variants" list declares a variant actually NAMED "off" -- an
        // unusual but valid free-form name, and the declared list wins over
        // the built-in meaning so it stays reachable by name.
        const bool declares_off =
            std::find(declared.begin(), declared.end(), "off") != declared.end();
        if (wanted == "off" && !declares_off) {
            if (current_variant.empty()) {
                return CommandResult::Ok("reasoning variant: none (base model already active)");
            }
            return SwitchTo(context, config::ModelRef{current.provider, base_id},
                            "reasoning variant: off (base model " + base_id + ")");
        }

        if (!declared.empty() &&
            std::find(declared.begin(), declared.end(), wanted) == declared.end()) {
            std::string list;
            for (const auto& name : declared) list += " " + name;
            return CommandResult::Error("unknown variant '" + wanted + "' for " +
                                        current.provider + "/" + base_id +
                                        " (declared:" + list + ")");
        }
        // An undeclared name on a model with no declared variants at all still
        // goes through Resolve: the error comes back from the config with the
        // model-listing suggestion, which is honest for a discover-entry whose
        // variants live behind a lazy fetch.
        return SwitchTo(context, config::ModelRef{current.provider, base_id + "@" + wanted},
                        "reasoning variant: " + wanted);
    }

private:
    // Pins only what was already pinned. With a /model choice in force the
    // variant replaces it (it IS that choice, at another effort) -- recorded,
    // so /agent keeps it. Without one the preset's model is swapped in place
    // and nothing is recorded, so a later /agent X runs X's own model rather
    // than this session's old one at the chosen effort. Subagents inherit
    // either way: spawn falls back to the parent's preset().model_ref.
    static CommandResult SwitchTo(CommandContext& context, const config::ModelRef& ref,
                                  std::string ok_message) {
        if (context.session().model_override()) {
            return SwitchModel(context, ref, ref.str(), std::move(ok_message));
        }
        const auto model = context.providers().Resolve(ref);
        if (model == nullptr) {
            return CommandResult::Error("unknown model '" + ref.str() +
                                        "' (run /model with no arguments for the list)");
        }
        auto preset = context.session().preset();
        minimal::agent::ApplyModel(preset, *model);
        context.session().ApplyPreset(std::move(preset));
        return CommandResult::Ok(std::move(ok_message));
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

// /rename <name>: renames THIS session (works on a subagent tab too -- ReadOnly
// means it runs on the command pool, past the subagent guard, and beside a live
// turn). The name is the raw tail verbatim, so it may contain spaces. The
// session persists the name first (name_source='explicit' -- a USER rename
// wins over any previous name and makes a later titler pass skip the session),
// then notifies attached clients via a partial SessionMetadataEvent; the tab
// label refresh needs nothing from this result beyond the acknowledgement.
class RenameCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "rename"; }
    std::string_view Summary() const override { return "Rename this session's tab"; }
    std::string_view Usage() const override { return "/rename <name>"; }
    bool ReadOnly() const override { return true; }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        // raw is everything after the command name, verbatim (CommandArgs):
        // splitting positional tokens would collapse the whitespace a name may
        // legitimately contain. Trim the edges only.
        const char* whitespace = " \t";
        const std::string& raw = args.raw;
        const auto first = raw.find_first_not_of(whitespace);
        if (first == std::string::npos) {
            return CommandResult::Error("usage: /rename <name>");
        }
        const auto last = raw.find_last_not_of(whitespace);
        const std::string name = raw.substr(first, last - first + 1);
        if (!context.session().RenameSession(name)) {
            return CommandResult::Error("rename failed (the session may be gone -- see the log)");
        }
        return CommandResult::Ok("renamed to: " + name);
    }
};

// Bare /subagents opens the client's subagent picker (display=View, view_hint
// "subagents"), the same shape as /sessions: the command carries no data, the
// client fetches the FOCUSED session's children over
// ListSessions(include_subagents) filtered to that parent. ReadOnly so it
// answers beside a live turn and on a nested subagent's tab (listing that
// child's own spawns).
class SubagentsCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "subagents"; }
    std::string_view Summary() const override { return "List this session's subagents"; }
    std::string_view Usage() const override { return "/subagents"; }
    bool ReadOnly() const override { return true; }
    minimal::commands::CommandDisplay Display() const override {
        return minimal::commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "subagents"; }

    CommandResult Execute(CommandContext&, const CommandArgs&) override {
        return CommandResult::Ok("subagent list requested");
    }
};

// /permissions -- what gates THIS session, and why. Bare: the effective
// policy (run mode, category modes, per-tool rules, pattern counts, MCP) and
// the session grants held. `revoke <key>|all` drops grants. `check <tool>
// <json-input>` is a dry run through the real gate (ToolExecutor::Explain):
// the mode, the rule that decided it, and whether held grants would answer
// an Ask -- no prompt, no hook, nothing executed.
//
// Not ReadOnly: the grant set and the policy belong to the session worker
// (Prepare mutates grants, ApplyPreset swaps the policy), so every branch
// runs there, at a turn boundary.
class PermissionsCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "permissions"; }
    std::string_view Summary() const override {
        return "Show this session's permission rules and grants; revoke grants; dry-run a call";
    }
    std::string_view Usage() const override {
        return "/permissions [revoke <key>|all] [check <tool> <json-input>]";
    }

    CommandResult Execute(CommandContext& context, const CommandArgs& args) override {
        auto& executor = context.session().executor();
        if (args.positional.empty()) return CommandResult::Ok(Describe(executor));

        const std::string& verb = args.positional.front();
        if (verb == "revoke") {
            if (args.positional.size() != 2) {
                return CommandResult::Error("usage: /permissions revoke <key>|all");
            }
            if (args.positional[1] == "all") {
                return CommandResult::Ok("revoked " + std::to_string(executor.RevokeAllGrants()) +
                                         " grant(s)");
            }
            if (!executor.RevokeGrant(args.positional[1])) {
                return CommandResult::Error("no such grant held: " + args.positional[1]);
            }
            return CommandResult::Ok("revoked " + args.positional[1]);
        }
        if (verb == "check") return Check(context, executor, args);
        return CommandResult::Error("usage: " + std::string(Usage()));
    }

private:
    static std::string Describe(const minimal::tools::ToolExecutor& executor) {
        using minimal::tools::mode_name;
        const auto& policy = executor.policy();
        std::ostringstream out;
        out << "run mode: " << minimal::config::ToString(executor.run_mode()) << "\n";
        out << "categories: read_only=" << mode_name(policy.read_only)
            << " file_write=" << mode_name(policy.file_write)
            << " execute=" << mode_name(policy.execute)
            << " · out_of_workdir=" << mode_name(policy.out_of_workdir)
            << " network=" << mode_name(policy.network) << "\n";
        auto rules = [&out](std::string_view where,
                            const std::unordered_map<std::string, minimal::tools::PermissionMode>&
                                map) {
            if (map.empty()) return;
            std::vector<std::string> entries;
            for (const auto& [key, mode] : map) {
                entries.push_back(key + "=" + std::string(mode_name(mode)));
            }
            std::sort(entries.begin(), entries.end());
            out << where << ":";
            for (const auto& entry : entries) out << " " << entry;
            out << "\n";
        };
        if (policy.layers) {
            rules("preset rules", policy.layers->preset.per_tool_overrides);
            rules("permissions.json rules", policy.layers->global.per_tool_overrides);
            if (!policy.layers->preset_trusted) {
                out << "(project preset: may only narrow permissions.json)\n";
            }
        } else {
            rules("per-tool rules", policy.per_tool_overrides);
        }
        if (!policy.pattern_overrides.empty()) {
            std::vector<std::string> entries;
            for (const auto& [key, list] : policy.pattern_overrides) {
                entries.push_back(key + " (" + std::to_string(list.size()) + ")");
            }
            std::sort(entries.begin(), entries.end());
            out << "patterns:";
            for (const auto& entry : entries) out << " " << entry;
            out << "\n";
        }
        // Each permission model describes its own layer; core knows none of
        // their shapes.
        for (const auto& bound : executor.permission_models()) {
            const std::string line = bound.model->Describe(bound.layer.get());
            if (!line.empty()) out << bound.name << ": " << line << "\n";
        }
        const auto grants = executor.granted_keys();
        out << "session grants (" << grants.size() << ")";
        for (const auto& key : grants) out << "\n  " << key;
        return out.str();
    }

    static CommandResult Check(CommandContext& context, minimal::tools::ToolExecutor& executor,
                               const CommandArgs& args) {
        if (args.positional.size() < 2) {
            return CommandResult::Error("usage: /permissions check <tool> <json-input>");
        }
        const std::string& tool = args.positional[1];
        // The input is everything after the tool name. JSON carries spaces the
        // tokenizer split on, so the raw tail is authoritative; rejoining the
        // positional tokens is the fallback for callers that set no raw.
        std::string tail;
        if (!args.raw.empty()) {
            auto at = args.raw.find("check");
            at = at == std::string::npos ? std::string::npos : args.raw.find(tool, at + 5);
            if (at != std::string::npos) tail = args.raw.substr(at + tool.size());
        } else {
            for (std::size_t i = 2; i < args.positional.size(); ++i) {
                tail += " " + args.positional[i];
            }
        }
        nlohmann::json input = nlohmann::json::object();
        if (tail.find_first_not_of(" \t") != std::string::npos) {
            try {
                input = nlohmann::json::parse(tail);
            } catch (const std::exception& error) {
                return CommandResult::Error(std::string("input is not JSON: ") + error.what());
            }
        }
        minimal::tools::ToolContext ctx(context.logger());
        ctx.session_id = context.session().session_id();
        ctx.project_root = context.session().project_root();
        bool grants_satisfy = false;
        std::optional<minimal::tools::ToolExecutor::Verdict> verdict;
        try {
            verdict = executor.Explain(tool, input, ctx, &grants_satisfy);
        } catch (const std::exception& error) {
            return CommandResult::Error(std::string("the tool rejected this input: ") +
                                        error.what());
        }
        if (!verdict) return CommandResult::Error("no such tool: " + tool);
        std::string answer = tool + ": " + std::string(minimal::tools::mode_name(verdict->mode)) +
                             " -- " + verdict->reason;
        if (grants_satisfy) answer += " (held grants answer it: runs without a prompt)";
        if (verdict->mode == minimal::tools::PermissionMode::Ask && !verdict->ask_grantable) {
            answer += " (an ask pattern: always prompts, grants or not)";
        }
        return CommandResult::Ok(answer);
    }
};

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

    CommandResult Execute(CommandContext&, const CommandArgs& args) override {
        if (args.positional.size() != 2) {
            return CommandResult::Error("usage: /connect <provider> <api-key>");
        }
        const std::string& provider = args.positional[0];
        const std::string& key = args.positional[1];
        // docs/CONFIGURATION.md's actual rule: a provider name is anything
        // except `/` (it's a JSON object key here, not a path segment). The
        // old allowlist (alnum, `-`, `.`) was stricter than that contract --
        // a validly configured `my_provider` (underscore) could never be
        // connected by name.
        if (provider.empty() || key.empty() || provider.find('/') != std::string::npos) {
            return CommandResult::Error("invalid provider name or key");
        }

        const auto path = common::DataDir() / "auth.json";
        // The whole read-modify-write runs under a cross-process exclusive
        // lock: two sessions running /connect concurrently would otherwise
        // race (both read, both merge, last writer wins) and one provider's
        // entry would silently vanish. ExclusiveFileLock throws when the lock
        // is held -- the command surface has no exception handler, so the
        // refusal is converted to a command error below. The lock and the
        // save are caught separately: a failure writing the file (a key that
        // is not valid UTF-8 cannot be dumped as JSON; the data dir cannot be
        // created) is not a lock problem and must not be reported as one.
        std::optional<common::ExclusiveFileLock> lock;
        try {
            lock.emplace(path);
        } catch (const std::exception& e) {
            return CommandResult::Error("could not lock " + path.string() +
                                        " for update: " + e.what());
        }
        try {
            return SaveCredential(path, provider, key);
        } catch (const std::exception& e) {
            return CommandResult::Error("could not store the credential in " + path.string() +
                                        ": " + e.what());
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

}  // namespace

// /trust-directory and /untrust-directory: the operator's grant for a project's
// own agents, skills and commands (settings.json project_local.mode). The grant
// covers the project's git root, so it holds for every session started anywhere
// inside the repository. It takes effect for what is resolved from now on
// (/agent, a new session, the next listing); the session's current preset is not
// swapped underneath it.
class TrustDirectoryCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "trust-directory"; }
    std::string_view Summary() const override {
        return "Load this project's own agents, skills and commands";
    }
    std::string_view Usage() const override { return "/trust-directory"; }

    CommandResult Execute(CommandContext& context, const CommandArgs&) override {
        minimal::config::ProjectTrust* trust = context.presets().project_trust();
        if (trust == nullptr) return CommandResult::Error("project trust is not configured");
        if (trust->mode() == minimal::config::ProjectLocalMode::kAll) {
            return CommandResult::Ok(
                "project_local.mode is \"all\": every project's own agents, skills and "
                "commands already load, nothing to trust");
        }
        if (trust->mode() == minimal::config::ProjectLocalMode::kNone) {
            return CommandResult::Error(
                "project_local.mode is \"none\": project-local content never loads. Set it to "
                "\"trusted\" in settings.json to use /trust-directory");
        }
        const std::filesystem::path root =
            minimal::config::ProjectTrust::RootFor(context.session().project_root());
        if (root.empty()) return CommandResult::Error("this session has no project directory");
        if (trust->IsTrusted(root)) {
            return CommandResult::Ok(root.string() + " is already trusted");
        }

        const auto inventory = trust->Inspect(root);
        minimal::commands::ConfirmationRequest request;
        request.kind = "command_confirmation";
        request.title = "Trust " + root.string() + "?";
        request.detail =
            "Its own agents, skills and commands will load in every session under this "
            "directory (found now: " +
            inventory.Describe() +
            "). Repository content can steer the model; trust only code you would run.";
        request.options = {"Trust", "Cancel"};
        const auto answer = context.confirmations().RequestConfirmation(request);
        if (!answer.answered()) return CommandResult::Error("/trust-directory was not confirmed");
        if (answer.chosen_option != "Trust") return CommandResult::Ok("not trusted");

        std::string error;
        if (!trust->Trust(root, error)) {
            return CommandResult::Error("could not record the trust: " + error);
        }
        return CommandResult::Ok("trusted " + root.string() + " (" + inventory.Describe() +
                                 " now load; use /agent to pick up its agents)");
    }
};

class UntrustDirectoryCommand : public HarnessCommand {
public:
    std::string_view Name() const override { return "untrust-directory"; }
    std::string_view Summary() const override {
        return "Stop loading this project's own agents, skills and commands";
    }
    std::string_view Usage() const override { return "/untrust-directory"; }

    CommandResult Execute(CommandContext& context, const CommandArgs&) override {
        minimal::config::ProjectTrust* trust = context.presets().project_trust();
        if (trust == nullptr) return CommandResult::Error("project trust is not configured");
        const std::filesystem::path root =
            minimal::config::ProjectTrust::RootFor(context.session().project_root());
        if (root.empty()) return CommandResult::Error("this session has no project directory");
        if (!trust->IsTrusted(root)) return CommandResult::Ok(root.string() + " was not trusted");
        std::string error;
        if (!trust->Revoke(root, error)) {
            return CommandResult::Error("could not record the change: " + error);
        }
        return CommandResult::Ok("no longer trusting " + root.string());
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
    out.push_back(std::make_unique<ThinkCommand>());
    out.push_back(std::make_unique<SessionsCommand>());
    out.push_back(std::make_unique<SubagentsCommand>());
    out.push_back(std::make_unique<RenameCommand>());
    out.push_back(std::make_unique<ConnectCommand>());
    out.push_back(std::make_unique<PermissionsCommand>());
    out.push_back(std::make_unique<TrustDirectoryCommand>());
    out.push_back(std::make_unique<UntrustDirectoryCommand>());
    return out;
}

std::unique_ptr<Command> MakeHelpCommand(const minimal::commands::CommandRegistry& registry) {
    return std::make_unique<HelpCommand>(registry);
}

}  // namespace minimal::commands_core

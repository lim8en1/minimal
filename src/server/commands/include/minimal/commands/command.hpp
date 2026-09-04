#pragma once

// `/`-commands: harness control operations, dispatched from the client over the
// bidi stream and executed as CODE. They bypass the agent turn loop and the LLM
// entirely -- except LLMCommand, which deliberately does not.
//
// See ARCHITECTURE.md "`/`-command system".
//
// TWO PROPERTIES DRIVE EVERYTHING ELSE, and both are on the interface rather
// than in a table somewhere:
//
//   ReadOnly()             -- may run beside a live turn, on a short-lived task.
//                             Anything else queues for the session worker and
//                             runs at a turn boundary.
//   RequiresConfirmation() -- blocks on a round-trip to the human first.
//
// Both default to the safe answer, so a command author who thinks about neither
// gets "runs at a turn boundary, no confirmation" -- which is correct for almost
// everything and never dangerous.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minimal::commands {

// Parsed client-side: the wire carries structured args, so the server never sees
// a raw command line it has to re-split. `raw` is everything after the command
// name, verbatim, for commands that want it unparsed.
struct CommandArgs {
    std::vector<std::string> positional;
    std::unordered_map<std::string, std::string> flags;
    std::string raw;
};

// Where a CommandResult surfaces -- never the chat (docs/EXTENSIBILITY.md 4.4:
// a result is appended to no history and reaches no provider; feeding `git
// tree` or a session list to an LLM makes no sense).
enum class CommandDisplay {
    kStatus,  // ephemeral notice line; errors too, styled as errors
    kView,    // open/refresh the client view named by ViewHint()
    kSilent,  // status bar only; no transient line
};

struct CommandResult {
    enum class Status { kOk, kError };

    Status status = Status::kOk;
    std::string message;
    CommandDisplay display = CommandDisplay::kStatus;
    std::string view_hint;

    static CommandResult Ok(std::string message = {}) {
        CommandResult result;
        result.status = Status::kOk;
        result.message = std::move(message);
        return result;
    }
    static CommandResult Error(std::string message) {
        CommandResult result;
        result.status = Status::kError;
        result.message = std::move(message);
        return result;
    }
};

class CommandContext;  // defined in command_context.hpp; commands take it by ref

class Command {
public:
    virtual ~Command() = default;

    virtual std::string_view Name() const = 0;
    virtual std::string_view Summary() const = 0;
    virtual std::string_view Usage() const = 0;

    // Blocks on a ConfirmationBroker round-trip before Execute. For anything
    // that destroys work the user cannot get back.
    virtual bool RequiresConfirmation() const { return false; }

    // True only if this touches NO session state -- /help and /providers, not
    // /agent or /clear. A read-only command runs on a short-lived task so it can
    // answer while a turn is in flight; everything else waits for a turn
    // boundary, which is what keeps a mutation from landing between a tool_use
    // and its tool_result.
    //
    // Defaults to false: the conservative answer is the one you get for free.
    virtual bool ReadOnly() const { return false; }

    // Whether Execute needs a live session (a non-null CommandContext session
    // ref). Defaults to true -- the safe answer: a command author who thinks
    // about neither this nor ReadOnly() gets "session-bound" and can never
    // dereference a null ref. The Sessionless<> mixin below is the sanctioned
    // way to answer false; the dispatcher REFUSES a sessionless invocation of
    // anything else (DispatchSessionless), so a plain command can never see
    // context.has_session() == false.
    //
    // This is the COMMAND-level answer, not the invocation-level one. /agent
    // and /model are per-invocation (bare form lists, named form mutates), and
    // that wrinkle is owned by the CLIENT: on a sessionless tab it stages the
    // named form into the tab's pending create-options (the choice rides
    // CreateSessionRequest) and opens its own pickers for the bare form, so
    // only genuinely sessionless commands are ever dispatched sessionless.
    virtual bool RequiresSession() const { return true; }

    // Where this command's result surfaces on the client (the wire carries both
    // fields on CommandResult). The default is the ephemeral status line -- the
    // safe answer, and what every existing command keeps getting for free. A
    // View-displaying command pairs this with ViewHint(); the client falls back
    // to the status line when the hint names no view it knows.
    //
    // STATIC, args-independent metadata: describes the command in the
    // abstract, for the catalog (ListCommands/the client's /help palette),
    // where no particular invocation's args exist yet.
    virtual CommandDisplay Display() const { return CommandDisplay::kStatus; }

    // The client view id Display() == kView opens/refreshes. Empty otherwise.
    virtual std::string_view ViewHint() const { return {}; }

    // The dispatcher's actual per-invocation decision (command_dispatcher.cpp):
    // defaults to the static Display()/ViewHint() above, which is correct for
    // every command whose result always surfaces the same way. Override this
    // pair instead of Display()/ViewHint() when a command's own args change
    // where THIS invocation's result belongs -- e.g. bare "/agent" opens the
    // picker (kView), but "/agent <name>" is a direct switch with nothing to
    // show (kStatus); Display()/ViewHint() still answer kView/"agents" for the
    // catalog, since the command AS A WHOLE is view-capable.
    virtual CommandDisplay DisplayFor(const CommandArgs& args) const {
        (void)args;
        return Display();
    }
    virtual std::string_view ViewHintFor(const CommandArgs& args) const {
        (void)args;
        return ViewHint();
    }

    virtual CommandResult Execute(CommandContext& context, const CommandArgs& args) = 0;
};

// Never touches LlmProvider. The read-only ones may run concurrently with a turn.
class HarnessCommand : public Command {};

// Issues a real provider request (/compact), so it needs an idle session and can
// never be ReadOnly(). Split from HarnessCommand as a type rather than a flag
// because the distinction is about what the command IS, and a type cannot be
// forgotten the way a flag can.
class LLMCommand : public Command {
public:
    bool ReadOnly() const final { return false; }
};

// Submits an interpolated prompt AS THE USER TURN -- its own Execute enqueues
// the turn and answers with an empty kOk; the model's reply arrives as ordinary
// turn output, not as a CommandResult. The ONLY command kind whose content
// reaches the model, which is exactly why it is its own type: a prompt macro is
// an honest thing to register, not a HarnessCommand that smuggles a turn.
// (docs/EXTENSIBILITY.md 4.4.)
//
// The base contributes the contract, not the template: the kind is derivable by
// dynamic_cast for ListCommands, and SessionControl::SubmitTurn is the one
// submission path. A shorthand that produced its prompt some other way would
// break the invariant tests, not just the taxonomy.
class PromptShorthandCommand : public Command {};

// Mixin, not a closed marker subclass: `Sessionless<X>` composes with any
// command kind, so a sessionless command keeps whatever taxonomy its base
// carries (dynamic_cast<X*> still sees through it -- the same property
// ListCommands' kind derivation relies on) instead of the codebase growing one
// SessionlessHarnessCommand, SessionlessLLMCommand, ... per kind.
//
// Re-parent onto this to declare "Execute must tolerate a NULL session ref in
// CommandContext". The final keeps the taxonomy honest: a command cannot
// accidentally override itself back to session-bound. Only /help and /sessions
// qualify today -- and ReadOnly() must hold too, which the sessionless
// dispatch path (DispatchSessionless) enforces rather than trusts.
template <typename Base>
class Sessionless : public Base {
public:
    bool RequiresSession() const final { return false; }
};

}  // namespace minimal::commands

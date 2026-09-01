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

    // Where this command's result surfaces on the client (the wire carries both
    // fields on CommandResult). The default is the ephemeral status line -- the
    // safe answer, and what every existing command keeps getting for free. A
    // View-displaying command pairs this with ViewHint(); the client falls back
    // to the status line when the hint names no view it knows.
    virtual CommandDisplay Display() const { return CommandDisplay::kStatus; }

    // The client view id Display() == kView opens/refreshes. Empty otherwise.
    virtual std::string_view ViewHint() const { return {}; }

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

}  // namespace minimal::commands

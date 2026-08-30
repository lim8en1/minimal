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

struct CommandResult {
    enum class Status { kOk, kError };

    Status status = Status::kOk;
    std::string message;

    static CommandResult Ok(std::string message = {}) {
        return CommandResult{Status::kOk, std::move(message)};
    }
    static CommandResult Error(std::string message) {
        return CommandResult{Status::kError, std::move(message)};
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

}  // namespace minimal::commands

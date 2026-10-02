#pragma once

// The worker-blocks / reader-resolves rendezvous. This is the primitive every
// "blocks until answered" interface in the design rests on -- PermissionChecker::
// check, ConfirmationBroker::RequestConfirmation, blocking subagent spawn -- and
// the reason the three-thread-per-session topology exists at all. See
// ARCHITECTURE.md "Concurrency model".
//
// WAITS INDEFINITELY BY DEFAULT. S5 allows a session to run detached, so a
// prompt can now legitimately have nobody to answer it; the default is still
// to wait rather than fail-closed, on the same reasoning as before -- a human
// may simply be away, and auto-denying their work is worse than waiting. An
// operator who wants a hard bound opts in per-session via
// sessions.unattended_prompt_timeout_seconds (0 = wait forever), which rides
// in as an optional timeout on AwaitResponse. Termination still cancels
// explicitly, as it always did.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "minimal/tools/permission_checker.hpp"
#include "minimal/tools/prompt.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

// WHAT AN ANSWER IS, made a type rather than left as a string. Three brokers
// share one PendingRequests (the permission checker, the confirmation broker,
// and the plugin prompt broker), and a bare std::string payload kept them apart only by
// convention: the wire reader knew which kind it was dispatching, the waiter
// guessed by parsing or comparing. With the variant, a cross-delivered answer
// -- a client responding to a prompt id with a permission payload, or vice
// versa -- is structurally unreadable at the waiter instead of merely failing
// to parse. The "perm-"/"confirm-"/"prompt-" id prefixes remain, as diagnostics,
// but they are no longer load-bearing.
struct PermissionAnswer {
    tools::PermissionDecision decision = tools::PermissionDecision::Deny;
    // NOT YET FILTERED against any particular disclosure: a grant key is only
    // meaningful as the answer to something a specific prompt offered, and
    // only the waiter that built that prompt (WirePermissionChecker, holding
    // the original PermissionRequest::disclosure) can filter it. The reader
    // thread that decodes the client's wire message has no disclosure to
    // filter against, so it hands these across verbatim.
    std::vector<std::string> granted_keys;
    std::string deny_message{};   // deny-only, see tools::PermissionResponse
    bool stop_turn = false;     // deny-only, see tools::PermissionResponse
};
struct ConfirmationAnswer {
    std::string chosen_option;
};
// A plugin prompt's answer, verbatim off the wire (proto PluginPromptResponse).
// Nothing is parsed here: WirePluginPromptBroker parses the JSON, and the tool
// that asked validates it.
struct PluginPromptAnswer {
    // Only Answered, Dismissed or Unavailable ever arrive from a client; the
    // reader maps the wire enum (UNSPECIFIED reads as Unavailable).
    tools::PromptOutcome outcome = tools::PromptOutcome::Unavailable;
    std::string answer_json;
};
using PendingAnswer = std::variant<PermissionAnswer, ConfirmationAnswer, PluginPromptAnswer>;

class PendingRequests {
public:
    // Registration is separate from waiting, and that separation is the whole
    // point: the slot must exist before the request is put on the wire, or a
    // fast client can answer before there is anywhere to put the answer.
    // Register() returning the handle you need in order to wait makes the
    // correct order the only expressible one:
    //
    //     auto registration = pending.Register(id);
    //     queue.Push(request_event);            // now safe: the slot exists
    //     auto answer = registration.AwaitResponse();
    //
    // RAII on the slot matters as much as ordering here: the waiter is the
    // session worker running tool dispatch, which can throw, and a leaked slot
    // would be a permanent entry in a map keyed by an id nobody will ever
    // resolve again.
    class Registration {
    public:
        ~Registration();

        Registration(Registration&& other) noexcept;
        Registration& operator=(Registration&& other) noexcept;
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

        // Blocks until Resolve() names this id, cancellation, or (if timeout
        // is set to a positive duration) the deadline passing unanswered.
        // nullopt means "no answer" -- turn cancelled, session terminating,
        // or timed out. Callers must treat it as exactly that, never as a
        // decision: mapping it onto Deny here would put a policy choice
        // inside a synchronisation primitive. std::nullopt (the default)
        // means wait forever, matching the type this session's
        // unattended_prompt_timeout_seconds setting is 0 for.
        std::optional<PendingAnswer> AwaitResponse(
            std::optional<std::chrono::milliseconds> timeout = std::nullopt);

        const std::string& request_id() const { return request_id_; }

        // True once the turn was cancelled or the session shut down: no answer
        // can arrive for this registration any more. Lets a caller that waits in
        // slices tell "slice elapsed" from "gave up".
        bool Abandoned() const;

    private:
        friend class PendingRequests;
        Registration(PendingRequests* owner, std::string request_id, std::uint64_t epoch);

        PendingRequests* owner_ = nullptr;  // null once moved from
        std::string request_id_;
        // The cancel generation this registration was made in. CancelAll() bumps
        // the counter, so every waiter registered before it wakes with nullopt
        // and every waiter registered after it is unaffected.
        std::uint64_t epoch_ = 0;
    };

    //
    // `prompt` is the wire event that asked the question, kept while the slot is
    // open so a stream that attaches AFTER it was published (the first client
    // crashed, a second one joins) can be shown the prompt too -- see
    // OpenPrompts(). Without it the worker waits for an answer nobody can see.
    //
    // `expected_kind` is the PendingAnswer variant index this slot accepts. An
    // answer of another kind (a buggy client replying to a permission prompt
    // with a confirmation payload) is refused by Resolve -- false, slot still
    // open -- so the prompt stays answerable instead of being consumed by a
    // payload the waiter cannot read and turned into a Deny.
    static constexpr std::size_t kAnyKind = std::variant_npos;
    [[nodiscard]] Registration Register(std::string request_id,
                                        std::optional<v1::ServerEvent> prompt = std::nullopt,
                                        std::size_t expected_kind = kAnyKind);

    // The prompt events whose slots are still unanswered, in registration order.
    // Any thread.
    std::vector<v1::ServerEvent> OpenPrompts() const;

    // Reader thread. Returns false if no slot is registered under this id, which
    // is not an error: a client may answer a request whose session has already
    // terminated, and the honest response to that is to drop it. Unknown ids are
    // never buffered for a future waiter -- that would let a client grow this map
    // without bound by answering requests that were never asked. The reader
    // constructs the answer variant from the ClientEvent's own payload case, so
    // it never parses anything and the type travels with the value.
    bool Resolve(const std::string& request_id, PendingAnswer answer);

    // TWO CANCELLATIONS, because the session has two of them and they mean
    // different things (Session::Cancel ends the TURN, Session::Terminate ends
    // the SESSION). A single permanent latch served both, which made one
    // CancelTurn deny-lock the session forever: every subsequent permission
    // prompt returned "no answer" instantly and WirePermissionChecker mapped
    // that to Deny, with no log line and no way for the user to tell.

    // Wakes every CURRENT waiter with nullopt and leaves the object usable.
    // Registrations made after this call block normally, so a cancelled turn
    // does not poison the next one. Idempotent.
    void CancelAll();

    // Wakes every waiter with nullopt, now and forever. Registrations made after
    // this return immediately rather than blocking, so a worker that reaches a
    // permission gate during teardown does not park on a reader that has already
    // stopped. Idempotent, and deliberately irreversible -- a session that is
    // terminating never comes back.
    void Shutdown();

    bool shut_down() const;
    std::size_t pending_count() const;

private:
    void Unregister(const std::string& request_id);
    // Callers hold mutex_.
    bool AnswerIsUnreachable(std::uint64_t epoch) const { return shutdown_ || epoch != epoch_; }

    struct Slot {
        std::optional<PendingAnswer> answer;
        std::optional<v1::ServerEvent> prompt;
        std::size_t expected_kind = kAnyKind;
        std::uint64_t order = 0;  // registration order, for OpenPrompts()
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Slot> slots_;
    std::uint64_t epoch_ = 0;
    std::uint64_t next_order_ = 0;
    bool shutdown_ = false;
};

}  // namespace minimal::rpc

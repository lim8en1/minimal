#pragma once

// The worker-blocks / reader-resolves rendezvous. This is the primitive every
// "blocks until answered" interface in the design rests on -- PermissionChecker::
// check, ConfirmationBroker::RequestConfirmation, blocking subagent spawn -- and
// the reason the three-thread-per-session topology exists at all. See
// ARCHITECTURE.md "Concurrency model".
//
// WAITS INDEFINITELY, deliberately. There is no timeout and no fail-closed
// path. A client is present by construction (no detached sessions -- see
// "Session lifetime"), so the only way a wait outlives its answer is termination,
// which cancels explicitly. A human may simply be away, and auto-denying their
// work because they went to lunch is worse than waiting.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace minimal::rpc {

// WHAT AN ANSWER IS, made a type rather than left as a string. Two brokers
// share one PendingRequests (the permission checker and the confirmation
// broker), and a bare std::string payload kept them apart only by convention:
// the wire reader knew which kind it was dispatching, the waiter guessed by
// parsing or comparing. With the variant, a cross-delivered answer -- a client
// responding to a confirmation id with a permission payload, or vice versa --
// is structurally unreadable at the waiter instead of merely failing to parse.
// The "perm-"/"confirm-" id prefixes remain, as diagnostics, but they are no
// longer load-bearing.
struct PermissionAnswer {
    // Still serialized at this layer: parsing the proto stays on the worker,
    // which can afford to fail, not on the reader, which must not stall.
    std::string serialized_response;
};
struct ConfirmationAnswer {
    std::string chosen_option;
};
using PendingAnswer = std::variant<PermissionAnswer, ConfirmationAnswer>;

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

        // Blocks until Resolve() names this id, or until cancellation.
        // nullopt means "no answer" -- either the turn was cancelled or the
        // session is terminating. Callers must treat it as exactly that, never
        // as a decision: mapping it onto Deny here would put a policy choice
        // inside a synchronisation primitive.
        std::optional<PendingAnswer> AwaitResponse();

        const std::string& request_id() const { return request_id_; }

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

    [[nodiscard]] Registration Register(std::string request_id);

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
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Slot> slots_;
    std::uint64_t epoch_ = 0;
    bool shutdown_ = false;
};

}  // namespace minimal::rpc

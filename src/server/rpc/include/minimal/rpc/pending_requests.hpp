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
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace minimal::rpc {

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
        // nullopt means cancelled -- the session is terminating. Callers must
        // treat that as "no answer", never as a decision: mapping it onto Deny
        // here would put a policy choice inside a synchronisation primitive.
        std::optional<std::string> AwaitResponse();

        const std::string& request_id() const { return request_id_; }

    private:
        friend class PendingRequests;
        Registration(PendingRequests* owner, std::string request_id);

        PendingRequests* owner_ = nullptr;  // null once moved from
        std::string request_id_;
    };

    [[nodiscard]] Registration Register(std::string request_id);

    // Reader thread. Returns false if no slot is registered under this id, which
    // is not an error: a client may answer a request whose session has already
    // terminated, and the honest response to that is to drop it. Unknown ids are
    // never buffered for a future waiter -- that would let a client grow this map
    // without bound by answering requests that were never asked.
    bool Resolve(const std::string& request_id, std::string chosen_option);

    // Wakes every waiter with nullopt. Idempotent, and permanent: registrations
    // made after cancellation return immediately rather than blocking, so a
    // worker that reaches a permission gate during teardown does not park on a
    // reader that has already stopped.
    void CancelAll();

    bool cancelled() const;
    std::size_t pending_count() const;

private:
    void Unregister(const std::string& request_id);

    struct Slot {
        std::optional<std::string> answer;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Slot> slots_;
    bool cancelled_ = false;
};

}  // namespace minimal::rpc

#include "minimal/rpc/pending_requests.hpp"

#include <algorithm>
#include <utility>

namespace minimal::rpc {

PendingRequests::Registration::Registration(PendingRequests* owner, std::string request_id,
                                            std::uint64_t epoch)
    : owner_(owner), request_id_(std::move(request_id)), epoch_(epoch) {}

PendingRequests::Registration::Registration(Registration&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      request_id_(std::move(other.request_id_)),
      epoch_(other.epoch_) {}

PendingRequests::Registration& PendingRequests::Registration::operator=(
    Registration&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) owner_->Unregister(request_id_);
        owner_ = std::exchange(other.owner_, nullptr);
        request_id_ = std::move(other.request_id_);
        epoch_ = other.epoch_;
    }
    return *this;
}

PendingRequests::Registration::~Registration() {
    if (owner_ != nullptr) owner_->Unregister(request_id_);
}

std::optional<PendingAnswer> PendingRequests::Registration::AwaitResponse(
    std::optional<std::chrono::milliseconds> timeout) {
    if (owner_ == nullptr) return std::nullopt;

    std::unique_lock<std::mutex> lock(owner_->mutex_);
    // Waiting on the SLOT's answer rather than on "was I notified" is what makes
    // resolve-before-await work: by the time this runs the answer may already be
    // sitting in the slot, and a notification-based wait would have missed it and
    // blocked forever.
    auto ready = [this] {
        if (owner_->AnswerIsUnreachable(epoch_)) return true;
        const auto it = owner_->slots_.find(request_id_);
        return it == owner_->slots_.end() || it->second.answer.has_value();
    };
    if (timeout.has_value() && *timeout > std::chrono::milliseconds::zero()) {
        // A timeout that elapses with no answer is indistinguishable here from
        // any other "no answer" outcome -- the check below falls through to
        // the same nullopt every unresolved wakeup produces, deliberately: the
        // caller (WirePermissionChecker / WireConfirmationBroker) already
        // treats nullopt as its one "nobody answered" case.
        owner_->cv_.wait_for(lock, *timeout, ready);
    } else {
        owner_->cv_.wait(lock, ready);
    }

    // Checked again rather than inferred from the predicate: an answer that
    // landed in the same wakeup as a cancellation is still an answer nobody is
    // going to act on, since the turn it belonged to is over.
    if (owner_->AnswerIsUnreachable(epoch_)) return std::nullopt;
    const auto it = owner_->slots_.find(request_id_);
    if (it == owner_->slots_.end()) return std::nullopt;
    return it->second.answer;
}

PendingRequests::Registration PendingRequests::Register(std::string request_id,
                                                         std::optional<v1::ServerEvent> prompt) {
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[request_id];  // default-constructs an empty slot
        slot.prompt = std::move(prompt);
        slot.order = next_order_++;
        epoch = epoch_;
    }
    return Registration(this, std::move(request_id), epoch);
}

std::vector<v1::ServerEvent> PendingRequests::OpenPrompts() const {
    std::vector<std::pair<std::uint64_t, v1::ServerEvent>> open;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return {};
        for (const auto& [id, slot] : slots_) {
            if (slot.prompt && !slot.answer) open.emplace_back(slot.order, *slot.prompt);
        }
    }
    std::sort(open.begin(), open.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<v1::ServerEvent> events;
    events.reserve(open.size());
    for (auto& entry : open) events.push_back(std::move(entry.second));
    return events;
}

bool PendingRequests::Resolve(const std::string& request_id, PendingAnswer answer) {
    bool won = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(request_id);
        if (it == slots_.end()) return false;
        // First answer wins. A duplicate is a client retransmitting, not a
        // change of mind, and silently overwriting an answer a worker may
        // already have acted on would be worse than ignoring it. The return
        // value distinguishes the two cases -- S5.3 needs to know whether
        // THIS call was the one that actually answered, so it broadcasts
        // PromptResolvedEvent to dismiss every other subscriber's modal
        // exactly once, not once per client that happened to answer.
        if (!it->second.answer.has_value()) {
            it->second.answer = std::move(answer);
            won = true;
        }
    }
    cv_.notify_all();
    return won;
}

void PendingRequests::CancelAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Bumped, not latched. Every waiter registered in the old generation
        // wakes with nullopt; the next Register() takes the new one and blocks
        // normally, so the turn after a cancel can still ask its questions.
        ++epoch_;
    }
    cv_.notify_all();
}

void PendingRequests::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

bool PendingRequests::shut_down() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
}

std::size_t PendingRequests::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.size();
}

void PendingRequests::Unregister(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.erase(request_id);
}

}  // namespace minimal::rpc

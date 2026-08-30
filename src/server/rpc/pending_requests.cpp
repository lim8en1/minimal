#include "minimal/rpc/pending_requests.hpp"

#include <utility>

namespace minimal::rpc {

PendingRequests::Registration::Registration(PendingRequests* owner, std::string request_id)
    : owner_(owner), request_id_(std::move(request_id)) {}

PendingRequests::Registration::Registration(Registration&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), request_id_(std::move(other.request_id_)) {}

PendingRequests::Registration& PendingRequests::Registration::operator=(
    Registration&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) owner_->Unregister(request_id_);
        owner_ = std::exchange(other.owner_, nullptr);
        request_id_ = std::move(other.request_id_);
    }
    return *this;
}

PendingRequests::Registration::~Registration() {
    if (owner_ != nullptr) owner_->Unregister(request_id_);
}

std::optional<std::string> PendingRequests::Registration::AwaitResponse() {
    if (owner_ == nullptr) return std::nullopt;

    std::unique_lock<std::mutex> lock(owner_->mutex_);
    // Waiting on the SLOT's answer rather than on "was I notified" is what makes
    // resolve-before-await work: by the time this runs the answer may already be
    // sitting in the slot, and a notification-based wait would have missed it and
    // blocked forever.
    owner_->cv_.wait(lock, [this] {
        if (owner_->cancelled_) return true;
        const auto it = owner_->slots_.find(request_id_);
        return it == owner_->slots_.end() || it->second.answer.has_value();
    });

    const auto it = owner_->slots_.find(request_id_);
    if (it == owner_->slots_.end()) return std::nullopt;
    return it->second.answer;  // nullopt when woken by cancellation
}

PendingRequests::Registration PendingRequests::Register(std::string request_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_[request_id];  // default-constructs an empty slot
    }
    return Registration(this, std::move(request_id));
}

bool PendingRequests::Resolve(const std::string& request_id, std::string chosen_option) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(request_id);
        if (it == slots_.end()) return false;
        // First answer wins. A duplicate is a client retransmitting, not a
        // change of mind, and silently overwriting an answer a worker may
        // already have acted on would be worse than ignoring it.
        if (!it->second.answer.has_value()) it->second.answer = std::move(chosen_option);
    }
    cv_.notify_all();
    return true;
}

void PendingRequests::CancelAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
    }
    cv_.notify_all();
}

bool PendingRequests::cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
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

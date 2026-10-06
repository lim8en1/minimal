#pragma once

// A value a plugin swaps in while it runs -- its settings, after
// minimal_plugin_reconfigure. An SDK header.
//
// Readers take a snapshot (Get) and use it for the whole operation: a tool
// call never sees half of an old config and half of a new one, and a swap
// never waits on a running call.

#include <memory>
#include <mutex>
#include <utility>

namespace minimal::common {

template <typename T>
class LiveValue {
public:
    explicit LiveValue(T initial) : current_(std::make_shared<const T>(std::move(initial))) {}

    std::shared_ptr<const T> Get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    void Set(T next) {
        auto replacement = std::make_shared<const T>(std::move(next));
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = std::move(replacement);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const T> current_;
};

}  // namespace minimal::common

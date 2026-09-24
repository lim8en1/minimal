#include "minimal/rpc/server_housekeeping.hpp"

#include <utility>

namespace minimal::rpc {

ServerHousekeeping::ServerHousekeeping(Config config) : config_(std::move(config)) {
    task_ = std::make_unique<common::PeriodicTask>(
        [interval = config_.tick] { return interval; },
        [this] { Tick(std::chrono::steady_clock::now()); });
}

ServerHousekeeping::~ServerHousekeeping() { Stop(); }

void ServerHousekeeping::Stop() {
    if (task_ != nullptr) task_->Stop();
}

void ServerHousekeeping::Tick(std::chrono::steady_clock::time_point now) {
    if (config_.idle_shutdown.count() > 0 && config_.attached_clients && config_.on_idle_expired) {
        bool quiet = config_.attached_clients() == 0 &&
                     !(config_.sessions_hold && config_.sessions_hold());
        if (quiet && config_.client_idle_for) {
            quiet = config_.client_idle_for() >= config_.idle_shutdown;
        }
        if (quiet) {
            if (!idle_since_) idle_since_ = now;
            if (now - *idle_since_ >= config_.idle_shutdown) config_.on_idle_expired();
        } else {
            idle_since_.reset();
        }
    }
    if (config_.periodic_checkpoint) config_.periodic_checkpoint();
}

}  // namespace minimal::rpc

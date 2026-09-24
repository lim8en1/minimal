#include "minimal/rpc/auth_provider.hpp"

#include "minimal/common/logger.hpp"

namespace minimal::rpc {

void AuthProviderRegistry::Register(std::shared_ptr<AuthProvider> provider) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!provider) return;
    if (claimed_) {
        // Exclusive on purpose: see the header. A second registration is a
        // configuration error worth naming, not a silent overwrite.
        common::DefaultLogger().warn(
            "a second auth provider was registered; the first one stays in force -- "
            "two simultaneous answers to 'who may connect' is a misconfiguration");
        return;
    }
    provider_ = std::move(provider);
    owner_ = attribution_;
    claimed_ = true;
}

void AuthProviderRegistry::SetAttribution(std::string plugin_name) {
    std::lock_guard<std::mutex> lock(mu_);
    attribution_ = std::move(plugin_name);
}

void AuthProviderRegistry::ClearAttribution() {
    std::lock_guard<std::mutex> lock(mu_);
    attribution_.clear();
}

bool AuthProviderRegistry::OwnedBy(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    return provider_ != nullptr && owner_ == plugin_name;
}

void AuthProviderRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lock(mu_);
    if (provider_ != nullptr && owner_ == plugin_name) {
        provider_.reset();
        owner_.clear();
        claimed_ = false;
    }
}

}  // namespace minimal::rpc

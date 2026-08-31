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
    claimed_ = true;
}

}  // namespace minimal::rpc

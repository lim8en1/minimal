#pragma once

// AuthProvider: the exclusive "one global choice" extension point
// (ARCHITECTURE.md, "MentionResolver and AuthProvider"). The default is NO
// authentication, matching the local-only single-user topology: the registry
// empty means every request passes, and that pass-through is the actual
// default rather than a missing feature.
//
// A provider is a policy, not a credential store: it sees the ServerContext
// (peer identity, transport security) per RPC and answers allow/deny. The
// shared-token implementation ships as a plugin example, deferred; nothing in
// core hardcodes a credential.
//
// EXCLUSIVE by design: two simultaneous answers to "who may connect" is not a
// composition question, it is a misconfiguration. First registration wins; a
// second logs and is ignored, mirroring the ABI's exclusive-symbol semantics.

#include <grpcpp/grpcpp.h>

#include <memory>
#include <mutex>

namespace minimal::rpc {

class AuthProvider {
public:
    virtual ~AuthProvider() = default;

    // Called before a dispatched service method acts on the request. false ->
    // gRPC UNAUTHENTICATED. Must be cheap and non-blocking: it runs on the RPC
    // handler thread, and for OpenSession that thread is the reader.
    virtual bool Authenticate(const grpc::ServerContext& context) const = 0;
};

class AuthProviderRegistry {
public:
    // First registration wins; later ones are recorded and refused.
    void Register(std::shared_ptr<AuthProvider> provider);

    // Null when no provider registered -- the pass-through default. Locked
    // against Register()'s write; note this only serializes the pointer read
    // itself, not the pointee's lifetime past the lock -- safe because
    // Register() is first-registration-wins and expected to run once, at
    // startup.
    const AuthProvider* provider() const {
        std::lock_guard<std::mutex> lock(mu_);
        return provider_.get();
    }

private:
    std::shared_ptr<AuthProvider> provider_;
    bool claimed_ = false;  // guarded by mu_ (plugin loading is single-threaded;
                            // the mutex makes the claim race-free regardless)
    mutable std::mutex mu_;
};

}  // namespace minimal::rpc

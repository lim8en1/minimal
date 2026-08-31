#include "minimal/rpc/auth_interceptor.hpp"

namespace minimal::rpc {

grpc::Status EnsureAuthenticated(const AuthProviderRegistry* registry,
                                  grpc::ServerContext* context) {
    const AuthProvider* provider = registry != nullptr ? registry->provider() : nullptr;
    if (provider == nullptr) return grpc::Status::OK;  // the pass-through default
    if (provider->Authenticate(*context)) return grpc::Status::OK;
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "the server's authentication provider refused this connection");
}

}  // namespace minimal::rpc

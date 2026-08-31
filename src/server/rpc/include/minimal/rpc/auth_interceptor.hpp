#pragma once

// The auth check for the gRPC surface, delegating to the registered
// AuthProvider (auth_provider.hpp) -- pass-through when none is registered.
//
// WHY A GUARD AT THE HEAD OF EACH SERVICE METHOD rather than a gRPC
// interceptor: grpc's sync server API does not invoke experimental server
// interceptors for synchronous handlers, so an interceptor here would be
// decorative -- every method would silently skip it. The guard is explicit,
// which is also why the service methods name their ServerContext parameters.
//
// Called once per RPC before any state is touched. A local unix socket with an
// empty registry passes without cost beyond the branch.

#include <grpcpp/grpcpp.h>

#include "minimal/rpc/auth_provider.hpp"

namespace minimal::rpc {

// OK when no provider is registered (registry may be null -- a service built
// without the seam) or the provider authenticates the context; UNAUTHENTICATED
// with the provider's decision otherwise.
grpc::Status EnsureAuthenticated(const AuthProviderRegistry* registry,
                                  grpc::ServerContext* context);

}  // namespace minimal::rpc

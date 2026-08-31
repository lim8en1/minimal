// Auth is the exclusive "one global choice" extension point. Tier 1 covers the
// registry and the guard in isolation; the last case runs the guard over a real
// channel, because a check that never executes on the wire protects nothing.
#include <catch2/catch_test_macros.hpp>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "deadline.hpp"
#include "fakes.hpp"
#include "temp_dir.hpp"
#include "minimal/rpc/auth_interceptor.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/auth_provider.hpp"
#include "minimal/rpc/session_service.hpp"
#include "minimal/v1/session_service.grpc.pb.h"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

class DenyingProvider : public rpc::AuthProvider {
public:
    bool Authenticate(const grpc::ServerContext&) const override { return false; }
};

class AllowingProvider : public rpc::AuthProvider {
public:
    bool Authenticate(const grpc::ServerContext&) const override { return true; }
};

}  // namespace

TEST_CASE("the registry is exclusive: the first provider stays in force",
          "[auth][invariant]") {
    // Two simultaneous answers to "who may connect" is a misconfiguration, not
    // a composition question. First-wins matches the ABI's exclusive-symbol
    // semantics; last-wins would let whichever plugin loaded later decide.
    rpc::AuthProviderRegistry registry;
    registry.Register(std::make_shared<DenyingProvider>());
    registry.Register(std::make_shared<AllowingProvider>());  // must be ignored

    grpc::ServerContext context;
    const auto status = rpc::EnsureAuthenticated(&registry, &context);
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("an empty registry is the pass-through default", "[auth]") {
    // No provider means NO AUTHENTICATION, deliberately: the local-only
    // single-user topology is the security assumption, and the default must be
    // the honest one rather than an accidental lock-out.
    rpc::AuthProviderRegistry registry;
    grpc::ServerContext context;
    CHECK(rpc::EnsureAuthenticated(&registry, &context).ok());
}

TEST_CASE("an allowing provider passes, a denying one is UNAUTHENTICATED", "[auth]") {
    grpc::ServerContext context;

    rpc::AuthProviderRegistry allow;
    allow.Register(std::make_shared<AllowingProvider>());
    CHECK(rpc::EnsureAuthenticated(&allow, &context).ok());

    rpc::AuthProviderRegistry deny;
    deny.Register(std::make_shared<DenyingProvider>());
    const auto status = rpc::EnsureAuthenticated(&deny, &context);
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK_FALSE(status.error_message().empty());
}

TEST_CASE("the guard runs on the wire: a denying provider rejects OpenSession",
          "[auth][e2e]") {
    // The guard sits at the head of each service method because the sync gRPC
    // API does not invoke interceptors for synchronous handlers. That placement
    // is exactly what makes this end-to-end case necessary: if the check were
    // decorative, every in-process assertion above would still pass.
    testing::TempDir dir;
    const auto root = dir.path();

    // Minimal config: the preset registry requires providers.json to exist, and
    // the guard fires long before any of it matters.
    { std::ofstream(root / "providers.json") << "{\"providers\":{}}"; }
    { std::ofstream(root / "settings.json") << "{}"; }
    { std::ofstream(root / "permissions.json") << "{}"; }
    std::filesystem::create_directories(root / "agents");
    config::SkillRegistry skills = config::SkillRegistry::Load(root);
    auto presets = std::make_unique<agent::AgentPresetRegistry>(root, skills);
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    agent::SessionObserverRegistry observers(common::DefaultLogger());
    session::SessionDatabase db(root / "sessions.db");
    agent::SessionRegistry sessions(*presets, tools, providers, observers, db,
                                    agent::LoopConfig{}, common::DefaultLogger());
    rpc::SessionServiceImpl service(sessions, db, config::SettingsConfig{},
                                    config::PermissionsConfig{}, common::DefaultLogger());

    rpc::AuthProviderRegistry auth;
    auth.Register(std::make_shared<DenyingProvider>());
    service.set_auth_providers(&auth);

    const auto socket = root / "auth.sock";
    rpc::Server server(service, socket);
    REQUIRE(server.listening());
    auto channel = grpc::CreateChannel(rpc::SocketTarget(socket),
                                       grpc::InsecureChannelCredentials());
    auto stub = v1::SessionService::NewStub(channel);

    grpc::ClientContext context;
    auto stream = stub->OpenSession(&context);
    v1::ClientEvent attach;
    attach.set_session_id("anything");
    attach.mutable_attach();

    const auto sent = testing::RunInBackground([&] { return static_cast<bool>(stream->Write(attach)); });
    REQUIRE(sent.WaitFor(5s));

    v1::ServerEvent event;
    // OpenSession refuses at the head: no stream body, just the status. Read
    // fails immediately, and Finish carries the verdict.
    while (stream->Read(&event)) {
    }
    const auto status = stream->Finish();
    CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

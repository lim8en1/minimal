// Unit tier for rpc::ApplyPermissionAdvisor -- the composition recipe shared
// by SessionServiceImpl's top-level advisor path and SubagentManager's
// advisor-inheritance path, replacing what used to be two copies of the same
// four steps.

#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "minimal/rpc/session_wiring.hpp"

using namespace minimal;

namespace {

struct AlwaysAllow : tools::PermissionAdvisor {
    std::optional<tools::PermissionResponse> Advise(const tools::PermissionRequest&,
                                                     const tools::AdvisorContext&) override {
        tools::PermissionResponse response;
        response.decision = tools::PermissionDecision::Allow;
        return response;
    }
};

}  // namespace

TEST_CASE("a null advisor leaves SessionParams untouched", "[session_wiring]") {
    agent::SessionParams params;
    params.session_id = "s1";
    rpc::ApplyPermissionAdvisor(params, nullptr, std::chrono::milliseconds(1000));

    CHECK(params.channels == nullptr);
    CHECK(params.permission_checker == nullptr);
    CHECK(params.advisor_cancel_source == nullptr);
}

TEST_CASE("a real advisor gets channels, an advised checker, and a cancel source",
          "[session_wiring]") {
    agent::SessionParams params;
    params.session_id = "s1";
    rpc::ApplyPermissionAdvisor(params, std::make_shared<AlwaysAllow>(),
                               std::chrono::milliseconds(1000));

    REQUIRE(params.channels != nullptr);
    REQUIRE(params.permission_checker != nullptr);
    REQUIRE(params.advisor_cancel_source != nullptr);

    // The advisor answers directly -- this must not block on the wire
    // checker underneath, which nothing here ever answers.
    tools::PermissionRequest request;
    request.tool_name = "bash";
    const auto response = params.permission_checker->check(request);
    CHECK(response.decision == tools::PermissionDecision::Allow);
}

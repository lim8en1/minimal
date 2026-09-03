// Unit tier for rpc::ApplyPermissionAdvisor/FinishSessionWiring -- the
// composition recipes shared by SessionServiceImpl's top-level advisor path,
// SubagentManager's advisor-inheritance path, and SessionRegistry::
// CreateResolved's unconditional funnel.

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

    CHECK(params.transport_handle == nullptr);
    CHECK(params.event_sink == nullptr);
    CHECK(params.permission_checker == nullptr);
    CHECK(params.advisor_cancel_source == nullptr);
}

TEST_CASE("a real advisor gets a transport, an advised checker, and a cancel source",
          "[session_wiring]") {
    agent::SessionParams params;
    params.session_id = "s1";
    rpc::ApplyPermissionAdvisor(params, std::make_shared<AlwaysAllow>(),
                               std::chrono::milliseconds(1000));

    REQUIRE(params.transport_handle != nullptr);
    REQUIRE(params.event_sink != nullptr);
    REQUIRE(params.permission_checker != nullptr);
    REQUIRE(params.advisor_cancel_source != nullptr);

    // The advisor answers directly -- this must not block on the wire
    // checker underneath, which nothing here ever answers.
    tools::PermissionRequest request;
    request.tool_name = "bash";
    const auto response = params.permission_checker->check(request);
    CHECK(response.decision == tools::PermissionDecision::Allow);
}

TEST_CASE("FinishSessionWiring builds every default when nothing was composed early",
          "[session_wiring]") {
    agent::SessionParams params;
    params.session_id = "s2";
    rpc::FinishSessionWiring(params);

    REQUIRE(params.transport_handle != nullptr);
    REQUIRE(params.event_sink != nullptr);
    REQUIRE(params.permission_checker != nullptr);
    REQUIRE(params.confirmation_broker != nullptr);
    REQUIRE(params.cancel_pending_requests != nullptr);
    REQUIRE(params.shutdown_pending_requests != nullptr);
    REQUIRE(params.close_transport != nullptr);
    // No advisor applied, so no cancel source was minted for one.
    CHECK(params.advisor_cancel_source == nullptr);
}

TEST_CASE("FinishSessionWiring reuses an advisor's transport and leaves its checker alone",
          "[session_wiring]") {
    agent::SessionParams params;
    params.session_id = "s3";
    rpc::ApplyPermissionAdvisor(params, std::make_shared<AlwaysAllow>(),
                               std::chrono::milliseconds(1000));
    const auto* checker_before = params.permission_checker.get();
    const auto transport_before = params.transport_handle;

    rpc::FinishSessionWiring(params);

    // The advised checker survives untouched -- FinishSessionWiring only
    // builds a default when none was already composed.
    CHECK(params.permission_checker.get() == checker_before);
    // The SAME transport, not a second one -- an advised session's
    // confirmation broker must publish on the same stream its permission
    // requests already use.
    CHECK(params.transport_handle == transport_before);
    REQUIRE(params.confirmation_broker != nullptr);
}

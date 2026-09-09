// Unit tier for rpc::FinishSessionWiring -- the composition recipe shared by
// SessionRegistry::CreateResolved's unconditional funnel and every test that
// builds a Session directly. SessionServiceImpl's advisor path and
// SubagentManager's advisor-inheritance path only ever SELECT an advisor
// (SessionParams::selected_advisor/advisor_timeout); this file exercises
// what FinishSessionWiring does with that selection.

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

TEST_CASE("FinishSessionWiring builds every default when no advisor is selected",
          "[session_wiring]") {
    agent::SessionParams params;
    params.session_id = "s2";
    rpc::FinishSessionWiring(params);

    REQUIRE(params.transport_handle != nullptr);
    REQUIRE(params.event_sink != nullptr);
    REQUIRE(params.permission_checker != nullptr);
    REQUIRE(params.confirmation_broker != nullptr);
    REQUIRE(params.ask_broker != nullptr);
    REQUIRE(params.cancel_pending_requests != nullptr);
    REQUIRE(params.shutdown_pending_requests != nullptr);
    REQUIRE(params.close_transport != nullptr);
    // No advisor applied, so no cancel source was minted for one.
    CHECK(params.advisor_cancel_source == nullptr);
}

TEST_CASE("a selected advisor gets composed into a checker with a transport and cancel source",
          "[session_wiring]") {
    agent::SessionParams params;
    params.session_id = "s1";
    params.selected_advisor = std::make_shared<AlwaysAllow>();
    params.advisor_timeout = std::chrono::milliseconds(1000);
    rpc::FinishSessionWiring(params);

    REQUIRE(params.transport_handle != nullptr);
    REQUIRE(params.event_sink != nullptr);
    REQUIRE(params.permission_checker != nullptr);
    REQUIRE(params.advisor_cancel_source != nullptr);
    REQUIRE(params.confirmation_broker != nullptr);

    // The advisor answers directly -- this must not block on the wire
    // checker underneath, which nothing here ever answers.
    tools::PermissionRequest request;
    request.tool_name = "bash";
    const auto response = params.permission_checker->check(request);
    CHECK(response.decision == tools::PermissionDecision::Allow);
}

TEST_CASE("FinishSessionWiring reuses a producer's transport and leaves its checker alone",
          "[session_wiring]") {
    // Mirrors how a test builds a Session directly: FinishSessionWiring runs
    // once for the advisor composition, and the checker/transport it built
    // are forwarded into a second SessionParams that FinishSessionWiring
    // wires again (session_test.cpp's SessionFixture::Start does exactly
    // this).
    agent::SessionParams composed;
    composed.session_id = "s3";
    composed.selected_advisor = std::make_shared<AlwaysAllow>();
    composed.advisor_timeout = std::chrono::milliseconds(1000);
    rpc::FinishSessionWiring(composed);
    const auto* checker_before = composed.permission_checker.get();
    const auto transport_before = composed.transport_handle;

    agent::SessionParams params;
    params.session_id = "s3";
    params.permission_checker = std::move(composed.permission_checker);
    params.transport_handle = composed.transport_handle;
    params.event_sink = composed.event_sink;
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

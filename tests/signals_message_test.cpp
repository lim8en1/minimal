// The message_session tool against a scripted core.signals service: the
// outcome mapping IS the tool's behavior (delivered / not-subscribed / self
// / degraded host), plus the registration surface a third-party producer
// would reuse. Router-side delivery semantics live in signal_router_test.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/signal_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/tools/tool.hpp"

// The bundled plugin's tool factory -- built into this test binary directly
// (the checklist pattern: the .so is exercised by the loader tests; here the
// tool's BEHAVIOR is the subject).
#include "message_tool.hpp"

using namespace minimal;

namespace {

class ScriptedSignalService final : public agent::SignalService {
public:
    agent::SignalDelivery next = agent::SignalDelivery::kDelivered;
    std::string last_from, last_target, last_content;

    agent::SignalDelivery DeliverInternal(const std::string& from_session,
                                          const std::string& target_session,
                                          const std::string& content) override {
        last_from = from_session;
        last_target = target_session;
        last_content = content;
        return next;
    }

    // The external ingress is not what message_session uses.
    bool RegisterExternalChannel(const agent::ExternalChannel&, agent::ExternalChannelSource*,
                                 std::string&) override {
        return false;
    }
    void SetChannelInstructions(const std::string&, const std::string&) override {}
    agent::SignalDelivery DeliverExternal(const std::string&, const std::string&,
                                          const std::string&,
                                          const std::map<std::string, std::string>&) override {
        return agent::SignalDelivery::kInvalid;
    }
};

struct Fixture {
    ScriptedSignalService service;
    plugins::ServiceRegistry services;

    Fixture() { services.Provide(agent::kSignalServiceName, &service); }

    std::unique_ptr<tools::Tool> Tool() {
        return signals_message::MakeMessageSessionTool(&services);
    }

    tools::ToolContext Context(const std::string& session_id = "sender-1") {
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = session_id;
        return ctx;
    }
};

}  // namespace

TEST_CASE("message_session: delivered and not-subscribed are honest outcomes",
          "[signals][message_session]") {
    Fixture f;
    auto tool = f.Tool();
    auto ctx = f.Context();

    f.service.next = agent::SignalDelivery::kDelivered;
    const auto ok = tool->execute(nlohmann::json{{"target_session", "target-1"},
                                                 {"content", "build finished"}},
                                  ctx);
    CHECK_FALSE(ok.is_error);
    CHECK(ok.content_text.find("delivered to session target-1") != std::string::npos);
    CHECK(f.service.last_from == "sender-1");
    CHECK(f.service.last_target == "target-1");
    CHECK(f.service.last_content == "build finished");

    f.service.next = agent::SignalDelivery::kNoSubscriber;
    const auto missed = tool->execute(nlohmann::json{{"target_session", "target-1"},
                                                     {"content", "build finished"}},
                                      ctx);
    // NOT an error result: the miss is a real answer the model can act on.
    CHECK_FALSE(missed.is_error);
    CHECK(missed.content_text.find("NOT delivered") != std::string::npos);
    CHECK(missed.content_text.find("did not subscribe") != std::string::npos);
}

TEST_CASE("message_session: self-address is refused with the reason", "[signals][message_session]") {
    Fixture f;
    auto tool = f.Tool();
    auto ctx = f.Context("me");

    // The ROUTER refuses self-address (kInvalid); the scripted service stands
    // in for that verdict here.
    f.service.next = agent::SignalDelivery::kInvalid;
    const auto result = tool->execute(nlohmann::json{{"target_session", "me"},
                                                     {"content", "note to self"}},
                                      ctx);
    REQUIRE(result.is_error);
    CHECK(result.content_text.find("timer") != std::string::npos);

    // A kInvalid that is NOT self-address (e.g. empty content that slipped
    // past validation) degrades to the generic message.
    const auto other = tool->execute(nlohmann::json{{"target_session", "t"},
                                                    {"content", "x"}},
                                     ctx);
    CHECK(other.is_error);
    CHECK(other.content_text.find("timer") == std::string::npos);
}

TEST_CASE("message_session: input validation and the degraded host", "[signals][message_session]") {
    Fixture f;
    auto tool = f.Tool();
    auto ctx = f.Context();

    CHECK(tool->execute(nlohmann::json{{"content", "x"}}, ctx).is_error);
    CHECK(tool->execute(nlohmann::json{{"target_session", "t"}}, ctx).is_error);
    CHECK(tool->execute(nlohmann::json{{"target_session", ""}, {"content", "x"}}, ctx).is_error);

    // A host that offers no services: a clear error, never a crash.
    auto orphan = signals_message::MakeMessageSessionTool(nullptr);
    const auto degraded = orphan->execute(nlohmann::json{{"target_session", "t"},
                                                         {"content", "x"}},
                                          ctx);
    CHECK(degraded.is_error);
    CHECK(degraded.content_text.find("no services") != std::string::npos);

    // A host with a registry that does not provide core.signals: same story.
    plugins::ServiceRegistry empty_services;
    auto missing = signals_message::MakeMessageSessionTool(&empty_services);
    const auto absent = missing->execute(nlohmann::json{{"target_session", "t"},
                                                        {"content", "x"}},
                                         ctx);
    CHECK(absent.is_error);
    CHECK(absent.content_text.find("core.signals") != std::string::npos);
}

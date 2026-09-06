#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "fakes.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/tools/aliased_tool.hpp"

using namespace minimal;
using namespace minimal::testing;
using tools::ToolCategory;

TEST_CASE("AliasedTool rewrites only the name and the definition name", "[aliased_tool]") {
    auto inner = std::make_unique<FakeTool>("original", ToolCategory::FileWrite, "inner result");
    const auto* inner_ptr = inner.get();
    tools::AliasedTool wrapped(std::move(inner), "surface");

    CHECK(wrapped.spec().name == "surface");
    CHECK(wrapped.spec().definition.name == "surface");
    // Everything else is the inner spec, untouched.
    CHECK(wrapped.spec().category == ToolCategory::FileWrite);
    CHECK(wrapped.spec().definition.description == "fake tool");

    tools::ToolContext ctx(common::DefaultLogger());
    const auto result = wrapped.execute(nlohmann::json{{"k", "v"}}, ctx);
    CHECK(result.content_text == "inner result");
    REQUIRE(inner_ptr->calls.size() == 1);
    CHECK(inner_ptr->calls[0] == nlohmann::json{{"k", "v"}});
}

TEST_CASE("AliasedTool delegates the opt-in surface verbatim", "[aliased_tool]") {
    auto inner = std::make_unique<FakeTool>("original", ToolCategory::FileWrite);
    inner->set_path_key("path");
    tools::AliasedTool wrapped(std::move(inner), "surface");

    CHECK(wrapped.path_confinement_supported());
    CHECK(wrapped.ExtractPath(nlohmann::json{{"path", "/x"}}) == "/x");
}

TEST_CASE("AliasedTool forwards the declarative virtuals", "[aliased_tool]") {
    // network_access_required, parallel_safe and mode_override_for must reach
    // the inner tool: the Tool defaults through the wrapper would let an
    // aliased web_fetch skip the network permission fold and an aliased bash
    // lose its parallel-dispatch opt-out and race.
    struct DeclarativeTool : tools::Tool {
        const tools::ToolSpec& spec() const override { return spec_; }
        tools::ToolResult execute(const nlohmann::json&, tools::ToolContext&) override {
            return {false, "ok"};
        }
        bool network_access_required() const override { return true; }
        bool parallel_safe() const override { return false; }
        std::optional<tools::PermissionMode> mode_override_for(
            const nlohmann::json&, const tools::ToolContext&) const override {
            return tools::PermissionMode::AlwaysDeny;
        }
        tools::ToolSpec spec_{"inner", ToolCategory::Execute,
                              {"inner", "inner tool", nlohmann::json::object()}};
    };

    tools::AliasedTool wrapped(std::make_shared<DeclarativeTool>(), "surface");
    tools::ToolContext ctx(common::DefaultLogger());

    CHECK(wrapped.network_access_required());
    CHECK_FALSE(wrapped.parallel_safe());
    CHECK(wrapped.mode_override_for(nlohmann::json::object(), ctx) ==
          tools::PermissionMode::AlwaysDeny);
}

TEST_CASE("the default disclosure is restated under the surface name", "[aliased_tool]") {
    // A tool that does not override Disclose inherits the base default: its own
    // name, no grants, complete. Through the wrapper that must read as the
    // SURFACE name -- the Ask prompt should show what the model called it.
    auto inner = std::make_unique<FakeTool>("original", ToolCategory::Execute);
    tools::AliasedTool wrapped(std::move(inner), "surface");

    tools::ToolContext ctx(common::DefaultLogger());
    const auto disclosure = wrapped.Disclose(nlohmann::json::object(), ctx);
    CHECK(disclosure.summary == "surface");
    CHECK(disclosure.grants.empty());
    CHECK(disclosure.complete);
}

TEST_CASE("a real disclosure override passes through untouched", "[aliased_tool]") {
    auto inner = std::make_unique<FakeTool>("original", ToolCategory::Execute);
    tools::PermissionDisclosure custom;
    custom.summary = "bash: make 2>&1 | tail -50";
    custom.grants.push_back({"bash:/usr/bin/make", "/usr/bin/make *"});
    inner->set_disclosure(custom);
    tools::AliasedTool wrapped(std::move(inner), "ssh_bash");

    tools::ToolContext ctx(common::DefaultLogger());
    const auto disclosure = wrapped.Disclose(nlohmann::json::object(), ctx);
    CHECK(disclosure.summary == "bash: make 2>&1 | tail -50");
    REQUIRE(disclosure.grants.size() == 1);
    CHECK(disclosure.grants[0].key == "bash:/usr/bin/make");
}

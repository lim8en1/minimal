#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/context_contributor.hpp"
#include "minimal/common/logger.hpp"

using namespace minimal;

namespace {

class FixedContributor final : public agent::ContextContributor {
public:
    FixedContributor(std::string title, std::string body)
        : title_(std::move(title)), body_(std::move(body)) {}

    std::optional<agent::ContextSection> Contribute(const agent::ContextRequest& request) override {
        last_request = request;
        return agent::ContextSection{title_, body_};
    }

    std::optional<agent::ContextRequest> last_request;

private:
    std::string title_, body_;
};

class ThrowingContributor final : public agent::ContextContributor {
public:
    std::optional<agent::ContextSection> Contribute(const agent::ContextRequest&) override {
        throw std::runtime_error("boom");
    }
};

class SilentContributor final : public agent::ContextContributor {
public:
    std::optional<agent::ContextSection> Contribute(const agent::ContextRequest&) override {
        return std::nullopt;
    }
};

agent::ContextRequest Request() {
    agent::ContextRequest request;
    request.session_id = "s1";
    request.project_root = "/proj";
    request.preset_name = "build";
    return request;
}

}  // namespace

TEST_CASE("context contributors render in registration order under headings", "[context]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    registry.SetAttribution("alpha");
    registry.Register(std::make_shared<FixedContributor>("Alpha", "first"));
    registry.ClearAttribution();
    registry.SetAttribution("beta");
    registry.Register(std::make_shared<FixedContributor>("Beta", "second"));
    registry.ClearAttribution();

    CHECK(registry.Render(Request()) == "## Alpha\nfirst\n\n## Beta\nsecond");
}

TEST_CASE("context contributors: the request reaches the contributor", "[context]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    auto contributor = std::make_shared<FixedContributor>("T", "b");
    registry.Register(contributor);
    auto request = Request();
    request.is_subagent = true;
    request.resume = true;
    registry.Render(request);
    REQUIRE(contributor->last_request.has_value());
    CHECK(contributor->last_request->session_id == "s1");
    CHECK(contributor->last_request->preset_name == "build");
    CHECK(contributor->last_request->is_subagent);
    CHECK(contributor->last_request->resume);
}

TEST_CASE("context contributors: a throwing or silent contributor is skipped", "[context]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    registry.Register(std::make_shared<ThrowingContributor>());
    registry.Register(std::make_shared<SilentContributor>());
    registry.Register(std::make_shared<FixedContributor>("Kept", "body"));
    CHECK(registry.Render(Request()) == "## Kept\nbody");
}

TEST_CASE("context contributors: an empty registry renders nothing", "[context]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    CHECK(registry.Render(Request()).empty());
}

TEST_CASE("context contributors: disabled plugins are skipped by attribution", "[context]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    registry.SetAttribution("memory");
    registry.Register(std::make_shared<FixedContributor>("Memory", "index"));
    registry.ClearAttribution();
    registry.SetAttribution("other");
    registry.Register(std::make_shared<FixedContributor>("Other", "x"));
    registry.ClearAttribution();

    agent::ContextContributorSettings settings;
    settings.disabled = {"memory"};
    registry.Configure(settings);
    CHECK(registry.Render(Request()) == "## Other\nx");
}

TEST_CASE("context contributors: an over-budget body is cut at a UTF-8 boundary", "[context]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    // "é" is two bytes; a 5-byte cap lands inside the third one.
    registry.Register(std::make_shared<FixedContributor>("T", "\xC3\xA9\xC3\xA9\xC3\xA9"));
    agent::ContextContributorSettings settings;
    settings.max_bytes_per_section = 5;
    registry.Configure(settings);
    CHECK(registry.Render(Request()) == "## T\n\xC3\xA9\xC3\xA9\n[... section truncated]");
}

TEST_CASE("context contributors: RemoveByAttribution rolls back a failed plugin's registrations",
          "[context]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    registry.Register(std::make_shared<FixedContributor>("Keep", "k"));
    registry.SetAttribution("broken");
    registry.Register(std::make_shared<FixedContributor>("Drop", "d"));
    registry.ClearAttribution();
    CHECK(registry.CountOwnedBy("broken") == 1);
    registry.RemoveByAttribution("broken");
    CHECK(registry.count() == 1);
    CHECK(registry.Render(Request()) == "## Keep\nk");
}

namespace {

class Ranked final : public agent::ContextContributor {
public:
    Ranked(std::string body, int priority) : body_(std::move(body)), priority_(priority) {}
    std::optional<agent::ContextSection> Contribute(const agent::ContextRequest&) override {
        return agent::ContextSection{"", body_};
    }
    int priority() const override { return priority_; }

private:
    std::string body_;
    int priority_;
};

}  // namespace

TEST_CASE("context sections render by declared priority, 0 first, ties in registration order",
          "[context_contributor]") {
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    registry.Register(std::make_shared<Ranked>("late-a", 100));
    registry.Register(std::make_shared<Ranked>("first", 0));
    registry.Register(std::make_shared<Ranked>("late-b", 100));
    auto sections = registry.RenderSections(agent::ContextRequest{});
    // Core's own signal section slots in by its priority.
    sections.push_back({agent::kSignalContextPriority, "signals"});
    CHECK(agent::JoinContextSections(std::move(sections)) ==
          "first\n\nsignals\n\nlate-a\n\nlate-b");
}

TEST_CASE("a contributor's own cap replaces the generic one", "[context_contributor]") {
    class Big final : public agent::ContextContributor {
    public:
        std::optional<agent::ContextSection> Contribute(const agent::ContextRequest&) override {
            return agent::ContextSection{"", std::string(100, 'x')};
        }
        std::optional<std::int64_t> max_bytes() const override { return 0; }  // uncapped
    };
    agent::ContextContributorRegistry registry(common::DefaultLogger());
    config::ContextContributorSettings settings;
    settings.max_bytes_per_section = 10;
    registry.Configure(settings);
    registry.Register(std::make_shared<Big>());
    registry.Register(std::make_shared<Ranked>(std::string(100, 'y'), 100));
    const auto sections = registry.RenderSections(agent::ContextRequest{});
    REQUIRE(sections.size() == 2);
    CHECK(sections[0].text == std::string(100, 'x'));
    CHECK(sections[1].text.find("[... section truncated]") != std::string::npos);
}

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include "minimal/agent/tool_source_activation.hpp"

using namespace std::chrono_literals;
using minimal::agent::ActivateToolSources;
using minimal::tools::ToolSource;
using minimal::tools::ToolSourceRegistry;

namespace {

class FakeSource : public ToolSource {
public:
    enum class Mode { kOk, kFail, kThrow, kHang };
    explicit FakeSource(Mode mode, std::string hint = {}) : mode_(mode), hint_(std::move(hint)) {}
    std::vector<std::string> Members() const override { return {}; }
    std::string ReconnectHint() const override { return hint_; }
    std::optional<std::string> Activate(const std::string& member, std::string& error) override {
        switch (mode_) {
            case Mode::kOk: return "group-" + member;
            case Mode::kFail: error = "refused " + member; return std::nullopt;
            case Mode::kThrow: throw std::runtime_error("boom");
            case Mode::kHang: std::this_thread::sleep_for(400ms); return "late-" + member;
        }
        return std::nullopt;
    }

private:
    Mode mode_;
    std::string hint_;
};

void Add(ToolSourceRegistry& registry, const std::string& name, FakeSource::Mode mode) {
    registry.Register(name, std::make_shared<FakeSource>(mode));
}

}  // namespace

TEST_CASE("ActivateToolSources reports groups and failures per member", "[session][tool_sources]") {
    ToolSourceRegistry registry;
    Add(registry, "ok", FakeSource::Mode::kOk);
    Add(registry, "bad", FakeSource::Mode::kFail);
    Add(registry, "throws", FakeSource::Mode::kThrow);
    const std::map<std::string, std::vector<std::string>> wanted{
        {"bad", {"b"}}, {"missing", {"m"}}, {"ok", {"a1", "a2"}}, {"throws", {"t"}}};

    const auto activation = ActivateToolSources(wanted, &registry, 2s);

    REQUIRE(activation.groups == std::vector<std::string>{"group-a1", "group-a2"});
    REQUIRE(activation.failures.size() == 3);
    CHECK(activation.failures[0].source_name == "bad");
    CHECK(activation.failures[0].message == "refused b");
    CHECK(activation.failures[1].source_name == "missing");
    CHECK(activation.failures[1].message.find("not available") != std::string::npos);
    CHECK(activation.failures[2].message == "boom");
}

TEST_CASE("ActivateToolSources leaves out a member that misses the deadline",
          "[session][tool_sources]") {
    ToolSourceRegistry registry;
    Add(registry, "ok", FakeSource::Mode::kOk);
    Add(registry, "slow", FakeSource::Mode::kHang);
    const std::map<std::string, std::vector<std::string>> wanted{{"ok", {"a"}}, {"slow", {"s"}}};

    const auto started = std::chrono::steady_clock::now();
    const auto activation = ActivateToolSources(wanted, &registry, 100ms);

    CHECK(std::chrono::steady_clock::now() - started < 380ms);
    REQUIRE(activation.groups == std::vector<std::string>{"group-a"});
    REQUIRE(activation.failures.size() == 1);
    CHECK(activation.failures[0].member == "s");
    CHECK(activation.failures[0].message.find("did not become ready") != std::string::npos);
    // No hint from this source: core adds none of its own.
    CHECK(activation.failures[0].message.find("(") == std::string::npos);
}

TEST_CASE("a timed-out member's message carries its source's reconnect hint",
          "[session][tool_sources]") {
    ToolSourceRegistry registry;
    registry.Register("slow", std::make_shared<FakeSource>(FakeSource::Mode::kHang,
                                                           "reconnect it with /fake"));
    const auto activation = ActivateToolSources({{"slow", {"s"}}}, &registry, 50ms);
    REQUIRE(activation.failures.size() == 1);
    CHECK(activation.failures[0].message.find("(reconnect it with /fake)") != std::string::npos);
}

TEST_CASE("ActivateToolSources with no registry fails every named member",
          "[session][tool_sources]") {
    const auto activation = ActivateToolSources({{"mcp", {"x", "y"}}}, nullptr, 1s);
    CHECK(activation.groups.empty());
    CHECK(activation.failures.size() == 2);
}

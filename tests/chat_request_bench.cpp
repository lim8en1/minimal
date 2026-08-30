// How much does assembling a ChatRequest cost at the context sizes this project
// claims to handle?
//
// ARCHITECTURE.md's first goal is "512k-1M token conversation histories without
// holding full history in memory", and BuildChatRequest is called once per tool
// cycle and once per retry. So the per-call cost of the messages field is paid
// on the hottest path there is. DEVELOPMENT_PLAN.md section 6 asks for a number
// rather than an argument -- the bash rework was 8008 ms -> 0 ms, measured.
//
// Hidden by default (the leading dot in the tag): this reports rather than
// asserts, and a timing threshold in CI would be a flake generator. Run with
//   ./minimal_tests "[.bench]"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "minimal/agent/agent_loop.hpp"

using namespace minimal;
using namespace minimal::testing;

namespace {

// ~500k tokens at the bytes/4 estimate this codebase uses elsewhere: 2 MB of
// message text spread over a realistic number of turns.
constexpr int kMessages = 2000;
constexpr int kBytesPerMessage = 1000;

}  // namespace

TEST_CASE("cost of assembling a ChatRequest at 512k-token scale", "[.bench]") {
    FakeLlmProvider provider;
    tools::ToolRegistry registry;
    AlwaysAllowPermissionChecker checker;
    InMemoryHistoryStore store;
    tools::ToolExecutor executor(registry, checker, AllowAllPolicy(), tools::McpPermissionPolicy{},
                                 tools::PermissionRunMode::Normal);

    agent::AgentPreset preset;
    preset.allowed_tools = {"*"};
    preset.provider.model_name = "m";
    agent::SessionContext session{common::DefaultLogger()};
    session.session_id = "bench";

    agent::AgentLoop loop(provider, executor, registry, store, preset, agent::LoopConfig{},
                          std::move(session), [] { return false; },
                          [](const llm::StreamEvent&) { return true; }, {});

    // Fill history through the public path.
    const std::string filler(kBytesPerMessage, 'x');
    for (int i = 0; i < kMessages; ++i) {
        provider.PushResponse(TextResponse(filler));
    }
    for (int i = 0; i < kMessages / 2; ++i) {
        loop.RunTurn(UserText(filler));
    }
    const std::size_t history_bytes =
        static_cast<std::size_t>(kMessages) * static_cast<std::size_t>(kBytesPerMessage);

    constexpr int kCalls = 200;
    const auto started = std::chrono::steady_clock::now();
    std::size_t sink = 0;
    for (int i = 0; i < kCalls; ++i) {
        const auto request = loop.BuildChatRequest();
        sink += request.messages.size();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    UNSCOPED_INFO("history: " << loop.history().size() << " messages, ~" << history_bytes / 1024
                              << " KiB of text");
    UNSCOPED_INFO("BuildChatRequest x" << kCalls << ": " << us << " us total, "
                                       << (us / static_cast<double>(kCalls)) << " us/call");
    CHECK(sink > 0);
}

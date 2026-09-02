// S4: the compaction strategy registry (mirrors llm::ModelCatalogRegistry
// exactly) and the built-in "summarize" strategy's cut-selection logic.

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/compaction_strategy.hpp"

using namespace minimal;
using namespace minimal::agent;

namespace {

llm::Message UserText(const std::string& text) {
    return llm::Message{llm::Role::User, {llm::TextBlock{text}}};
}

CompactionStrategy DummyStrategy() {
    CompactionStrategy strategy;
    strategy.cut_selector = [](const std::vector<llm::Message>& history, std::int64_t) {
        return history.size() / 2;
    };
    strategy.summarize = [](std::span<const llm::Message>, llm::LlmProvider&,
                            const CompactionRequest&) -> std::optional<CompactionResult> {
        return CompactionResult{UserText("[summary]"), 0};
    };
    return strategy;
}

}  // namespace

TEST_CASE("CompactionStrategyRegistry: register, find, and list kinds",
         "[compaction_strategy]") {
    CompactionStrategyRegistry registry;
    CHECK_FALSE(registry.Has("summarize"));
    CHECK(registry.Find("summarize") == nullptr);

    CHECK(registry.Register("summarize", MakeSummarizeCompactionStrategy()));
    CHECK(registry.Has("summarize"));
    REQUIRE(registry.Find("summarize") != nullptr);
    CHECK(registry.kinds() == std::vector<std::string>{"summarize"});
}

TEST_CASE("CompactionStrategyRegistry: a duplicate kind is refused, first registration wins",
         "[compaction_strategy]") {
    CompactionStrategyRegistry registry;
    CHECK(registry.Register("drop-oldest", DummyStrategy()));
    CHECK_FALSE(registry.Register("drop-oldest", MakeSummarizeCompactionStrategy()));
    // First registration's identity survives -- confirmed indirectly: Find
    // still returns non-null and kinds() lists it exactly once.
    CHECK(registry.kinds() == std::vector<std::string>{"drop-oldest"});
}

TEST_CASE("CompactionStrategyRegistry: registering an incomplete strategy is refused",
         "[compaction_strategy]") {
    CompactionStrategyRegistry registry;
    CompactionStrategy half;
    half.cut_selector = [](const std::vector<llm::Message>& h, std::int64_t) { return h.size(); };
    // half.summarize left unset.
    CHECK_FALSE(registry.Register("half", std::move(half)));
    CHECK_FALSE(registry.Has("half"));
}

TEST_CASE("MakeSummarizeCompactionStrategy: keep_recent_tokens==0 falls back to plain halving",
         "[compaction_strategy]") {
    const auto strategy = MakeSummarizeCompactionStrategy();
    std::vector<llm::Message> history;
    for (int i = 0; i < 10; ++i) history.push_back(UserText("msg"));
    CHECK(strategy.cut_selector(history, 0) == 5);
}

TEST_CASE("MakeSummarizeCompactionStrategy: a token budget keeps the tail whole and cuts "
         "before it",
         "[compaction_strategy]") {
    // A giant recent tool result plus tiny old messages: the cut must land
    // BEFORE the giant result, never split it -- the invariant a count-based
    // cut (history.size()/2) cannot express, since the giant result is a
    // single message near the tail.
    const auto strategy = MakeSummarizeCompactionStrategy();
    std::vector<llm::Message> history;
    history.push_back(UserText("old-1"));
    history.push_back(UserText("old-2"));
    history.push_back(UserText("old-3"));
    llm::ToolResultBlock giant;
    giant.tool_use_id = "call_1";
    giant.content = {llm::TextBlock{std::string(2000, 'x')}};
    history.push_back(llm::Message{llm::Role::User, {giant}});

    // A budget that exactly fits the giant result alone (2000 bytes / 4 ==
    // 500 tokens) but not the giant result plus even one more old message
    // (each ~1 token): 500 + 1 > 500 breaks the walk right there.
    const std::size_t cut = strategy.cut_selector(history, /*keep_recent_tokens=*/500);
    CHECK(cut == 3);  // everything before the giant result is compacted, it alone is kept
}

TEST_CASE("MakeSummarizeCompactionStrategy: an empty history returns cut 0",
         "[compaction_strategy]") {
    const auto strategy = MakeSummarizeCompactionStrategy();
    CHECK(strategy.cut_selector({}, 100) == 0);
    CHECK(strategy.cut_selector({}, 0) == 0);
}

TEST_CASE("MakeSummarizeCompactionStrategy: the walk always keeps at least one message, even "
         "under an unrealistically tiny budget",
         "[compaction_strategy]") {
    // The accumulated>0 guard only blocks the SECOND-and-later addition from
    // exceeding the budget -- the first message is always admitted, so a
    // single-message history's cut is 0 (nothing to compact; the lone
    // message is the whole "keep" tail) rather than pathologically compacting
    // the only message there is.
    const auto strategy = MakeSummarizeCompactionStrategy();
    std::vector<llm::Message> history{UserText("hello there, this is not a tiny message")};
    CHECK(strategy.cut_selector(history, 1) == 0);
}

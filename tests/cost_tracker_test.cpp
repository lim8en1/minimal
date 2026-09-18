// cost-tracker: rate resolution, cost arithmetic, aggregation, the storage
// wrapper, the observer, /cost and the .rpc service. Compiled directly into
// the test binary like every other bundled plugin's pure logic (plugin.cpp,
// the extern "C" entry points, stays out -- the loader tests cover that shape).

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "commands_core_fixture.hpp"
#include "cost_command.hpp"
#include "cost_observer.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"
#include "cost_tracking_service_impl.hpp"
#include "minimal/plugins/plugin_storage.hpp"
#include "temp_dir.hpp"

// tests/ sits beside the include dirs of EVERY bundled plugin, and checklist
// also ships an rpc_service.hpp -- quoted lookup finds that one first. This
// plugin's own headers are reached the same way commands_core_fixture.hpp
// reaches its plugin: a relative path.
#include "../plugins/server/cost-tracker/rpc_service.hpp"
#include "../plugins/server/cost-tracker/rates_fetch.hpp"
#include "../plugins/server/cost-tracker/rates_refresh.hpp"

using namespace minimal;
using namespace minimal::cost_tracker;
using Status = commands::CommandResult::Status;

namespace {

struct Harness {
    minimal::testing::TempDir dir;
    plugins::PluginStorage storage{dir.path() / "plugin_storage.db"};
    CostStore store{&storage};
    InFlightUsage in_flight;
};

TurnRecord Turn(const std::string& model, std::int64_t in, std::int64_t out,
                std::int64_t cr = 0, std::int64_t cw = 0, bool reported = true,
                std::int64_t first_seq = -1, std::int64_t last_seq = -1) {
    TurnRecord record;
    record.model = model;
    record.at_unix_ms = 0;
    record.usage = UsageRecord{in, out, cr, cw, reported};
    record.first_seq = first_seq;
    record.last_seq = last_seq;
    return record;
}

}  // namespace

TEST_CASE("rate table parsing keeps usable rows and skips junk", "[cost_tracker]") {
    const nlohmann::json table = nlohmann::json::parse(R"({
        "gpt-4o": {"input": 2.5, "output": 10.0, "cache_read": 1.25, "cache_write": 0},
        "cacheless": {"input": 1.0, "output": 2.0},
        "negative": {"input": -1.0, "output": 2.0},
        "partial": {"input": "free", "output": 2.0},
        "not-object": 7
    })");
    const auto rates = ParseRateTable(table);

    // not-object (7) is skipped; the other four survive.
    REQUIRE(rates.size() == 4);
    CHECK(rates.at("gpt-4o").output_per_mtok == 10.0);
    CHECK(rates.at("gpt-4o").cache_read_per_mtok == 1.25);
    // Absent fields are zero -- correct for a model without cache pricing.
    CHECK(rates.at("cacheless").cache_read_per_mtok == 0.0);
    // A negative price and a non-numeric input are absent, not fatal: one bad
    // row must not lose every other model's price.
    CHECK(rates.at("negative").input_per_mtok == 0.0);
    CHECK(rates.at("partial").input_per_mtok == 0.0);
    CHECK(rates.at("partial").output_per_mtok == 2.0);
}

TEST_CASE("LiteLLM ingestion converts per-token to per-mtok and skips the rest",
          "[cost_tracker]") {
    const nlohmann::json document = nlohmann::json::parse(R"({
        "sample_spec": {"max_tokens": 8192},
        "some-model": {"input_cost_per_token": 0.0000025, "output_cost_per_token": 0.00001,
                       "cache_read_input_token_cost": 0.00000125,
                       "cache_creation_input_token_cost": 0.000003125},
        "embedding-only": {"input_cost_per_token": 0.0000001},
        "image-only": {"input_cost_per_image": 3.0}
    })");
    const auto rates = IngestLiteLlmPrices(document);

    REQUIRE(rates.size() == 1);
    // Per-token 0.0000025 == $2.50 per million tokens.
    CHECK(rates.at("some-model").input_per_mtok == 2.5);
    CHECK(rates.at("some-model").output_per_mtok == 10.0);
    CHECK(rates.at("some-model").cache_read_per_mtok == 1.25);
    CHECK(rates.at("some-model").cache_write_per_mtok == 3.125);
}

TEST_CASE("OpenRouter ingestion parses string pricing and skips the rest", "[cost_tracker]") {
    const nlohmann::json document = nlohmann::json::parse(R"({
        "data": [
            {"id": "z-ai/glm-5.3-flash",
             "pricing": {"prompt": "0.00000015", "completion": "0.0000005",
                         "input_cache_read": "0.00000003"}},
            {"id": "free-model", "pricing": {"prompt": "0", "completion": "0"}},
            {"id": "numeric-values", "pricing": {"prompt": 0.0000025, "completion": 0.00001}},
            {"id": "missing-completion", "pricing": {"prompt": "0.1"}},
            {"id": "junk-price", "pricing": {"prompt": "ask", "completion": "0.1"}},
            {"id": "trailing-junk", "pricing": {"prompt": "0.1 USD", "completion": "0.2"}},
            {"id": "negative", "pricing": {"prompt": "-1", "completion": "0.2"}},
            {"id": "no-pricing"},
            {"id": "", "pricing": {"prompt": "0.1", "completion": "0.2"}}
        ],
        "total_count": 9
    })");
    const auto rates = IngestOpenRouterPrices(document);

    REQUIRE(rates.size() == 3);
    // Per-token 0.00000015 == $0.15 per million tokens.
    CHECK(rates.at("z-ai/glm-5.3-flash").input_per_mtok == 0.15);
    CHECK(rates.at("z-ai/glm-5.3-flash").output_per_mtok == 0.5);
    CHECK(rates.at("z-ai/glm-5.3-flash").cache_read_per_mtok == 0.03);
    // A free tier is a genuine $0 rate (usage still reported, so priced).
    CHECK(rates.at("free-model").input_per_mtok == 0.0);
    CHECK(rates.at("numeric-values").input_per_mtok == 2.5);
    CHECK(rates.at("numeric-values").output_per_mtok == 10.0);

    // No "data" array (or a non-array one) is simply an empty table.
    CHECK(IngestOpenRouterPrices(nlohmann::json::parse(R"({"error": "x"})")).empty());
}

TEST_CASE("fetched-rates blob round-trips and rejects corruption", "[cost_tracker]") {
    FetchedRates rates;
    rates.fetched_at = "2026-08-31T00:00:00Z";
    rates.fetched_at_unix = 1780000000;
    rates.models["m1"] = Rate{1.0, 2.0, 0.5, 0.0};
    const auto decoded = FetchedFromJson(FetchedToJson(rates).dump());

    REQUIRE(decoded);
    CHECK(decoded->fetched_at == "2026-08-31T00:00:00Z");
    CHECK(decoded->fetched_at_unix == 1780000000);
    REQUIRE(decoded->models.count("m1") == 1);
    CHECK(decoded->models.at("m1").output_per_mtok == 2.0);

    CHECK(!FetchedFromJson("not json"));
    CHECK(!FetchedFromJson(R"([])"));
    CHECK(!FetchedFromJson(R"({"fetched_at": "x"})"));

    // The machine timestamp is optional: a blob written before it existed
    // decodes with the struct's 0 -- which the staleness gate reads as stale,
    // so an old cache always triggers one refresh rather than pinning.
    const auto legacy = FetchedFromJson(
        R"({"fetched_at": "2026-08-31T00:00:00Z", "models": {"m1": {"input": 1.0, "output": 2.0}}})");
    REQUIRE(legacy);
    CHECK(legacy->fetched_at_unix == 0);
    CHECK(legacy->models.at("m1").input_per_mtok == 1.0);
}

TEST_CASE("cost arithmetic bills every tier and refuses unreported usage",
          "[cost_tracker][invariant]") {
    const Rate rate{2.0, 8.0, 0.5, 12.5};
    // 1M in, 0.5M out, 0.25M cache-read, 0.1M cache-write:
    CHECK(CostFor(rate, UsageRecord{1000000, 500000, 250000, 100000, true}) == 2.0 + 4.0 + 0.125 + 1.25);
    // THE HONESTY RULE: all-zero reported usage is the known proxy gap -- a
    // missing price is not a price of zero.
    CHECK(CostFor(rate, UsageRecord{0, 0, 0, 0, false}) == 0.0);
}

TEST_CASE("aggregation groups per model and keeps unpriced turns honest",
          "[cost_tracker][invariant]") {
    const std::vector<TurnRecord> turns = {
        Turn("m1", 1000000, 0),             // priced: $2
        Turn("m1", 1000000, 0),             // priced: $2
        Turn("m2", 1000000, 1000000),       // no rate: unpriced
        Turn("m3", 1000000, 0, 0, 0, false) // unreported: unpriced
    };
    const auto totals = Aggregate("s1", "", turns, [](const std::string& model) {
        return model == "m1" ? std::optional<Rate>{Rate{2.0, 8.0}} : std::nullopt;
    });

    REQUIRE(totals.models.size() == 3);
    CHECK(totals.models[0].model == "m1");
    CHECK(totals.models[0].turns == 2);
    CHECK(totals.models[0].cost == 4.0);
    CHECK(totals.models[0].cost_known);
    CHECK(totals.models[1].model == "m2");
    CHECK(totals.models[1].unpriced_turns == 1);
    CHECK(!totals.models[1].cost_known);
    CHECK(totals.models[2].turns == 1);
    CHECK(totals.models[2].unpriced_turns == 1);
    // Total cost covers priced turns only; the flag says the number is partial.
    CHECK(totals.total_cost == 4.0);
    CHECK(!totals.total_cost_known);
    CHECK(totals.total_turns == 4);
    CHECK(totals.unpriced_turns == 2);
}

TEST_CASE("switching models mid-session sums re-visited and distinct models correctly",
          "[cost_tracker][invariant]") {
    // by_model is keyed by model id, not by "the current model" or a
    // contiguous run -- so a session that goes cloud(A) -> local(B) ->
    // cloud(A) again must fold both A runs into ONE bucket (not lose the
    // first), and cloud(A) -> local(B) -> cloud(C) must keep A and C as
    // separate priced buckets that both contribute to total_cost. Order
    // and interleaving must not matter.
    const std::vector<TurnRecord> revisited = {
        Turn("cloud-a", 1000000, 0),               // priced: $2
        Turn("local-b", 1000000, 1000000, 0, 0, false),  // unreported: unpriced
        Turn("cloud-a", 1000000, 0),               // priced: $2, SAME model again
    };
    const auto resolve_a_only = [](const std::string& model) {
        return model == "cloud-a" ? std::optional<Rate>{Rate{2.0, 8.0}} : std::nullopt;
    };
    const auto revisited_totals = Aggregate("s1", "", revisited, resolve_a_only);
    REQUIRE(revisited_totals.models.size() == 2);  // one bucket per DISTINCT model, not per run
    const auto& a_bucket = revisited_totals.models[0].model == "cloud-a" ? revisited_totals.models[0]
                                                                         : revisited_totals.models[1];
    CHECK(a_bucket.turns == 2);
    CHECK(a_bucket.cost == 4.0);   // both runs' $2 summed into the one bucket
    CHECK(a_bucket.cost_known);
    CHECK(revisited_totals.total_cost == 4.0);
    CHECK_FALSE(revisited_totals.total_cost_known);  // local-b's unpriced turn still taints it

    const std::vector<TurnRecord> two_clouds = {
        Turn("cloud-a", 1000000, 0),                     // priced: $2
        Turn("local-b", 1000000, 1000000, 0, 0, false),  // unreported: unpriced
        Turn("cloud-c", 1000000, 0),                     // priced: $3, a DIFFERENT model
    };
    const auto resolve_a_and_c = [](const std::string& model) -> std::optional<Rate> {
        if (model == "cloud-a") return Rate{2.0, 8.0};
        if (model == "cloud-c") return Rate{3.0, 8.0};
        return std::nullopt;
    };
    const auto two_cloud_totals = Aggregate("s1", "", two_clouds, resolve_a_and_c);
    REQUIRE(two_cloud_totals.models.size() == 3);
    // total_cost is A's $2 + C's $3, regardless of the unpriced leg between them.
    CHECK(two_cloud_totals.total_cost == 5.0);
    CHECK_FALSE(two_cloud_totals.total_cost_known);
}

TEST_CASE("rate resolution is config > openrouter > litellm > bundled",
          "[cost_tracker][invariant]") {
    Harness harness;
    harness.store.SetFetchedRates(FetchedRates{
        "t", 0, {{"m", Rate{10.0, 10.0}}, {"only-litellm", Rate{7.0, 7.0}}}});
    harness.store.SetOpenRouterFetchedRates(FetchedRates{
        "t", 0, {{"m", Rate{20.0, 20.0}}, {"only-openrouter", Rate{30.0, 30.0}}}});

    RateSources sources;
    sources.bundled_models = nlohmann::json::parse(R"({"m": {"input": 1.0, "output": 1.0}})");
    sources.config_rates = nlohmann::json::parse(R"({"m": {"input": 100.0, "output": 100.0}})");
    RateTable table(sources, &harness.storage);
    table.RefreshFetched();

    REQUIRE(table.Resolve("m"));
    CHECK(table.Resolve("m")->input_per_mtok == 100.0);
    // A slug both snapshots carry resolves to OpenRouter's own price.
    sources.config_rates = nlohmann::json::object();
    RateTable no_config(sources, &harness.storage);
    no_config.RefreshFetched();
    REQUIRE(no_config.Resolve("m"));
    CHECK(no_config.Resolve("m")->input_per_mtok == 20.0);
    REQUIRE(no_config.Resolve("only-openrouter"));
    CHECK(no_config.Resolve("only-openrouter")->input_per_mtok == 30.0);
    REQUIRE(no_config.Resolve("only-litellm"));
    CHECK(no_config.Resolve("only-litellm")->input_per_mtok == 7.0);
    CHECK(!no_config.Resolve("absolutely-unknown"));

    // The store write a later process makes is picked up by RefreshFetched --
    // the snapshot outranking the stale bundled copy is what makes fetching
    // useful without a restart.
    sources.config_rates = nlohmann::json::object();
    RateTable reload(sources, &harness.storage);
    reload.RefreshFetched();
    REQUIRE(reload.Resolve("m"));
    CHECK(reload.Resolve("m")->input_per_mtok == 20.0);

    // One corrupt snapshot blob clears only ITS tier: the LiteLLM data must
    // survive a corrupt OpenRouter blob (and vice versa).
    harness.storage.Set("cost-tracker", "rates/fetched_openrouter", "{not json");
    RateTable with_corrupt(sources, &harness.storage);
    with_corrupt.RefreshFetched();
    REQUIRE(with_corrupt.Resolve("only-litellm"));
    CHECK(with_corrupt.Resolve("only-litellm")->input_per_mtok == 7.0);
    CHECK(!with_corrupt.Resolve("only-openrouter"));
}

TEST_CASE("the store round-trips history and pins the parent", "[cost_tracker]") {
    Harness harness;
    harness.store.AppendTurn("s1", "root", Turn("m1", 10, 5, 2, 1, true));
    harness.store.AppendTurn("s1", "root", Turn("m1", 0, 0, 0, 0, false));

    const auto history = harness.store.Load("s1");
    CHECK(history.parent_session_id == "root");
    REQUIRE(history.turns.size() == 2);
    CHECK(history.turns[0].usage.input_tokens == 10);
    CHECK(history.turns[0].usage.cache_write_tokens == 1);
    CHECK(history.turns[1].usage.usage_reported == false);

    // The parent is pinned by the first turn: SessionInfo repeats it, and the
    // blob must not flip-flop if a later payload ever carried a different one.
    harness.store.AppendTurn("s1", "different", Turn("m1", 1, 1));
    CHECK(harness.store.Load("s1").parent_session_id == "root");

    const auto ids = harness.store.SessionIds();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "s1");

    CHECK(harness.store.Load("missing").turns.empty());
}

TEST_CASE("the store degrades with null storage and throws on corruption",
          "[cost_tracker]") {
    CostStore orphan(nullptr);
    CHECK(!orphan.available());
    // A no-op append, not a crash: recording must survive a host without the
    // storage seam.
    orphan.AppendTurn("s1", "", Turn("m", 1, 1));
    CHECK(orphan.Load("s1").turns.empty());
    CHECK(orphan.SessionIds().empty());
    CHECK(!orphan.GetFetchedRates());

    Harness harness;
    harness.storage.Set("cost-tracker", "sessions/s1", "{not json");
    CHECK_THROWS_AS(harness.store.Load("s1"), std::runtime_error);
}

TEST_CASE("negative token counts in a stored blob clamp to zero on decode",
          "[cost_tracker][invariant]") {
    // DecodeTurn accepted any integer: a corrupt (or hostile) blob with
    // negative tokens poisoned the totals. Fields clamp at zero on decode.
    Harness harness;
    harness.storage.Set(
        "cost-tracker", "sessions/s1",
        R"({"turns":[{"model":"m","in":-500,"out":-10,"cr":-1,"cw":-1,"reported":true}]})");
    const auto history = harness.store.Load("s1");
    REQUIRE(history.turns.size() == 1);
    CHECK(history.turns[0].usage.input_tokens == 0);
    CHECK(history.turns[0].usage.output_tokens == 0);
    CHECK(history.turns[0].usage.cache_read_tokens == 0);
    CHECK(history.turns[0].usage.cache_write_tokens == 0);
}

TEST_CASE("AppendTurn refuses an empty session id instead of storing under a degenerate key",
          "[cost_tracker][invariant]") {
    Harness harness;
    CHECK_THROWS_AS(harness.store.AppendTurn("", "", Turn("m", 1, 1)), std::runtime_error);
    CHECK(harness.store.SessionIds().empty());
}

TEST_CASE("the observer records turns with zero-usage honesty", "[cost_tracker]") {
    Harness harness;
    CostSessionObserver observer(harness.store, harness.in_flight);

    agent::SessionInfo info;
    info.session_id = "s1";
    info.parent_session_id = "root";

    agent::TurnSummary good;
    good.model = "gpt-4o";
    // The turn's ACROSS-rounds sum is what gets priced, not the last round's
    // occupancy-shaped count.
    good.turn_usage = llm::Usage{100, 50, 20, 10};
    observer.OnTurnCompleted(info, good);

    agent::TurnSummary failed;  // StopReason::Unknown, zero usage: the failure shape
    failed.model = "gpt-4o";
    observer.OnTurnCompleted(info, failed);

    const auto history = harness.store.Load("s1");
    REQUIRE(history.parent_session_id == "root");
    REQUIRE(history.turns.size() == 2);
    CHECK(history.turns[0].model == "gpt-4o");
    CHECK(history.turns[0].usage.input_tokens == 100);
    CHECK(history.turns[0].usage.usage_reported);
    CHECK(!history.turns[1].usage.usage_reported);
}

TEST_CASE("the observer prices the whole turn, not the last round", "[cost_tracker]") {
    // A multi-round turn bills every round -- each is a separate provider
    // request. TurnSummary::usage is the LAST round's count (occupancy
    // semantics); pricing that would drop rounds 1..n-1 from the bill.
    Harness harness;
    CostSessionObserver observer(harness.store, harness.in_flight);

    agent::SessionInfo info;
    info.session_id = "s1";
    agent::TurnSummary multi;
    multi.model = "gpt-4o";
    multi.usage = llm::Usage{3000, 30, 0, 0};  // the last round only
    multi.turn_usage = llm::Usage{1000 + 2000 + 3000, 10 + 20 + 30, 0, 0};
    observer.OnTurnCompleted(info, multi);

    const auto history = harness.store.Load("s1");
    REQUIRE(history.turns.size() == 1);
    CHECK(history.turns[0].usage.input_tokens == 6000);
    CHECK(history.turns[0].usage.output_tokens == 60);
    CHECK(history.turns[0].usage.usage_reported);
}

TEST_CASE("a turn with any unreported round prices as unreported", "[cost_tracker][invariant]") {
    // The zero-usage fallback mirrors its bytes/4 estimate into the usage
    // fields so the status bar can render; an estimated ROUND makes the
    // turn's summed count a lower bound. Billing the reported rounds as if
    // they were whole would understate silently -- the whole turn goes
    // unpriced instead.
    Harness harness;
    CostSessionObserver observer(harness.store, harness.in_flight);

    agent::SessionInfo info;
    info.session_id = "s1";

    agent::TurnSummary partial;
    partial.model = "gpt-4o";
    partial.turn_usage = llm::Usage{1234, 0, 0, 0};  // only the reported rounds' sum
    partial.turn_usage_fully_reported = false;
    observer.OnTurnCompleted(info, partial);

    const auto history = harness.store.Load("s1");
    REQUIRE(history.turns.size() == 1);
    CHECK(!history.turns[0].usage.usage_reported);
    const auto totals = Aggregate("s1", "", history.turns, [](const std::string&) {
        return std::optional<Rate>{Rate{2.0, 8.0}};
    });
    CHECK(totals.models[0].unpriced_turns == 1);
    CHECK(totals.models[0].cost == 0.0);
    CHECK(!totals.models[0].cost_known);
}

TEST_CASE("turn records carry transcript seq bounds; old blobs decode with the sentinel",
          "[cost_tracker]") {
    Harness harness;
    harness.store.AppendTurn("s1", "root", Turn("m1", 10, 5, 0, 0, true, /*first_seq=*/1,
                                                /*last_seq=*/1));
    harness.store.AppendTurn("s1", "root", Turn("m1", 1, 1, 0, 0, true, 3, 5));

    const auto history = harness.store.Load("s1");
    REQUIRE(history.turns.size() == 2);
    CHECK(history.turns[0].first_seq == 1);
    CHECK(history.turns[0].last_seq == 1);
    CHECK(history.turns[1].last_seq == 5);

    // An OLD-SCHEMA blob, written before the seq bounds existed: tolerant
    // decode yields the -1 sentinel, which is what keeps such records from
    // ever being cut on a rewind.
    harness.storage.Set("cost-tracker", "sessions/old",
                        R"({"parent_session_id": "root", "turns": [)"
                        R"({"model": "m", "at": 0, "in": 1, "out": 1, "cr": 0, "cw": 0, )"
                        R"("reported": true}]})");
    const auto legacy = harness.store.Load("old");
    REQUIRE(legacy.turns.size() == 1);
    CHECK(legacy.turns[0].usage.input_tokens == 1);
    CHECK(legacy.turns[0].first_seq == -1);
    CHECK(legacy.turns[0].last_seq == -1);
}

TEST_CASE("rewind removes the records past the cut; redo and unclassifiable records stay",
          "[cost_tracker][invariant]") {
    // /rewind + retry used to double-count: the discarded turns stayed in the
    // history blob and the re-executed turns appended on top.
    Harness harness;
    CostSessionObserver observer(harness.store, harness.in_flight);
    agent::SessionInfo info;
    info.session_id = "s1";

    auto completed = [&observer, &info](std::int64_t first, std::int64_t last) {
        agent::TurnSummary summary;
        summary.model = "m1";
        summary.first_seq = first;
        summary.last_seq = last;
        observer.OnTurnCompleted(info, summary);
    };
    completed(1, 1);
    completed(3, 3);
    completed(5, 7);

    // Redo (-1) is a no-op: which records the restored tail maps to is
    // unknowable here.
    observer.OnSessionRewound(info, -1);
    CHECK(harness.store.Load("s1").turns.size() == 3);

    // Rewind to seq 3: the cut lands ON the target user message (RunRewind
    // marks rows with seq >= that discarded), so the turn starting there and
    // everything after are gone; the earlier turn stays.
    observer.OnSessionRewound(info, 3);
    const auto history = harness.store.Load("s1");
    REQUIRE(history.turns.size() == 1);
    CHECK(history.turns[0].last_seq == 1);

    // A record without seq bounds (old blob) cannot be classified: never cut.
    SessionHistory mixed;
    mixed.turns.push_back(Turn("m", 1, 1));  // sentinel seqs
    mixed.turns.push_back(Turn("m", 1, 1, 0, 0, true, 4, 4));
    harness.store.Save("s2", mixed);
    agent::SessionInfo other;
    other.session_id = "s2";
    observer.OnSessionRewound(other, 4);
    const auto survivors = harness.store.Load("s2").turns;
    REQUIRE(survivors.size() == 1);
    CHECK(survivors[0].last_seq == -1);
}

TEST_CASE("plugin settings parse from the config slice with defaults",
          "[cost_tracker]") {
    const auto defaults = ParseCostSettings(nlohmann::json::object());
    CHECK(!defaults.rates_fetch_url.empty());
    CHECK(!defaults.openrouter_rates_fetch_url.empty());
    CHECK(defaults.rates_fetch_timeout_ms == 30000);

    const auto custom = ParseCostSettings(nlohmann::json::parse(
        R"({"rates_fetch_url": "http://mirror/x.json", "rates_fetch_timeout_ms": 5, )"
        R"("openrouter_rates_fetch_url": "http://mirror/or.json"})"));
    CHECK(custom.rates_fetch_url == "http://mirror/x.json");
    CHECK(custom.openrouter_rates_fetch_url == "http://mirror/or.json");
    CHECK(custom.rates_fetch_timeout_ms == 5);

    // Junk values keep defaults rather than poisoning the fetch.
    const auto junk = ParseCostSettings(nlohmann::json::parse(
        R"({"rates_fetch_url": 3, "openrouter_rates_fetch_url": [], "rates_fetch_timeout_ms": -1})"));
    CHECK(junk.rates_fetch_url == defaults.rates_fetch_url);
    CHECK(junk.openrouter_rates_fetch_url == defaults.openrouter_rates_fetch_url);
    CHECK(junk.rates_fetch_timeout_ms == 30000);
}

TEST_CASE("/cost reports a session's totals and its unpriced turns",
          "[cost_tracker][commands]") {
    Harness harness;
    RateSources sources;
    sources.config_rates = nlohmann::json::parse(R"({"m1": {"input": 2.0, "output": 8.0}})");
    RateTable rates(sources, &harness.storage);
    harness.store.AppendTurn("s1", "", Turn("m1", 1000000, 500000));
    harness.store.AppendTurn("s1", "", Turn("mystery", 1, 1));

    testing::CommandFixture fixture;
    fixture.registry.Register(
        std::make_unique<CostCommand>(harness.store, rates));

    const auto result = fixture.Run("cost");
    REQUIRE(result.status == Status::kOk);
    CHECK(result.message.find("m1") != std::string::npos);
    // 1M in @ $2 + 0.5M out @ $8 = $6.
    CHECK(result.message.find("$6.0000") != std::string::npos);
    CHECK(result.message.find("mystery") != std::string::npos);
    CHECK(result.message.find("unpriced") != std::string::npos);
}

TEST_CASE("the /cost table sizes columns to content and never fakes a price",
          "[cost_tracker][commands]") {
    // Dynamic widths: a long model id must not push the later columns out of
    // alignment (the fixed setw(19) failure this replaced).
    SessionTotals one_model;
    ModelTotals priced;
    priced.model = "long-model-name";
    priced.turns = 2;
    priced.input_tokens = 1'000'000;
    priced.output_tokens = 0;
    priced.cost = 2.0;  // 1M in @ $2/Mtok
    one_model.models.push_back(priced);
    one_model.total_turns = 2;
    one_model.total_cost = 2.0;
    const auto text = FormatSessionTotals(one_model);
    const auto lines = [&] {
        std::vector<std::string> out;
        std::string current;
        for (const char c : text) {
            if (c == '\n') {
                out.push_back(std::exchange(current, {}));
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) out.push_back(current);
        return out;
    }();
    REQUIRE(lines.size() == 3);
    // Expected rows built piecewise (widths are content-derived: model 15,
    // turns 5, in 9, out 3, cost 7) so the spaces cannot be miscounted.
    const auto pad = [](std::string text, const std::size_t width, const bool right) {
        text.insert(right ? text.begin() : text.end(), width - text.size(), ' ');
        return text;
    };
    const std::string sep = "  ";
    CHECK(lines[0] == "  " + pad("model", 15, false) + sep + pad("turns", 5, true) + sep +
                          pad("in", 9, true) + sep + pad("out", 3, true) + sep +
                          pad("cost", 7, true));
    CHECK(lines[1] == "  " + pad("long-model-name", 15, false) + sep + pad("2", 5, true) + sep +
                          pad("1,000,000", 9, true) + sep + pad("0", 3, true) + sep +
                          pad("$2.0000", 7, true));
    CHECK(lines[2] == "  total 2 turn(s), est. $2.0000");

    // A model id longer than the header widens the column, and every row
    // still pads to the same width (the shorter id's row pads the model
    // cell, the long one fills it).
    SessionTotals two_models;
    ModelTotals short_row = priced;
    short_row.model = "m2";
    two_models.models.push_back(short_row);
    two_models.models.push_back(priced);
    two_models.total_turns = 4;
    two_models.total_cost = 4.0;
    const auto two_text = FormatSessionTotals(two_models);
    CHECK(two_text.find("  " + pad("m2", 15, false) + sep + pad("2", 5, true) + sep +
                        pad("1,000,000", 9, true) + sep + pad("0", 3, true) + sep +
                        pad("$2.0000", 7, true) + "\n") != std::string::npos);

    // THE HONESTY RULE: an unpriced row renders "?", never "$0.0000" and
    // never the old "+" suffix -- a missing price is not a price of zero.
    SessionTotals unknown;
    ModelTotals unpriced = priced;
    unpriced.cost = 0.0;
    unpriced.cost_known = false;
    unpriced.unpriced_turns = 1;
    unknown.models.push_back(unpriced);
    unknown.total_turns = 1;
    unknown.total_cost_known = false;
    unknown.unpriced_turns = 1;
    const auto unknown_text = FormatSessionTotals(unknown);
    CHECK(unknown_text.find("?") != std::string::npos);
    CHECK(unknown_text.find("$0.0000") == std::string::npos);
    CHECK(unknown_text.find('+') == std::string::npos);
    CHECK(unknown_text.find(
              "(1 turn(s) unpriced -- prices refresh at server start; refresh now with "
              "minimal-server --refresh-cost-rates)") != std::string::npos);

    // Partially priced: the priced portion is shown as a floor, ">= $X".
    SessionTotals partial = unknown;
    partial.models.front().cost = 6.0;
    partial.total_cost = 6.0;
    const auto partial_text = FormatSessionTotals(partial);
    CHECK(partial_text.find(">= $6.0000") != std::string::npos);
    CHECK(partial_text.find("$0.0000") == std::string::npos);

    // An all-zero cache column is noise (OpenAI-shaped backends): dropped.
    CHECK(text.find("cache r/w") == std::string::npos);
    // Present again as soon as any cache tokens exist.
    SessionTotals cached = one_model;
    cached.models.front().cache_read_tokens = 17'536;
    CHECK(FormatSessionTotals(cached).find("cache r/w") != std::string::npos);
}

TEST_CASE("/cost handles the empty and degraded cases", "[cost_tracker][commands]") {
    Harness harness;
    RateTable rates(RateSources{}, &harness.storage);
    testing::CommandFixture fixture;
    fixture.registry.Register(std::make_unique<CostCommand>(harness.store, rates));

    CHECK(fixture.Run("cost").message.find("no recorded cost") != std::string::npos);

    // A separate fixture: a second /cost on the same registry would be a name
    // collision, the failure mode the startup check exists to catch.
    CostStore orphan(nullptr);
    testing::CommandFixture degraded_fixture;
    degraded_fixture.registry.Register(std::make_unique<CostCommand>(orphan, rates));
    const auto degraded = degraded_fixture.Run("cost");
    CHECK(degraded.status == Status::kError);
    CHECK(degraded.message.find("storage") != std::string::npos);
}

TEST_CASE("/cost all lists every session with subagents adjacent to their parent",
          "[cost_tracker][commands]") {
    Harness harness;
    RateSources sources;
    sources.config_rates = nlohmann::json::parse(R"({"m1": {"input": 2.0, "output": 0.0}})");
    RateTable rates(sources, &harness.storage);
    harness.store.AppendTurn("child", "root1", Turn("m1", 1000000, 0));
    harness.store.AppendTurn("root1", "", Turn("m1", 2000000, 0));
    harness.store.AppendTurn("root2", "", Turn("m1", 3000000, 0));

    testing::CommandFixture fixture;
    fixture.registry.Register(std::make_unique<CostCommand>(harness.store, rates));
    const auto result = fixture.Run("cost", {"all"});
    REQUIRE(result.status == Status::kOk);

    const auto child = result.message.find("child");
    const auto root1 = result.message.find("root1");
    const auto root2 = result.message.find("root2");
    REQUIRE(child != std::string::npos);
    REQUIRE(root1 != std::string::npos);
    REQUIRE(root2 != std::string::npos);
    // Roots by spend: root2 ($6) before root1 ($4); the child sits next to
    // its parent root1, after it.
    CHECK(root2 < root1);
    CHECK(child > root1);
    CHECK(result.message.find("(subagent of root1)") != std::string::npos);
}

TEST_CASE("the .rpc service answers totals, list and malformed calls",
          "[cost_tracker][rpc]") {
    Harness harness;
    RateSources sources;
    sources.config_rates = nlohmann::json::parse(R"({"m1": {"input": 2.0, "output": 8.0}})");
    RateTable rates(sources, &harness.storage);
    harness.store.AppendTurn("s1", "root", Turn("m1", 1000000, 500000));
    CostRpcService service(harness.store, rates, CostSettings{}, harness.in_flight);

    const auto totals = service.Handle("totals", R"({"session_id": "s1"})");
    REQUIRE(totals.ok);
    const nlohmann::json body = nlohmann::json::parse(totals.payload);
    CHECK(body.at("session_id") == "s1");
    CHECK(body.at("parent_session_id") == "root");
    CHECK(body.at("total_cost") == 6.0);
    CHECK(body.at("total_cost_known") == true);
    CHECK(body.at("currency") == "USD");
    CHECK(body.at("models").at(0).at("model") == "m1");

    const auto list = service.Handle("list", "");
    REQUIRE(list.ok);
    const nlohmann::json sessions = nlohmann::json::parse(list.payload);
    REQUIRE(sessions.at("sessions").size() == 1);

    // A missing session is a plugin-domain refusal, not a crash.
    CHECK(!service.Handle("totals", R"({"session_id": "nope"})").ok);
    CHECK(!service.Handle("totals", "").ok);                       // no session_id
    CHECK(!service.Handle("totals", "{broken").ok);                // malformed JSON
    CHECK(!service.Handle("no-such-method", "").ok);               // unknown method
    CHECK(service.Handle("no-such-method", "").error.find("no-such-method") !=
          std::string::npos);
}

TEST_CASE("the in-flight accumulator merges rounds per model and clears on demand",
          "[cost_tracker]") {
    InFlightUsage in_flight;
    agent::SessionInfo info;
    info.session_id = "s1";

    agent::RoundUsage round;
    round.model = "m1";
    round.usage = llm::Usage{100, 10, 5, 0};
    in_flight.RecordRound("s1", round);
    round.model = "m2";  // a mid-turn model switch keeps per-model rows
    round.usage = llm::Usage{200, 20, 0, 0};
    in_flight.RecordRound("s1", round);

    const auto records = in_flight.Records("s1");
    REQUIRE(records.size() == 2);
    CHECK(records[0].model == "m1");
    CHECK(records[0].usage.input_tokens == 100);
    CHECK(records[0].usage.usage_reported);
    CHECK(records[1].model == "m2");
    CHECK(records[1].usage.input_tokens == 200);

    // Sessions do not bleed into each other.
    CHECK(in_flight.Records("s2").empty());

    in_flight.Clear("s1");
    CHECK(in_flight.Records("s1").empty());
}

TEST_CASE("an estimated round is recorded as existing but never as tokens",
          "[cost_tracker][invariant]") {
    // The bytes/4 fallback fabricates counts; the provisional total must not
    // price them. It must also not silently pretend the round was fine: the
    // session's live rows go unreported, which is what renders "cost?".
    InFlightUsage in_flight;

    agent::RoundUsage reported;
    reported.model = "m1";
    reported.usage = llm::Usage{100, 10, 0, 0};
    in_flight.RecordRound("s1", reported);

    agent::RoundUsage estimated;
    estimated.model = "m1";
    estimated.usage = llm::Usage{9999, 999, 0, 0};  // the fabricated estimate
    estimated.usage_estimated = true;
    in_flight.RecordRound("s1", estimated);

    const auto records = in_flight.Records("s1");
    REQUIRE(records.size() == 1);
    CHECK(records[0].usage.input_tokens == 100);
    CHECK(!records[0].usage.usage_reported);
}

TEST_CASE("the observer feeds rounds live and retires them at turn end",
          "[cost_tracker]") {
    // THE UX CONTRACT: a round is visible in the live view the moment it
    // lands, and the authoritative turn record replaces it exactly once --
    // never both (double count), never neither (drop).
    Harness harness;
    CostSessionObserver observer(harness.store, harness.in_flight);
    agent::SessionInfo info;
    info.session_id = "s1";

    agent::RoundUsage round;
    round.model = "m1";
    round.usage = llm::Usage{1000, 10, 0, 0};
    observer.OnProviderRound(info, round);
    observer.OnProviderRound(info, round);

    auto live = harness.in_flight.Records("s1");
    REQUIRE(live.size() == 1);
    CHECK(live[0].usage.input_tokens == 2000);

    agent::TurnSummary summary;
    summary.model = "m1";
    summary.turn_usage = llm::Usage{3000, 30, 0, 0};  // rounds 1+2+3, the last
    observer.OnTurnCompleted(info, summary);          // one landed after the polls

    CHECK(harness.in_flight.Records("s1").empty());   // live rows retired...
    const auto history = harness.store.Load("s1");
    REQUIRE(history.turns.size() == 1);
    CHECK(history.turns[0].usage.input_tokens == 3000);  // ...record keeps all three
}

TEST_CASE("the .rpc totals roll up the whole subagent subtree and the live rounds",
          "[cost_tracker][rpc]") {
    // The status line polls the FOCUSED session; a subagent's spend is
    // recorded under the child's own id. Without the roll-up the operator's
    // bill silently excludes everything the subagents did -- the exact
    // complaint this feature exists to answer.
    Harness harness;
    RateSources sources;
    sources.config_rates = nlohmann::json::parse(R"({"m1": {"input": 2.0, "output": 0.0}})");
    RateTable rates(sources, &harness.storage);
    harness.store.AppendTurn("root", "", Turn("m1", 1000000, 0));    // $2
    harness.store.AppendTurn("child", "root", Turn("m1", 500000, 0));    // $1
    harness.store.AppendTurn("grandchild", "child", Turn("m1", 250000, 0));  // $0.50
    harness.store.AppendTurn("other", "", Turn("m1", 1000000, 0));   // must NOT merge
    CostRpcService service(harness.store, rates, CostSettings{}, harness.in_flight);

    agent::RoundUsage round;  // a live round mid-turn on the child
    round.model = "m1";
    round.usage = llm::Usage{125000, 0, 0, 0};
    harness.in_flight.RecordRound("child", round);

    const auto reply = service.Handle("totals", R"({"session_id": "root"})");
    REQUIRE(reply.ok);
    const nlohmann::json body = nlohmann::json::parse(reply.payload);
    CHECK(body.at("total_cost") == 3.75);
    CHECK(body.at("includes_subagents") == true);
    CHECK(body.at("in_flight") == true);

    // A leaf asks for itself plus nothing: same arithmetic, honest flags.
    harness.in_flight.Clear("child");
    const auto leaf = service.Handle("totals", R"({"session_id": "grandchild"})");
    REQUIRE(leaf.ok);
    const nlohmann::json leaf_body = nlohmann::json::parse(leaf.payload);
    CHECK(leaf_body.at("total_cost") == 0.5);
    CHECK(leaf_body.at("includes_subagents") == false);
    CHECK(leaf_body.at("in_flight") == false);

    // A fresh session with only live rounds and NO stored blob yet still
    // answers -- the first turn's cost must not wait for its record.
    InFlightUsage fresh;
    fresh.RecordRound("new", round);
    CostRpcService fresh_service(harness.store, rates, CostSettings{}, fresh);
    const auto live_only = fresh_service.Handle("totals", R"({"session_id": "new"})");
    REQUIRE(live_only.ok);
    CHECK(nlohmann::json::parse(live_only.payload).at("total_cost") == 0.25);
}

TEST_CASE("the tracking service reads the same store, degrading to nullopt",
          "[cost_tracker]") {
    Harness harness;
    RateSources sources;
    sources.config_rates = nlohmann::json::parse(R"({"m1": {"input": 2.0, "output": 0.0}})");
    RateTable rates(sources, &harness.storage);
    CostTrackingServiceImpl service(harness.store, rates);

    CHECK(!service.Totals("s1"));
    harness.store.AppendTurn("s1", "", Turn("m1", 1000000, 0));
    const auto totals = service.Totals("s1");
    REQUIRE(totals);
    CHECK(totals->total_cost == 2.0);
    REQUIRE(service.SessionIds().size() == 1);
    CHECK(service.SessionIds()[0] == "s1");

    // Null storage is "no data", the ServiceRegistry runtime condition.
    CostStore orphan(nullptr);
    CostTrackingServiceImpl degraded(orphan, rates);
    CHECK(!degraded.Totals("s1"));
    CHECK(degraded.SessionIds().empty());
}

TEST_CASE("a failed rates fetch keeps the old table", "[cost_tracker]") {
    Harness harness;
    RateSources sources;
    sources.config_rates = nlohmann::json::parse(R"({"m1": {"input": 2.0, "output": 0.0}})");
    RateTable rates(sources, &harness.storage);
    harness.store.AppendTurn("s1", "", Turn("m1", 1000000, 0));

    // Unroutable ports for BOTH sources: every fetch entry point (the startup
    // auto-refresh, --refresh-cost-rates, the .rpc method) must fail cleanly
    // and the price must survive. (No live network in tests; this hits
    // nothing.) Drives FetchAndUpdateRates directly -- it is the shared path
    // all three run on.
    CostSettings settings;
    settings.rates_fetch_url = "http://127.0.0.1:1/x.json";
    settings.openrouter_rates_fetch_url = "http://127.0.0.1:1/or.json";
    settings.rates_fetch_timeout_ms = 2000;

    const auto outcome = FetchAndUpdateRates(settings, harness.store, rates);
    CHECK_FALSE(outcome.ok);

    const auto totals = Aggregate("s1", "", harness.store.Load("s1").turns,
                                  [&rates](const std::string& m) { return rates.Resolve(m); });
    CHECK(totals.total_cost == 2.0);
}

TEST_CASE("the startup refresh gate fetches only when a snapshot is stale", "[cost_tracker]") {
    Harness harness;
    CostSettings settings;  // default 7-day horizon

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const FetchedRates fresh{"t", now, {{"m", Rate{1.0, 1.0}}}};
    const FetchedRates old{"t", now - 8 * 86400, {{"m", Rate{1.0, 1.0}}}};
    const FetchedRates boundary{"t", now - 6 * 86400, {{"m", Rate{1.0, 1.0}}}};

    // No cache at all: the first boot fetches.
    CHECK(RefreshRatesDue(std::nullopt, std::nullopt, settings));
    // Either tier stale (or timestamp-less: a pre-fetched_at_unix cache) is
    // enough -- one tier of prices silently aging out is what the gate exists
    // to prevent.
    CHECK(RefreshRatesDue(old, fresh, settings));
    CHECK(RefreshRatesDue(fresh, old, settings));
    CHECK(RefreshRatesDue(FetchedRates{"t", 0, {}}, fresh, settings));
    // Fresh on both sides of the horizon: skip. (The boundary case sits one
    // day inside the 7-day window; "at least 7 days" is covered by `old`.)
    CHECK_FALSE(RefreshRatesDue(fresh, fresh, settings));
    CHECK_FALSE(RefreshRatesDue(fresh, boundary, settings));
    // rates_refresh_days == 0 means fetch on every start, fresh or not.
    CostSettings always;
    always.rates_refresh_days = 0;
    CHECK(RefreshRatesDue(fresh, fresh, always));
}

TEST_CASE("the startup refresh runs in the background and teardown joins it", "[cost_tracker]") {
    Harness harness;
    RateSources sources;
    sources.config_rates = nlohmann::json::parse(R"({"m1": {"input": 2.0, "output": 0.0}})");
    RateTable rates(sources, &harness.storage);
    // Unroutable ports: the worker runs its full (bounded) fetch and fails,
    // which is exactly the shape the teardown join must tolerate.
    CostSettings settings;
    settings.rates_fetch_url = "http://127.0.0.1:1/x.json";
    settings.openrouter_rates_fetch_url = "http://127.0.0.1:1/or.json";
    settings.rates_fetch_timeout_ms = 2000;

    RatesRefresher refresher(settings, harness.store, rates, &harness.storage);
    // No cache: the gate opens.
    refresher.MaybeStart();

    // The owner is the real teardown path: its destructor joins the worker.
    // The suite surviving this scope (no hang, no ASan use-after-free) IS the
    // assertion; destruction order here mirrors main()'s -- owner first, then
    // everything the worker can still touch.
    {
        RatesRefresherOwner owner(refresher);
    }
}

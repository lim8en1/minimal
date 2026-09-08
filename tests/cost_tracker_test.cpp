// cost-tracker: rate resolution, cost arithmetic, aggregation, the storage
// wrapper, the observer, /cost and the .rpc service. Compiled directly into
// the test binary like every other bundled plugin's pure logic (plugin.cpp,
// the extern "C" entry points, stays out -- the loader tests cover that shape).

#include <memory>
#include <string>

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
#include "update_rates_command.hpp"

// tests/ sits beside the include dirs of EVERY bundled plugin, and checklist
// also ships an rpc_service.hpp -- quoted lookup finds that one first. This
// plugin's own headers are reached the same way commands_core_fixture.hpp
// reaches its plugin: a relative path.
#include "../plugins/server/cost-tracker/rpc_service.hpp"

using namespace minimal;
using namespace minimal::cost_tracker;
using Status = commands::CommandResult::Status;

namespace {

struct Harness {
    minimal::testing::TempDir dir;
    plugins::PluginStorage storage{dir.path() / "plugin_storage.db"};
    CostStore store{&storage};
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

TEST_CASE("fetched-rates blob round-trips and rejects corruption", "[cost_tracker]") {
    FetchedRates rates;
    rates.fetched_at = "2026-08-31T00:00:00Z";
    rates.models["m1"] = Rate{1.0, 2.0, 0.5, 0.0};
    const auto decoded = FetchedFromJson(FetchedToJson(rates).dump());

    REQUIRE(decoded);
    CHECK(decoded->fetched_at == "2026-08-31T00:00:00Z");
    REQUIRE(decoded->models.count("m1") == 1);
    CHECK(decoded->models.at("m1").output_per_mtok == 2.0);

    CHECK(!FetchedFromJson("not json"));
    CHECK(!FetchedFromJson(R"([])"));
    CHECK(!FetchedFromJson(R"({"fetched_at": "x"})"));
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

TEST_CASE("rate resolution is config > fetched > bundled", "[cost_tracker][invariant]") {
    Harness harness;
    harness.store.SetFetchedRates(
        FetchedRates{"t", {{"m", Rate{10.0, 10.0}}, {"only-fetched", Rate{7.0, 7.0}}}});

    RateSources sources;
    sources.bundled_models = nlohmann::json::parse(R"({"m": {"input": 1.0, "output": 1.0}})");
    sources.config_rates = nlohmann::json::parse(R"({"m": {"input": 100.0, "output": 100.0}})");
    RateTable table(sources, &harness.storage);
    table.RefreshFetched();

    REQUIRE(table.Resolve("m"));
    CHECK(table.Resolve("m")->input_per_mtok == 100.0);
    REQUIRE(table.Resolve("only-fetched"));
    CHECK(table.Resolve("only-fetched")->input_per_mtok == 7.0);
    CHECK(!table.Resolve("absolutely-unknown"));

    // The store write a later process makes is picked up by RefreshFetched --
    // the snapshot outranking the stale bundled copy is what makes fetching
    // useful without a restart.
    sources.config_rates = nlohmann::json::object();
    RateTable reload(sources, &harness.storage);
    reload.RefreshFetched();
    REQUIRE(reload.Resolve("m"));
    CHECK(reload.Resolve("m")->input_per_mtok == 10.0);
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
    CostSessionObserver observer(harness.store);

    agent::SessionInfo info;
    info.session_id = "s1";
    info.parent_session_id = "root";

    agent::TurnSummary good;
    good.model = "gpt-4o";
    good.usage = llm::Usage{100, 50, 20, 10};
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

TEST_CASE("the observer never prices an estimated turn", "[cost_tracker][invariant]") {
    // The zero-usage fallback mirrors its bytes/4 estimate into the usage
    // fields so the status bar can render; the estimated flag is what keeps
    // that count OUT of cost pricing. Non-zero fields alone must not flip
    // usage_reported once the flag is set.
    Harness harness;
    CostSessionObserver observer(harness.store);

    agent::SessionInfo info;
    info.session_id = "s1";

    agent::TurnSummary estimated;
    estimated.model = "gpt-4o";
    estimated.usage = llm::Usage{1234, 0, 0, 0};  // the estimate, mirrored in
    estimated.usage_estimated = true;
    observer.OnTurnCompleted(info, estimated);

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
    CostSessionObserver observer(harness.store);
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
    CHECK(defaults.rates_fetch_timeout_ms == 30000);

    const auto custom = ParseCostSettings(nlohmann::json::parse(
        R"({"rates_fetch_url": "http://mirror/x.json", "rates_fetch_timeout_ms": 5})"));
    CHECK(custom.rates_fetch_url == "http://mirror/x.json");
    CHECK(custom.rates_fetch_timeout_ms == 5);

    // Junk values keep defaults rather than poisoning the fetch.
    const auto junk = ParseCostSettings(
        nlohmann::json::parse(R"({"rates_fetch_url": 3, "rates_fetch_timeout_ms": -1})"));
    CHECK(junk.rates_fetch_url == defaults.rates_fetch_url);
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
    CostRpcService service(harness.store, rates, CostSettings{});

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

    // An unroutable port: the command must fail cleanly and the price must
    // survive. (No live network in tests; this hits nothing.)
    CostSettings settings;
    settings.rates_fetch_url = "http://127.0.0.1:1/x.json";
    settings.rates_fetch_timeout_ms = 2000;

    testing::CommandFixture fixture;
    fixture.registry.Register(
        std::make_unique<UpdateRatesCommand>(harness.store, rates, settings));
    const auto result = fixture.Run("cost-update-rates");
    CHECK(result.status == Status::kError);

    const auto totals = Aggregate("s1", "", harness.store.Load("s1").turns,
                                  [&rates](const std::string& m) { return rates.Resolve(m); });
    CHECK(totals.total_cost == 2.0);
}

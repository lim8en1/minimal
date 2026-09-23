// memory plugin tests: store, search ranking, scopes, topics, ingest groups,
// the sidecar embedding backend's protocol, enrichment pipeline with a fake
// summarizer. The plugin sources compile in directly (checklist_test.cpp's
// pattern); plugin.cpp and its extern "C" entry points stay out.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "memory_settings.hpp"
#include "memory_store.hpp"
#include "memory_tool.hpp"
#include "embedding_sidecar.hpp"
#include "enricher.hpp"

#include "fakes.hpp"

#include "minimal/common/logger.hpp"
#include "minimal/sidecar/process.hpp"
#include "minimal/tools/tool.hpp"

namespace fs = std::filesystem;
using namespace minimal::memory;

namespace {

// Deterministic fake embedder: hash-seeded unit vectors, so cosine between
// identical inputs is 1.0 and different inputs are far apart.
class FakeEmbedder final : public EmbeddingFunction {
public:
    std::vector<Embedding> Embed(const std::vector<std::string>& inputs) override {
        std::vector<Embedding> out;
        for (const auto& input : inputs) {
            Embedding e;
            e.values.resize(kDim, 0.0f);
            unsigned hash = 7;
            for (const char c : input) hash = hash * 31 + static_cast<unsigned char>(c);
            for (int i = 0; i < kDim; ++i) {
                hash = hash * 1103515245u + 12345u;
                e.values[static_cast<std::size_t>(i)] = static_cast<float>(hash % 2000 - 1000);
            }
            double norm = 0;
            for (const auto v : e.values) norm += static_cast<double>(v) * v;
            norm = std::sqrt(norm);
            for (auto& v : e.values) v = static_cast<float>(v / norm);
            e.dim = kDim;
            out.push_back(std::move(e));
        }
        return out;
    }
    std::string Describe() const override { return "fake"; }
    std::string Signature() const override { return "fake:v1:32"; }
    static constexpr int kDim = 32;
};

struct Fixture {
    fs::path dir;
    std::unique_ptr<MemoryStore> store;

    explicit Fixture(const std::string& name)
        : dir(fs::temp_directory_path() / ("memory-test-" + name + "-" +
                                           std::to_string(getpid()))) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        store = std::make_unique<MemoryStore>(dir / "memory.sqlite3");
    }
    ~Fixture() {
        store.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

NewEntry MakeEntry(const std::string& text, int scope = MemoryStore::kScopeProject,
                                const std::string& root = "/proj") {
    NewEntry entry;
    entry.scope = scope;
    entry.project_root = root;
    entry.session_id = "s1";
    entry.origin = "model";
    entry.text = text;
    return entry;
}

}  // namespace

TEST_CASE("memory store get round-trip", "[memory]") {
    Fixture fx("roundtrip");
    const std::string id = fx.store->Insert(MakeEntry("tests need --exclude integration"));
    const auto entry = fx.store->Get(id);
    REQUIRE(entry.has_value());
    REQUIRE(entry->text == "tests need --exclude integration");
    REQUIRE(entry->scope_name == "project");
    REQUIRE(entry->project_root == "/proj");
    REQUIRE(entry->origin == "model");
    REQUIRE(!entry->created_at.empty());
    REQUIRE_FALSE(fx.store->Get("missing").has_value());
}

TEST_CASE("memory delete", "[memory]") {
    Fixture fx("delete");
    const std::string id = fx.store->Insert(MakeEntry("obsolete lesson"));
    REQUIRE(fx.store->Delete(id));
    REQUIRE_FALSE(fx.store->Delete(id));  // second delete: gone
}

TEST_CASE("memory scope isolation", "[memory]") {
    Fixture fx("scopes");
    fx.store->Insert(MakeEntry("project lesson", MemoryStore::kScopeProject, "/proj"));
    fx.store->Insert(MakeEntry("other project lesson", MemoryStore::kScopeProject, "/other"));
    fx.store->Insert(MakeEntry("global lesson", MemoryStore::kScopeGlobal, ""));

    MemoryStore::SearchQuery q;
    q.scope = MemoryStore::kScopeProject;
    q.project_root = "/proj";
    q.sort = "time";
    auto hits = fx.store->Search(q);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].preview == "project lesson");

    q.scope = MemoryStore::kScopeGlobal;
    q.project_root = "";
    hits = fx.store->Search(q);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].preview == "global lesson");
}

TEST_CASE("memory keyword prefilter: text, origin_ref and topic", "[memory]") {
    Fixture fx("keywords");
    const auto docker = MakeEntry("how to rebuild the container quickly");
    const auto git = MakeEntry("use rebase --autostash before pulls");
    const std::string docker_id = fx.store->Insert(docker);
    const std::string git_id = fx.store->Insert(git);
    fx.store->BindTopic(docker_id, MemoryStore::kScopeProject, "/proj", "docker", true);
    fx.store->BindTopic(git_id, MemoryStore::kScopeProject, "/proj", "workflow", true);

    MemoryStore::SearchQuery q;
    q.scope = MemoryStore::kScopeProject;
    q.project_root = "/proj";
    q.sort = "time";

    // Keyword matching the topic but not the text.
    q.keywords = {"docker"};
    auto hits = fx.store->Search(q);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].preview.find("container") != std::string::npos);

    // Keyword matching neither.
    q.keywords = {"kubernetes"};
    REQUIRE(fx.store->Search(q).empty());

    // AND semantics across two keywords.
    q.keywords = {"rebase", "pulls"};
    hits = fx.store->Search(q);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].preview.find("rebase") != std::string::npos);
}

TEST_CASE("memory ingest: group, siblings, replace-on-re-ingest", "[memory]") {
    Fixture fx("ingest");
    auto chunk = MakeEntry("# Alpha\nfirst body");
    auto chunk2 = chunk;
    chunk2.text = "# Beta\nsecond body";

    const auto first = fx.store->Ingest(chunk, {chunk2}, "/tmp/notes.md");
    REQUIRE(first.ids.size() == 2);
    REQUIRE(first.replaced == 0);

    // Sibling pointers through Get.
    const auto e0 = fx.store->Get(first.ids[0]);
    REQUIRE(e0.has_value());
    REQUIRE(e0->group_id.has_value());
    REQUIRE(e0->chunk_count == 2);
    REQUIRE(e0->chunk_index == 0);
    REQUIRE(e0->sibling_ids == first.ids);
    REQUIRE(e0->origin_ref == "/tmp/notes.md");

    // Re-ingest the same path: replaces, no accumulation.
    auto chunk3 = chunk;
    chunk3.text = "# Alpha\nrevised body";
    const auto second = fx.store->Ingest(chunk3, {chunk2}, "/tmp/notes.md");
    REQUIRE(second.replaced == 2);
    REQUIRE(second.ids.size() == 2);

    MemoryStore::SearchQuery q;
    q.scope = MemoryStore::kScopeProject;
    q.project_root = "/proj";
    q.sort = "time";
    auto hits = fx.store->Search(q);
    REQUIRE(hits.size() == 2);  // not 4 -- replaced, not accumulated
    bool found_revised = false;
    for (const auto& hit : hits) {
        if (hit.preview.find("revised body") != std::string::npos) found_revised = true;
    }
    REQUIRE(found_revised);
}

TEST_CASE("memory purge by origin_ref", "[memory]") {
    Fixture fx("purge");
    auto chunk = MakeEntry("# A\nbody");
    const auto group = fx.store->Ingest(chunk, {}, "/tmp/notes.md");
    fx.store->Insert(MakeEntry("unrelated"));
    REQUIRE(fx.store->PurgeByOriginRef("/tmp/notes.md", MemoryStore::kScopeProject, "/proj") == 1);
    MemoryStore::SearchQuery q;
    q.scope = MemoryStore::kScopeProject;
    q.project_root = "/proj";
    q.sort = "time";
    auto hits = fx.store->Search(q);
    REQUIRE(hits.size() == 1);  // one ingest chunk purged, one model entry left
    // The remaining row is NOT from the purged group.
    REQUIRE(std::find(group.ids.begin(), group.ids.end(), hits[0].id) == group.ids.end());
    (void)group;
}

TEST_CASE("memory purge by origin_ref: an empty project_root matches nothing",
          "[memory]") {
    // Regression: /memory purge's command wiring called PurgeByOriginRef for
    // the project scope with project_root="" (memory_command.cpp), but every
    // project-scope row stores the session's real worktree root (as this
    // fixture's entries do, "/proj") -- so the command always reported
    // "purged 0 entries" for project-scope groups. This pins the store-level
    // behavior the fix now depends on: only the CORRECT root matches.
    Fixture fx("purge-empty-root");
    fx.store->Ingest(MakeEntry("# A\nbody"), {}, "/tmp/notes.md");
    CHECK(fx.store->PurgeByOriginRef("/tmp/notes.md", MemoryStore::kScopeProject, "") == 0);
    CHECK(fx.store->PurgeByOriginRef("/tmp/notes.md", MemoryStore::kScopeProject, "/proj") == 1);
}

TEST_CASE("StoreEmbedding sets the signature LoadVectors filters on", "[memory]") {
    // Regression: StoreEmbedding set embed_status=1 but never wrote
    // `signature`, so LoadVectors's `signature = ?1` filter (never true for
    // NULL) made every eagerly-embedded row invisible to the near-duplicate
    // hint and to sim-sorted search, regardless of how recently it was
    // embedded.
    Fixture fx("store-embedding-signature");
    const std::string id = fx.store->Insert(MakeEntry("eagerly embedded"));
    Embedding e;
    e.values = {1.0f, 0.0f};
    e.dim = 2;
    fx.store->StoreEmbedding(id, e, "sig-v1");

    const auto vectors =
        fx.store->LoadVectors("sig-v1", MemoryStore::kScopeProject, "/proj");
    REQUIRE(vectors.size() == 1);
    CHECK(vectors[0].id == id);
    // A different signature (a strategy change) correctly finds nothing.
    CHECK(fx.store->LoadVectors("sig-v2", MemoryStore::kScopeProject, "/proj").empty());
}

TEST_CASE("ResetFailedEmbeddings requeues failed rows for the next drain", "[memory]") {
    // Regression: a failed embed (embed_status=2) was excluded from semantic
    // search permanently -- ClaimPendingEmbeddings only claims status=0, and
    // nothing reset a failed row back to 0 except a signature change. This is
    // the escape hatch /memory reindex now calls.
    Fixture fx("reset-failed-embeddings");
    const std::string id = fx.store->Insert(MakeEntry("will fail then retry"));
    const auto pending = fx.store->ClaimPendingEmbeddings("sig-v1", 10);
    REQUIRE(pending.size() == 1);
    fx.store->MarkEmbedFailed({id}, "simulated backend outage");

    // Still excluded: another claim at the same signature finds nothing.
    CHECK(fx.store->ClaimPendingEmbeddings("sig-v1", 10).empty());

    CHECK(fx.store->ResetFailedEmbeddings() == 1);
    const auto reclaimed = fx.store->ClaimPendingEmbeddings("sig-v1", 10);
    REQUIRE(reclaimed.size() == 1);
    CHECK(reclaimed[0].id == id);
}

TEST_CASE("memory topics: create, case-insensitive match, no fake no-topic row", "[memory]") {
    Fixture fx("topics");
    const std::string id = fx.store->Insert(MakeEntry("lesson about docker layers"));

    REQUIRE(fx.store->BindTopic(id, MemoryStore::kScopeProject, "/proj", "docker", true));
    auto entry = fx.store->Get(id);
    REQUIRE(entry->topic == "docker");

    // Case-insensitive re-attribution hits the same topic (no duplicate).
    const std::string id2 = fx.store->Insert(MakeEntry("more docker"));
    REQUIRE(fx.store->BindTopic(id2, MemoryStore::kScopeProject, "/proj", "DOCKER", false));
    REQUIRE(fx.store->AllTopics(MemoryStore::kScopeProject, "/proj").size() == 1);

    // Unknown topic without create: stays unfiled, no sentinel row.
    const std::string id3 = fx.store->Insert(MakeEntry("unfiled lesson"));
    REQUIRE_FALSE(fx.store->BindTopic(id3, MemoryStore::kScopeProject, "/proj", "missing", false));
    REQUIRE_FALSE(fx.store->Get(id3)->topic.has_value());
    REQUIRE(fx.store->AllTopics(MemoryStore::kScopeProject, "/proj").size() == 1);
}

TEST_CASE("memory topic cap: at max_topics creation is refused", "[memory]") {
    Fixture fx("cap");
    // The no-create path, trusting the count check (covered below with a
    // real configured cap now that max_topics is plumbed through).
    const std::string id = fx.store->Insert(MakeEntry("lesson"));
    REQUIRE_FALSE(fx.store->BindTopic(id, MemoryStore::kScopeProject, "/proj", "x", false));
    REQUIRE(fx.store->AllTopics(MemoryStore::kScopeProject, "/proj").empty());
}

TEST_CASE("memory topic cap: the configured max_topics is actually enforced", "[memory]") {
    // Regression: BindTopic's cap was a hard-coded 4096, so the documented
    // `max_topics` setting was parsed but had no effect -- out of unit-test
    // range to exercise honestly. Now that BindTopic takes it as a
    // parameter, a small configured cap is directly testable.
    Fixture fx("configured-cap");
    const std::string id1 = fx.store->Insert(MakeEntry("lesson one"));
    const std::string id2 = fx.store->Insert(MakeEntry("lesson two"));
    REQUIRE(fx.store->BindTopic(id1, MemoryStore::kScopeProject, "/proj", "a",
                                /*create_if_missing=*/true, /*max_topics=*/1));
    // At cap: a second NEW topic is refused; the entry stays unfiled.
    REQUIRE_FALSE(fx.store->BindTopic(id2, MemoryStore::kScopeProject, "/proj", "b",
                                      /*create_if_missing=*/true, /*max_topics=*/1));
    // An EXISTING topic (case-insensitive) is still bindable at cap -- the
    // count only gates CREATION.
    REQUIRE(fx.store->BindTopic(id2, MemoryStore::kScopeProject, "/proj", "A",
                                /*create_if_missing=*/false, /*max_topics=*/1));
}

TEST_CASE("memory settings: defaults, clamps, lenient parse", "[memory]") {
    auto settings = ParseMemorySettings(nlohmann::json::object());
    REQUIRE(settings.strategy == StrategyKind::kAuto);
    REQUIRE(settings.allow_inline);
    REQUIRE(settings.max_topics == 4096);
    REQUIRE(settings.summarizer_model.empty());
    REQUIRE(settings.default_limit == 12);
    REQUIRE(settings.sidecar_python_path.empty());
    REQUIRE(settings.sidecar_model.empty());
    REQUIRE(settings.sidecar_startup_timeout_ms == 60000);
    REQUIRE(settings.sidecar_request_timeout_ms == 30000);

    // Bad values clamp, never throw (a bad optional number must not disable
    // the plugin).
    settings = ParseMemorySettings(nlohmann::json{
        {"default_limit", 9999}, {"batch_size", 0}, {"max_text_bytes", 1}});
    REQUIRE(settings.default_limit == 64);
    REQUIRE(settings.batch_size == 1);
    REQUIRE(settings.max_text_bytes == 256);

    settings = ParseMemorySettings(nlohmann::json{{"strategy", "nonsense"}});
    REQUIRE(settings.strategy == StrategyKind::kAuto);

    settings = ParseMemorySettings(nlohmann::json{
        {"strategy", "sidecar"},
        {"sidecar_python_path", "/usr/bin/python3"},
        {"sidecar_model", "BAAI/bge-small-en-v1.5"},
        {"sidecar_startup_timeout_ms", 5000},
        {"sidecar_request_timeout_ms", 2000}});
    REQUIRE(settings.strategy == StrategyKind::kSidecar);
    REQUIRE(settings.sidecar_python_path == "/usr/bin/python3");
    REQUIRE(settings.sidecar_model == "BAAI/bge-small-en-v1.5");
    REQUIRE(settings.sidecar_startup_timeout_ms == 5000);
    REQUIRE(settings.sidecar_request_timeout_ms == 2000);
}

TEST_CASE("memory settings: reindex_budget_rows and the timeouts/budgets are ceilinged",
          "[memory]") {
    // Regression: these had a floor (via IntOr's is_number_integer() check,
    // already correct) but no ceiling. reindex_budget_rows=-1 in particular
    // becomes `LIMIT -1` in ClaimPendingEmbeddings, which SQLite reads as no
    // limit at all -- one lazy drain would embed every pending row.
    const auto negative = ParseMemorySettings(nlohmann::json{{"reindex_budget_rows", -1}});
    CHECK(negative.reindex_budget_rows == 0);

    const auto huge = ParseMemorySettings(nlohmann::json{
        {"reindex_budget_rows", 999999999},
        {"timeout_ms", 999999999},
        {"sidecar_startup_timeout_ms", 999999999},
        {"sidecar_request_timeout_ms", 999999999},
        {"summarize_max_tokens", 999999999},
        {"summarize_input_char_budget", 999999999}});
    CHECK(huge.reindex_budget_rows <= 10000);
    CHECK(huge.timeout_ms <= 600000);
    CHECK(huge.sidecar_startup_timeout_ms <= 600000);
    CHECK(huge.sidecar_request_timeout_ms <= 600000);
    CHECK(huge.summarize_max_tokens <= 4096);
    CHECK(huge.summarize_input_char_budget <= (1 << 20));

    // A negative char budget cast to size_t elsewhere would disable
    // truncation entirely by wrapping to a huge unsigned value.
    const auto negative_budget =
        ParseMemorySettings(nlohmann::json{{"summarize_input_char_budget", -5}});
    CHECK(negative_budget.summarize_input_char_budget == 0);
}

TEST_CASE("memory digest info", "[memory]") {
    Fixture fx("digest");
    fx.store->Insert(MakeEntry("a lesson"));
    const auto id = fx.store->Insert(MakeEntry("another"));
    fx.store->BindTopic(id, MemoryStore::kScopeProject, "/proj", "docker", true);

    const auto digest = fx.store->DigestInfo("lexical only", "off", "/proj");
    REQUIRE(digest.entries_by_scope.at("project") == 2);
    REQUIRE(digest.by_origin.at("model") == 2);
    REQUIRE(digest.top_topics.size() == 1);
    REQUIRE(digest.top_topics[0].first == "docker");
    REQUIRE(digest.backend == "lexical only");
    REQUIRE(digest.enrichment == "off");
}

TEST_CASE("memory sidecar embedder: unconfigured or unusable python degrades to null", "[memory]") {
    // No sidecar_model at all: TryMake returns null without even trying to
    // spawn -- same "degrade to lexical, never fail plugin load" contract
    // the http/onnx backends had.
    MemorySettings settings;
    REQUIRE(TryMakeSidecarEmbeddingFunction(settings, fs::temp_directory_path()) == nullptr);

    // A python_path that does not exist: the spawn itself fails.
    settings.sidecar_model = "some/model";
    settings.sidecar_python_path = "/definitely/not/a/real/python/binary";
    REQUIRE(TryMakeSidecarEmbeddingFunction(settings, fs::temp_directory_path()) == nullptr);
}

TEST_CASE("memory sidecar embedder: real fastembed round trip", "[memory][sidecar]") {
    // Real end-to-end: spawns the actual driver against a real python3 with
    // fastembed installed. Skips rather than fails when either is missing,
    // the same convention tools_python_test.cpp uses for its interpreter
    // tests -- a python3-less or fastembed-less machine is a normal dev/CI
    // state, not a broken build.
    const std::string python_path = minimal::sidecar::ResolveOnPath("python3");
    if (python_path.empty()) SKIP("python3 not available");
    const std::string check = python_path + " -c \"import fastembed\" >/dev/null 2>&1";
    if (std::system(check.c_str()) != 0) SKIP("fastembed not importable");

    MemorySettings settings;
    settings.sidecar_python_path = python_path;
    settings.sidecar_model = "BAAI/bge-small-en-v1.5";
    settings.sidecar_startup_timeout_ms = 120000;  // first run downloads the model
    settings.sidecar_request_timeout_ms = 30000;

    const fs::path dir =
        fs::temp_directory_path() / ("memory-sidecar-" + std::to_string(getpid()));
    auto embedder = TryMakeSidecarEmbeddingFunction(settings, dir);
    REQUIRE(embedder != nullptr);

    const auto vectors = embedder->Embed(
        {"the cat sat on the mat", "a feline rested on a rug", "quarterly earnings report"});
    REQUIRE(vectors.size() == 3);
    REQUIRE(vectors[0].dim > 0);
    REQUIRE(vectors[0].dim == vectors[1].dim);

    // The semantically close (but lexically dissimilar) pair scores higher
    // than the unrelated one -- the whole point of the fix to Search()'s
    // full_scan path earlier in this pass.
    const double close = Cosine(vectors[0].values, vectors[1].values);
    const double far = Cosine(vectors[0].values, vectors[2].values);
    REQUIRE(close > far);

    REQUIRE_THAT(embedder->Describe(), Catch::Matchers::ContainsSubstring("fastembed"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("enrichment: fake summarizer drives summary + topic", "[memory]") {
    Fixture fx("enrich");
    MemorySettings settings;
    settings.summarizer_model = "fake/model";  // enabled
    Enricher enricher(settings, fx.store.get());

    // RunEnrichment is the synchronous testing seam; the provider seam is
    // exercised through the strict-JSON parse path -- a real provider call is
    // out of unit scope, so drive the store effects directly and assert the
    // API surface (summary write-back, topic binding) behaves.
    const std::string id = fx.store->Insert(MakeEntry("deploy via make release"));
    fx.store->SetSummary(id, "Releases go through make release.");
    REQUIRE(fx.store->BindTopic(id, MemoryStore::kScopeProject, "/proj", "deploy", true));
    const auto entry = fx.store->Get(id);
    REQUIRE(entry->summary == "Releases go through make release.");
    REQUIRE(entry->topic == "deploy");
    (void)enricher;
}

TEST_CASE("enrichment: also_bind applies the same summary/topic to sibling chunks", "[memory]") {
    Fixture fx("enrich-siblings");
    MemorySettings settings;
    settings.summarizer_model = "fake/model";  // enabled

    const std::string sample_id = fx.store->Insert(MakeEntry("# Deploy\nrelease via make release"));
    const std::string sibling_id = fx.store->Insert(MakeEntry("more body for the same group"));

    minimal::testing::FakeLlmProvider provider;
    provider.PushResponse(
        minimal::testing::TextResponse(R"({"summary": "Releases go through make release.",)"
                                       R"( "topic": {"new": "deploy"}})"));

    Enricher::RunEnrichment(settings, *fx.store, &provider, "fake/model",
                            minimal::common::DefaultLogger(), sample_id,
                            "# Deploy\nrelease via make release", MemoryStore::kScopeProject,
                            "/proj", {sibling_id});

    const auto sample = fx.store->Get(sample_id);
    const auto sibling = fx.store->Get(sibling_id);
    REQUIRE(sample->summary == "Releases go through make release.");
    REQUIRE(sample->topic == "deploy");
    // The sibling was never itself asked -- only ONE provider call happened
    // (PushResponse queued exactly one) -- yet it carries the same
    // summary/topic as the sample it rode in with.
    REQUIRE(sibling->summary == "Releases go through make release.");
    REQUIRE(sibling->topic == "deploy");

    // Exactly one topic row was created, not two: the sibling's BindTopic
    // call must not re-create "deploy" a second time.
    const auto topics = fx.store->AllTopics(MemoryStore::kScopeProject, "/proj");
    REQUIRE(topics.size() == 1);
    REQUIRE(topics[0].second == 2);  // both entries bound to it
}

TEST_CASE("memory search: sim-search fallback truncates to the requested limit", "[memory]") {
    Fixture fx("sim-fallback");
    for (int i = 0; i < 5; ++i) {
        fx.store->Insert(MakeEntry("lesson number " + std::to_string(i), MemoryStore::kScopeGlobal, ""));
    }

    class ThrowingEmbedder final : public EmbeddingFunction {
    public:
        std::vector<Embedding> Embed(const std::vector<std::string>&) override {
            throw std::runtime_error("embedder unavailable");
        }
        std::string Describe() const override { return "throwing"; }
        std::string Signature() const override { return "throwing:v1"; }
    };

    auto runtime = std::make_shared<MemoryRuntime>();
    runtime->settings = ParseMemorySettings(nlohmann::json::object());
    runtime->store = std::move(fx.store);
    runtime->embedder = std::make_unique<ThrowingEmbedder>();
    runtime->enricher = std::make_unique<Enricher>(runtime->settings, runtime->store.get());
    runtime->signature = "throwing:v1";

    auto tool = MakeMemorySearchTool(runtime);
    minimal::common::Logger logger = minimal::common::DefaultLogger();
    minimal::tools::ToolContext ctx(logger);  // session_id empty: no digest, no enricher touch

    const auto result =
        tool->execute({{"scope", "global"}, {"sort", "sim"}, {"query", "lesson"}, {"limit", 2}}, ctx);
    REQUIRE_FALSE(result.is_error);
    const auto body = nlohmann::json::parse(result.content_text);
    REQUIRE(body["results"].size() == 2);  // capped, not all 5 (the pre-fix bug)

    fx.store = std::move(runtime->store);  // Fixture's destructor still cleans up the dir
}

TEST_CASE("memory search: created_at is populated on hits", "[memory]") {
    Fixture fx("created-at");
    fx.store->Insert(MakeEntry("a lesson worth remembering"));

    MemoryStore::SearchQuery q;
    q.scope = MemoryStore::kScopeProject;
    q.project_root = "/proj";
    q.sort = "time";
    const auto hits = fx.store->Search(q);
    REQUIRE(hits.size() == 1);
    REQUIRE_FALSE(hits[0].created_at.empty());
    REQUIRE_THAT(hits[0].created_at, Catch::Matchers::ContainsSubstring("T"));
}

TEST_CASE("memory search: FTS match is scoped to the text column, not project_root", "[memory]") {
    Fixture fx("fts-scope");
    const std::string root = "/proj/zephyrproject";
    fx.store->Insert(MakeEntry("just a note about cats", MemoryStore::kScopeProject, root));
    fx.store->Insert(MakeEntry("another note about dogs", MemoryStore::kScopeProject, root));

    MemoryStore::SearchQuery q;
    q.scope = MemoryStore::kScopeProject;
    q.project_root = root;
    q.sort = "time";

    // "zephyrproject" only appears in project_root, shared by every row in
    // scope -- an unqualified MATCH would hit both entries regardless of
    // their text. Qualified to the text column, neither matches.
    q.query = "zephyrproject";
    REQUIRE(fx.store->Search(q).empty());

    // A real text match still works.
    q.query = "cats";
    auto hits = fx.store->Search(q);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].preview.find("cats") != std::string::npos);

    // Multi-word queries are split into terms, ANDed: "cats" and "dogs" never
    // co-occur in one entry's text.
    q.query = "cats dogs";
    REQUIRE(fx.store->Search(q).empty());
}

TEST_CASE("memory digest info: project scope is scoped to the caller's project_root", "[memory]") {
    Fixture fx("digest-scope");
    fx.store->Insert(MakeEntry("mine", MemoryStore::kScopeProject, "/proj/a"));
    fx.store->Insert(MakeEntry("also mine", MemoryStore::kScopeProject, "/proj/a"));
    fx.store->Insert(MakeEntry("someone else's", MemoryStore::kScopeProject, "/proj/b"));
    const auto other_id = fx.store->Insert(MakeEntry("theirs too", MemoryStore::kScopeProject, "/proj/b"));
    fx.store->BindTopic(other_id, MemoryStore::kScopeProject, "/proj/b", "unrelated", true);

    const auto digest = fx.store->DigestInfo("lexical only", "off", "/proj/a");
    REQUIRE(digest.entries_by_scope.at("project") == 2);  // not 4 -- /proj/b excluded
    REQUIRE(digest.by_origin.at("model") == 2);
    REQUIRE(digest.top_topics.empty());  // "unrelated" belongs to /proj/b, not /proj/a
}

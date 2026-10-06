// memory plugin tests: store (schema v2 + v1 migration), lexical and hybrid
// search, tags, ingest chunking, the embedding service's breaker and poison-
// row isolation, the tagger with a fake provider, the context index, and the
// tools end to end. The plugin sources compile in directly (checklist_test's
// pattern); plugin.cpp and its extern "C" entry points stay out.

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "embedding_service.hpp"
#include "embedding_sidecar.hpp"
#include "ingest.hpp"
#include "memory_runtime.hpp"
#include "memory_settings.hpp"
#include "memory_store.hpp"
#include "search.hpp"
#include "tagger.hpp"

#include "fakes.hpp"

#include "minimal/common/logger.hpp"
#include "minimal/sidecar/process.hpp"
#include "minimal/tools/tool.hpp"

namespace fs = std::filesystem;
using namespace minimal::memory;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr int kDim = 16;

std::vector<float> HashVector(const std::string& input) {
    std::vector<float> values(kDim, 0.0f);
    unsigned hash = 7;
    for (const char c : input) hash = hash * 31 + static_cast<unsigned char>(c);
    for (int i = 0; i < kDim; ++i) {
        hash = hash * 1103515245u + 12345u;
        values[static_cast<std::size_t>(i)] = static_cast<float>(static_cast<int>(hash % 2000) - 1000);
    }
    return values;
}

// Deterministic embedder. Unrelated inputs get hash-seeded (near-orthogonal)
// vectors; texts containing a registered keyword get that keyword's vector,
// so a test can declare "these two mean the same" without real semantics.
class FakeEmbedder final : public EmbeddingFunction {
public:
    std::map<std::string, std::vector<float>> concepts;
    std::atomic<int> calls{0};
    bool down = false;
    std::string poison;  // any input containing this throws

    std::vector<Embedding> Embed(const std::vector<std::string>& inputs) override {
        ++calls;
        if (down) throw std::runtime_error("backend down");
        std::vector<Embedding> out;
        for (const auto& input : inputs) {
            if (!poison.empty() && input.find(poison) != std::string::npos) {
                throw std::runtime_error("cannot embed this input");
            }
            Embedding e;
            e.values = HashVector(input);
            for (const auto& [keyword, vector] : concepts) {
                if (input.find(keyword) != std::string::npos) {
                    e.values = vector;
                    break;
                }
            }
            e.dim = kDim;
            out.push_back(std::move(e));
        }
        return out;
    }
    std::string Describe() const override { return "fake"; }
    std::string Signature() const override { return "fake:v1"; }
};

std::vector<float> Axis(int i) {
    std::vector<float> v(kDim, 0.0f);
    v[static_cast<std::size_t>(i)] = 1.0f;
    return v;
}

struct Fixture {
    fs::path dir;
    std::unique_ptr<MemoryStore> store;

    explicit Fixture(const std::string& name)
        : dir(fs::temp_directory_path() /
              ("memory-test-" + name + "-" + std::to_string(getpid()))) {
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

NewEntry Entry(std::string title, std::string description, std::string body,
               int scope = kScopeProject, std::string root = "/proj") {
    NewEntry e;
    e.scope = scope;
    e.project_root = scope == kScopeGlobal ? "" : std::move(root);
    e.title = std::move(title);
    e.description = std::move(description);
    e.body = std::move(body);
    return e;
}

ScopeFilter From(std::string root = "/proj") {
    ScopeFilter f;
    f.project_root = std::move(root);
    return f;
}

MemoryStore::Filters Filters(std::string root = "/proj") {
    MemoryStore::Filters f;
    f.scope = From(std::move(root));
    return f;
}

// An EmbeddingService over a FakeEmbedder the test keeps a handle to. The
// breaker backoff is an hour, so an open breaker stays open for the test.
std::unique_ptr<EmbeddingService> FakeService(MemoryStore& store, FakeEmbedder*& handle) {
    EmbeddingPlan plan;
    plan.strategy = "fake";
    plan.signature = "fake:v1";
    plan.factory = [&handle]() -> std::unique_ptr<EmbeddingFunction> {
        auto owned = std::make_unique<FakeEmbedder>();
        handle = owned.get();
        return owned;
    };
    EmbeddingService::Options options;
    options.breaker_base = std::chrono::hours(1);
    options.breaker_max = std::chrono::hours(1);
    return std::make_unique<EmbeddingService>(&store, std::move(plan),
                                              minimal::common::DefaultLogger(), options);
}

const std::string& PromptOf(const minimal::llm::ChatRequest& request) {
    return std::get<minimal::llm::TextBlock>(request.messages[0].content[0]).text;
}

}  // namespace

// ------------------------------------------------------------------ settings

TEST_CASE("memory settings: defaults, and out-of-range values are refused", "[memory]") {
    const auto defaults = ParseMemorySettings(nlohmann::json::object());
    CHECK(defaults.tagger_model.empty());
    CHECK(defaults.max_tags == 100);
    CHECK(defaults.writes_prompt);
    CHECK(defaults.index_enabled);

    const auto tuned = ParseMemorySettings(
        {{"batch_size", 1}, {"default_limit", 64}, {"tag_merge_similarity", 1.0}});
    CHECK(tuned.batch_size == 1);
    CHECK(tuned.default_limit == 64);
    CHECK(tuned.tag_merge_similarity == 1.0);

    CHECK_THROWS_WITH(ParseMemorySettings({{"batch_size", -3}}),
                      ContainsSubstring("memory.config.batch_size: must be at least 1"));
    CHECK_THROWS_WITH(ParseMemorySettings({{"default_limit", 1000}}),
                      ContainsSubstring("default_limit: must be at most 64"));
    CHECK_THROWS_WITH(ParseMemorySettings({{"tag_merge_similarity", 5.0}}),
                      ContainsSubstring("tag_merge_similarity: must be between"));
    CHECK_THROWS_WITH(ParseMemorySettings({{"strategy", "bogus"}}),
                      ContainsSubstring("strategy: must be one of"));
}

TEST_CASE("memory settings: an unknown field is a named error", "[memory][settings]") {
    CHECK_THROWS_WITH(ParseMemorySettings({{"api_key_env", "K"}}),
                      ContainsSubstring("memory.config.api_key_env: unknown field"));
    CHECK_NOTHROW(ParseMemorySettings({{"//", "comment"}, {"api_key", "secret:embeddings"}}));
}

// ---------------------------------------------------------------------- tags

TEST_CASE("memory tags: normalization and match keys", "[memory]") {
    CHECK(NormalizeTagName("Unit-Tests") == "unit-tests");
    CHECK(NormalizeTagName("  unit_test ") == "unit-test");
    CHECK(NormalizeTagName("Docker / Compose!") == "docker-compose");
    CHECK(TagMatchKey(NormalizeTagName("Unit-Tests")) == TagMatchKey(NormalizeTagName("unit test")));
    CHECK(TagMatchKey("notes") == TagMatchKey("note"));
    // Short and unrelated names never collide (no edit distance).
    CHECK(TagMatchKey("qa") != TagMatchKey("db"));
    // The plural fold leaves these alone.
    CHECK(TagMatchKey("status") == "status");
    CHECK(TagMatchKey("analysis") == "analysis");
    CHECK(TagMatchKey("class") == "class");
    // UTF-8 names survive.
    CHECK(NormalizeTagName("Caf\xC3\xA9 Notes") == "caf\xC3\xA9-notes");
}

TEST_CASE("memory tags: create dedups by match key; merge, rename, GC", "[memory]") {
    Fixture fx("tags");
    const auto a = fx.store->CreateTag(kScopeProject, "/proj", "Unit-Tests", "testing");
    CHECK(fx.store->CreateTag(kScopeProject, "/proj", "unit test", "") == a);
    const auto b = fx.store->CreateTag(kScopeProject, "/proj", "testing", "also testing");
    CHECK(a != b);
    // Same name, other scope: a different tag.
    CHECK(fx.store->CreateTag(kScopeGlobal, "", "unit-tests", "") != a);

    const auto id = fx.store->Insert(Entry("t", "d", "b"));
    fx.store->SetEntryTags(id, {a, b});
    CHECK(fx.store->Get(id, From())->tags == std::vector<std::string>{"testing", "unit-tests"});

    REQUIRE(fx.store->MergeTags(kScopeProject, "/proj", "unit-tests", "testing"));
    CHECK(fx.store->Get(id, From())->tags == std::vector<std::string>{"testing"});
    CHECK_FALSE(fx.store->FindTag(kScopeProject, "/proj", "unit-tests").has_value());

    REQUIRE(fx.store->RenameTag(kScopeProject, "/proj", "testing", "qa"));
    CHECK(fx.store->Get(id, From())->tags == std::vector<std::string>{"qa"});

    // GC: deleting the last entry drops its now-unused tags.
    REQUIRE(fx.store->Delete(id, From()) == WriteOutcome::kDone);
    CHECK(fx.store->TagCount(kScopeProject, "/proj") == 0);
}

// ------------------------------------------------------------- store basics

TEST_CASE("memory store: round trip and scope visibility", "[memory]") {
    Fixture fx("roundtrip");
    auto pinned = Entry("Prefers tabs", "The user wants tabs, not spaces", "Always tabs.");
    pinned.pinned = true;
    const auto a = fx.store->Insert(pinned);
    const auto b = fx.store->Insert(Entry("Other project", "d", "b", kScopeProject, "/other"));
    const auto g = fx.store->Insert(Entry("Global fact", "d", "b", kScopeGlobal));

    const auto row = fx.store->Get(a, From());
    REQUIRE(row);
    CHECK(row->title == "Prefers tabs");
    CHECK(row->description == "The user wants tabs, not spaces");
    CHECK(row->body == "Always tabs.");
    CHECK(row->pinned);
    CHECK(row->created_ms > 0);

    CHECK_FALSE(fx.store->Get(b, From()).has_value());  // another project's
    CHECK(fx.store->Get(g, From()).has_value());        // global is everywhere
    ScopeFilter global_only = From();
    global_only.include_project = false;
    CHECK_FALSE(fx.store->Get(a, global_only).has_value());
    // Update/delete are scope-checked too.
    CHECK(fx.store->Delete(b, From()) == WriteOutcome::kNotFound);
}

TEST_CASE("memory store: update keeps id and created_at, re-queues embed and tag", "[memory]") {
    Fixture fx("update");
    const auto id = fx.store->Insert(Entry("Old", "old description", "old body"));
    fx.store->StoreEmbedding(id, Embedding{HashVector("x"), kDim}, "sig");
    fx.store->SetEntryTags(id, {});
    const auto before = fx.store->Get(id, From());
    REQUIRE(fx.store->PendingEmbeddings(10).empty());
    REQUIRE(fx.store->PendingTagging(10).empty());

    EntryPatch pin_only;
    pin_only.pinned = true;
    REQUIRE(fx.store->Update(id, pin_only, From()) == WriteOutcome::kDone);
    CHECK(fx.store->PendingEmbeddings(10).empty());  // no text change, no re-embed

    EntryPatch patch;
    patch.body = "new body";
    REQUIRE(fx.store->Update(id, patch, From()) == WriteOutcome::kDone);
    const auto after = fx.store->Get(id, From());
    CHECK(after->id == id);
    CHECK(after->created_ms == before->created_ms);
    CHECK(after->body == "new body");
    CHECK(after->title == "Old");
    CHECK(after->pinned);
    CHECK(fx.store->PendingEmbeddings(10).size() == 1);
    CHECK(fx.store->PendingTagging(10).size() == 1);
    CHECK(fx.store->Update("nope", patch, From()) == WriteOutcome::kNotFound);
}

// ----------------------------------------------------------------- lexical

TEST_CASE("memory lexical: natural-language queries with short words find ranked hits",
          "[memory]") {
    // v1 ANDed every query word through a trigram index, so "do"/"I" (under
    // three characters) matched nothing and the whole query returned zero.
    Fixture fx("lexical");
    const auto tests = fx.store->Insert(
        Entry("Running the tests", "How to run the test suite", "Use make check; it runs ctest."));
    fx.store->Insert(Entry("Deployment", "Shipping releases", "Deploy via make release."));
    fx.store->Insert(Entry("Tea preference", "The user likes green tea", "Sencha, no sugar."));

    const auto hits = fx.store->Lexical("how do I run the tests", Filters(), 10);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.front().id == tests);

    // Prefix matching: "deploy" finds "Deployment".
    const auto deploy = fx.store->Lexical("deploy", Filters(), 10);
    REQUIRE(deploy.size() == 1);
    CHECK_THAT(deploy.front().snippet, ContainsSubstring("[Deploy]"));

    CHECK(fx.store->Lexical("kubernetes", Filters(), 10).empty());
}

TEST_CASE("memory lexical: a title hit outranks a body-only hit", "[memory]") {
    Fixture fx("weights");
    const auto body_only =
        fx.store->Insert(Entry("Misc notes", "assorted", "somewhere in here: gardening"));
    const auto titled = fx.store->Insert(Entry("Gardening", "plants and soil", "water weekly"));
    const auto hits = fx.store->Lexical("gardening", Filters(), 10);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == titled);
    CHECK(hits[1].id == body_only);
}

TEST_CASE("memory lexical: substring fallback for text without word boundaries", "[memory]") {
    Fixture fx("cjk");
    const auto id = fx.store->Insert(Entry("\xE6\x97\x85\xE8\xA1\x8C",
                                           "\xE6\x9D\xB1\xE4\xBA\xAC\xE3\x82\xBF\xE3\x83\xAF\xE3\x83\xBC",
                                           "\xE6\x9D\xB1\xE4\xBA\xAC\xE3\x82\xBF\xE3\x83\xAF\xE3\x83\xBC"));
    // unicode61 sees the whole run as one token, so a word query for a part
    // of it misses; the substring scan finds it.
    const auto hits = fx.store->Lexical("\xE6\x9D\xB1\xE4\xBA\xAC", Filters(), 10);
    REQUIRE(hits.size() == 1);
    CHECK(hits.front().id == id);
}

TEST_CASE("memory lexical: the tag filter; project_root never matches text", "[memory]") {
    Fixture fx("filters");
    const auto a = fx.store->Insert(Entry("Compose setup", "docker compose", "compose up -d"));
    const auto b = fx.store->Insert(Entry("Compose music", "writing songs", "compose a melody"));
    const auto tag = fx.store->CreateTag(kScopeProject, "/proj", "docker", "containers");
    fx.store->SetEntryTags(a, {tag});

    auto filters = Filters();
    filters.tags = {"Docker"};
    auto hits = fx.store->Lexical("compose", filters, 10);
    REQUIRE(hits.size() == 1);
    CHECK(hits.front().id == a);

    CHECK(fx.store->Lexical("compose", Filters(), 10).size() == 2);
    CHECK(fx.store->Lexical("melody", Filters(), 10).front().id == b);

    // A query that is only a substring of the project root matches nothing.
    CHECK(fx.store->Lexical("proj", Filters(), 10).empty());
}

// ------------------------------------------------------------------ hybrid

TEST_CASE("memory hybrid search: semantic hits without lexical overlap, fused with RRF",
          "[memory]") {
    Fixture fx("hybrid");
    FakeEmbedder* embedder = nullptr;
    auto service = FakeService(*fx.store, embedder);
    REQUIRE_FALSE(service->RunOnce());  // creates the backend; nothing pending yet
    REQUIRE(embedder != nullptr);
    embedder->concepts = {{"cat", Axis(0)}, {"feline", Axis(0)}};  // synonyms
    const auto feline = fx.store->Insert(Entry("Pets", "a feline rests on the rug", "whiskers"));
    const auto both = fx.store->Insert(Entry("Cat care", "the cat sat on the mat", "brush daily"));
    fx.store->Insert(Entry("Earnings", "quarterly report", "numbers"));
    REQUIRE(service->RunOnce());
    REQUIRE(fx.store->PendingEmbeddings(10).empty());

    SearchRequest request;
    request.query = "cat";
    request.filters = Filters();
    const auto results = HybridSearch(*fx.store, service.get(), request);
    REQUIRE(results.size() == 2);
    // Both lists agree on `both`; `feline` is found by meaning alone.
    CHECK(results[0].row.id == both);
    CHECK(results[0].score == 1.0);
    CHECK(results[1].row.id == feline);
    CHECK(results[1].semantic);

    // Without an embedder the same query is lexical only.
    const auto lexical = HybridSearch(*fx.store, nullptr, request);
    REQUIRE(lexical.size() == 1);
    CHECK(lexical[0].row.id == both);
}

TEST_CASE("memory hybrid search: browse lists pinned first, then most recent", "[memory]") {
    Fixture fx("browse");
    auto pinned = Entry("Pinned", "d", "b");
    pinned.pinned = true;
    const auto old_pinned = fx.store->Insert(pinned);
    const auto recent = fx.store->Insert(Entry("Recent", "d", "b"));
    SearchRequest request;
    request.filters = Filters();
    const auto results = HybridSearch(*fx.store, nullptr, request);
    REQUIRE(results.size() == 2);
    CHECK(results[0].row.id == old_pinned);
    CHECK(results[1].row.id == recent);
}

// ------------------------------------------------------- embedding service

TEST_CASE("memory embeddings: the breaker skips a dead backend after one failure", "[memory]") {
    Fixture fx("breaker");
    FakeEmbedder* embedder = nullptr;
    auto service = FakeService(*fx.store, embedder);
    fx.store->Insert(Entry("a", "b", "c"));
    REQUIRE(service->RunOnce());
    REQUIRE(fx.store->PendingEmbeddings(10).empty());

    embedder->down = true;
    fx.store->Insert(Entry("d", "e", "f"));
    const int before = embedder->calls;
    CHECK(service->RunOnce());  // batch fails, canary fails: an outage
    CHECK(fx.store->PendingEmbeddings(10).size() == 1);  // left pending, not failed
    CHECK(fx.store->Stats(From()).failed_embed == 0);
    const int after_outage = embedder->calls;
    CHECK(after_outage > before);

    // Breaker open: no further calls, no waiting -- lexical only.
    CHECK_FALSE(service->RunOnce());
    CHECK_FALSE(service->EmbedNow("query").has_value());
    CHECK(embedder->calls == after_outage);
    CHECK_THAT(service->Describe(), ContainsSubstring("unavailable"));
}

TEST_CASE("memory embeddings: one poison row is marked failed, its batch-mates embed",
          "[memory]") {
    Fixture fx("poison");
    FakeEmbedder* embedder = nullptr;
    auto service = FakeService(*fx.store, embedder);
    REQUIRE_FALSE(service->RunOnce());  // creates the backend; nothing pending
    REQUIRE(embedder != nullptr);
    embedder->poison = "POISON";
    fx.store->Insert(Entry("good one", "d", "b"));
    fx.store->Insert(Entry("bad POISON", "d", "b"));
    fx.store->Insert(Entry("good two", "d", "b"));
    REQUIRE(service->RunOnce());
    CHECK(fx.store->PendingEmbeddings(10).empty());
    CHECK(fx.store->Stats(From()).failed_embed == 1);
    CHECK_THAT(service->Describe(), !ContainsSubstring("unavailable"));
    CHECK(fx.store->ResetFailedEmbeddings() == 1);
}

TEST_CASE("memory embeddings: an unusable sidecar throws its reason", "[memory]") {
    MemorySettings settings;
    CHECK_THROWS_WITH(MakeSidecarEmbeddingFunction(settings, fs::temp_directory_path()),
                      ContainsSubstring("sidecar_model"));
    settings.sidecar_model = "some/model";
    settings.sidecar_python_path = "/definitely/not/a/real/python/binary";
    CHECK_THROWS(MakeSidecarEmbeddingFunction(settings, fs::temp_directory_path()));

    // The plan resolves without constructing anything.
    const auto plan = MakeEmbeddingPlan(settings, fs::temp_directory_path());
    CHECK(plan.strategy == "sidecar");
    CHECK(plan.signature == "sidecar:some/model");
    CHECK(MakeEmbeddingPlan(MemorySettings{}, fs::temp_directory_path()).strategy == "none");
}

TEST_CASE("memory embeddings: a malformed http key fails the plan; an unresolved one binds",
          "[memory]") {
    MemorySettings settings;
    settings.api_base = "http://127.0.0.1:1/v1";
    settings.api_key = "secrets:typo";
    CHECK_THROWS_WITH(MakeEmbeddingPlan(settings, fs::temp_directory_path()),
                      ContainsSubstring("unknown key reference scheme"));
    // Not resolvable YET binds and fails per request instead: a secret not
    // stored, or an unset env variable.
    settings.api_key = "secret:not-stored-yet";
    CHECK(MakeEmbeddingPlan(settings, fs::temp_directory_path()).strategy == "http");
    settings.api_key = "env:MEMORY_TEST_DEFINITELY_UNSET_KEY";
    CHECK(MakeEmbeddingPlan(settings, fs::temp_directory_path()).strategy == "http");
}

TEST_CASE("memory sidecar embedder: real fastembed round trip", "[memory][sidecar]") {
    const std::string python_path = minimal::sidecar::ResolveOnPath("python3");
    if (python_path.empty()) SKIP("python3 not available");
    const std::string check = python_path + " -c \"import fastembed\" >/dev/null 2>&1";
    if (std::system(check.c_str()) != 0) SKIP("fastembed not importable");

    MemorySettings settings;
    settings.sidecar_python_path = python_path;
    settings.sidecar_model = "BAAI/bge-small-en-v1.5";
    settings.sidecar_startup_timeout_ms = 120000;  // first run downloads the model
    const fs::path dir = fs::temp_directory_path() / ("memory-sidecar-" + std::to_string(getpid()));
    auto embedder = MakeSidecarEmbeddingFunction(settings, dir);
    const auto vectors = embedder->Embed(
        {"the cat sat on the mat", "a feline rested on a rug", "quarterly earnings report"});
    REQUIRE(vectors.size() == 3);
    auto cosine = [](const std::vector<float>& a, const std::vector<float>& b) {
        double dot = 0, na = 0, nb = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
        }
        return dot / (std::sqrt(na) * std::sqrt(nb));
    };
    CHECK(cosine(vectors[0].values, vectors[1].values) > cosine(vectors[0].values, vectors[2].values));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------ ingest

TEST_CASE("memory ingest: headings inside code fences do not split", "[memory]") {
    const std::string doc =
        "# Build\n"
        "Run the build like this.\n"
        "```bash\n"
        "# configure first\n"
        "cmake -B build\n"
        "## not a heading either\n"
        "```\n"
        "## Staging\n"
        "Deploy to staging first.\n";
    const auto chunks = ChunkMarkdown(doc, "notes", "", 8192);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0].title == "Build");
    CHECK_THAT(chunks[0].body, ContainsSubstring("# configure first"));
    CHECK_THAT(chunks[0].body, ContainsSubstring("## not a heading either"));
    CHECK(chunks[0].description == "Run the build like this.");
    CHECK(chunks[1].title == "Build > Staging");  // breadcrumb
    CHECK(chunks[1].description == "Deploy to staging first.");
}

TEST_CASE("memory ingest: front matter, preamble, heading-only sections", "[memory]") {
    const std::string doc =
        "---\n"
        "title: Garden journal\n"
        "description: What grows where\n"
        "tags: [ignored]\n"
        "---\n"
        "Intro before any heading. More text.\n"
        "# Beds\n"
        "## North bed\n"
        "- Tomatoes do well here.\n";
    const auto chunks = ChunkMarkdown(doc, "file-stem", "", 8192);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0].title == "Garden journal");
    CHECK(chunks[0].description == "Intro before any heading.");
    // "# Beds" has no body of its own: it lives on in the breadcrumb.
    CHECK(chunks[1].title == "Beds > North bed");
    CHECK(chunks[1].description == "Tomatoes do well here.");

    // One chunk: the document description wins; the tool argument is the
    // fallback when the file has none.
    const auto single = ChunkMarkdown("---\ndescription: Doc level\n---\nJust text.\n", "stem", "", 8192);
    REQUIRE(single.size() == 1);
    CHECK(single[0].title == "stem");
    CHECK(single[0].description == "Doc level");
    CHECK(ChunkMarkdown("Just text.\n", "stem", "From the tool", 8192)[0].description ==
          "From the tool");

    CHECK_THROWS(ChunkMarkdown("# Only a heading\n", "stem", "", 8192));
}

TEST_CASE("memory ingest: an oversized section splits on paragraphs, never inside a fence",
          "[memory]") {
    std::string doc = "# Big\n";
    for (int i = 0; i < 6; ++i) doc += "Paragraph " + std::to_string(i) + " " + std::string(60, 'x') + "\n\n";
    doc += "```\n" + std::string(40, 'a') + "\n\n" + std::string(40, 'b') + "\n```\n";
    const auto chunks = ChunkMarkdown(doc, "stem", "", 200);
    REQUIRE(chunks.size() > 1);
    bool fence_whole = false;
    for (const auto& chunk : chunks) {
        CHECK(chunk.body.size() <= 200);
        CHECK_THAT(chunk.title, ContainsSubstring("Big (part "));
        if (chunk.body.find("```") != std::string::npos) {
            fence_whole = chunk.body.find(std::string(40, 'a')) != std::string::npos &&
                          chunk.body.find(std::string(40, 'b')) != std::string::npos;
        }
    }
    CHECK(fence_whole);
}

TEST_CASE("memory ingest: re-ingest replaces; chunks refuse delete/update; group delete",
          "[memory]") {
    Fixture fx("ingest");
    auto chunk = [](std::string title) { return Entry(std::move(title), "d", "b"); };
    const auto first = fx.store->Ingest({chunk("A"), chunk("B")}, "/proj/notes.md");
    CHECK(first.replaced == 0);
    const auto second = fx.store->Ingest({chunk("A2"), chunk("B2"), chunk("C2")}, "/proj/notes.md");
    CHECK(second.replaced == 2);
    CHECK_FALSE(fx.store->Get(first.ids[0], From()).has_value());

    const auto row = fx.store->Get(second.ids[1], From());
    REQUIRE(row);
    CHECK(row->group_id == second.group_id);
    CHECK(row->chunk_index == 1);
    CHECK(row->chunk_count == 3);
    CHECK(row->origin == "ingest");
    CHECK(row->origin_ref == "/proj/notes.md");

    CHECK(fx.store->Delete(second.ids[0], From()) == WriteOutcome::kIngestChunk);
    EntryPatch patch;
    patch.body = "x";
    CHECK(fx.store->Update(second.ids[0], patch, From()) == WriteOutcome::kIngestChunk);

    CHECK(fx.store->DeleteGroup("/proj/notes.md", From("/elsewhere")) == 0);  // not visible
    CHECK(fx.store->DeleteGroup(second.group_id, From()) == 3);
    CHECK(fx.store->Stats(From()).entries_by_scope.empty());
}

// ------------------------------------------------------------------ tagger

namespace {

struct TaggerFixture {
    Fixture fx;
    MemorySettings settings;
    std::shared_ptr<minimal::testing::FakeLlmProvider> provider =
        std::make_shared<minimal::testing::FakeLlmProvider>();
    std::unique_ptr<EmbeddingService> service;
    FakeEmbedder* embedder = nullptr;
    std::unique_ptr<Tagger> tagger;

    explicit TaggerFixture(const std::string& name, bool with_embeddings = false) : fx(name) {
        settings.tagger_model = "fake/tagger";
        if (with_embeddings) {
            service = FakeService(*fx.store, embedder);
            service->RunOnce();  // creates the backend
        }
        Build();
    }
    void Build() {
        tagger = std::make_unique<Tagger>(
            settings, fx.store.get(), service.get(),
            [this]() -> std::optional<TaggerLlm> { return TaggerLlm{provider, "tagger-model"}; },
            minimal::common::DefaultLogger());
    }
    void Reply(const nlohmann::json& body) {
        // Fenced, as models do despite instructions.
        provider->PushResponse(minimal::testing::TextResponse("```json\n" + body.dump() + "\n```"));
    }
};

}  // namespace

TEST_CASE("memory tagger: files entries, reuses existing tags, creates described new ones",
          "[memory]") {
    TaggerFixture t("tagger");
    const auto existing = t.fx.store->CreateTag(kScopeProject, "/proj", "cooking", "recipes and food");
    const auto seed = t.fx.store->Insert(Entry("Pasta", "how to cook pasta", "salt the water"));
    t.fx.store->SetEntryTags(seed, {existing});
    const auto id = t.fx.store->Insert(Entry("Risotto", "stir constantly", "arborio rice"));

    t.Reply({{"entries",
              {{{"n", 1},
                {"tags", {"Cooking"}},
                {"new", {{{"name", "Italian Food"}, {"description", "dishes from Italy"}}}}}}}});
    REQUIRE(t.tagger->RunOnce());

    const auto row = t.fx.store->Get(id, From());
    CHECK(row->tags == std::vector<std::string>{"cooking", "italian-food"});
    const auto created = t.fx.store->FindTag(kScopeProject, "/proj", "italian food");
    REQUIRE(created);
    CHECK(created->description == "dishes from Italy");
    CHECK(t.fx.store->PendingTagging(10).empty());

    // The prompt carried the vocabulary and the full body.
    REQUIRE(t.provider->requests().size() == 1);
    const auto& request = t.provider->requests()[0];
    CHECK_THAT(PromptOf(request), ContainsSubstring("- cooking: recipes and food [1]"));
    CHECK_THAT(PromptOf(request), ContainsSubstring("arborio rice"));
    CHECK(request.model == "tagger-model");
    CHECK_FALSE(t.tagger->RunOnce());  // nothing left
}

TEST_CASE("memory tagger: similar entries' tags are offered as a hint", "[memory]") {
    TaggerFixture t("tagger-neighbors");
    const auto tag = t.fx.store->CreateTag(kScopeProject, "/proj", "gardening", "plants");
    const auto seed = t.fx.store->Insert(Entry("Tomatoes", "tomato watering schedule", "daily"));
    t.fx.store->SetEntryTags(seed, {tag});
    t.fx.store->Insert(Entry("Tomato pests", "tomato hornworms", "pick them off"));
    t.Reply({{"entries", {{{"n", 1}, {"tags", {"gardening"}}}}}});
    REQUIRE(t.tagger->RunOnce());
    CHECK_THAT(PromptOf(t.provider->requests()[0]),
               ContainsSubstring("Similar entries were tagged: gardening"));
}

TEST_CASE("memory tagger: a new tag that means an existing one maps onto it", "[memory]") {
    TaggerFixture t("tagger-dedup", /*with_embeddings=*/true);
    REQUIRE(t.embedder != nullptr);
    // The fake's notion of synonyms: anything mentioning "verification".
    t.embedder->concepts = {{"verification", Axis(3)}};
    const auto existing =
        t.fx.store->CreateTag(kScopeProject, "/proj", "qa", "testing and verification");
    t.fx.store->SetEntryTags(t.fx.store->Insert(Entry("x", "y", "z")), {existing});
    const auto id = t.fx.store->Insert(Entry("CI flakes", "flaky tests", "retry"));
    t.Reply({{"entries",
              {{{"n", 1},
                {"new", {{{"name", "checks"}, {"description", "verification of the code"}},
                         {{"name", "db"}, {"description", "databases"}}}}}}}});
    REQUIRE(t.tagger->RunOnce());
    // "checks" folded into qa; "db" -- two edits from "qa" -- stays its own tag.
    CHECK(t.fx.store->Get(id, From())->tags == std::vector<std::string>{"db", "qa"});
    CHECK_FALSE(t.fx.store->FindTag(kScopeProject, "/proj", "checks").has_value());
}

TEST_CASE("memory tagger: at the cap only existing tags apply", "[memory]") {
    TaggerFixture t("tagger-cap");
    t.settings.max_tags = 1;
    t.Build();
    const auto only = t.fx.store->CreateTag(kScopeProject, "/proj", "travel", "trips");
    t.fx.store->SetEntryTags(t.fx.store->Insert(Entry("a", "b", "c")), {only});
    const auto id = t.fx.store->Insert(Entry("Lisbon", "trip notes", "trams"));
    t.Reply({{"entries",
              {{{"n", 1},
                {"tags", {"travel"}},
                {"new", {{{"name", "portugal"}, {"description", "pt"}}}}}}}});
    REQUIRE(t.tagger->RunOnce());
    CHECK(t.fx.store->Get(id, From())->tags == std::vector<std::string>{"travel"});
    CHECK(t.fx.store->TagCount(kScopeProject, "/proj") == 1);
    CHECK_THAT(PromptOf(t.provider->requests()[0]), ContainsSubstring("vocabulary is FULL"));
}

TEST_CASE("memory tagger: each ingest chunk gets its own tags", "[memory]") {
    TaggerFixture t("tagger-chunks");
    const auto ingest =
        t.fx.store->Ingest({Entry("Soup", "a soup", "broth"), Entry("Taxes", "tax filing", "forms")},
                           "/proj/misc.md");
    t.Reply({{"entries",
              {{{"n", 1}, {"new", {{{"name", "cooking"}, {"description", "food"}}}}},
               {{"n", 2}, {"new", {{{"name", "finance"}, {"description", "money"}}}}}}}});
    REQUIRE(t.tagger->RunOnce());
    CHECK(t.fx.store->Get(ingest.ids[0], From())->tags == std::vector<std::string>{"cooking"});
    CHECK(t.fx.store->Get(ingest.ids[1], From())->tags == std::vector<std::string>{"finance"});
    CHECK(t.provider->requests().size() == 1);  // one call for the batch
}

TEST_CASE("memory tagger: malformed output twice skips the batch, no retry loop", "[memory]") {
    TaggerFixture t("tagger-malformed");
    t.fx.store->Insert(Entry("a", "b", "c"));
    t.provider->PushResponse(minimal::testing::TextResponse("no json here"));
    t.provider->PushResponse(minimal::testing::TextResponse("{still: not json"));
    REQUIRE(t.tagger->RunOnce());
    CHECK(t.provider->requests().size() == 2);
    CHECK(t.fx.store->PendingTagging(10).empty());
    CHECK_FALSE(t.tagger->RunOnce());
}

TEST_CASE("memory tagger: provider errors leave rows pending; no model = disabled", "[memory]") {
    TaggerFixture t("tagger-errors");
    t.fx.store->Insert(Entry("a", "b", "c"));
    t.provider->PushError(minimal::llm::LlmError(minimal::llm::LlmErrorCategory::Transport, "boom"));
    CHECK_FALSE(t.tagger->RunOnce());
    CHECK(t.fx.store->PendingTagging(10).size() == 1);
    CHECK_THAT(t.tagger->Describe(), ContainsSubstring("paused"));

    t.settings.tagger_model.clear();
    t.Build();
    CHECK_FALSE(t.tagger->enabled());
    CHECK_FALSE(t.tagger->RunOnce());
}

TEST_CASE("memory tagger: suggest-merges keeps only real pairs", "[memory]") {
    TaggerFixture t("tagger-merges");
    for (const char* name : {"recipes", "cooking", "travel"}) {
        const auto tag = t.fx.store->CreateTag(kScopeProject, "/proj", name, "");
        t.fx.store->SetEntryTags(t.fx.store->Insert(Entry(name, "d", "b")), {tag});
    }
    t.provider->PushResponse(minimal::testing::TextResponse(
        nlohmann::json{{"merges",
                        {{{"from", "recipes"}, {"to", "cooking"}, {"why", "same subject"}},
                         {{"from", "invented"}, {"to", "cooking"}}}}}
            .dump()));
    const auto merges = t.tagger->SuggestMerges(kScopeProject, "/proj");
    REQUIRE(merges.size() == 1);
    CHECK(merges[0].from == "recipes");
    CHECK(merges[0].to == "cooking");
}

// ------------------------------------------------------ runtime + tools

namespace {

struct RuntimeFixture {
    Fixture fx;
    std::shared_ptr<MemoryRuntime> rt = std::make_shared<MemoryRuntime>();
    fs::path project;

    explicit RuntimeFixture(const std::string& name, MemorySettings settings = {})
        : fx(name), project(fx.dir / "project") {
        fs::create_directories(project / ".git");
        fs::create_directories(project / "sub");
        rt->settings = settings;
        rt->store = std::move(fx.store);
        rt->embeddings = std::make_unique<EmbeddingService>(rt->store.get(), EmbeddingPlan{},
                                                            minimal::common::DefaultLogger());
        rt->tagger = std::make_unique<Tagger>(rt->settings, rt->store.get(), rt->embeddings.get(),
                                              Tagger::Resolver{}, minimal::common::DefaultLogger());
        std::call_once(rt->workers_started, [] {});  // tests drive workers by hand
    }
    ~RuntimeFixture() { rt.reset(); }

    minimal::tools::ToolResult Run(minimal::tools::Tool& tool, const nlohmann::json& input,
                                   fs::path root = {}) {
        minimal::tools::ToolContext ctx{minimal::common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = root.empty() ? project / "sub" : root;
        return tool.execute(input, ctx);
    }
};

nlohmann::json Parse(const minimal::tools::ToolResult& result) {
    return nlohmann::json::parse(result.content_text);
}

}  // namespace

TEST_CASE("memory tools: store, search, get, update, delete end to end", "[memory]") {
    RuntimeFixture f("tools");
    auto store = MakeMemoryStoreTool(f.rt);
    auto search = MakeMemorySearchTool(f.rt);
    auto get = MakeMemoryGetTool(f.rt);
    auto update = MakeMemoryUpdateTool(f.rt);
    auto del = MakeMemoryDeleteTool(f.rt);
    CHECK(store->spec().category == minimal::tools::ToolCategory::FileWrite);
    CHECK(search->spec().category == minimal::tools::ToolCategory::ReadOnly);

    // Validation errors the model can act on.
    auto result = f.Run(*store, {{"scope", "project"}, {"title", "T"}, {"body", "b"}});
    CHECK(result.is_error);
    CHECK_THAT(result.content_text, ContainsSubstring("description is empty"));
    result = f.Run(*store, {{"scope", "project"}, {"title", "T"}, {"description", "d"},
                            {"body", "b"}, {"path", "x.md"}});
    CHECK(result.is_error);

    result = f.Run(*store, {{"scope", "project"},
                            {"title", "Build command"},
                            {"description", "How this project is built"},
                            {"body", "Run make -j8 from the root."}});
    REQUIRE_FALSE(result.is_error);
    const std::string id = Parse(result)["id"];

    // Same title, same scope: flagged as a likely duplicate.
    result = f.Run(*store, {{"scope", "project"},
                            {"title", "Build command"},
                            {"description", "Building"},
                            {"body", "make"}});
    REQUIRE_FALSE(result.is_error);
    const auto dup = Parse(result);
    REQUIRE(dup.contains("similar"));
    CHECK(dup["similar"]["id"] == id);
    const std::string dup_id = dup["id"];

    result = f.Run(*store, {{"scope", "global"},
                            {"title", "Metric units"},
                            {"description", "The user prefers metric units"},
                            {"body", "Always convert to metric."}});
    REQUIRE_FALSE(result.is_error);

    // Search defaults to project + global, scoped to the worktree root even
    // from a subdirectory.
    result = f.Run(*search, {{"query", "how do I build it"}});
    REQUIRE_FALSE(result.is_error);
    auto hits = Parse(result)["results"];
    REQUIRE(hits.size() == 2);
    CHECK(hits[0]["title"] == "Build command");
    CHECK(Parse(f.Run(*search, {{"query", "units"}, {"scope", "project"}}))["results"].empty());
    CHECK(Parse(f.Run(*search, {{"query", "units"}}))["results"].size() == 1);
    // Another project sees only the global entry.
    const fs::path other = f.fx.dir / "other";
    fs::create_directories(other);
    hits = Parse(f.Run(*search, nlohmann::json::object(), other))["results"];
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]["scope"] == "global");

    result = f.Run(*get, {{"ids", {id, "missing"}}});
    REQUIRE_FALSE(result.is_error);
    const auto got = Parse(result);
    CHECK(got["entries"][0]["body"] == "Run make -j8 from the root.");
    CHECK(got["not_found"] == nlohmann::json{"missing"});

    result = f.Run(*update, {{"id", id}, {"body", "Run make -j16."}, {"pinned", true}});
    REQUIRE_FALSE(result.is_error);
    CHECK(f.rt->store->Get(id, VisibleFrom(f.project))->body == "Run make -j16.");

    REQUIRE_FALSE(f.Run(*del, {{"id", dup_id}}).is_error);
    CHECK(f.Run(*del, {{"id", dup_id}}).is_error);
}

TEST_CASE("memory tools: store by path ingests, re-stores replace, chunk delete points at the group",
          "[memory]") {
    RuntimeFixture f("tools-path");
    auto store = MakeMemoryStoreTool(f.rt);
    auto del = MakeMemoryDeleteTool(f.rt);
    std::ofstream(f.project / "notes.md") << "# One\nfirst part\n# Two\nsecond part\n";
    auto result = f.Run(*store, {{"scope", "project"}, {"path", "../notes.md"}});
    REQUIRE_FALSE(result.is_error);
    CHECK(Parse(result)["entries"] == nlohmann::json{"One", "Two"});

    result = f.Run(*store, {{"scope", "project"}, {"path", "../notes.md"}});
    REQUIRE_FALSE(result.is_error);
    const auto body = Parse(result);
    CHECK(body["replaced"] == 2);
    const std::string group = body["group_id"];

    const auto ids = f.rt->store->Browse(Filters(WorktreeRoot(f.project).string()), 10);
    REQUIRE(ids.size() == 2);
    result = f.Run(*del, {{"id", ids[0]}});
    CHECK(result.is_error);
    CHECK_THAT(result.content_text, ContainsSubstring(group));
    result = f.Run(*del, {{"group_id", group}});
    REQUIRE_FALSE(result.is_error);
    CHECK(Parse(result)["deleted"] == 2);
}

TEST_CASE("memory tools: allow_inline=false and writes_prompt=false", "[memory]") {
    MemorySettings settings;
    settings.allow_inline = false;
    settings.writes_prompt = false;
    RuntimeFixture f("tools-modes", settings);
    auto store = MakeMemoryStoreTool(f.rt);
    CHECK(store->spec().category == minimal::tools::ToolCategory::ReadOnly);
    CHECK_FALSE(store->spec().definition.input_schema["properties"].contains("body"));
    const auto result = f.Run(*store, {{"scope", "project"}, {"title", "t"}, {"description", "d"},
                                       {"body", "b"}});
    CHECK(result.is_error);
    CHECK_THAT(result.content_text, ContainsSubstring("allow_inline"));
}

// ---------------------------------------------------------- context index

TEST_CASE("memory context index: counts, pinned bodies, tags, entries, no ids", "[memory]") {
    RuntimeFixture f("index");
    const std::string root = WorktreeRoot(f.project).string();
    CHECK(RenderMemoryIndex(*f.rt, f.project) == "No memories stored yet.\n");

    auto pinned = Entry("Standing preference", "how to address the user", "Call them Sam.",
                        kScopeGlobal);
    pinned.pinned = true;
    f.rt->store->Insert(pinned);
    const auto id = f.rt->store->Insert(Entry("Build", "how to build", "make", kScopeProject, root));
    const auto tag = f.rt->store->CreateTag(kScopeProject, root, "tooling", "build tools");
    f.rt->store->SetEntryTags(id, {tag});

    const std::string index = RenderMemoryIndex(*f.rt, f.project);
    CHECK_THAT(index, ContainsSubstring("1 project and 1 global entries"));
    CHECK_THAT(index, ContainsSubstring("### Standing preference (global)\nCall them Sam."));
    CHECK_THAT(index, ContainsSubstring("\nTags: tooling(1)\n"));
    // Everything fit: no "of N", no fetch hints, no ids.
    CHECK_THAT(index, ContainsSubstring("\nEntries:\n- Build -- how to build\n"));
    CHECK_THAT(index, !ContainsSubstring("more"));
    CHECK_THAT(index, !ContainsSubstring(id));
    CHECK_THAT(index, !ContainsSubstring("- Standing preference"));  // pinned: not listed twice

    // The contributor: skipped for subagents by default.
    auto contributor = MakeMemoryIndexContributor(f.rt);
    minimal::agent::ContextRequest request;
    request.project_root = f.project;
    const auto section = contributor->Contribute(request);
    REQUIRE(section);
    CHECK(section->title == "Memory");
    request.is_subagent = true;
    CHECK_FALSE(contributor->Contribute(request).has_value());
}

TEST_CASE("memory context index: over budget, every omission is announced with its fetch path",
          "[memory]") {
    MemorySettings settings;
    settings.index_budget_bytes = 900;
    RuntimeFixture f("index-overflow", settings);
    const std::string root = WorktreeRoot(f.project).string();
    for (int i = 0; i < 8; ++i) {
        auto e = Entry("Pinned " + std::to_string(i), "pinned description " + std::to_string(i),
                       std::string(120, 'x'), kScopeGlobal);
        e.pinned = true;
        f.rt->store->Insert(e);
    }
    for (int i = 0; i < 40; ++i) {
        const auto id = f.rt->store->Insert(Entry("Entry " + std::to_string(i),
                                                  "an ordinary description", "b", kScopeProject, root));
        const auto tag = f.rt->store->CreateTag(kScopeProject, root, "tag-number-" + std::to_string(i), "");
        f.rt->store->SetEntryTags(id, {tag});
    }
    const std::string index = RenderMemoryIndex(*f.rt, f.project);
    CHECK(index.size() <= 900);
    CHECK_THAT(index, ContainsSubstring("40 project and 8 global entries"));
    CHECK_THAT(index, ContainsSubstring("### Pinned "));                         // some in full
    CHECK_THAT(index, ContainsSubstring("more pinned; memory_search with no query lists pinned"));
    CHECK_THAT(index, ContainsSubstring("more; memory_search with no query lists all tags"));
    CHECK_THAT(index, ContainsSubstring(" of 40; memory_search finds the rest)"));
}

TEST_CASE("memory tools: browsing returns the full tag vocabulary", "[memory]") {
    RuntimeFixture f("browse-tags");
    const std::string root = WorktreeRoot(f.project).string();
    const auto a = f.rt->store->Insert(Entry("A", "d", "b", kScopeProject, root));
    const auto g = f.rt->store->Insert(Entry("G", "d", "b", kScopeGlobal));
    f.rt->store->SetEntryTags(a, {f.rt->store->CreateTag(kScopeProject, root, "shared", "")});
    f.rt->store->SetEntryTags(g, {f.rt->store->CreateTag(kScopeGlobal, "", "shared", ""),
                                  f.rt->store->CreateTag(kScopeGlobal, "", "global-only", "")});
    auto search = MakeMemorySearchTool(f.rt);
    const auto browse = Parse(f.Run(*search, nlohmann::json::object()));
    CHECK(browse["tags"] == nlohmann::json{{"global-only", 1}, {"shared", 2}});
    // A query is a search, not a browse: no vocabulary attached.
    CHECK_FALSE(Parse(f.Run(*search, {{"query", "A"}})).contains("tags"));
}

TEST_CASE("memory workers: background embedding and tagging, prompt shutdown", "[memory]") {
    // The real threads, not RunOnce: rows stored after Start() get embedded
    // and tagged without anyone calling in, and destruction joins promptly.
    TaggerFixture t("workers");
    FakeEmbedder* embedder = nullptr;
    t.service = FakeService(*t.fx.store, embedder);
    t.Build();
    t.Reply({{"entries", {{{"n", 1}, {"new", {{{"name", "misc"}, {"description", "anything"}}}}}}}});
    t.service->Start();
    t.tagger->Start();
    const auto id = t.fx.store->Insert(Entry("Background", "stored after start", "body"));
    t.service->Notify();
    t.tagger->Notify();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           (!t.fx.store->PendingEmbeddings(10).empty() || !t.fx.store->PendingTagging(10).empty())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(t.fx.store->PendingEmbeddings(10).empty());
    CHECK(t.fx.store->Get(id, From())->tags == std::vector<std::string>{"misc"});

    const auto stop_start = std::chrono::steady_clock::now();
    t.tagger.reset();
    t.service.reset();
    CHECK(std::chrono::steady_clock::now() - stop_start < std::chrono::seconds(2));
}

TEST_CASE("memory runtime: a store that cannot open degrades instead of throwing", "[memory]") {
    // A bundled plugin whose registration throws aborts server startup, so a
    // bad store must come back as null + reason, never as an exception.
    const fs::path dir = fs::temp_directory_path() / ("memory-degrade-" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::string error;
    auto ok = BuildMemoryRuntime(nlohmann::json::object(), dir, nullptr, error);
    REQUIRE(ok != nullptr);
    CHECK(error.empty());
    ok.reset();

    SECTION("written by a newer binary") {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open((dir / "memory.sqlite3").string().c_str(), &db) == SQLITE_OK);
        REQUIRE(sqlite3_exec(db, "PRAGMA user_version = 99;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        CHECK(BuildMemoryRuntime(nlohmann::json::object(), dir, nullptr, error) == nullptr);
        CHECK_THAT(error, ContainsSubstring("newer"));
    }
    SECTION("not a database at all") {
        fs::remove(dir / "memory.sqlite3-wal");
        fs::remove(dir / "memory.sqlite3-shm");
        std::ofstream(dir / "memory.sqlite3", std::ios::trunc) << std::string(4096, 'z');
        CHECK(BuildMemoryRuntime(nlohmann::json::object(), dir, nullptr, error) == nullptr);
        CHECK_FALSE(error.empty());
    }

    const auto stub = MakeMemoryUnavailableCommand("some reason");
    CHECK(stub->Name() == "memory");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

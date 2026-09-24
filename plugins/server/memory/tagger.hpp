#pragma once

// Tagger -- files entries into each scope's controlled tag vocabulary using a
// cheap model, so the main model never spends turns or tokens on tags.
//
// Work queue = the database: entries with tag_status = 0. That makes tagging
// survive restarts, backfill entries stored while tagging was off, and
// re-file entries whose text was edited -- all with one query and no
// in-memory queue to lose. One owned worker thread; a circuit breaker keeps a
// down provider from being hammered.
//
// Per batch (entries of ONE scope), one provider call sees:
//   - each entry's full title, description and body (bodies are already
//     capped by max_text_bytes),
//   - the scope's whole vocabulary with each tag's description (bounded by
//     the max_tags cap),
//   - for each entry, the tags of its most similar already-tagged entries --
//     the strongest guard against inventing a synonym of an existing tag.
// It answers per entry: existing tags, plus new tags with descriptions.
//
// New tags are checked before creation, with no edit distance (on short
// names it merges unrelated tags -- "qa" and "db" are two edits apart):
//   1. normalized match key (case, separators, plural) -> the existing tag;
//   2. with an embedder, cosine of "name: description" against existing
//      tags >= tag_merge_similarity -> the existing tag;
//   3. at the vocabulary cap, new tags are dropped.
// Every failure degrades silently: the entry stays untagged and fully
// searchable. Malformed output twice marks the batch skipped (tag_status 2)
// rather than retrying forever.

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "embedding_service.hpp"
#include "memory_settings.hpp"
#include "memory_store.hpp"

namespace minimal::common {
class Logger;
}
namespace minimal::llm {
class LlmProvider;
}

namespace minimal::memory {

struct TaggerLlm {
    std::shared_ptr<llm::LlmProvider> provider;
    std::string model;
};

class Tagger {
public:
    // Resolves settings.tagger_model to a provider. Called lazily on the
    // worker (never during plugin registration: core services are wired
    // after plugins load). nullopt = cannot resolve right now.
    using Resolver = std::function<std::optional<TaggerLlm>()>;

    Tagger(MemorySettings settings, MemoryStore* store, EmbeddingService* embeddings,
           Resolver resolver, common::Logger& logger);
    ~Tagger();

    Tagger(const Tagger&) = delete;
    Tagger& operator=(const Tagger&) = delete;

    bool enabled() const { return !settings_.tagger_model.empty(); }
    void Start();
    void Notify();

    // One batch, synchronously -- the testing seam. True when it did work.
    bool RunOnce();

    std::string Describe() const;

    // /memory tags suggest-merges: asks the tagger model which tags of one
    // scope's vocabulary mean the same thing. Synchronous (the command
    // thread); proposes only -- the operator applies with `tags merge`.
    // Throws std::runtime_error when tagging is off, the model cannot be
    // resolved, or the call fails.
    struct MergeSuggestion {
        std::string from, to, why;
    };
    std::vector<MergeSuggestion> SuggestMerges(int scope, const std::string& project_root);

private:
    void WorkerLoop();
    std::optional<TaggerLlm> Llm();
    std::vector<std::string> NeighborTags(const PendingTagRow& row);
    std::optional<std::int64_t> ResolveNewTag(int scope, const std::string& project_root,
                                              const std::string& name,
                                              const std::string& description);
    std::optional<std::vector<float>> TagVector(std::int64_t tag_id, const std::string& text);

    MemorySettings settings_;
    MemoryStore* store_;
    EmbeddingService* embeddings_;  // may be null
    Resolver resolver_;
    common::Logger& logger_;
    CircuitBreaker breaker_;

    std::mutex llm_mu_;
    std::optional<TaggerLlm> llm_;

    std::map<std::int64_t, std::vector<float>> tag_vectors_;  // worker-thread only

    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool notified_ = false;
    std::thread worker_;
};

// Pulls the first parseable {...} object out of a model reply -- models add
// prose or code fences despite instructions.
std::optional<std::string> ExtractJsonObjectText(std::string_view text);

}  // namespace minimal::memory

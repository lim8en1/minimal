#pragma once

// Enricher -- the small-LLM pipeline: one-sentence summary + topic
// attribution per stored entry. The seam is SessionSupportService
// (ResolveModelRef), the exact one session-titler uses, including the
// detached-worker discipline: resolve provider/model while the session
// exists (the TOOL thread does the resolution and hands it to SetLlm), run
// the unbounded provider call detached, drain on destruction.
//
// FAILURE IS ALWAYS SILENT-DEGRADE: malformed JSON after one retry, provider
// error, unresolvable model ref -- the entry keeps its truncation preview and
// stays unfiled. Never a failed store, never a surfaced error: enrichment is
// garnish on a fully-functional store.
//
// The summarizer resolves "<provider>/<model>" through providers.json. EMPTY
// config = enrichment off (the inversion of the titler's empty-means-session-
// model: titling is the titler's whole job; this is optional).

#include <memory>
#include <string>
#include <vector>

#include "memory_settings.hpp"

namespace minimal::common {
class Logger;
}

namespace minimal::llm {
class LlmProvider;
}

namespace minimal::memory {

class MemoryStore;

class Enricher {
public:
    Enricher(MemorySettings settings, MemoryStore* store);
    ~Enricher();
    // The tool layer resolves the model ref through SessionSupportService on
    // the tool thread (the session exists there) and hands it over here;
    // nullopt/empty clears -- enrichment silently degrades until a next
    // resolution. The shared_ptr keeps the provider alive for detached
    // workers.
    void SetLlm(std::shared_ptr<llm::LlmProvider> provider, std::string model);

    // Enqueue one entry for enrichment. Runs detached; safe from any thread.
    // `also_bind`: sibling entry IDs (an ingest group's other chunks) that
    // should receive the SAME summary/topic once resolved, without an extra
    // provider call each -- the sample this asks about (entry_id) already
    // stands in for the whole group.
    void Enqueue(std::string entry_id, std::string text, int scope, std::string project_root,
                std::vector<std::string> also_bind = {});

    // The job body, synchronous -- the testing seam. One provider call:
    // strict-JSON {summary, topic}; retry once on malformed; attribute
    // (case-insensitive) / create-under-cap / null -> unfiled. Applied to
    // `entry_id` and every id in `also_bind` alike.
    static void RunEnrichment(const MemorySettings& settings, MemoryStore& store,
                              llm::LlmProvider* provider, const std::string& model,
                              common::Logger& logger, const std::string& entry_id,
                              const std::string& text, int scope,
                              const std::string& project_root,
                              const std::vector<std::string>& also_bind = {});

    // Drain: joins outstanding workers. Destructor calls it.
    void Drain();

    bool enabled() const { return !settings_.summarizer_model.empty(); }

private:
    MemorySettings settings_;
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace minimal::memory

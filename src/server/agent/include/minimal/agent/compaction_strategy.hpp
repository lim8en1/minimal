#pragma once

// S4: opens compaction strategy to plugins. Of every core-only subsystem,
// this is the one most worth it -- users genuinely differ (summarize /
// drop-oldest / tool-result-elide / external KV), and the deferred
// `llamacpp-kv` plugin needs exactly this seam.
//
// A "strategy" bundles the two independent halves already declared in
// context_compactor.hpp:
//   - CutSelector: proposes a candidate cut index over the working history.
//   - Compactor: summarizes the range core has ALREADY snapped to a safe
//     tool-call boundary (snap_compaction_boundary always runs, regardless
//     of which strategy proposed the candidate -- see agent_loop.cpp's
//     Compact()). A strategy cannot bypass that invariant even by mistake.
//
// Mirrors llm::ModelCatalogRegistry exactly: string-keyed, first
// registration wins, Register takes the value directly (no factory) because
// a strategy holds no per-instance state -- ProposeCut/Summarize take
// everything they need as parameters, the same statelessness contract
// LlmProvider and ModelCatalog already carry.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/agent/context_compactor.hpp"
#include "minimal/plugins/attributed_registry.hpp"

namespace minimal::agent {

struct CompactionStrategy {
    CutSelector cut_selector;
    Compactor summarize;
};

class CompactionStrategyRegistry : public plugins::AttributedRegistry<CompactionStrategy> {
public:
    // False if either half of `strategy` is unset or `kind` is already taken
    // (recorded in collisions()).
    bool Register(std::string kind, CompactionStrategy strategy);

    // nullptr when no plugin (or core) registered a strategy for this kind.
    // settings.json's memory.compaction_strategy naming an unregistered kind
    // is a startup sanity-check warning, not a crash -- the session falls
    // back to the built-in behavior (see agent_loop.hpp's LoopConfig comment).
    const CompactionStrategy* Find(std::string_view kind) const { return FindEntry(kind); }
};

// The built-in "summarize" strategy: today's production behavior.
// ProposeCut honors keep_recent_tokens (W11) when it is positive -- a
// token-budget walk from the tail, so a giant recent tool result is kept
// whole or pushes the cut before it, never split -- and falls back to plain
// history.size()/2 halving when it is 0 (the legacy default). Summarize is
// compact_messages, calling the provider with the preset's compression
// prompt. Registered by CORE in main.cpp, not a plugin, so the default path
// is never plugin-dependent.
CompactionStrategy MakeSummarizeCompactionStrategy();

// The cut-selection half of "summarize", exposed because the "structured"
// strategy shares it verbatim -- where to cut is orthogonal to how to
// summarize, and duplicating the walk would let the two drift.
std::size_t ProposeSummarizeCut(const std::vector<llm::Message>& history,
                                std::int64_t keep_recent_tokens);

}  // namespace minimal::agent

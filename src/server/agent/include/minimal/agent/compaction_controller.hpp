#pragma once

// Context compaction for the agent loop: WHEN to compact (MaybeCompact -- the
// trigger, the occupancy estimate, the two sanity guards), HOW (Compact -- resolve
// the summarizer, pick and snap the cut, elide or summarize, fold the working
// set, write the checkpoint, emit the event and hooks) and the record of a
// compaction that did not fold (the skipped notice). Owns the latches that stop a
// failing or ineffective compaction from re-running every tool round.
//
// Depends on the loop's working set, request builder, preset, config, store and
// provider by reference; the loop owns all of them and mutates the preset and
// provider in place, so the references see the current values. Worker-thread
// only.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/context_compactor.hpp"
#include "minimal/agent/loop_types.hpp"
#include "minimal/agent/request_builder.hpp"
#include "minimal/agent/working_history.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/session/history_store.hpp"

namespace minimal::agent {

class CompactionController {
public:
    CompactionController(const AgentPreset& preset, const LoopConfig& config,
                         const SessionContext& session, session::HistoryStore& store,
                         WorkingHistory& working, RequestBuilder& request,
                         const std::reference_wrapper<llm::LlmProvider>& provider,
                         Compactor compactor, const CompactionStrategyRegistry* strategies,
                         std::function<bool()> is_cancelled)
        : preset_(preset),
          config_(config),
          session_(session),
          store_(store),
          working_(working),
          request_(request),
          provider_(provider),
          compactor_(std::move(compactor)),
          compaction_strategies_(strategies),
          is_cancelled_(std::move(is_cancelled)) {}

    // A new turn: a compaction that failed during the previous one may be retried.
    void BeginTurn() { compaction_failed_this_turn_ = false; }

    // The occupancy-triggered entry point, called before each provider request.
    void MaybeCompact();

    // The shared body: every guard except the occupancy trigger, which is what
    // MaybeCompact adds and CompactNow deliberately skips. `trigger` names who
    // asked -- the pre/post_compact hook payloads carry it filterable, since
    // "auto compacted" and "the user ran /compact" are different events even
    // when the mechanics are identical. A non-empty `prompt_override` (/compact
    // <name>) replaces the preset's compression prompt for THIS invocation only;
    // auto-compact and bare /compact never pass one. Returns the number of
    // messages folded, or nullopt when nothing was compacted.
    std::optional<std::size_t> Compact(std::string_view trigger,
                                       std::string_view prompt_override = {});

    // Drops ThinkingBlocks from messages [first, last) -- the ones a compaction
    // kept verbatim -- when the provider rejects replaying thinking whose prefix
    // changed (LlmProvider::BindsThinkingToPrefix). Public because a checkpoint
    // RESUME reproduces the same post-fold state and calls it from the loop.
    void DropStaleThinking(std::size_t first, std::size_t last);

    // /compact: the TRIGGER is skipped -- the user asked, so occupancy,
    // auto_compact and the configured strategy are all beside the point.
    std::optional<std::size_t> CompactNow(std::string_view prompt_override = {});

    // Successful folds so far, any trigger. The one member read off the
    // worker: a /compact queued mid-turn records it on the reader, and is
    // skipped at its boundary if another compaction folded meanwhile.
    std::uint64_t folds() const { return folds_.load(std::memory_order_acquire); }

private:
    // Compact's phases, split out so each reads on its own.
    Compactor ResolveSummarizer();
    std::optional<std::size_t> ChooseCut(std::string_view trigger);
    // A compaction that did NOT fold (a guard stopped it): logged, sent to the
    // client as a skipped CompactionEvent, and recorded for scrollback.
    void EmitCompactionNotice(std::string_view trigger, std::string warning);

    const AgentPreset& preset_;
    const LoopConfig& config_;
    const SessionContext& session_;
    session::HistoryStore& store_;
    WorkingHistory& working_;
    RequestBuilder& request_;
    const std::reference_wrapper<llm::LlmProvider>& provider_;
    // The test/direct-API injection seam, and the registry presets resolve their
    // `compaction_strategy` name against at COMPACTION TIME (not construction time
    // -- a mid-session /agent switch must change the format the way it already
    // changes compression_prompt). Both may be null/empty: an injected compactor
    // wins outright (tests), and a null registry just skips the preset-name step.
    Compactor compactor_;
    const CompactionStrategyRegistry* compaction_strategies_ = nullptr;
    std::function<bool()> is_cancelled_;

    // Set when a compaction attempt fails, cleared at each turn: without it a
    // failing summarizer would be re-invoked on every tool cycle of the same
    // turn, resending half the context to the provider each time. The trigger
    // stays armed for the next turn, where the flag resets.
    bool compaction_failed_this_turn_ = false;
    std::atomic<std::uint64_t> folds_{0};
    // Warn-once latches: MaybeCompact runs on every tool cycle, and each
    // situation is one actionable line per session, not noise per cycle.
    bool compaction_trigger_warned_ = false;   // window-relative trigger, no window
    bool compaction_strategy_warned_ = false;  // preset names an unusable strategy
    bool compaction_floor_warned_ = false;     // trigger at/below the irreducible context
    // Auto-compaction sanity guards. `auto_compacted_last_check_`: the previous
    // trigger check compacted, so a trigger firing again at the very next check
    // means the fold cannot get the context under the trigger -- auto-compaction
    // halts (`auto_compaction_halted_`) with a notice instead of re-summarizing
    // every tool round, until occupancy drops below the trigger again.
    bool auto_compacted_last_check_ = false;
    bool auto_compaction_halted_ = false;
};

}  // namespace minimal::agent

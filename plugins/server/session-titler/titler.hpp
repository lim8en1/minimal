#pragma once

// session-titler -- names a session after its first turn, via the LLM.
//
// The ARCHITECTURE.md "Session naming, layered" second layer: the free default
// (first user text, truncated) is always there; this plugin replaces it with a
// generated title once, asynchronously, after turn 1. Everything it needs from
// core reaches it through ONE seam -- SessionSupportService, core's provided
// service (session_support_service.hpp), resolved via ServiceRegistry::Find.
// The database is authoritative; the plugin holds only a one-shot set.
//
// SHIPPED DISABLED (bundled provenance, DefaultsDisabled in
// plugin_discovery.cpp): this is the one bundled plugin whose whole job is to
// spend the user's tokens on its own initiative. Enable it with one
// plugins.json edit.

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "minimal/agent/session_observer.hpp"
#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/event_queue.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "policy.hpp"

namespace minimal::session_titler {

// Parsed from this plugin's "config" object on its plugins.json entry, leniently:
// a malformed or missing value falls back to the default, because a bad number
// in an optional cosmetic plugin must not disable it.
struct TitlerConfig {
    // "<provider>/<model>" override. Empty = the session's own model, which is
    // the default: the operator already chose it, and no second connection is
    // constructed for a title. Set it to a small model to spend less.
    std::string model;
    // A reasoning model spends hidden tokens before any visible text; too low a
    // cap yields an empty reply (GenerateTitle warns when that happens).
    int max_tokens = kDefaultMaxTokens;
    // The first user message is truncated to this before it is sent. A prompt
    // can carry inlined @-mention file content; only enough to title by is needed.
    int input_char_budget = kDefaultInputCharBudget;
    std::string prompt;  // empty = the default below
};

TitlerConfig ParseTitlerConfig(const nlohmann::json& config);

// The default instruction when the config names no prompt override.
std::string_view DefaultPrompt();

// Turns raw model output into a tab-label-shaped title: trimmed, control bytes
// dropped, whitespace-collapsed, stripped of the decoration models add anyway
// (wrapping quotes/markdown, a "Title:" label, a trailing period), capped at
// the 60 code points the free default uses. Empty input yields an empty
// string (caller keeps the default).
std::string CleanTitle(std::string_view text);

// One session, one title. OnTurnCompleted runs on the observer registry's
// single dispatch thread: it filters and enqueues a session id, nothing more.
// ONE owned worker thread does the rest -- resolve through the support
// service, read the first user text, call the provider, rename -- so the
// dispatcher (shared with every other observer) never waits on a provider.
//
// Resolving on the worker is safe after the session is gone: SessionSupport
// falls back to the database row for a session already erased from the
// registry (a one-shot headless turn terminates the instant it completes).
//
// Shutdown: the destructor drops queued jobs and joins the worker, so at most
// one in-flight provider call delays it. The host releases observers
// (SessionObserverRegistry::Shutdown) while the support service is still
// alive, which is what makes that join safe.
class TitlerObserver final : public agent::SessionObserver {
public:
    TitlerObserver(TitlerConfig config, plugins::ServiceRegistry* services,
                   common::Logger& logger);
    ~TitlerObserver() override;

    TitlerObserver(const TitlerObserver&) = delete;
    TitlerObserver& operator=(const TitlerObserver&) = delete;

    void OnTurnCompleted(const agent::SessionInfo& info,
                         const agent::TurnSummary& summary) override;
    // Releases the one-shot entry. Hygiene rather than leak control: a
    // resumed session's turns never start at seq 0, so it is never retitled.
    void OnSessionEnded(const agent::SessionInfo& info) override;

    // The worker's job body, synchronous -- also the testing seam, so tests
    // drive exactly the production path. Never throws; every failure keeps the
    // default name.
    void ProcessJob(const std::string& session_id) const;

    // Call the provider, clean the output, rename. Provider errors propagate;
    // ProcessJob is the catch-all.
    static void GenerateTitle(const TitlerConfig& config, common::Logger& logger,
                              llm::LlmProvider& provider, const std::string& model,
                              const std::string& session_id, const std::string& first_text,
                              agent::SessionSupportService& support);

private:
    void RunWorker();

    const TitlerConfig config_;
    plugins::ServiceRegistry* const services_;  // null = no seam; degrade
    common::Logger& logger_;
    // Touched only on the observer registry's dispatch thread.
    std::unordered_set<std::string> titled_;
    std::atomic<bool> stopping_{false};
    common::EventQueue<std::string> jobs_;
    std::thread worker_;  // last: started once everything above exists
};

}  // namespace minimal::session_titler

#pragma once

// session-titler -- names a session after its first turn, via the LLM.
//
// The ARCHITECTURE.md "Session naming, layered" second layer: the free default
// (first user text, truncated) is always there; this plugin replaces it with a
// generated title once, asynchronously, after turn 1. Everything it needs from
// core reaches it through ONE seam -- SessionSupportService, core's provided
// service (session_support_service.hpp), resolved via ServiceRegistry::Find.
// The plugin holds no session state beyond a one-shot set; the database is
// authoritative.
//
// SHIPPED DISABLED (bundled provenance, DefaultsDisabled in
// plugin_discovery.cpp): this is the one bundled plugin whose whole job is to
// spend the user's tokens on its own initiative. Enable it with one
// plugins.json edit.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "minimal/agent/session_observer.hpp"
#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/service_registry.hpp"

namespace minimal::session_titler {

// Parsed from this plugin's "config" object on its plugins.json entry, leniently:
// a malformed or missing value falls back to the default, because a bad number
// in an optional cosmetic plugin must not disable it.
struct TitlerConfig {
    // "<provider>/<model>" override. Empty = the session's own model, which is
    // the default: the operator already chose it, and no second connection is
    // constructed for a title. Set it to a small model to spend less.
    std::string model;
    int max_tokens = 64;
    // The first user message is truncated to this before it is sent. A prompt
    // can carry inlined @-mention file content; only enough to title by is needed.
    int input_char_budget = 2000;
    std::string prompt;  // empty = the default below
};

TitlerConfig ParseTitlerConfig(const nlohmann::json& config);

// The default instruction when the config names no prompt override.
std::string_view DefaultPrompt();

// Turns raw model output into a tab-label-shaped title: trimmed, unwrapped
// from quotes, whitespace-collapsed, capped at the 60 code points the free
// default uses. Empty input yields an empty string (caller keeps the default).
std::string CleanTitle(std::string_view text);

// One session, one title, ever -- per process. The guard set covers both
// "already titled" and "in flight": OnTurnCompleted runs on the observer
// registry's single serialized thread, so insertion alone decides, and no
// second job for a session can start. The set lives on a shared_ptr State
// block so the detached worker holds plugin state, never `this`.
//
// THE SPLIT BETWEEN SYNCHRONOUS AND DETACHED IS LOAD-BEARING. The stream that
// closes at turn completion TERMINATES the session, and a terminated session
// is erased from the registry -- so anything resolved through the live
// session (its transcript reader, its provider, its model) must be resolved
// on the observer thread, before the job goes async. Only the unbounded
// provider call runs detached. A rename AFTER termination still works: the
// database write does not need a live session, and the event push degrades to
// a no-op.
class TitlerObserver final : public agent::SessionObserver {
public:
    TitlerObserver(TitlerConfig config, plugins::ServiceRegistry* services,
                   common::Logger& logger);

    void OnTurnCompleted(const agent::SessionInfo& info,
                         const agent::TurnSummary& summary) override;
    // Releases the one-shot entry. Per-process sessions are bounded, so this is
    // hygiene rather than leak control -- the tools-bash cwd/env map is the
    // pattern, and OnSessionEnded is not optional garnish for it.
    void OnSessionEnded(const agent::SessionInfo& info) override;

    // The job body, synchronous -- the testing seam. Resolves through the
    // service, then runs GenerateTitle. The production path resolves on the
    // observer thread and runs ONLY GenerateTitle detached (see the class
    // comment); RunJobNow collapses the two so tests drive one entry point.
    void RunJobNow(const std::string& session_id, std::string first_text) const;

    // Call the provider, clean the output, rename. No service access: the
    // provider and model were resolved by the caller while the session was
    // still findable.
    static void GenerateTitle(const TitlerConfig& config, common::Logger& logger,
                              llm::LlmProvider& provider, const std::string& model,
                              const std::string& session_id, const std::string& first_text,
                              agent::SessionSupportService& support);

private:
    struct State {
        TitlerConfig config;
        plugins::ServiceRegistry* services = nullptr;  // null = no seam; degrade
        common::Logger* logger = nullptr;
        std::mutex mu;
        std::unordered_set<std::string> titled;
    };

    std::shared_ptr<State> state_;
};

}  // namespace minimal::session_titler

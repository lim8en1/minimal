#pragma once

// Hand-written fakes, deliberately without a mocking framework.
//
// The interfaces here are tiny by design -- LlmProvider is two methods,
// PermissionChecker / HistoryStore are one each -- so fakes are cheaper than
// GMock. That is a property worth protecting: an interface that needs a mocking
// framework to test is an interface that grew too big.
//
// This header is an installed target, not test-local code: plugin authors link
// the same fakes, which matters when most functionality ships as plugins.

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "minimal/commands/command.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/session/history_store.hpp"
#include "minimal/tools/permission.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::testing {

// Scripted provider. Queue responses (and optionally errors) in the order the
// loop should see them; every ChatRequest is recorded for assertion.
class FakeLlmProvider : public llm::LlmProvider {
public:
    struct Step {
        std::optional<llm::ChatResponse> response;
        std::optional<llm::LlmError> error;
        std::vector<llm::StreamEvent> events;  // emitted through the sink first
    };

    void PushResponse(llm::ChatResponse response, std::vector<llm::StreamEvent> events = {}) {
        std::lock_guard<std::mutex> lock(mu_);
        steps_.push_back(Step{std::move(response), std::nullopt, std::move(events)});
    }
    void PushError(llm::LlmError error) {
        std::lock_guard<std::mutex> lock(mu_);
        steps_.push_back(Step{std::nullopt, std::move(error), {}});
    }

    llm::ChatResponse Complete(const llm::ChatRequest& request) override {
        return StreamComplete(request, [](const llm::StreamEvent&) { return true; });
    }

    llm::ChatResponse StreamComplete(const llm::ChatRequest& request,
                                      const llm::StreamEventSink& sink) override {
        // ChatRequest::messages is a SPAN, and this outlives the call -- the
        // whole point of recording is to assert after the fact. So the messages
        // are copied into storage this owns and the recorded span is re-pointed
        // at that copy. Storing the request verbatim would dangle the moment the
        // loop's history reallocated, which is every append.
        //
        // A deque, not a vector: recorded spans point INTO the elements, so the
        // container must not move them when it grows.
        //
        // The mutex, not merely paranoia: a Session's worker thread runs the
        // call while the test's main thread spins on call_count() -- the
        // cancel-during-backoff rendezvous -- so the recorded state is written
        // and read from two threads with no other happens-before edge.
        Step step;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& owned = recorded_messages_.emplace_back(request.messages.begin(),
                                                          request.messages.end());
            llm::ChatRequest copy = request;
            copy.messages = owned;
            requests_.push_back(std::move(copy));
            if (steps_.empty()) {
                throw llm::LlmError(llm::LlmErrorCategory::RequestError,
                                    "FakeLlmProvider: no scripted step remaining");
            }
            step = std::move(steps_.front());
            steps_.pop_front();
        }

        for (const auto& event : step.events) {
            if (sink && !sink(event)) {
                // Sink asked to abort: mirror a real provider, which returns a
                // partial response rather than throwing.
                llm::ChatResponse cancelled;
                cancelled.stop_reason = llm::StopReason::Cancelled;
                return cancelled;
            }
        }
        if (step.error) throw *step.error;
        return *step.response;
    }

    std::size_t call_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_.size();
    }

    // Synchronised access ends here. The reference return is read-only, and
    // every caller reads it after the loop (or the session worker, via
    // Terminate) has joined -- thread join is a happens-before edge, so no
    // lock is taken and none may be: the references point into the container.
    const std::vector<llm::ChatRequest>& requests() const { return requests_; }

private:
    mutable std::mutex mu_;
    std::deque<Step> steps_;
    std::deque<std::vector<llm::Message>> recorded_messages_;
    std::vector<llm::ChatRequest> requests_;
};

// Read-only, so the dispatcher must answer it beside a live turn.
class BlockingReadOnlyCommand : public commands::HarnessCommand {
public:
    std::string_view Name() const override { return "peek"; }
    std::string_view Summary() const override { return "answers during a turn"; }
    std::string_view Usage() const override { return "/peek"; }
    bool ReadOnly() const override { return true; }
    commands::CommandResult Execute(commands::CommandContext&,
                                     const commands::CommandArgs&) override {
        return commands::CommandResult::Ok("peeked");
    }
};

// NOT read-only, so it must be queued for the worker and land at a turn
// boundary rather than mid-cycle.
// An LLMCommand, which is a DIFFERENT rule from "not read-only": a mutating
// HarnessCommand is meant to queue and run at the next turn boundary, while an
// LLMCommand needs the provider and the history to itself and must be refused
// while a turn is in flight. Only the type distinguishes them.
class RecordingLlmCommand : public commands::LLMCommand {
public:
    std::string_view Name() const override { return "llmrecord"; }
    std::string_view Summary() const override { return "an LLM command that records"; }
    std::string_view Usage() const override { return "/llmrecord"; }
    commands::CommandResult Execute(commands::CommandContext&,
                                     const commands::CommandArgs&) override {
        return commands::CommandResult::Ok("llm recorded");
    }
};

class RecordingCommand : public commands::HarnessCommand {
public:
    std::string_view Name() const override { return "record"; }
    std::string_view Summary() const override { return "records that it ran"; }
    std::string_view Usage() const override { return "/record"; }
    commands::CommandResult Execute(commands::CommandContext&,
                                     const commands::CommandArgs&) override {
        return commands::CommandResult::Ok("recorded");
    }
};

// Shared script for ScriptedProvider. Unlike FakeLlmProvider's per-instance
// queue, every provider instance handed the SAME script consumes one queue in
// real time -- which is what makes multi-session scripting deterministic: a
// blocking subagent reusing its parent's preset consumes the parent's steps
// in the order the sessions actually interleave.
struct ScriptedProviderScript {
    void Push(llm::ChatResponse response, std::vector<llm::StreamEvent> events = {}) {
        std::lock_guard<std::mutex> lock(mu);
        steps.push_back(
            FakeLlmProvider::Step{std::move(response), std::nullopt, std::move(events)});
    }
    void Push(llm::LlmError error) {
        std::lock_guard<std::mutex> lock(mu);
        steps.push_back(FakeLlmProvider::Step{std::nullopt, std::move(error), {}});
    }

    std::mutex mu;
    std::deque<FakeLlmProvider::Step> steps;
    // Recorded requests with owned message storage: ChatRequest::messages is a
    // span into the loop's history, which reallocates on every append. A deque
    // owns the copies because its elements do not move on growth.
    std::deque<std::vector<llm::Message>> owned_messages;
    std::vector<llm::ChatRequest> requests;
};

class ScriptedProvider : public llm::LlmProvider {
public:
    explicit ScriptedProvider(std::shared_ptr<ScriptedProviderScript> script)
        : script_(std::move(script)) {}

    llm::ChatResponse Complete(const llm::ChatRequest& request) override {
        return StreamComplete(request, [](const llm::StreamEvent&) { return true; });
    }

    llm::ChatResponse StreamComplete(const llm::ChatRequest& request,
                                     const llm::StreamEventSink& sink) override {
        FakeLlmProvider::Step step;
        {
            std::lock_guard<std::mutex> lock(script_->mu);
            auto& owned = script_->owned_messages.emplace_back(request.messages.begin(),
                                                               request.messages.end());
            llm::ChatRequest copy = request;
            copy.messages = owned;
            script_->requests.push_back(std::move(copy));
            if (script_->steps.empty()) {
                throw llm::LlmError(llm::LlmErrorCategory::RequestError,
                                    "ScriptedProvider: no scripted step remaining");
            }
            step = std::move(script_->steps.front());
            script_->steps.pop_front();
        }
        for (const auto& event : step.events) {
            if (sink && !sink(event)) {
                llm::ChatResponse cancelled;
                cancelled.stop_reason = llm::StopReason::Cancelled;
                return cancelled;
            }
        }
        if (step.error) throw *step.error;
        return *step.response;
    }

private:
    std::shared_ptr<ScriptedProviderScript> script_;
};

// Configurable tool. Records every invocation; can claim a path so the
// out-of-workdir composition can be exercised.
//
// Thread-safe by construction: parallel dispatch may invoke a parallel-safe
// tool CONCURRENTLY within one round, and a fake that models a parallel-safe
// tool must tolerate that -- the same obligation a real plugin tool takes on
// with parallel_safe() == true (ToolContext is per-invocation; this fake's own
// recording needs the mutex).
class FakeTool : public tools::Tool {
public:
    FakeTool(std::string name, tools::ToolCategory category, std::string result_text = "ok")
        : result_text_(std::move(result_text)) {
        spec_.name = std::move(name);
        spec_.category = category;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "fake tool";
        spec_.definition.input_schema = nlohmann::json::object();
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls.push_back(input);
        }
        if (ctx.emit_output) ctx.emit_output(result_text_);
        return tools::ToolResult{is_error_, result_text_, display_};
    }

    bool path_confinement_supported() const override { return path_key_.has_value(); }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        if (!path_key_) return std::nullopt;
        if (!input.contains(*path_key_)) return std::nullopt;
        return input.at(*path_key_).get<std::string>();
    }

    // Pattern-subject support, two flavors: command_subject_ mimics bash (the
    // subject is input["command"], not a path); otherwise the base default
    // applies (ExtractPath for confinement tools).
    std::optional<std::string> PatternSubject(const nlohmann::json& input) const override {
        if (!command_subject_) return tools::Tool::PatternSubject(input);
        if (!input.contains("command")) return std::nullopt;
        return input.at("command").get<std::string>();
    }
    void set_command_subject(bool value) { command_subject_ = value; }

    bool network_access_required() const override { return network_access_required_; }

    tools::PermissionDisclosure Disclose(const nlohmann::json& input,
                                         const tools::ToolContext& ctx) const override {
        if (!disclosure_) return tools::Tool::Disclose(input, ctx);
        return *disclosure_;
    }

    void set_path_key(std::string key) { path_key_ = std::move(key); }
    void set_network_access_required(bool value) { network_access_required_ = value; }
    void set_is_error(bool value) { is_error_ = value; }
    void set_display(nlohmann::json display) { display_ = std::move(display); }
    void set_disclosure(tools::PermissionDisclosure disclosure) {
        disclosure_ = std::move(disclosure);
    }

    std::vector<nlohmann::json> calls;

private:
    tools::ToolSpec spec_;
    std::string result_text_;
    std::optional<std::string> path_key_;
    bool command_subject_ = false;
    std::optional<tools::PermissionDisclosure> disclosure_;
    bool network_access_required_ = false;
    bool is_error_ = false;
    nlohmann::json display_ = nullptr;
    // Guards `calls` against concurrent executes (parallel dispatch).
    std::mutex mu_;
};

class InMemoryHistoryStore : public session::HistoryStore {
public:
    // Token counts are a persistence concern; the in-memory fake ignores them.
    // Attributions are recorded, parallel to `messages`, so loop tests can
    // assert what a persisted row would carry.
    void Append(const llm::Message& message, std::optional<int> output_tokens,
                const session::MessageAttribution& attribution) override {
        (void)output_tokens;
        messages.push_back(message);
        attributions.push_back(attribution);
    }
    // The fake starts empty and never compacts, so position is seq.
    std::int64_t last_appended_seq() const override {
        return static_cast<std::int64_t>(messages.size()) - 1;
    }
    // Checkpoint tracking mirrors the real store, so compaction tests can
    // assert what a resume would consume.
    void SaveCompactionCheckpoint(std::int64_t boundary_seq, const llm::Message& summary) override {
        checkpoint_boundary = boundary_seq;
        checkpoint_summary = summary;
        checkpoint_cleared = false;
    }
    void ClearCompactionCheckpoint() override {
        checkpoint_cleared = true;
        checkpoint_boundary.reset();
        checkpoint_summary = llm::Message{};
    }
    std::vector<llm::Message> messages;
    std::vector<session::MessageAttribution> attributions;   // parallel to messages
    std::optional<std::int64_t> checkpoint_boundary;
    llm::Message checkpoint_summary;
    bool checkpoint_cleared = false;
};

class AlwaysAllowPermissionChecker : public tools::PermissionChecker {
public:
    tools::PermissionResponse check(const tools::PermissionRequest& request) override {
        seen.push_back(request);
        return {tools::PermissionDecision::Allow, {}};
    }
    std::vector<tools::PermissionRequest> seen;
};

class AlwaysDenyPermissionChecker : public tools::PermissionChecker {
public:
    tools::PermissionResponse check(const tools::PermissionRequest& request) override {
        seen.push_back(request);
        return {tools::PermissionDecision::Deny, {}};
    }
    std::vector<tools::PermissionRequest> seen;
};

// Never consulted; asserting `called` stayed false proves a decision was reached
// without a round-trip (Bypass, AlwaysAllow, AlwaysDeny, or a satisfied grant).
class UnreachablePermissionChecker : public tools::PermissionChecker {
public:
    tools::PermissionResponse check(const tools::PermissionRequest&) override {
        called = true;
        return {tools::PermissionDecision::Allow, {}};
    }
    bool called = false;
};

// Allows, and takes every grant the disclosure offers -- the "operator clicked
// every 'allow ... this session' button" case, which is what makes the
// second-invocation-is-silent behaviour testable.
class GrantingPermissionChecker : public tools::PermissionChecker {
public:
    explicit GrantingPermissionChecker(std::vector<std::string> keys_to_grant = {})
        // grant_all_ reads keys_, not the parameter: the parameter has already
        // been moved from by the time this initialiser runs.
        : keys_(std::move(keys_to_grant)), grant_all_(keys_.empty()) {}

    tools::PermissionResponse check(const tools::PermissionRequest& request) override {
        seen.push_back(request);
        tools::PermissionResponse response{tools::PermissionDecision::Allow, {}};
        for (const auto& offer : request.disclosure.grants) {
            const bool wanted =
                grant_all_ || std::find(keys_.begin(), keys_.end(), offer.key) != keys_.end();
            if (wanted) response.granted_keys.push_back(offer.key);
        }
        return response;
    }

    std::vector<tools::PermissionRequest> seen;

private:
    std::vector<std::string> keys_;
    bool grant_all_;
};

inline tools::PermissionPolicy AllowAllPolicy() {
    tools::PermissionPolicy p;
    p.read_only = p.file_write = p.execute = p.out_of_workdir = p.network =
        tools::PermissionMode::AlwaysAllow;
    return p;
}

inline tools::PermissionPolicy DenyAllPolicy() {
    tools::PermissionPolicy p;
    p.read_only = p.file_write = p.execute = p.out_of_workdir = p.network =
        tools::PermissionMode::AlwaysDeny;
    return p;
}

// --- Message construction helpers -------------------------------------------

inline llm::Message UserText(std::string text) {
    return llm::Message{llm::Role::User, {llm::TextBlock{std::move(text)}}};
}

inline llm::ChatResponse TextResponse(std::string text,
                                       llm::StopReason stop = llm::StopReason::EndTurn) {
    llm::ChatResponse response;
    response.content.push_back(llm::TextBlock{std::move(text)});
    response.stop_reason = stop;
    response.usage.input_tokens = 10;
    response.usage.output_tokens = 5;
    return response;
}

inline llm::ChatResponse ToolUseResponse(std::string id, std::string name,
                                          nlohmann::json input = nlohmann::json::object()) {
    llm::ChatResponse response;
    response.content.push_back(llm::ToolUseBlock{std::move(id), std::move(name), std::move(input)});
    response.stop_reason = llm::StopReason::ToolUse;
    response.usage.input_tokens = 10;
    response.usage.output_tokens = 5;
    return response;
}

}  // namespace minimal::testing

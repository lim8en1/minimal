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

#include "minimal/agent/history_store.hpp"
#include "minimal/commands/command.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/llm/provider.hpp"
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
        steps_.push_back(Step{std::move(response), std::nullopt, std::move(events)});
    }
    void PushError(llm::LlmError error) {
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
        auto& owned = recorded_messages_.emplace_back(request.messages.begin(),
                                                      request.messages.end());
        llm::ChatRequest copy = request;
        copy.messages = owned;
        requests_.push_back(std::move(copy));
        if (steps_.empty()) {
            throw llm::LlmError(llm::LlmErrorCategory::RequestError,
                                "FakeLlmProvider: no scripted step remaining");
        }
        Step step = std::move(steps_.front());
        steps_.pop_front();

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

    const std::vector<llm::ChatRequest>& requests() const { return requests_; }
    std::size_t call_count() const { return requests_.size(); }

private:
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
        calls.push_back(input);
        if (ctx.emit_output) ctx.emit_output(result_text_);
        return tools::ToolResult{is_error_, result_text_};
    }

    bool path_confinement_supported() const override { return path_key_.has_value(); }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        if (!path_key_) return std::nullopt;
        if (!input.contains(*path_key_)) return std::nullopt;
        return input.at(*path_key_).get<std::string>();
    }

    tools::PermissionDisclosure Disclose(const nlohmann::json& input,
                                         const tools::ToolContext& ctx) const override {
        if (!disclosure_) return tools::Tool::Disclose(input, ctx);
        return *disclosure_;
    }

    void set_path_key(std::string key) { path_key_ = std::move(key); }
    void set_is_error(bool value) { is_error_ = value; }
    void set_disclosure(tools::PermissionDisclosure disclosure) {
        disclosure_ = std::move(disclosure);
    }

    std::vector<nlohmann::json> calls;

private:
    tools::ToolSpec spec_;
    std::string result_text_;
    std::optional<std::string> path_key_;
    std::optional<tools::PermissionDisclosure> disclosure_;
    bool is_error_ = false;
};

class InMemoryHistoryStore : public agent::HistoryStore {
public:
    // Token counts are a persistence concern; the in-memory fake ignores them.
    void Append(const llm::Message& message, std::optional<int> output_tokens) override {
        (void)output_tokens;
        messages.push_back(message);
    }
    // The fake starts empty and never compacts, so position is seq.
    std::int64_t last_appended_seq() const override {
        return static_cast<std::int64_t>(messages.size()) - 1;
    }
    std::vector<llm::Message> messages;
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
    p.read_only = p.file_write = p.execute = p.out_of_workdir = tools::PermissionMode::AlwaysAllow;
    return p;
}

inline tools::PermissionPolicy DenyAllPolicy() {
    tools::PermissionPolicy p;
    p.read_only = p.file_write = p.execute = p.out_of_workdir = tools::PermissionMode::AlwaysDeny;
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

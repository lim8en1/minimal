#include "minimal/agent/tool_source_activation.hpp"

#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace minimal::agent {
namespace {

struct Outcome {
    bool up = false;
    std::string error;
};

struct Pending {
    std::string source_name;
    std::string member;
    std::shared_future<Outcome> result;
    std::string reconnect_hint;  // the source's, for the failure message
};

}  // namespace

ToolSourceActivation ActivateToolSources(
    const std::map<std::string, std::vector<std::string>>& tool_sources,
    const tools::ToolSourceRegistry* sources, std::chrono::steady_clock::duration budget) {
    ToolSourceActivation activation;
    std::vector<Pending> pending;
    for (const auto& [source_name, members] : tool_sources) {
        const std::shared_ptr<tools::ToolSource> source =
            sources != nullptr ? sources->Find(source_name) : nullptr;
        for (const auto& member : members) {
            auto promise = std::make_shared<std::promise<Outcome>>();
            pending.push_back({source_name, member, promise->get_future().share(),
                               source != nullptr ? source->ReconnectHint(member) : std::string()});
            if (source == nullptr) {
                Outcome outcome;
                outcome.error = "tool source '" + source_name +
                                "' is not available (no loaded plugin provides it)";
                promise->set_value(std::move(outcome));
                continue;
            }
            // Active whatever the outcome below: a member that is down now
            // fills its group when it comes up (a late connect, a reconnect),
            // and the session sees it on its next request.
            activation.groups.push_back(source->GroupOf(member));
            // Detached: a member past the deadline must not block the session,
            // and the thread owns everything it touches (source + promise).
            std::thread([source, member, source_name, promise] {
                Outcome outcome;
                try {
                    outcome.up = source->Activate(member, outcome.error);
                } catch (const std::exception& e) {
                    outcome.up = false;
                    outcome.error = e.what();
                } catch (...) {
                    outcome.up = false;
                    outcome.error = "tool source '" + source_name + "' threw";
                }
                promise->set_value(std::move(outcome));
            }).detach();
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (auto& entry : pending) {
        std::string error;
        if (entry.result.wait_until(deadline) == std::future_status::ready) {
            const Outcome& outcome = entry.result.get();
            if (outcome.up) continue;
            error = outcome.error;
        } else {
            error = "did not become ready within " +
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::seconds>(budget).count()) +
                    "s; its tools appear once it does";
        }
        if (!entry.reconnect_hint.empty()) error += " (" + entry.reconnect_hint + ")";
        activation.failures.push_back({entry.source_name, entry.member, std::move(error)});
    }
    return activation;
}

}  // namespace minimal::agent

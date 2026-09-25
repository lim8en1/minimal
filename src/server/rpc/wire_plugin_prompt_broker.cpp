#include "minimal/rpc/wire_plugin_prompt_broker.hpp"

#include <utility>

namespace minimal::rpc {

WirePluginPromptBroker::WirePluginPromptBroker(std::string session_id, EventBroadcaster& queue,
                                               PendingRequests& pending,
                                               std::chrono::milliseconds unattended_timeout)
    : prompt_(std::move(session_id), queue, pending, unattended_timeout, "prompt-") {}

tools::PromptResult WirePluginPromptBroker::Prompt(const tools::PromptRequest& request) {
    const auto reply = prompt_.AskDetailed<PluginPromptAnswer>(
        [&request](v1::ServerEvent& event, const std::string& request_id) {
            auto* wire = event.mutable_plugin_prompt();
            wire->set_request_id(request_id);
            wire->set_kind(request.kind);
            wire->set_payload_json(
                request.payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        });
    tools::PromptResult result;
    if (!reply.answer) {
        result.outcome = reply.timed_out ? tools::PromptOutcome::TimedOut
                                         : tools::PromptOutcome::Cancelled;
        return result;
    }
    result.outcome = reply.answer->outcome;
    if (result.outcome != tools::PromptOutcome::Answered) return result;

    // Fail closed on a malformed answer: the user's intent cannot be read, so
    // it is "not answered", the same rule an unoffered option gets in
    // WireConfirmationBroker.
    const std::string& json = reply.answer->answer_json;
    if (json.size() > kMaxAnswerBytes) {
        result.outcome = tools::PromptOutcome::Dismissed;
        return result;
    }
    result.answer = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (result.answer.is_discarded()) {
        result.answer = nullptr;
        result.outcome = tools::PromptOutcome::Dismissed;
    }
    return result;
}

}  // namespace minimal::rpc

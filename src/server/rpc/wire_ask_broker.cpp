#include "minimal/rpc/wire_ask_broker.hpp"

#include <utility>

namespace minimal::rpc {
namespace {

// One answer slot is usable when it names only labels that were OFFERED
// (exact match -- a client cannot invent one, the same rule an unoffered
// option gets in WireConfirmationBroker), respects that question's
// multiple/allow_other flags, and never mixes a free text with a selection.
// Empty is "question not answered", always valid.
bool SlotIsValid(const tools::AskQuestion& question, const tools::AskAnswer::Slot& slot) {
    if (slot.other.empty() && slot.selected.empty()) return true;
    if (!slot.other.empty()) {
        if (!question.allow_other) return false;
        return slot.selected.empty();
    }
    if (slot.selected.size() > 1 && !question.multiple) return false;
    for (const auto& label : slot.selected) {
        bool offered = false;
        for (const auto& option : question.options) {
            if (label == option.label) {
                offered = true;
                break;
            }
        }
        if (!offered) return false;
    }
    return true;
}

}  // namespace

WireAskBroker::WireAskBroker(std::string session_id, EventBroadcaster& queue,
                             PendingRequests& pending,
                             std::chrono::milliseconds unattended_timeout)
    : session_id_(std::move(session_id)),
      queue_(queue),
      pending_(pending),
      unattended_timeout_(unattended_timeout) {}

tools::AskAnswer WireAskBroker::Ask(const tools::AskRequest& request) {
    const std::string request_id = request_ids_.Next();

    // Before the push, always -- see the header.
    auto registration = pending_.Register(request_id);

    v1::ServerEvent event;
    event.set_session_id(session_id_);
    auto* wire = event.mutable_ask_request();
    wire->set_request_id(request_id);
    for (const auto& question : request.questions) {
        auto* wire_question = wire->add_questions();
        wire_question->set_question(question.question);
        wire_question->set_header(question.header);
        wire_question->set_multiple(question.multiple);
        wire_question->set_allow_other(question.allow_other);
        for (const auto& option : question.options) {
            auto* wire_option = wire_question->add_options();
            wire_option->set_label(option.label);
            wire_option->set_description(option.description);
        }
    }
    queue_.Publish(std::move(event));  // event is moved-from below the publish

    // Cancelled, terminating, timed out, or an answer of the other brokers'
    // type delivered against this id -- all "no answer", never a choice (see
    // WireConfirmationBroker for the identical fail-closed shape).
    const std::optional<AskUserAnswer> answer =
        AwaitTypedAnswer<AskUserAnswer>(registration, unattended_timeout_);
    if (!answer || !answer->answered) return {};

    tools::AskAnswer result;
    result.answered = true;
    result.slots.resize(request.questions.size());
    for (std::size_t i = 0; i < result.slots.size(); ++i) {
        const tools::AskQuestion& question = request.questions[i];
        if (question.options.empty()) continue;  // nothing was offered to pick from
        if (i >= answer->answers.size()) continue;
        const AskUserAnswer::Slot& wire_slot = answer->answers[i];
        tools::AskAnswer::Slot slot;
        slot.selected = wire_slot.selected;
        slot.other = wire_slot.other;
        if (!SlotIsValid(question, slot)) continue;  // shape-invalid: that question is unanswered
        result.slots[i] = std::move(slot);
    }
    return result;
}

}  // namespace minimal::rpc

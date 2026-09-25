#include "ask_types.hpp"

#include <algorithm>

namespace minimal::tool_ask {
namespace {

bool SlotIsValid(const AskQuestion& question, const AskSlot& slot) {
    if (slot.other.empty() && slot.selected.empty()) return true;
    if (!slot.other.empty()) return question.allow_other && slot.selected.empty();
    if (slot.selected.size() > 1 && !question.multiple) return false;
    for (std::size_t i = 0; i < slot.selected.size(); ++i) {
        const auto& label = slot.selected[i];
        const bool offered =
            std::any_of(question.options.begin(), question.options.end(),
                        [&label](const AskOption& option) { return option.label == label; });
        if (!offered) return false;
        // The same label twice is not two picks.
        if (std::find(slot.selected.begin(), slot.selected.begin() + i, label) !=
            slot.selected.begin() + i) {
            return false;
        }
    }
    return true;
}

std::optional<AskSlot> ReadSlot(const nlohmann::json& wire) {
    if (!wire.is_object()) return std::nullopt;
    AskSlot slot;
    if (const auto it = wire.find("selected"); it != wire.end()) {
        if (!it->is_array()) return std::nullopt;
        for (const auto& label : *it) {
            if (!label.is_string()) return std::nullopt;
            slot.selected.push_back(label.get<std::string>());
        }
    }
    if (const auto it = wire.find("other"); it != wire.end()) {
        if (!it->is_string()) return std::nullopt;
        slot.other = it->get<std::string>();
    }
    return slot;
}

}  // namespace

nlohmann::json ToPayload(const AskRequest& request) {
    nlohmann::json questions = nlohmann::json::array();
    for (const auto& question : request.questions) {
        nlohmann::json options = nlohmann::json::array();
        for (const auto& option : question.options) {
            nlohmann::json wire = nlohmann::json::object();
            wire["label"] = option.label;
            wire["description"] = option.description;
            options.push_back(std::move(wire));
        }
        nlohmann::json wire = nlohmann::json::object();
        wire["question"] = question.question;
        wire["header"] = question.header;
        wire["multiple"] = question.multiple;
        wire["allow_other"] = question.allow_other;
        wire["options"] = std::move(options);
        questions.push_back(std::move(wire));
    }
    nlohmann::json payload = nlohmann::json::object();
    payload["questions"] = std::move(questions);
    return payload;
}

std::vector<AskSlot> ReadAnswer(const AskRequest& request, const nlohmann::json& answer) {
    std::vector<AskSlot> slots(request.questions.size());
    if (!answer.is_object()) return slots;
    const auto answers = answer.find("answers");
    if (answers == answer.end() || !answers->is_array()) return slots;
    for (std::size_t i = 0; i < slots.size() && i < answers->size(); ++i) {
        auto slot = ReadSlot((*answers)[i]);
        if (slot && SlotIsValid(request.questions[i], *slot)) slots[i] = std::move(*slot);
    }
    return slots;
}

}  // namespace minimal::tool_ask

#include "ask_tool.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace minimal::tool_ask {
namespace {

constexpr std::size_t kMaxQuestions = 4;
constexpr std::size_t kMaxOptions = 5;

tools::ToolSpec MakeSpec() {
    tools::ToolSpec spec;
    spec.name = "question";
    // The tool IS the prompt: its only effect is the question it raises, so
    // it must not itself sit behind a permission gate. ReadOnly is
    // pre-allowed for exactly this kind of self-describing tool (the skill
    // tool precedent).
    spec.category = tools::ToolCategory::ReadOnly;
    spec.definition.name = "question";
    spec.definition.description =
        "Ask the user 1-4 multiple-choice questions and wait for their "
        "answers. Use when a decision is genuinely the user's to make -- "
        "ambiguous instructions, competing approaches, a destructive choice "
        "-- not for facts you can look up or choices you can reasonably "
        "make yourself. Each question offers 1-5 labeled options; the user "
        "may also type a custom answer.";
    spec.definition.input_schema = nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"questions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"question",
                 {{"type", "string"},
                  {"description", "The question, phrased so its answers are the options below."}}},
                {"header",
                 {{"type", "string"},
                  {"description",
                   "Short label for the question (1-6 words), shown as its title."}}},
                {"options",
                 {{"type", "array"},
                  {"items",
                   {{"type", "object"},
                    {"properties",
                     {{"label", {{"type", "string"},
                                 {"description",
                                  "The choice, returned to you verbatim if picked."}}},
                      {"description", {{"type", "string"},
                                       {"description",
                                        "One line on what choosing it means."}}}}},
                    {"required", nlohmann::json::array({"label", "description"})}}}}},
                {"multiple",
                 {{"type", "boolean"},
                  {"description",
                   "true: several options may be selected. Default false."}}}}},
              {"required", nlohmann::json::array({"question", "header", "options"})}}}}}},
         {"required", nlohmann::json::array({"questions"})}},
    };
    return spec;
}

bool NonEmptyString(const nlohmann::json& value) {
    return value.is_string() && !value.get<std::string>().empty();
}

// Returns an error string on the first shape violation found, nullopt when
// the input parses. Limits are enforced HERE, not in the broker: a model
// that sends 40 questions gets a tool-result error it can adapt to, not a
// silently truncated prompt.
std::optional<std::string> ParseQuestions(const nlohmann::json& input,
                                          tools::AskRequest& request) {
    if (!input.is_object() || !input.contains("questions") || !input.at("questions").is_array()) {
        return std::string("question requires \"questions\" as an array");
    }
    const auto& questions = input.at("questions");
    if (questions.empty()) {
        return std::string("question requires at least one question");
    }
    if (questions.size() > kMaxQuestions) {
        return "question accepts at most " + std::to_string(kMaxQuestions) +
               " questions per call (got " + std::to_string(questions.size()) + ")";
    }
    for (const auto& item : questions) {
        if (!item.is_object() || !NonEmptyString(item.value("question", nlohmann::json{}))) {
            return std::string("each question requires a non-empty \"question\" string");
        }
        if (!NonEmptyString(item.value("header", nlohmann::json{}))) {
            return std::string("each question requires a non-empty \"header\" string");
        }
        tools::AskQuestion question;
        question.question = item.at("question").get<std::string>();
        question.header = item.at("header").get<std::string>();
        if (!item.contains("options") || !item.at("options").is_array() ||
            item.at("options").empty()) {
            return std::string("each question requires a non-empty \"options\" array");
        }
        if (item.at("options").size() > kMaxOptions) {
            return "each question accepts at most " + std::to_string(kMaxOptions) +
                   " options (got " + std::to_string(item.at("options").size()) + ")";
        }
        for (const auto& option : item.at("options")) {
            if (!option.is_object() || !NonEmptyString(option.value("label", nlohmann::json{}))) {
                return std::string("each option requires a non-empty \"label\" string");
            }
            if (!NonEmptyString(option.value("description", nlohmann::json{}))) {
                return std::string("each option requires a non-empty \"description\" string");
            }
            tools::AskOption parsed;
            parsed.label = option.at("label").get<std::string>();
            parsed.description = option.at("description").get<std::string>();
            question.options.push_back(std::move(parsed));
        }
        if (item.contains("multiple")) {
            if (!item.at("multiple").is_boolean()) {
                return std::string("question \"multiple\" must be a boolean");
            }
            question.multiple = item.at("multiple").get<bool>();
        }
        request.questions.push_back(std::move(question));
    }
    return std::nullopt;
}

// Prose result, one block per question -- how Claude Code formats answers.
// The label is echoed verbatim, so the model correlates answer to question
// without guessing; the header (or index) carries the correlation the other
// way.
std::string FormatAnswer(std::size_t index, const tools::AskQuestion& question,
                         const tools::AskAnswer::Slot& slot) {
    const std::string name = question.header.empty()
                                 ? "Question " + std::to_string(index + 1)
                                 : question.header;
    if (!slot.other.empty()) {
        return name + ": the user answered: " + slot.other;
    }
    if (slot.selected.empty()) {
        return name + ": the user did not answer";
    }
    std::string joined;
    for (const auto& label : slot.selected) {
        if (!joined.empty()) joined += ", ";
        joined += label;
    }
    return name + ": the user selected: " + joined;
}

}  // namespace

class AskTool final : public tools::Tool {
public:
    AskTool() : spec_(MakeSpec()) {}

    const tools::ToolSpec& spec() const override { return spec_; }

    // One modal slot per session tab; two concurrent asks of one round would
    // overwrite each other's prompt client-side. Runs serialized on the
    // session worker like bash.
    bool parallel_safe() const override { return false; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        // Input shape first: a model that sends garbage gets the schema error
        // it can fix, whether or not this session can ask at all.
        tools::AskRequest request;
        if (const auto error = ParseQuestions(input, request)) {
            return {true, *error};
        }
        if (!ctx.ask_user) {
            return {true, "question is not available in this session (no interactive user is "
                          "attached); decide without asking"};
        }

        const tools::AskAnswer answer = ctx.ask_user(request);
        if (!answer.answered) {
            return {false,
                    "The user dismissed the questions without answering. Do not re-ask the "
                    "same thing; proceed with a sensible default or explain what you need "
                    "in your reply."};
        }

        std::string result;
        for (std::size_t i = 0; i < request.questions.size(); ++i) {
            const tools::AskAnswer::Slot empty;
            const tools::AskAnswer::Slot& slot =
                i < answer.slots.size() ? answer.slots[i] : empty;
            if (!result.empty()) result += "\n";
            result += FormatAnswer(i, request.questions[i], slot);
        }
        return {false, result};
    }

    // The question text is the disclosure: an operator who pins this tool to
    // Ask via per_tool_overrides should see what would be asked.
    tools::PermissionDisclosure Disclose(const nlohmann::json& input,
                                         const tools::ToolContext& ctx) const override {
        (void)ctx;
        tools::PermissionDisclosure disclosure;
        std::string first;
        if (input.is_object() && input.contains("questions") && input.at("questions").is_array() &&
            !input.at("questions").empty() && input.at("questions")[0].is_object()) {
            const auto& question = input.at("questions")[0].value("question", std::string{});
            first = question;
        }
        constexpr std::size_t kMaxPreview = 100;
        if (first.size() > kMaxPreview) first = first.substr(0, kMaxPreview) + "...";
        disclosure.summary =
            std::string("ask the user") + (first.empty() ? "" : ": " + first);
        disclosure.complete = true;
        return disclosure;
    }

private:
    tools::ToolSpec spec_;
};

}  // namespace minimal::tool_ask

namespace minimal::tool_ask {

std::unique_ptr<tools::Tool> MakeAskTool() { return std::make_unique<AskTool>(); }

}  // namespace minimal::tool_ask

#include "ask_tool.hpp"

#include "ask_types.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace minimal::tool_ask {
namespace {

constexpr std::size_t kMaxQuestions = 4;
constexpr std::size_t kMaxOptions = 5;
// Byte caps on the model-authored strings. The header renders as a title and
// the labels come back verbatim as answer keys, so both stay short; the
// question and descriptions wrap and only need a bound.
constexpr std::size_t kMaxHeaderBytes = 40;
constexpr std::size_t kMaxQuestionBytes = 1000;
constexpr std::size_t kMaxLabelBytes = 200;
constexpr std::size_t kMaxDescriptionBytes = 500;
// How much of the question text stands in for a missing header in the result.
constexpr std::size_t kMaxQuestionEcho = 60;

tools::ToolSpec MakeSpec() {
    tools::ToolSpec spec;
    spec.name = "ask_user";
    // The tool IS the prompt: its only effect is the question it raises, so
    // it must not itself sit behind a permission gate. ReadOnly is
    // pre-allowed for exactly this kind of self-describing tool (the skill
    // tool precedent).
    spec.category = tools::ToolCategory::ReadOnly;
    spec.definition.name = "ask_user";
    spec.definition.description =
        "Ask the user 1-4 multiple-choice questions and wait for their "
        "answers. Use when a decision is genuinely the user's to make -- "
        "ambiguous instructions, competing approaches, a destructive choice "
        "-- not for facts you can look up or choices you can reasonably "
        "make yourself. Each question offers 1-5 labeled options; the user "
        "may also type a custom answer.";
    // Built with explicit object() + operator[] assignment, not nested
    // brace-init lists: nlohmann::json's initializer_list constructor
    // silently degrades a whole level to a JSON ARRAY the instant one
    // "pair" in that level has more than 2 elements (an easy brace-nesting
    // mistake to make several levels deep), producing a schema that looks
    // right in the source but is structurally invalid on the wire -- with
    // no compile error or test failure to catch it.
    nlohmann::json option_schema = nlohmann::json::object();
    option_schema["type"] = "object";
    option_schema["properties"]["label"]["type"] = "string";
    option_schema["properties"]["label"]["description"] =
        "The choice, returned to you verbatim if picked.";
    option_schema["properties"]["description"]["type"] = "string";
    option_schema["properties"]["description"]["description"] =
        "One line on what choosing it means. May be empty.";
    option_schema["required"] = nlohmann::json::array({"label"});

    nlohmann::json question_schema = nlohmann::json::object();
    question_schema["type"] = "object";
    question_schema["properties"]["question"]["type"] = "string";
    question_schema["properties"]["question"]["description"] =
        "The question, phrased so its answers are the options below.";
    question_schema["properties"]["header"]["type"] = "string";
    question_schema["properties"]["header"]["description"] =
        "Short label for the question (1-6 words, at most 40 characters), shown as its "
        "title.";
    question_schema["properties"]["options"]["type"] = "array";
    question_schema["properties"]["options"]["items"] = option_schema;
    question_schema["properties"]["multiple"]["type"] = "boolean";
    question_schema["properties"]["multiple"]["description"] =
        "true: several options may be selected. Default false.";
    question_schema["properties"]["allow_other"]["type"] = "boolean";
    question_schema["properties"]["allow_other"]["description"] =
        "false: the user must pick one of the options, no typed answer. Default true.";
    question_schema["required"] = nlohmann::json::array({"question", "options"});

    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    schema["properties"]["questions"]["type"] = "array";
    schema["properties"]["questions"]["items"] = question_schema;
    schema["required"] = nlohmann::json::array({"questions"});
    spec.definition.input_schema = std::move(schema);
    return spec;
}

bool NonEmptyString(const nlohmann::json& value) {
    return value.is_string() && !value.get<std::string>().empty();
}

// Reads an OPTIONAL string field: absent leaves `out` alone; present but not a
// string, or longer than `max_bytes`, is an error -- a wrong type is never
// silently dropped.
std::optional<std::string> OptionalString(const nlohmann::json& object, const char* key,
                                          std::size_t max_bytes, std::string& out) {
    if (!object.contains(key)) return std::nullopt;
    const auto& value = object.at(key);
    if (!value.is_string()) {
        return std::string("ask_user \"") + key + "\" must be a string";
    }
    out = value.get<std::string>();
    if (out.size() > max_bytes) {
        return std::string("ask_user \"") + key + "\" is longer than " +
               std::to_string(max_bytes) + " characters";
    }
    return std::nullopt;
}

// Same for an optional boolean.
std::optional<std::string> OptionalBool(const nlohmann::json& object, const char* key,
                                        bool& out) {
    if (!object.contains(key)) return std::nullopt;
    if (!object.at(key).is_boolean()) {
        return std::string("ask_user \"") + key + "\" must be a boolean";
    }
    out = object.at(key).get<bool>();
    return std::nullopt;
}

// Returns an error string on the first shape violation found, nullopt when
// the input parses. Limits are enforced HERE, not in the broker: a model
// that sends 40 questions gets a tool-result error it can adapt to, not a
// silently truncated prompt.
std::optional<std::string> ParseQuestions(const nlohmann::json& input,
                                          AskRequest& request) {
    if (!input.is_object() || !input.contains("questions") || !input.at("questions").is_array()) {
        return std::string("ask_user requires \"questions\" as an array");
    }
    const auto& questions = input.at("questions");
    if (questions.empty()) {
        return std::string("ask_user requires at least one question");
    }
    if (questions.size() > kMaxQuestions) {
        return "ask_user accepts at most " + std::to_string(kMaxQuestions) +
               " questions per call (got " + std::to_string(questions.size()) + ")";
    }
    for (const auto& item : questions) {
        if (!item.is_object() || !NonEmptyString(item.value("question", nlohmann::json{}))) {
            return std::string("each question requires a non-empty \"question\" string");
        }
        AskQuestion question;
        question.question = item.at("question").get<std::string>();
        if (question.question.size() > kMaxQuestionBytes) {
            return "ask_user \"question\" is longer than " + std::to_string(kMaxQuestionBytes) +
                   " characters";
        }
        if (auto error = OptionalString(item, "header", kMaxHeaderBytes, question.header)) {
            return error;
        }
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
            AskOption parsed;
            parsed.label = option.at("label").get<std::string>();
            if (parsed.label.size() > kMaxLabelBytes) {
                return "ask_user option \"label\" is longer than " +
                       std::to_string(kMaxLabelBytes) + " characters";
            }
            // The label IS the answer key: two identical labels would make
            // the answer ambiguous.
            for (const auto& earlier : question.options) {
                if (earlier.label == parsed.label) {
                    return "ask_user option labels must be unique within a question (\"" +
                           parsed.label + "\" repeats)";
                }
            }
            if (auto error = OptionalString(option, "description", kMaxDescriptionBytes,
                                            parsed.description)) {
                return error;
            }
            question.options.push_back(std::move(parsed));
        }
        if (auto error = OptionalBool(item, "multiple", question.multiple)) return error;
        if (auto error = OptionalBool(item, "allow_other", question.allow_other)) return error;
        request.questions.push_back(std::move(question));
    }
    return std::nullopt;
}

// Prose result, one line per question. The 1-based index always leads, so
// duplicate or missing headers stay unambiguous; the header (or, without one,
// the start of the question) names it. Labels are echoed verbatim, since they
// were the model's own strings. Free text is JSON-quoted so a newline in it
// cannot break the one-line-per-question shape.
std::string FormatAnswer(std::size_t index, const AskQuestion& question,
                         const AskSlot& slot) {
    std::string name = question.header;
    if (name.empty()) {
        name = question.question.size() > kMaxQuestionEcho
                   ? question.question.substr(0, kMaxQuestionEcho) + "..."
                   : question.question;
        name = nlohmann::json(name).dump(-1, ' ', false,
                                         nlohmann::json::error_handler_t::replace);
    }
    name = std::to_string(index + 1) + ". " + name;
    if (!slot.other.empty()) {
        return name + ": the user answered: " +
               nlohmann::json(slot.other).dump(-1, ' ', false,
                                               nlohmann::json::error_handler_t::replace);
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

constexpr const char* kDismissedText =
    "The user dismissed the questions without answering. Do not re-ask the same thing; "
    "proceed with a sensible default or explain what you need in your reply.";
constexpr const char* kTimedOutText =
    "No answer arrived before the unattended-prompt timeout: the user is probably away. "
    "Proceed with a sensible default and state the assumption you made in your reply.";
constexpr const char* kCancelledText = "The questions were cancelled along with the turn.";
constexpr const char* kUnavailableText =
    "ask_user is not available in this session (no interactive user can answer); decide "
    "without asking and state the assumption you made in your reply.";

const char* OutcomeName(tools::PromptOutcome outcome) {
    switch (outcome) {
        case tools::PromptOutcome::Answered: return "answered";
        case tools::PromptOutcome::Dismissed: return "dismissed";
        case tools::PromptOutcome::TimedOut: return "timed_out";
        case tools::PromptOutcome::Cancelled: return "cancelled";
        case tools::PromptOutcome::Unavailable: return "unavailable";
    }
    return "unavailable";
}

// The transcript's record of the exchange, for the client's "ask" display
// renderer: every question with what (if anything) the user answered. Never
// sent to the model -- content_text is what it reads.
nlohmann::json Display(const AskRequest& request, tools::PromptOutcome outcome,
                       const std::vector<AskSlot>& slots) {
    nlohmann::json questions = nlohmann::json::array();
    for (std::size_t i = 0; i < request.questions.size(); ++i) {
        nlohmann::json wire = nlohmann::json::object();
        wire["header"] = request.questions[i].header;
        wire["question"] = request.questions[i].question;
        wire["selected"] = i < slots.size() ? slots[i].selected : std::vector<std::string>{};
        wire["other"] = i < slots.size() ? slots[i].other : std::string{};
        questions.push_back(std::move(wire));
    }
    nlohmann::json display = nlohmann::json::object();
    display["kind"] = kPromptKind;
    display["outcome"] = OutcomeName(outcome);
    display["questions"] = std::move(questions);
    return display;
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
        AskRequest request;
        if (const auto error = ParseQuestions(input, request)) {
            return {true, *error};
        }

        tools::PromptResult reply;   // Unavailable unless a broker says otherwise
        if (ctx.prompt_user) reply = ctx.prompt_user({kPromptKind, ToPayload(request)});

        std::vector<AskSlot> slots;
        tools::PromptOutcome outcome = reply.outcome;
        if (outcome == tools::PromptOutcome::Answered) {
            slots = ReadAnswer(request, reply.answer);
            // Submitted with nothing picked anywhere: that is a dismissal,
            // not an answer, and the model should hear it the same way.
            const bool any = std::any_of(slots.begin(), slots.end(), [](const AskSlot& slot) {
                return !slot.selected.empty() || !slot.other.empty();
            });
            if (!any) outcome = tools::PromptOutcome::Dismissed;
        }

        tools::ToolResult result;
        switch (outcome) {
            case tools::PromptOutcome::Answered:
                for (std::size_t i = 0; i < request.questions.size(); ++i) {
                    if (!result.content_text.empty()) result.content_text += "\n";
                    result.content_text += FormatAnswer(i, request.questions[i], slots[i]);
                }
                break;
            case tools::PromptOutcome::Dismissed: result.content_text = kDismissedText; break;
            case tools::PromptOutcome::TimedOut: result.content_text = kTimedOutText; break;
            case tools::PromptOutcome::Cancelled: result.content_text = kCancelledText; break;
            case tools::PromptOutcome::Unavailable: result.content_text = kUnavailableText; break;
        }
        result.display = Display(request, outcome, slots);
        return result;
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

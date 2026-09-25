#include "ask_display.hpp"

#include <string>

#include "minimal/client/view/tool_status.hpp"

namespace minimal::ask_view {

namespace plugins = minimal::client::plugins;
using minimal::client::Theme;

namespace {

std::string AnswerText(const nlohmann::json& question) {
    const auto& other = question.at("other").get_ref<const std::string&>();
    if (!other.empty()) return "your answer: " + other;
    std::string joined;
    for (const auto& label : question.at("selected")) {
        if (!joined.empty()) joined += ", ";
        joined += label.get<std::string>();
    }
    return joined;
}

// Why nothing was answered, in the user's terms.
std::string OutcomeText(const std::string& outcome) {
    if (outcome == "dismissed") return "dismissed without answering";
    if (outcome == "timed_out") return "no answer before the unattended timeout";
    if (outcome == "cancelled") return "cancelled with the turn";
    if (outcome == "unavailable") return "no one could answer (no interactive client)";
    return "not answered";
}

}  // namespace

ftxui::Element AskDisplayRenderer::Render(const plugins::ToolResultView& result,
                                          const Theme& theme,
                                          const plugins::RowRenderContext& ctx) const {
    using namespace ftxui;
    using minimal::client::ToolStatus;
    const auto& display = result.display;
    const auto& questions = display.at("questions");
    const auto& outcome = display.at("outcome").get_ref<const std::string&>();
    const bool answered = outcome == "answered";

    const std::string name =
        result.tool_name.empty() ? std::string("ask_user") : std::string(result.tool_name);
    Elements line{minimal::client::Disclosure(ctx, theme),
                  minimal::client::ToolHeadline(
                      result.is_error ? ToolStatus::Failed : ToolStatus::Ok, name, theme)};

    if (ctx.collapsed) {
        // One line: the first question and its answer (or why there is none).
        std::string summary;
        if (!questions.empty()) {
            summary = questions[0].at("question").get<std::string>();
            const std::string answer = answered ? AnswerText(questions[0]) : std::string{};
            summary += "  -> " + (answer.empty() ? (answered ? std::string("(not answered)")
                                                             : OutcomeText(outcome))
                                                 : answer);
            if (questions.size() > 1) {
                summary += "  (+" + std::to_string(questions.size() - 1) + " more)";
            }
        }
        line.push_back(text("  " + summary) | minimal::client::Muted(theme));
        return hbox(std::move(line));
    }

    Elements rows{hbox(std::move(line))};
    for (const auto& question : questions) {
        const auto& header = question.at("header").get_ref<const std::string&>();
        const auto& text_q = question.at("question").get_ref<const std::string&>();
        rows.push_back(paragraph((header.empty() ? "" : header + ": ") + text_q) |
                       color(theme.ColorFor("system_message")));
        if (!answered) continue;
        const std::string answer = AnswerText(question);
        rows.push_back(answer.empty()
                           ? paragraph("  (not answered)") | minimal::client::Muted(theme)
                           : paragraph("  -> " + answer) | bold |
                                 color(theme.ColorFor("assistant_message")));
    }
    if (!answered) {
        rows.push_back(paragraph("  " + OutcomeText(outcome)) | minimal::client::Muted(theme));
    }
    return vbox(std::move(rows));
}

}  // namespace minimal::ask_view

#include "export_render.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>
#include <sstream>

#include "minimal/client/sdk/utf8.hpp"
#include "minimal/common/clock.hpp"
#include "minimal/proto/struct_json.hpp"
#include "policy.hpp"

namespace minimal::session_export {

namespace {

std::string FormatLocal(std::int64_t unix_ms, const char* format) {
    const std::time_t stamp = common::UnixMillisToTimeT(unix_ms);
    std::tm local{};
    localtime_r(&stamp, &local);
    char buffer[64];
    return std::strftime(buffer, sizeof(buffer), format, &local) > 0 ? buffer : "";
}

std::string Stamp(std::int64_t unix_ms) {
    return unix_ms > 0 ? FormatLocal(unix_ms, "%Y-%m-%d %H:%M") : std::string{};
}

// The first non-blank line, cut to the preview width.
std::string Preview(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t\r#>*-");
        if (first == std::string::npos) continue;
        line.erase(0, first);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
        return client::TruncateUtf8Columns(line, kPreviewColumns);
    }
    return {};
}

std::string FirstUserText(const Transcript& transcript) {
    for (const auto& stored : transcript.messages) {
        if (stored.message().role() != v1::ROLE_USER) continue;
        for (const auto& block : stored.message().content()) {
            if (block.has_text() && !Preview(block.text().text()).empty()) {
                return Preview(block.text().text());
            }
        }
    }
    return {};
}

// A fence longer than any backtick run inside `body`, so the body cannot
// close it early.
std::string FenceFor(const std::string& body) {
    std::size_t longest = 0;
    std::size_t run = 0;
    for (const char c : body) {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return std::string(std::max<std::size_t>(3, longest + 1), '`');
}

std::string Indent(const std::string& body, const std::string& prefix) {
    std::string out;
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        out += line.empty() ? prefix.substr(0, prefix.find_last_not_of(' ') + 1) : prefix + line;
        out += '\n';
    }
    return out;
}

std::string TrimTrailingNewlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

// A tool result's own content: its text blocks, joined.
std::string ToolResultText(const v1::ToolResultBlock& result) {
    std::string text;
    for (const auto& inner : result.content()) {
        if (!inner.has_text()) continue;
        if (!text.empty()) text += '\n';
        text += inner.text().text();
    }
    return TrimTrailingNewlines(std::move(text));
}

// One labeled, verbatim body: a fenced block in markdown, an indented one in
// plain text.
std::string Verbatim(const std::string& label, const std::string& body, const std::string& lang,
                     bool plain) {
    if (plain) return "[" + label + "]\n" + Indent(body, "    ");
    const std::string fence = FenceFor(body);
    return "**" + label + "**\n\n" + fence + lang + "\n" + body + "\n" + fence + "\n";
}

// The rendered blocks of one message under `options`; empty = nothing left
// to show, and the message is dropped.
std::vector<std::string> RenderBlocks(const v1::Message& message, const ExportOptions& options) {
    const bool plain = options.plain_text;
    std::vector<std::string> parts;
    for (const auto& block : message.content()) {
        if (block.has_text()) {
            const std::string text = TrimTrailingNewlines(block.text().text());
            if (!text.empty()) parts.push_back(text + "\n");
        } else if (block.has_thinking()) {
            if (!options.thinking) continue;
            const auto& thinking = block.thinking();
            const std::string text = thinking.text().empty() && thinking.has_data()
                                         ? std::string("(redacted)")
                                         : TrimTrailingNewlines(thinking.text());
            if (text.empty()) continue;
            parts.push_back(plain ? "[thinking]\n" + Indent(text, "    ")
                                  : "> **Thinking**\n>\n" + Indent(text, "> "));
        } else if (block.has_tool_use()) {
            if (!options.tool_calls) continue;
            const auto& use = block.tool_use();
            const std::string input = proto::StructToJson(use.input()).dump(2);
            parts.push_back(Verbatim("Tool call: " + use.name(), input, "json", plain));
        } else if (block.has_tool_result()) {
            if (!options.tool_calls) continue;
            const auto& result = block.tool_result();
            const std::string label = result.is_error() ? "Tool result (error)" : "Tool result";
            parts.push_back(Verbatim(label, ToolResultText(result), "", plain));
        }
    }
    return parts;
}

// A user-role row that only carries tool results is the tools speaking, not
// the user.
bool OnlyToolResults(const v1::Message& message) {
    if (message.content().empty()) return false;
    return std::all_of(message.content().begin(), message.content().end(),
                       [](const v1::ContentBlock& block) { return block.has_tool_result(); });
}

std::string Heading(const v1::StoredMessage& stored, bool plain) {
    const auto& message = stored.message();
    std::string heading = message.role() == v1::ROLE_ASSISTANT ? "Assistant"
                          : OnlyToolResults(message)            ? "Tool"
                                                                : "User";
    if (const std::string when = Stamp(stored.created_at_unix_ms()); !when.empty()) {
        heading += " · " + when;
    }
    if (!stored.agent_preset().empty()) heading += " · " + stored.agent_preset();
    if (!stored.model().empty()) heading += " · " + stored.model();
    return plain ? "=== " + heading + " ===\n" : "## " + heading + "\n";
}

// The first seq the export includes: everything after the compaction's
// boundary, or every message.
std::int64_t FirstSeq(const Transcript& transcript, const ExportOptions& options) {
    if (!options.compaction || *options.compaction >= transcript.compactions.size()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return transcript.compactions[*options.compaction].boundary_seq() + 1;
}

}  // namespace

std::vector<StartPoint> BuildStartPoints(const Transcript& transcript) {
    std::vector<StartPoint> points;
    std::string start = "from start";
    if (const std::string first = FirstUserText(transcript); !first.empty()) {
        start += "  ·  " + first;
    }
    points.push_back({std::nullopt, std::move(start)});
    for (std::size_t i = 0; i < transcript.compactions.size(); ++i) {
        const auto& compaction = transcript.compactions[i];
        if (compaction.skipped()) continue;
        std::string label = "compaction";
        if (const std::string when = Stamp(compaction.created_at_unix_ms()); !when.empty()) {
            label += " " + when;
        }
        label += "  ·  " + (compaction.trigger().empty() ? std::string("auto") : compaction.trigger());
        label += "  ·  " + std::to_string(compaction.folded_messages()) + " folded";
        if (const std::string preview = Preview(compaction.summary()); !preview.empty()) {
            label += "  ·  " + preview;
        }
        points.push_back({i, std::move(label)});
    }
    return points;
}

std::string Render(const Transcript& transcript, const ExportOptions& options) {
    const bool plain = options.plain_text;
    std::string out = plain ? "Session " + options.session_id + "\n"
                            : "# Session " + options.session_id + "\n";

    const std::int64_t first_seq = FirstSeq(transcript, options);
    if (options.compaction && *options.compaction < transcript.compactions.size()) {
        const auto& compaction = transcript.compactions[*options.compaction];
        std::string heading = "Summary (compacted";
        if (const std::string when = Stamp(compaction.created_at_unix_ms()); !when.empty()) {
            heading += " " + when;
        }
        heading += ", " + std::to_string(compaction.folded_messages()) + " messages)";
        out += "\n" + (plain ? "=== " + heading + " ===\n" : "## " + heading + "\n");
        out += "\n" + TrimTrailingNewlines(compaction.summary()) + "\n";
    }

    for (const auto& stored : transcript.messages) {
        if (stored.seq() < first_seq) continue;
        const auto parts = RenderBlocks(stored.message(), options);
        if (parts.empty()) continue;
        out += "\n" + Heading(stored, plain);
        for (const auto& part : parts) out += "\n" + part;
    }
    return out;
}

std::size_t CountExported(const Transcript& transcript, const ExportOptions& options) {
    const std::int64_t first_seq = FirstSeq(transcript, options);
    std::size_t count = 0;
    for (const auto& stored : transcript.messages) {
        if (stored.seq() >= first_seq && !RenderBlocks(stored.message(), options).empty()) ++count;
    }
    return count;
}

std::filesystem::path DefaultPath(const std::filesystem::path& dir, const std::string& session_id,
                                  std::int64_t now_unix_ms) {
    return dir / (std::string(kFileNamePrefix) + session_id + "-" +
                  FormatLocal(now_unix_ms, "%Y%m%d-%H%M%S") + ".md");
}

bool IsPlainText(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".txt";
}

}  // namespace minimal::session_export

// Core renderers for the core display-payload kinds (docs/EXTENSIBILITY.md,
// "Tool-result display payloads"). Each throws on a payload it cannot draw --
// nlohmann's at()/get() do that for a missing or mistyped field -- and
// ChatView contains it and falls back to the text row.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/utf8.hpp"

namespace minimal::client::plugins {
namespace {

using ftxui::Element;
using ftxui::Elements;

constexpr std::size_t kOutputTailBytes = 4000;   // the default row's own bound
constexpr std::size_t kCommandHeaderChars = 120;

// Tabs and CRs mis-measure in a terminal cell grid; this text is display-only.
std::string Printable(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (const char c : line) {
        if (c == '\t') {
            out += "    ";
        } else if (c != '\r') {
            out += c;
        }
    }
    return out;
}

// The path's extension when a grammar supports it. The highlighter's alias
// table already maps the common extensions ("py", "rs", "h") to languages.
std::string LanguageOf(std::string_view path, const SyntaxHighlighter& highlighter) {
    const auto slash = path.find_last_of('/');
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) return {};
    std::string extension(name.substr(dot + 1));
    return highlighter.Supports(extension) ? extension : std::string{};
}

std::string PadLeft(const std::string& text, std::size_t width) {
    return text.size() >= width ? text : std::string(width - text.size(), ' ') + text;
}

// Highlighted lines for `lines` (already Printable), one Element each; plain
// text when the language is unknown or the highlighter disagrees on the count.
Elements CodeLines(const std::vector<std::string>& lines, const std::string& language,
                   const SyntaxHighlighter& highlighter, const Theme& theme) {
    using namespace ftxui;
    if (!language.empty() && !lines.empty()) {
        std::string code;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) code += '\n';
            code += lines[i];
        }
        Elements highlighted = highlighter.Highlight(code, language);
        if (highlighted.size() == lines.size()) return highlighted;
    }
    Elements plain;
    for (const auto& line : lines) {
        plain.push_back(text(line) | color(theme.ColorFor("assistant_message")));
    }
    return plain;
}

// "▎tool <name>" -- the default row's own prefix, so rows read alike whoever
// draws them -- then per-kind detail, then the collapse affordances.
Element Header(const ToolResultView& result, Elements detail, const Theme& theme,
               const RowRenderContext& ctx) {
    using namespace ftxui;
    const std::string name =
        result.tool_name.empty() ? std::string("tool") : std::string(result.tool_name);
    Elements line;
    line.push_back(text("▎tool " + name) | color(theme.ColorFor("syntax_function")));
    for (auto& element : detail) line.push_back(std::move(element));
    if (ctx.collapsed) {
        line.push_back(text(" (collapsed -- " + ctx.toggle_collapse_hint + " to expand)") | Muted(theme));
    }
    if (!ctx.collapse_marker.empty()) line.push_back(text(std::string(ctx.collapse_marker)) | Muted(theme));
    return hbox(std::move(line));
}

std::string FormatDuration(std::int64_t ms) {
    if (ms < 1000) return std::to_string(ms) + "ms";
    if (ms < 60'000) {
        const auto tenths = ms / 100;
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "s";
    }
    return std::to_string(ms / 60'000) + "m " + std::to_string((ms % 60'000) / 1000) + "s";
}

// --- diff --------------------------------------------------------------------

class DiffRenderer final : public DisplayRenderer {
public:
    explicit DiffRenderer(const SyntaxHighlighter& highlighter) : highlighter_(highlighter) {}

    std::string_view kind() const override { return "diff"; }

    Element Render(const ToolResultView& result, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        using namespace ftxui;
        const auto& path = result.display.at("path").get_ref<const std::string&>();
        const auto& hunks = result.display.at("hunks");

        int added = 0;
        int removed = 0;
        int widest = 1;
        for (const auto& hunk : hunks) {
            for (const auto& line : hunk.at("lines")) {
                const auto& op = line.at("op").get_ref<const std::string&>();
                if (op == "+") ++added;
                if (op == "-") ++removed;
            }
            widest = std::max({widest,
                               hunk.at("old_start").get<int>() + hunk.at("old_count").get<int>(),
                               hunk.at("new_start").get<int>() + hunk.at("new_count").get<int>()});
        }

        Elements detail{text(" " + path) | color(theme.ColorFor("assistant_message"))};
        if (result.display.value("created", false)) {
            detail.push_back(text(" (new file)") | color(theme.ColorFor("diff_added")));
        }
        detail.push_back(text("  +" + std::to_string(added)) | color(theme.ColorFor("diff_added")));
        detail.push_back(text(" -" + std::to_string(removed)) |
                         color(theme.ColorFor("diff_removed")));
        Elements parts{Header(result, std::move(detail), theme, ctx)};
        if (ctx.collapsed) return vbox(std::move(parts));
        if (hunks.empty()) {
            parts.push_back(text("  (no changes)") | Muted(theme));
            return vbox(std::move(parts));
        }

        const std::size_t gutter = std::to_string(widest).size();
        const std::string language = LanguageOf(path, highlighter_);
        bool first = true;
        for (const auto& hunk : hunks) {
            if (!first) {
                parts.push_back(text(std::string(gutter * 2 + 2, ' ') + "⋯") |
                                color(theme.ColorFor("diff_hunk")));
            }
            first = false;
            AppendHunk(hunk, gutter, language, theme, parts);
        }
        return vbox(std::move(parts));
    }

private:
    void AppendHunk(const nlohmann::json& hunk, std::size_t gutter, const std::string& language,
                    const Theme& theme, Elements& parts) const {
        using namespace ftxui;
        // Context lines are highlighted as one block of the NEW side (context
        // plus added lines), so multi-line constructs keep their colors;
        // added/removed lines take the flat diff colors -- the marker that
        // matters most in a review.
        std::vector<std::string> texts;
        std::vector<std::string> new_side;
        for (const auto& line : hunk.at("lines")) {
            texts.push_back(Printable(line.at("text").get_ref<const std::string&>()));
            if (line.at("op").get_ref<const std::string&>() != "-") new_side.push_back(texts.back());
        }
        const Elements highlighted = CodeLines(new_side, language, highlighter_, theme);

        const int old_count = hunk.at("old_count").get<int>();
        const int new_count = hunk.at("new_count").get<int>();
        // An empty side's start is the line BEFORE the hunk (unified-diff rule).
        int old_no = hunk.at("old_start").get<int>() + (old_count > 0 ? 0 : 1);
        int new_no = hunk.at("new_start").get<int>() + (new_count > 0 ? 0 : 1);
        std::size_t new_index = 0;
        const auto& lines = hunk.at("lines");
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& op = lines[i].at("op").get_ref<const std::string&>();
            const std::string old_col =
                op == "+" ? std::string(gutter, ' ') : PadLeft(std::to_string(old_no++), gutter);
            const std::string new_col =
                op == "-" ? std::string(gutter, ' ') : PadLeft(std::to_string(new_no++), gutter);
            Element numbers = text(old_col + " " + new_col + " ") | Muted(theme);
            if (op == "+") {
                ++new_index;
                parts.push_back(hbox({numbers, text("+ " + texts[i]) |
                                                   color(theme.ColorFor("diff_added"))}));
            } else if (op == "-") {
                parts.push_back(hbox({numbers, text("- " + texts[i]) |
                                                   color(theme.ColorFor("diff_removed"))}));
            } else {
                parts.push_back(hbox({numbers, text("  "), highlighted[new_index++]}));
            }
        }
    }

    const SyntaxHighlighter& highlighter_;
};

// --- file --------------------------------------------------------------------

class FileRenderer final : public DisplayRenderer {
public:
    explicit FileRenderer(const SyntaxHighlighter& highlighter) : highlighter_(highlighter) {}

    std::string_view kind() const override { return "file"; }

    Element Render(const ToolResultView& result, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        using namespace ftxui;
        const auto& display = result.display;
        const auto& path = display.at("path").get_ref<const std::string&>();
        const int start = display.at("start_line").get<int>();
        const int end = display.at("end_line").get<int>();
        const auto total = display.at("total_lines").get<std::int64_t>();

        Elements detail{text(" " + path) | color(theme.ColorFor("assistant_message")),
                        text("  lines " + std::to_string(start) + "–" + std::to_string(end) +
                             " of " + std::to_string(total)) |
                            Muted(theme)};
        Elements parts{Header(result, std::move(detail), theme, ctx)};
        if (ctx.collapsed) return vbox(std::move(parts));

        std::vector<std::string> lines;
        for (const auto& line : display.at("lines")) {
            lines.push_back(Printable(line.get_ref<const std::string&>()));
        }
        const Elements code = CodeLines(lines, LanguageOf(path, highlighter_), highlighter_, theme);
        const std::size_t gutter = std::to_string(end).size();
        for (std::size_t i = 0; i < code.size(); ++i) {
            parts.push_back(hbox({text(PadLeft(std::to_string(start + static_cast<int>(i)), gutter) +
                                       "  ") |
                                      Muted(theme),
                                  code[i]}));
        }
        if (display.value("truncated", false) && total > end) {
            parts.push_back(text("  ··· " + std::to_string(total - end) + " more lines") | Muted(theme));
        }
        return vbox(std::move(parts));
    }

private:
    const SyntaxHighlighter& highlighter_;
};

// --- process -----------------------------------------------------------------

class ProcessRenderer final : public DisplayRenderer {
public:
    std::string_view kind() const override { return "process"; }

    Element Render(const ToolResultView& result, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        using namespace ftxui;
        const auto& display = result.display;
        std::string command = display.at("command").get<std::string>();
        const auto newline = command.find('\n');
        const bool multiline = newline != std::string::npos;
        if (multiline) command.resize(newline);
        if (command.size() > kCommandHeaderChars) {
            command = TruncateUtf8(command, kCommandHeaderChars) + "…";
        } else if (multiline) {
            command += " …";
        }

        Elements detail{text(" $ " + command) | color(theme.ColorFor("assistant_message"))};
        detail.push_back(Badge(display, theme));
        if (display.contains("duration_ms")) {
            detail.push_back(text("  " + FormatDuration(display.at("duration_ms").get<std::int64_t>())) |
                             Muted(theme));
        }
        Elements parts{Header(result, std::move(detail), theme, ctx)};
        if (ctx.collapsed) return vbox(std::move(parts));

        std::string output = display.value("output", std::string{});
        bool cut = display.value("output_truncated", false);
        if (output.size() > kOutputTailBytes) {
            // Forward from the window's start, never backward (see the default
            // row's identical rule): the kept slice stays bounded.
            const std::size_t tail_start = output.size() - kOutputTailBytes;
            const auto line_start = output.find('\n', tail_start);
            output = line_start != std::string::npos
                         ? output.substr(line_start + 1)
                         : output.substr(Utf8CeilToBoundary(output, tail_start));
            cut = true;
        }
        if (!output.empty()) {
            parts.push_back(paragraph((cut ? "... " : "") + Printable(output)) |
                            color(theme.ColorFor("system_message")));
        }
        return vbox(std::move(parts));
    }

private:
    static Element Badge(const nlohmann::json& display, const Theme& theme) {
        using namespace ftxui;
        if (display.value("cancelled", false)) {
            return text("  cancelled") | color(theme.ColorFor("attention"));
        }
        if (display.value("timed_out", false)) {
            return text("  timed out") | color(theme.ColorFor("attention"));
        }
        if (display.contains("signal")) {
            return text("  signal " + std::to_string(display.at("signal").get<int>())) |
                   color(theme.ColorFor("error"));
        }
        if (!display.contains("exit_code")) return text("  exit ?") | color(theme.ColorFor("error"));
        const int code = display.at("exit_code").get<int>();
        if (code == 0) return text("  ✓") | color(theme.ColorFor("working"));
        return text("  exit " + std::to_string(code)) | color(theme.ColorFor("error"));
    }
};

// --- markdown ----------------------------------------------------------------

class MarkdownDisplayRenderer final : public DisplayRenderer {
public:
    explicit MarkdownDisplayRenderer(const MarkdownEngine& engine) : engine_(engine) {}

    std::string_view kind() const override { return "markdown"; }

    Element Render(const ToolResultView& result, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        using namespace ftxui;
        Elements parts{Header(result, {}, theme, ctx)};
        if (ctx.collapsed) return vbox(std::move(parts));
        // `text` is optional: absent means "render the result text itself"
        // (the one sanctioned exception to self-containment -- identical
        // content, so duplicating it would buy nothing).
        const auto it = result.display.find("text");
        const std::string_view body = it != result.display.end()
                                          ? std::string_view(it->get_ref<const std::string&>())
                                          : result.result_text;
        parts.push_back(engine_.Render(body, theme));
        return vbox(std::move(parts));
    }

private:
    const MarkdownEngine& engine_;
};

}  // namespace

void RegisterCoreDisplayRenderers(DisplayRendererRegistry& registry, const MarkdownEngine& engine,
                                  const SyntaxHighlighter& highlighter) {
    // Core FILLS, it does not claim -- the RegisterCoreMessageRenderers rule:
    // a kind a plugin already took is skipped silently (the claim is the point).
    const auto fill = [&registry](std::unique_ptr<DisplayRenderer> renderer) {
        if (registry.Find(renderer->kind()) == nullptr) registry.Register(std::move(renderer));
    };
    fill(std::make_unique<DiffRenderer>(highlighter));
    fill(std::make_unique<FileRenderer>(highlighter));
    fill(std::make_unique<ProcessRenderer>());
    fill(std::make_unique<MarkdownDisplayRenderer>(engine));
}

}  // namespace minimal::client::plugins

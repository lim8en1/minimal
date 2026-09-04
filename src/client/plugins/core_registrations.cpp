#include "minimal/client/plugins/core_registrations.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "minimal/client/render/streaming_markdown_view.hpp"

namespace minimal::client::plugins {

using ftxui::Element;

// --- Md4cEngine: the default markdown-engine registrant ----------------------
//
// Render delegates to MarkdownRenderer; Stream wraps the core
// StreamingMarkdownView, whose debounce-and-finalize contract ("partial input
// cannot soundly parse"; Finalize() always reparses authoritatively) the core
// implements.
class Md4cEngine final : public MarkdownEngine {
public:
    explicit Md4cEngine(const MarkdownRenderer& renderer) : renderer_(renderer) {}

    Element Render(std::string_view markdown_text, const Theme&) const override {
        return renderer_.Render(markdown_text);
    }
    std::unique_ptr<Stream> MakeStream() const override {
        return std::make_unique<Md4cStream>(renderer_);
    }

private:
    class Md4cStream final : public Stream {
    public:
        explicit Md4cStream(const MarkdownRenderer& renderer)
            : view_([&renderer](std::string_view text) { return renderer.Render(text); }) {}

        void AppendDelta(std::string_view text) override { view_.AppendDelta(text); }
        void Finalize() override { view_.Finalize(); }
        Element Render(const Theme&) const override { return view_.Render(); }

    private:
        // The core streaming view owns the debounce/finalize contract; the
        // theme was bound at MarkdownRenderer construction.
        StreamingMarkdownView view_;
    };

    const MarkdownRenderer& renderer_;
};

namespace {

using ftxui::Element;

// --- Core message renderers ---------------------------------------------------
//
// Extracted from ChatView::RebuildRows verbatim; the kind table's semantics
// live here now. Every renderer colors through the theme, never hardcoded.

Element RenderUserText(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    // Verbatim: the user's text is echoed, never re-rendered as markdown
    // (their underscores are not emphasis).
    //
    // Labeled, like the assistant rows below: an unlabeled bold block forced
    // the reader to infer the speaker from styling alone -- exactly the
    // "difficult to map user/assistant" complaint. Same one-word label shape
    // the assistant rows use, so the roles scan as a pair.
    return vbox({
        text("▎you") | color(theme.ColorFor("user_message")) | bold,
        paragraph(row.text) | color(theme.ColorFor("user_message")) | bold,
    });
}

// A large `@`-mention (a 5k-symbol file, say) is unusable inline -- collapsed
// shows only the header (path + size), expanded shows the full framed block,
// capped at kAttachmentExpandedRenderCapChars so an accidentally-expanded
// huge file cannot wedge the renderer (distinct from -- and much larger than
// -- chat_model.hpp's CollapsedByDefault threshold, which only decides
// whether a row STARTS collapsed).
constexpr std::size_t kAttachmentExpandedRenderCapChars = 200'000;

Element RenderUserAttachment(const ChatRow& row, const Theme& theme,
                             const RowRenderContext& ctx) {
    using namespace ftxui;
    Elements parts;
    const int lines = static_cast<int>(std::count(row.text.begin(), row.text.end(), '\n')) + 1;
    parts.push_back(hbox({text("▎@") | color(theme.ColorFor("user_message")),
                          text(row.attachment_path) | color(theme.ColorFor("user_message")),
                          text(" (attached, " + std::to_string(lines) + " lines)") | dim}));
    if (!ctx.collapsed) {
        std::string body = row.text;
        bool truncated = false;
        if (body.size() > kAttachmentExpandedRenderCapChars) {
            body = body.substr(0, kAttachmentExpandedRenderCapChars);
            truncated = true;
        }
        parts.push_back(paragraph(body) | color(theme.ColorFor("system_message")));
        if (truncated) {
            parts.push_back(text("··· truncated, too large to render in full ···") | dim |
                            color(theme.ColorFor("system_message")));
        }
    }
    return vbox(std::move(parts));
}

Element RenderThinking(const ChatRow& row, const Theme& theme, const RowRenderContext& ctx) {
    using namespace ftxui;
    if (ctx.collapsed) {
        return hbox({text("▎thinking ") | color(theme.ColorFor("thinking")) | dim,
                     text("(" + std::to_string(static_cast<int>(row.text.size())) +
                          " chars, collapsed -- alt+c to expand)") |
                         color(theme.ColorFor("thinking")) | dim});
    }
    return vbox({
        text("▎thinking") | color(theme.ColorFor("thinking")) | dim,
        paragraph(row.text) | color(theme.ColorFor("thinking")) | dim,
    });
}

// A bounded one-line summary of what the tool was asked to do, for the row
// header: the model's input IS the orientation ("which file", "which
// command"), and a bare "tool bash" line with nothing under it reads as
// noise. Compact JSON, truncated -- the authoritative record is persisted.
std::string InputSummary(const nlohmann::json& input) {
    if (input.is_null() || input.empty()) return "";
    try {
        std::string summary = input.dump();
        if (summary.size() > 120) summary = summary.substr(0, 120) + "…";
        return summary;
    } catch (const std::exception&) {
        return "";   // a non-serializable input degrades to no summary
    }
}

// bash's own input shape (tools-bash plugin): {"command": "..."}. Every other
// tool falls back to InputSummary's generic JSON dump -- this is a header
// improvement for the single most common tool, not a general input schema.
std::optional<std::string> BashCommandOf(const ChatRow& row) {
    if (row.tool_name != "bash") return std::nullopt;
    const auto it = row.tool_input.find("command");
    if (it == row.tool_input.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

Element RenderToolActivity(const ChatRow& row, const Theme& theme, const RowRenderContext& ctx) {
    using namespace ftxui;
    const std::string name = row.tool_name.empty() ? std::string("tool") : row.tool_name;
    Elements line;
    line.push_back(text("▎tool " + name) | color(theme.ColorFor("syntax_function")));
    if (row.tool_is_error) {
        line.push_back(text(" FAILED") | color(theme.ColorFor("error")));
    }
    if (row.tool_pending) {
        // The call has no result in this page yet: mark it rather than
        // presenting an empty-ended entry ("tool bash" with nothing under it
        // read as a broken render, not as "running").
        line.push_back(text(" ·") | color(theme.ColorFor("system_message")) | dim);
    }
    if (ctx.collapsed) line.push_back(text(" (collapsed -- alt+c to expand)") | dim);
    Elements parts;
    parts.push_back(hbox(std::move(line)));

    const auto bash_command = BashCommandOf(row);
    if (bash_command.has_value()) {
        // bash's real command, not a JSON dump of it -- the header IS the
        // orientation for the most common tool call.
        parts.push_back(paragraph("$ " + *bash_command) |
                        color(theme.ColorFor("syntax_function")));
    } else {
        // The input summary rides the header of every OTHER tool row
        // (pending and result alike): it is what makes "tool <name>" say
        // what was run.
        const std::string summary = InputSummary(row.tool_input);
        if (!summary.empty()) {
            parts.push_back(paragraph(summary) | color(theme.ColorFor("system_message")) | dim);
        }
    }
    if (ctx.collapsed) return vbox(std::move(parts));

    if (!row.text.empty()) {
        // Bounded body: the capped persisted ToolResultBlock is the
        // authoritative record; this is orientation, not an archive.
        std::string body = row.text;
        if (body.size() > 4000) {
            const auto last_newline = body.rfind('\n', body.size() - 4000);
            // A newline within the tail window: keep from there, as before.
            // No newline at all (a megabyte of single-line JSON/base64, say)
            // used to fall through and render the FULL body -- rfind returns
            // npos both when the tail has no '\n' and (via the ternary this
            // replaced) when body was short enough to skip the search
            // entirely, and the old code treated both as "render everything"
            // instead of "hard-truncate the tail".
            body = "... " + (last_newline != std::string::npos ? body.substr(last_newline)
                                                                : body.substr(body.size() - 4000));
        }
        parts.push_back(paragraph(body) | color(theme.ColorFor("system_message")) | dim);
    }
    return vbox(std::move(parts));
}

Element RenderSystem(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    // Muted and prefixed: "the harness told you something" must never read as
    // "the model said something".
    return hbox({text("▎[minimal] ") | color(theme.ColorFor("system_message")) | bold,
                 paragraph(row.text) | color(theme.ColorFor("system_message"))});
}

Element RenderAgentMarkdown(const ChatRow& row, const Theme& theme,
                            const MarkdownEngine& engine) {
    return engine.Render(row.text, theme);
}

#define MINIMAL_CORE_RENDERER(kind, body)                                       \
    class Core##kind##Renderer final : public MessageRenderer {                 \
    public:                                                                     \
        Element Render(const ChatRow& row, const Theme& theme,                  \
                       [[maybe_unused]] const RowRenderContext& ctx)            \
            const override body                                                \
    }

MINIMAL_CORE_RENDERER(UserText, { return RenderUserText(row, theme); });
MINIMAL_CORE_RENDERER(UserAttachment, { return RenderUserAttachment(row, theme, ctx); });
MINIMAL_CORE_RENDERER(Thinking, { return RenderThinking(row, theme, ctx); });
MINIMAL_CORE_RENDERER(ToolActivity, { return RenderToolActivity(row, theme, ctx); });
MINIMAL_CORE_RENDERER(System, { return RenderSystem(row, theme); });

#undef MINIMAL_CORE_RENDERER

// The one renderer that needs the engine is registered separately below, so
// the macro above stays uniform.
class CoreAgentMarkdownRenderer final : public MessageRenderer {
public:
    explicit CoreAgentMarkdownRenderer(const MarkdownEngine& engine) : engine_(engine) {}
    Element Render(const ChatRow& row, const Theme& theme,
                  const RowRenderContext&) const override {
        using namespace ftxui;
        // A persisted row loses the streaming block's transient "assistant"
        // label the moment the turn finalizes (chat_view.cpp only shows that
        // label while streaming_text is non-empty) -- without this, a
        // finished assistant message is the only row kind with no role
        // marker at all. Same theme key as the streaming label, for visual
        // continuity between "in flight" and "settled".
        return vbox({
            text("▎assistant") | color(theme.ColorFor("assistant_message")) | bold,
            RenderAgentMarkdown(row, theme, engine_),
        });
    }

private:
    const MarkdownEngine& engine_;
};

// --- Core grammar registration -------------------------------------------------

class VendoredGrammar final : public SyntaxGrammar {
public:
    explicit VendoredGrammar(const LanguageDefinition& definition) : definition_(definition) {}
    std::string_view Language() const override { return definition_.name; }
    LanguageFnPtr LanguageFn() const override { return definition_.language_fn; }
    std::span<const SyntaxRule> Rules() const override { return definition_.rules; }

private:
    LanguageDefinition definition_;
};

}  // namespace

void RegisterCoreMessageRenderers(MessageRendererRegistry& registry) {
    // Core FILLS, it does not claim: kinds a plugin already took are skipped
    // silently (the plugin's claim is the point). The warning in
    // MessageRendererRegistry::Register is for plugin-vs-plugin duplicates,
    // which the collision resolve turns fatal.
    const auto fill = [&registry](ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer) {
        if (registry.Find(kind) == nullptr) registry.Register(kind, std::move(renderer));
    };
    fill(ChatRowKind::UserText, std::make_unique<CoreUserTextRenderer>());
    fill(ChatRowKind::UserAttachment, std::make_unique<CoreUserAttachmentRenderer>());
    fill(ChatRowKind::Thinking, std::make_unique<CoreThinkingRenderer>());
    fill(ChatRowKind::ToolActivity, std::make_unique<CoreToolActivityRenderer>());
    fill(ChatRowKind::System, std::make_unique<CoreSystemRenderer>());
    // AgentMarkdown needs the engine and is registered by the caller once the
    // default engine is settled.
}

void RegisterCoreAgentMarkdownRenderer(MessageRendererRegistry& registry,
                                       const MarkdownEngine& engine) {
    if (registry.Find(ChatRowKind::AgentMarkdown) != nullptr) return;
    registry.Register(ChatRowKind::AgentMarkdown,
                      std::make_unique<CoreAgentMarkdownRenderer>(engine));
}

void RegisterCoreSyntaxGrammars(SyntaxGrammarRegistry& registry) {
    for (const LanguageDefinition& definition : VendoredLanguages()) {
        registry.Register(std::make_unique<VendoredGrammar>(definition));
    }
}

void RegisterDefaultMarkdownEngine(MarkdownEngineRegistry& registry,
                                   const MarkdownRenderer& renderer) {
    if (registry.record.registered) return;  // a plugin owns the slot
    registry.Register(std::make_unique<Md4cEngine>(renderer));
    registry.record.plugin_name.clear();  // core, not a plugin
}

}  // namespace minimal::client::plugins

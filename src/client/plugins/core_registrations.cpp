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
                          text(" (attached, " + std::to_string(lines) + " lines)") | Muted(theme),
                          text(std::string(ctx.collapse_marker)) | Muted(theme)}));
    if (!ctx.collapsed) {
        std::string body = row.text;
        bool truncated = false;
        if (body.size() > kAttachmentExpandedRenderCapChars) {
            body = body.substr(0, kAttachmentExpandedRenderCapChars);
            truncated = true;
        }
        parts.push_back(paragraph(body) | color(theme.ColorFor("system_message")));
        if (truncated) {
            parts.push_back(text("··· truncated, too large to render in full ···") |
                            color(theme.ColorFor("system_message")));
        }
    }
    return vbox(std::move(parts));
}

Element RenderThinking(const ChatRow& row, const Theme& theme, const RowRenderContext& ctx) {
    using namespace ftxui;
    if (ctx.collapsed) {
        return hbox({text("▎thinking ") | color(theme.ColorFor("thinking")),
                     text("(" + std::to_string(static_cast<int>(row.text.size())) +
                          " chars, collapsed -- " + ctx.toggle_collapse_hint +
                          " to expand)") |
                         color(theme.ColorFor("thinking")),
                     text(std::string(ctx.collapse_marker)) |
                         color(theme.ColorFor("thinking"))});
    }
    return vbox({
        hbox({text("▎thinking") | color(theme.ColorFor("thinking")),
              text(std::string(ctx.collapse_marker)) | color(theme.ColorFor("thinking"))}),
        paragraph(row.text) | color(theme.ColorFor("thinking")),
    });
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
        line.push_back(text(" ·") | color(theme.ColorFor("system_message")));
    }
    if (ctx.collapsed)
        line.push_back(text(" (collapsed -- " + ctx.toggle_collapse_hint + " to expand)") | Muted(theme));
    if (!ctx.collapse_marker.empty())
        line.push_back(text(std::string(ctx.collapse_marker)) | Muted(theme));
    Elements parts;
    parts.push_back(hbox(std::move(line)));

    const auto bash_command = BashCommandOf(row.tool_name, row.tool_input);
    const auto ask_question = AskQuestionOf(row.tool_name, row.tool_input);
    if (bash_command.has_value()) {
        // bash's real command, not a JSON dump of it -- the header IS the
        // orientation for the most common tool call.
        parts.push_back(paragraph("$ " + *bash_command) |
                        color(theme.ColorFor("syntax_function")));
    } else if (ask_question.has_value()) {
        // ask_user's real question, not a JSON dump of it. Non-dim: this
        // row is the durable record of a human decision (expanded by
        // default -- CollapsedByDefault), and the answers below it are the
        // point of the whole transcript entry.
        parts.push_back(paragraph(*ask_question) |
                        color(theme.ColorFor("system_message")));
    } else {
        // The input summary rides the header of every OTHER tool row
        // (pending and result alike): it is what makes "tool <name>" say
        // what was run.
        const std::string summary = InputSummary(row.tool_input);
        if (!summary.empty()) {
            parts.push_back(paragraph(summary) | color(theme.ColorFor("system_message")));
        }
    }
    if (ctx.collapsed) return vbox(std::move(parts));

    if (!row.text.empty()) {
        // Bounded body: the capped persisted ToolResultBlock is the
        // authoritative record; this is orientation, not an archive.
        std::string body = row.text;
        if (body.size() > 4000) {
            // FORWARD from the tail window's start, not backward from it:
            // rfind(pos) searches toward index 0 and returns the nearest '\n'
            // at-or-before pos, which can sit anywhere earlier in the string
            // (e.g. "exit\n" + a 1 MB line -- the sole '\n' at index 4 kept
            // ~1 MB, not ~4000 bytes). find(pos) only ever returns an index
            // >= pos, so the kept slice is bounded no matter where newlines
            // fall; no newline inside the window hard-truncates, as before.
            const std::size_t tail_start = body.size() - 4000;
            const auto newline = body.find('\n', tail_start);
            body = "... " + (newline != std::string::npos ? body.substr(newline + 1)
                                                            : body.substr(tail_start));
        }
        parts.push_back(paragraph(body) | color(theme.ColorFor("system_message")));
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
            // The body's base color: markdown only styles spans, so without
            // this the prose fell through to the terminal's own foreground.
            RenderAgentMarkdown(row, theme, engine_) |
                color(theme.ColorFor("assistant_message")),
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

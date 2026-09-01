#include "minimal/client/plugins/core_registrations.hpp"

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
    return paragraph(row.text) | color(theme.ColorFor("user_message")) | bold;
}

Element RenderUserAttachment(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    // Collapsed by default: the path header is the information; the body is
    // one dimmed preview tail (v1).
    return vbox({
        hbox({text("@") | color(theme.ColorFor("user_message")),
              text(row.attachment_path) | color(theme.ColorFor("user_message")),
              text(" (attached)") | dim}),
        paragraph(row.text.substr(0, 400)) | color(theme.ColorFor("system_message")) | dim,
    });
}

Element RenderThinking(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    return hbox({text("thinking ") | color(theme.ColorFor("thinking")) | dim,
                 text("(" + std::to_string(static_cast<int>(row.text.size())) +
                      " chars, hidden)") |
                     color(theme.ColorFor("thinking")) | dim});
}

Element RenderToolActivity(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    const std::string name = row.tool_name.empty() ? std::string("tool") : row.tool_name;
    Elements line;
    line.push_back(text("tool " + name) | color(theme.ColorFor("syntax_function")));
    if (row.tool_is_error) {
        line.push_back(text(" FAILED") | color(theme.ColorFor("error")));
    }
    Elements parts;
    parts.push_back(hbox(std::move(line)));
    if (!row.text.empty()) {
        // Bounded body: the capped persisted ToolResultBlock is the
        // authoritative record; this is orientation, not an archive.
        std::string body = row.text;
        const auto last_newline =
            body.size() > 4000 ? body.rfind('\n', body.size() - 4000) : std::string::npos;
        if (last_newline != std::string::npos) body = "... " + body.substr(last_newline);
        parts.push_back(paragraph(body) | color(theme.ColorFor("system_message")) | dim);
    }
    return vbox(std::move(parts));
}

Element RenderSystem(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    // Muted and prefixed: "the harness told you something" must never read as
    // "the model said something".
    return hbox({text("[minimal] ") | color(theme.ColorFor("system_message")) | bold,
                 paragraph(row.text) | color(theme.ColorFor("system_message"))});
}

Element RenderAgentMarkdown(const ChatRow& row, const Theme& theme,
                            const MarkdownEngine& engine) {
    return engine.Render(row.text, theme);
}

#define MINIMAL_CORE_RENDERER(kind, body)                          \
    class Core##kind##Renderer final : public MessageRenderer {    \
    public:                                                        \
        Element Render(const ChatRow& row, const Theme& theme)     \
            const override body                                    \
    }

MINIMAL_CORE_RENDERER(UserText, { return RenderUserText(row, theme); });
MINIMAL_CORE_RENDERER(UserAttachment, { return RenderUserAttachment(row, theme); });
MINIMAL_CORE_RENDERER(Thinking, { return RenderThinking(row, theme); });
MINIMAL_CORE_RENDERER(ToolActivity, { return RenderToolActivity(row, theme); });
MINIMAL_CORE_RENDERER(System, { return RenderSystem(row, theme); });

#undef MINIMAL_CORE_RENDERER

// The one renderer that needs the engine is registered separately below, so
// the macro above stays uniform.
class CoreAgentMarkdownRenderer final : public MessageRenderer {
public:
    explicit CoreAgentMarkdownRenderer(const MarkdownEngine& engine) : engine_(engine) {}
    Element Render(const ChatRow& row, const Theme& theme) const override {
        return RenderAgentMarkdown(row, theme, engine_);
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

#include "md4c_engine.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "markdown_renderer.hpp"
#include "streaming_markdown_view.hpp"

namespace minimal::client::markdown_md4c {

namespace {

using ftxui::Element;

class Md4cEngine final : public plugins::MarkdownEngine {
public:
    Md4cEngine(const CodeHighlighter* highlighter, plugins::HostUi* ui)
        : highlighter_(highlighter), ui_(ui) {}

    Element Render(std::string_view markdown_text, const Theme& theme) const override {
        return RendererFor(theme).Render(markdown_text);
    }
    std::unique_ptr<Stream> MakeStream() const override {
        return std::make_unique<Md4cStream>(*this);
    }

    // The renderer, bound to the host's theme. The host swaps a theme's
    // CONTENTS in place (/theme), so its address is stable and the renderer
    // reads the new palette on its next call; a different address (a test's
    // own theme) gets a renderer of its own.
    const MarkdownRenderer& RendererFor(const Theme& theme) const {
        if (!renderer_ || bound_ != &theme) {
            renderer_.emplace(&theme, highlighter_);
            bound_ = &theme;
        }
        return *renderer_;
    }

    plugins::HostUi* ui() const { return ui_; }

private:
    class Md4cStream final : public Stream {
    public:
        explicit Md4cStream(const Md4cEngine& engine)
            : engine_(engine),
              view_([this](std::string_view text) { return engine_.RendererFor(*theme_).Render(text); },
                    std::chrono::steady_clock::now,
                    [ui = engine.ui()](std::chrono::milliseconds delay) {
                        if (ui != nullptr) ui->RequestRedrawAfter(delay);
                    }) {}

        void AppendDelta(std::string_view text) override { view_.AppendDelta(text); }
        void Finalize() override {
            if (theme_ != nullptr) view_.Finalize();
            finalize_pending_ = theme_ == nullptr;  // no theme seen yet: at the first Render
        }
        Element Render(const Theme& theme) const override {
            theme_ = &theme;
            if (finalize_pending_) {
                finalize_pending_ = false;
                view_.Finalize();
            }
            return view_.Render();
        }

    private:
        const Md4cEngine& engine_;
        mutable const Theme* theme_ = nullptr;  // the render's own, for the reparse
        mutable bool finalize_pending_ = false;
        mutable StreamingMarkdownView view_;
    };

    const CodeHighlighter* highlighter_;
    plugins::HostUi* ui_;
    mutable std::optional<MarkdownRenderer> renderer_;
    mutable const Theme* bound_ = nullptr;
};

}  // namespace

void RegisterEngine(plugins::MarkdownEngineRegistry& registry, const CodeHighlighter* highlighter,
                    plugins::HostUi* ui) {
    registry.Register(std::make_unique<Md4cEngine>(highlighter, ui));
}

}  // namespace minimal::client::markdown_md4c

#include "minimal/client/plugins/core_registrations.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace minimal::client::plugins {

using ftxui::Element;

// --- The plain-text fallback engine --------------------------------------------
//
// What markdown renders as with no markdown plugin loaded (the slot is
// optional): the text itself, a line per line -- legible, never formatted.
// Streaming needs no debounce: there is no parse to fall behind.
class PlainMarkdownEngine final : public MarkdownEngine {
public:
    Element Render(std::string_view markdown_text, const Theme&) const override {
        return Lines(markdown_text);
    }
    std::unique_ptr<Stream> MakeStream() const override { return std::make_unique<PlainStream>(); }

private:
    static Element Lines(std::string_view text) {
        ftxui::Elements lines;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = text.find('\n', start);
            const std::string_view line =
                text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            lines.push_back(ftxui::paragraph(std::string(line)));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return ftxui::vbox(std::move(lines));
    }

    class PlainStream final : public Stream {
    public:
        void AppendDelta(std::string_view text) override { text_.append(text); }
        void Finalize() override {}
        Element Render(const Theme&) const override { return Lines(text_); }

    private:
        std::string text_;
    };
};

void RegisterPlainMarkdownEngine(MarkdownEngineRegistry& registry) {
    if (registry.held()) return;  // a markdown plugin owns the slot
    registry.Register(std::make_unique<PlainMarkdownEngine>());  // outside any window: core's
}

}  // namespace minimal::client::plugins

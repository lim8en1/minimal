// The EXCLUSIVE-slot fixture: claims both the chat surface and the markdown
// engine. Loading it twice (two candidates for the same .so) exercises
// "second registration warns and is ignored" through a real dlopen.
#include "minimal/client/plugins/client_plugin_abi.h"

namespace {

using minimal::client::Theme;
namespace plugins = minimal::client::plugins;

class FixtureSurface final : public plugins::ChatSurface {
public:
    const plugins::ChatSurface::Spec& spec() const override { return spec_; }
    ftxui::Component component() override {
        return ftxui::Renderer([] { return ftxui::text("fixture surface"); });
    }
    void OnEvent(const minimal::v1::ServerEvent& event) override {
        std::lock_guard<std::mutex> lock(mu_);
        last_event_session = event.session_id();
        ++event_count;
    }
    void OnTheme(const Theme&) override {}

    std::mutex mu_;
    std::string last_event_session;
    int event_count = 0;

private:
    plugins::ChatSurface::Spec spec_{"fixture.surface", "Fixture Surface"};
};

class FixtureEngine final : public plugins::MarkdownEngine {
public:
    ftxui::Element Render(std::string_view, const Theme&) const override {
        return ftxui::text("fixture engine");
    }
    std::unique_ptr<Stream> MakeStream() const override {
        return std::make_unique<FixtureStream>();
    }

private:
    class FixtureStream final : public Stream {
    public:
        void AppendDelta(std::string_view text) override { accumulated_ += text; }
        void Finalize() override { finalized_ = true; }
        ftxui::Element Render(const Theme&) const override {
            return ftxui::text(accumulated_ + (finalized_ ? "!" : ""));
        }

    private:
        std::string accumulated_;
        bool finalized_ = false;
    };
};

}  // namespace

extern "C" {
// The compile-time tag, not AbiTag(): see client_plugin_abi.h.
const char* minimal_client_plugin_abi_tag() { return MINIMAL_ABI_TAG; }
const char* minimal_client_plugin_name() { return "fixture-client-exclusive"; }
const char* minimal_client_plugin_version() { return "1.0.0"; }

void minimal_client_plugin_register_chat_surface(
    plugins::ChatSurfaceRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureSurface>());
}

void minimal_client_plugin_register_markdown_engine(
    plugins::MarkdownEngineRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureEngine>());
}
}

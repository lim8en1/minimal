// The bundled chat-view plugin's surface: config, the fold preference shared
// by every pane and kept in the plugin's state, actions and the mouse routed
// to the view, and a pane drawing a ChatFrame.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "chat_renderers_fixture.hpp"
#include "chat_surface.hpp"
#include "ftxui_harness.hpp"

#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/render/theme.hpp"

namespace plugins = minimal::client::plugins;
namespace chat_view = minimal::client::chat_view;
using minimal::client::ColorDepth;
using minimal::client::Theme;
using minimal_test::RenderElement;

namespace {

const Theme& TestTheme() {
    static const Theme theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    return theme;
}

// Keeps what SaveState stored, like the host's per-plugin file.
class StateUi final : public plugins::HostUi {
public:
    nlohmann::json state;
    void RequestRedraw() override {}
    void ShowView(const std::string&) override {}
    void CloseView(const std::string&) override {}
    void SetPanelVisible(const std::string&, bool) override {}
    void FocusPanel(const std::string&) override {}
    std::vector<plugins::KeyBindingInfo> KeyBindings() const override { return {}; }
    std::vector<minimal::client::CommandCatalogEntry> CommandCatalog() const override { return {}; }
    std::vector<std::string> ThemeNames() const override { return {}; }
    nlohmann::json LoadState() const override { return state; }
    void SaveState(const nlohmann::json& saved) override { state = saved; }
};

class FakeChatHost final : public plugins::ChatHost {
public:
    FakeChatHost() {
        plugins::RegisterPlainMarkdownEngine(engines);
        minimal_test::RegisterPlainChatRenderers(messages);
    }
    const plugins::MessageRendererRegistry& MessageRenderers() const override { return messages; }
    const plugins::ToolResultRendererRegistry& ToolRenderers() const override { return tools; }
    const plugins::DisplayRendererRegistry& DisplayRenderers() const override { return displays; }
    const plugins::MarkdownEngine& Markdown() const override { return *engines.get(); }
    std::string KeyFor(std::string_view action) const override { return std::string(action); }
    void CancelQueued(const std::string& tab_id, std::uint64_t turn_id) override {
        cancelled.emplace_back(tab_id, turn_id);
    }
    void Notice(const std::string&, const std::string& text) override { notices.push_back(text); }
    void RequestRedraw() override {}

    plugins::MarkdownEngineRegistry engines;
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tools;
    plugins::DisplayRendererRegistry displays;
    std::vector<std::pair<std::string, std::uint64_t>> cancelled;
    std::vector<std::string> notices;
};

minimal::v1::StoredMessage UserText(std::int64_t seq, const std::string& text) {
    minimal::v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(minimal::v1::ROLE_USER);
    stored.mutable_message()->add_content()->mutable_text()->set_text(text);
    return stored;
}

}  // namespace

TEST_CASE("chat surface: the fold config is auto, expanded or collapsed", "[chat_surface]") {
    CHECK(chat_view::ParseSurfaceConfig(nlohmann::json::object()).fold_thinking ==
          chat_view::kFoldDefaultAuto);
    const auto config = chat_view::ParseSurfaceConfig(
        {{"fold_thinking", "collapsed"}, {"fold_tools", "expanded"}});
    CHECK(config.fold_thinking == chat_view::kFoldCollapsed);
    CHECK(config.fold_tools == chat_view::kFoldExpanded);
    CHECK_THROWS(chat_view::ParseSurfaceConfig({{"fold_tools", "sideways"}}));
    CHECK_THROWS(chat_view::ParseSurfaceConfig({{"unknown", 1}}));
}

TEST_CASE("chat surface: declares the chat keys", "[chat_surface]") {
    chat_view::Surface surface({}, nullptr);
    std::vector<std::string> ids;
    for (const auto& action : surface.Actions()) ids.push_back(action.id);
    CHECK(std::find(ids.begin(), ids.end(), "chat.scroll_bottom") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "chat.toggle_thinking_fold") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "chat.load_older") == ids.end());   // core's
}

TEST_CASE("chat surface: a fold toggle flips every pane and is kept", "[chat_surface]") {
    StateUi ui;
    FakeChatHost host;
    {
        chat_view::Surface surface({}, &ui);
        auto a = surface.Make(host, "tab-a", TestTheme());
        auto b = surface.Make(host, "tab-b", TestTheme());
        a->OnAction("chat.toggle_thinking_fold");
        REQUIRE(host.notices.size() == 1);
        CHECK(host.notices[0].find("thinking rows: ") == 0);
        CHECK(host.notices[0].find("(saved)") != std::string::npos);
        CHECK(surface.fold_thinking() != chat_view::kFoldDefaultAuto);
    }
    // The next launch starts from the saved choice, over the config.
    const std::string saved = ui.state.value("fold_thinking", "");
    CHECK((saved == "collapsed" || saved == "expanded"));
    chat_view::Surface next({}, &ui);
    CHECK(next.fold_thinking() ==
          (saved == "collapsed" ? chat_view::kFoldCollapsed : chat_view::kFoldExpanded));
}

TEST_CASE("chat pane: draws a frame; the mouse outside the chat falls through",
          "[chat_surface]") {
    FakeChatHost host;
    chat_view::Surface surface({}, nullptr);
    auto pane = surface.Make(host, "tab-a", TestTheme());
    plugins::ChatFrame frame;
    frame.session_id = "s";
    frame.transcript_version = 1;
    frame.scrollback_page = std::make_shared<const std::vector<minimal::v1::StoredMessage>>(
        std::vector<minimal::v1::StoredMessage>{UserText(1, "hello pane")});
    const auto drawn = RenderElement(pane->Render(frame), 80, 10);
    CHECK(drawn.find("hello pane") != std::string::npos);

    ftxui::Mouse wheel;
    wheel.button = ftxui::Mouse::WheelUp;
    wheel.motion = ftxui::Mouse::Pressed;
    wheel.y = 500;   // far below the chat's viewport
    CHECK_FALSE(pane->OnMouse(ftxui::Event::Mouse("", wheel)));
    pane->OnSubmitted();   // no crash, nothing to assert beyond following the newest
}

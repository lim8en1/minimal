#pragma once

// chat-view -- the chat as the TUI's body (registries.hpp ChatSurface): one
// ChatView per tab, fed the tab's ChatFrame each frame, its live markdown
// streams kept here, the chat.* key actions and mouse handled here, and the
// fold preferences shared by every tab and kept across restarts.

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "chat_view.hpp"
#include "minimal/client/plugins/registries.hpp"

namespace minimal::client::chat_view {

namespace plugins = minimal::client::plugins;

class Pane;

// The plugin's config (client-plugins.json): the fold a session starts with,
// until a toggle (chat.toggle_thinking_fold / chat.toggle_tool_fold) saves
// its own. "auto" (the kind's default), "expanded" or "collapsed".
struct SurfaceConfig {
    int fold_thinking = kFoldDefaultAuto;
    int fold_tools = kFoldDefaultAuto;
};
SurfaceConfig ParseSurfaceConfig(const nlohmann::json& config);

class Surface final : public plugins::ChatSurface {
public:
    // `ui` (may be null: nothing persisted) keeps the fold toggles.
    Surface(SurfaceConfig config, plugins::HostUi* ui);
    std::vector<plugins::DeclaredAction> Actions() const override;
    std::unique_ptr<plugins::ChatPane> Make(plugins::ChatHost& host, const std::string& tab_id,
                                            const Theme& theme) override;

    // Pane bookkeeping and the shared fold preference.
    void Attach(Pane* pane);
    void Detach(Pane* pane);
    void ToggleFold(Pane& origin, bool thinking);
    int fold_thinking() const { return fold_thinking_; }
    int fold_tools() const { return fold_tools_; }

private:
    plugins::HostUi* ui_;
    int fold_thinking_;
    int fold_tools_;
    std::vector<Pane*> panes_;
};

class Pane final : public plugins::ChatPane {
public:
    Pane(Surface& surface, plugins::ChatHost& host, std::string tab_id, const Theme& theme);
    ~Pane() override;

    ftxui::Element Render(const plugins::ChatFrame& frame) override;
    void OnAction(std::string_view action) override;
    bool OnMouse(const ftxui::Event& event) override;
    void OnSubmitted() override;

    // A transient line on this tab.
    void Notice(const std::string& text);
    ChatView& view() { return view_; }
    const std::string& tab_id() const { return tab_id_; }

private:
    // The live text segments' markdown streams: one per segment id, fed its
    // new text, finalized when its block ends, dropped with the segment.
    void MaintainStreams(const plugins::ChatFrame& frame);

    Surface& surface_;
    plugins::ChatHost& host_;
    std::string tab_id_;
    const Theme& theme_;
    ChatView view_;
    std::vector<LiveTextStreamSlot> streams_;
};

}  // namespace minimal::client::chat_view

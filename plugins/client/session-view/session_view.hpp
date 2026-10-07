#pragma once

// session-view -- the stored sessions and a session's subagents, as the
// views core and the server open by role: "sessions" (ctrl+o, a bare
// /sessions on a sessionless tab, the /sessions view hint) and "subagents"
// (the /subagents view hint, for the focused session). Opening a row is a
// HostActions verb (OpenSession / OpenSubagent / NewTab); deleting one is a
// confirmed ClientApi::DeleteSession, then a fresh list.

#include <atomic>
#include <memory>
#include <string>

#include <ftxui/component/component.hpp>

#include "minimal/client/sdk/host.hpp"
#include "minimal/client/sdk/views.hpp"
#include "minimal/client/sdk/widgets.hpp"
#include "minimal/client/sdk/task_worker.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::session_view {

namespace client = minimal::client;
namespace plugins = minimal::client::plugins;

inline constexpr const char* kSessionsViewId = "sessions";
inline constexpr const char* kSubagentsViewId = "subagents";

// The host seams the views use (non-null; host-owned).
struct Seams {
    plugins::HostUi* ui;
    plugins::HostActions* actions;
    const plugins::WidgetKit* widgets;
    plugins::ClientApi* api;
    plugins::FocusProvider* focus;
};

struct SessionsState;
struct SubagentsState;

// The stored sessions, newest first, paged (arrow-down past the last row
// loads more), this project's or every project's (ctrl+a), sorted by recency
// or name (tab), with a "+ new session" row; ctrl+d deletes the highlighted
// one after a confirmation.
class SessionsView final : public plugins::View {
public:
    explicit SessionsView(Seams seams);
    ~SessionsView() override;
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    // A fresh first page of this project's sessions, a fresh filter.
    void OnShow() const override;

private:
    Seams seams_;
    client::Theme theme_;
    std::shared_ptr<SessionsState> state_;
    std::unique_ptr<plugins::TaskWorker> worker_;  // last: joined first
};

// The focused session's subagents: live first, ended ones behind ctrl+a.
class SubagentsView final : public plugins::View {
public:
    explicit SubagentsView(Seams seams);
    ~SubagentsView() override;
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    // The children of the session focused now, fetched whole.
    void OnShow() const override;

private:
    Seams seams_;
    client::Theme theme_;
    std::shared_ptr<SubagentsState> state_;
    std::unique_ptr<plugins::TaskWorker> worker_;  // last: joined first
};

}  // namespace minimal::session_view

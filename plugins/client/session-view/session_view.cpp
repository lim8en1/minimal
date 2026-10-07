#include "session_view.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/policy.hpp"
#include "minimal/client/sdk/utf8.hpp"
#include "minimal/client/sdk/rebuilding.hpp"
#include "session_rows.hpp"

namespace minimal::session_view {

namespace {

// A view root rebuilt from the view's state whenever it is marked stale
// (client::RebuildingComponent's rule: at the start of a render or an event,
// never mid-frame), which also claims Escape while `wants_escape` says so --
// the delete confirmation's "cancel", which would otherwise close the whole
// view (the host owns Escape for a plugin modal).
class StateView final : public ftxui::ComponentBase, public client::EscapeAware {
public:
    StateView(std::shared_ptr<std::atomic<bool>> stale, std::function<ftxui::Component()> build,
              std::function<bool()> wants_escape)
        : stale_(std::move(stale)), build_(std::move(build)), wants_escape_(std::move(wants_escape)) {}

    bool WantsEscape() const override { return wants_escape_ && wants_escape_(); }

private:
    void Sync() {
        if (child_ && !stale_->exchange(false)) return;
        stale_->store(false);
        if (child_) DetachAllChildren();
        child_ = build_();
        Add(child_);
    }
    ftxui::Element OnRender() override {
        Sync();
        return child_->Render();
    }
    bool OnEvent(ftxui::Event event) override {
        Sync();
        const ftxui::Component target = child_;  // a callback may mark it stale
        return target->OnEvent(std::move(event));
    }
    bool Focusable() const override { return true; }
    ftxui::Component ActiveChild() override { return child_; }

    std::shared_ptr<std::atomic<bool>> stale_;
    std::function<ftxui::Component()> build_;
    std::function<bool()> wants_escape_;
    ftxui::Component child_;
};

// The delete confirmation: the kit's modal chrome over a yes/no answered by
// key. Not a filterable menu -- a "type to filter" line over two fixed rows
// read as a picker, and Enter on whichever row the cursor rested on was one
// keystroke from a permanent delete. Here only 'y' deletes; 'n' or Escape
// cancels, and Enter does nothing.
class DeleteConfirm final : public ftxui::ComponentBase {
public:
    DeleteConfirm(const plugins::WidgetKit* widgets, const client::Theme* theme,
                  std::string session_label, std::function<void(bool)> on_answer)
        : widgets_(widgets),
          theme_(theme),
          label_(std::move(session_label)),
          on_answer_(std::move(on_answer)) {}

private:
    ftxui::Element OnRender() override {
        using namespace ftxui;
        return widgets_->ModalFrame(
            *theme_, "delete session?",
            {text(" " + label_) | bold,
             text(" its transcript and its subagent sessions go, permanently")},
            {}, "y: delete  ·  n / esc: cancel", client::ModalAccent::Attention);
    }
    bool OnEvent(ftxui::Event event) override {
        using ftxui::Event;
        if (event == Event::Character('y') || event == Event::Character('Y')) {
            on_answer_(true);
            return true;
        }
        if (event == Event::Character('n') || event == Event::Character('N') ||
            event == Event::Escape) {
            on_answer_(false);
            return true;
        }
        return !event.is_mouse();  // a modal: keys go nowhere else
    }
    bool Focusable() const override { return true; }

    const plugins::WidgetKit* widgets_;
    const client::Theme* theme_;
    std::string label_;
    std::function<void(bool)> on_answer_;
};

}  // namespace

// --- sessions ------------------------------------------------------------------

struct SessionsState {
    std::mutex mtx;
    // In server delivery order (updated_at DESC): "load more"'s cursor is the
    // last row the SERVER sent, never whatever the current sort shows last.
    std::vector<v1::SessionSummary> sessions;
    bool include_all_dirs = false;
    bool has_more = false;
    bool loading = false;
    std::string error;
    // One load-more in flight per cursor: key repeat on arrow-down-at-end
    // would otherwise append the same page twice.
    bool fetch_in_flight = false;
    // Bumped by every fetch sent; an answer for an older one (a reset fired
    // while it was out) is dropped rather than appended to the new list.
    std::uint64_t generation = 0;
    PickerSort sort = PickerSort::kRecent;  // display only: no refetch
    std::string confirm_delete;             // the row asked about; "" = the list
    std::shared_ptr<client::MenuMemory> memory = std::make_shared<client::MenuMemory>();
    std::shared_ptr<std::atomic<bool>> stale = std::make_shared<std::atomic<bool>>(true);
};

namespace {

// Asks for the first page (`reset`) or the next one. Any thread.
void FetchPage(const std::shared_ptr<SessionsState>& state, plugins::TaskWorker& worker,
               plugins::ClientApi* api, bool reset) {
    plugins::ClientApi::SessionQuery query;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        if (reset) {
            state->sessions.clear();
            state->has_more = false;
        } else if (state->fetch_in_flight) {
            return;
        }
        state->fetch_in_flight = true;
        state->loading = true;
        state->error.clear();
        generation = ++state->generation;
        query.include_all_dirs = state->include_all_dirs;
        // One row beyond the page: the wire has no has_more of its own.
        query.limit = client::kSessionListPageSize + 1;
        if (!reset && !state->sessions.empty()) {
            query.before_updated_at = state->sessions.back().updated_at_unix_ms();
            query.before_id = state->sessions.back().session_id();
        }
    }
    state->stale->store(true);
    worker.Post([state, api, query, generation] {
        std::string error;
        auto answer = api->ListSessions(query, &error);
        std::lock_guard<std::mutex> lock(state->mtx);
        if (generation != state->generation) return;  // superseded
        state->fetch_in_flight = false;
        state->loading = false;
        if (!answer) {
            state->error = error.empty() ? "the session list is unavailable" : error;
        } else {
            const auto& page = answer->sessions();
            const int fetched = page.size();
            state->has_more = fetched > client::kSessionListPageSize;
            const int take = state->has_more ? client::kSessionListPageSize : fetched;
            state->sessions.insert(state->sessions.end(), page.begin(), page.begin() + take);
        }
        state->stale->store(true);
    });
}

}  // namespace

SessionsView::SessionsView(Seams seams)
    : seams_(seams),
      state_(std::make_shared<SessionsState>()),
      worker_(std::make_unique<plugins::TaskWorker>("session-view", [ui = seams.ui] {
          ui->RequestRedraw();
      })) {}

SessionsView::~SessionsView() = default;

const plugins::ViewSpec& SessionsView::spec() const {
    static const plugins::ViewSpec kSpec{kSessionsViewId, "resume a session",
                                         plugins::ViewPlacement::Modal};
    return kSpec;
}

void SessionsView::OnShow() const {
    {
        // Always this project's first page: a picker that sometimes opened
        // on every project's sessions surprised more than it helped.
        std::lock_guard<std::mutex> lock(state_->mtx);
        state_->include_all_dirs = false;
        state_->confirm_delete.clear();
        state_->memory = std::make_shared<client::MenuMemory>();
    }
    FetchPage(state_, *worker_, seams_.api, /*reset=*/true);
}

ftxui::Component SessionsView::component() {
    plugins::TaskWorker* worker = worker_.get();
    auto build = [seams = seams_, state = state_, worker, theme = &theme_]() -> ftxui::Component {
        // A snapshot, and the lock released before the kit is called: the
        // menu may run its row builder (which locks) while it is built.
        std::string id;
        std::vector<v1::SessionSummary> sessions;
        bool has_more = false;
        client::MenuStatus status;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            id = state->confirm_delete;
            sessions = state->sessions;
            has_more = state->has_more;
            status = {state->loading, state->error, state->memory};
        }
        const auto close = [ui = seams.ui] { ui->CloseView(kSessionsViewId); };

        if (!id.empty()) {
            // Destructive and permanent: it asks.
            std::string label = id;
            for (const auto& session : sessions) {
                if (session.session_id() != id) continue;
                if (!session.name().empty()) label = client::StripControlChars(session.name());
            }
            return ftxui::Make<DeleteConfirm>(
                seams.widgets, theme, std::move(label),
                [state, worker, api = seams.api, id](bool confirmed) {
                         {
                             std::lock_guard<std::mutex> back_lock(state->mtx);
                             state->confirm_delete.clear();
                             state->stale->store(true);
                         }
                         if (!confirmed) return;
                         worker->Post([state, worker, api, id] {
                             std::string error;
                             const bool deleted = api->DeleteSession(id, &error);
                             // The removed row drops out of a fresh list.
                             FetchPage(state, *worker, api, /*reset=*/true);
                             if (!deleted) {
                                 std::lock_guard<std::mutex> error_lock(state->mtx);
                                 state->error = "delete failed: " + error;
                             }
                         });
                });
        }

        client::FilterableMenuSpec spec{
            .window_title = "resume a session",
            .hint_line = " enter: open  ·  ctrl+d: delete  ·  tab: sort  ·  "
                         "ctrl+a: all dirs  ·  esc: close",
            .status_suffix =
                [state](const std::string&) {
                    std::lock_guard<std::mutex> suffix_lock(state->mtx);
                    std::string suffix =
                        state->sort == PickerSort::kRecent ? "  ·  sort: recent" : "  ·  sort: name";
                    suffix += state->include_all_dirs ? "  ·  all dirs" : "  ·  this project";
                    if (state->has_more) suffix += "  ·  more below (↓ to load)";
                    return suffix;
                },
            .empty_message = " (no other stored sessions here)",
            .empty_threshold = 1,  // the "+ new session" row is always there
        };
        spec.status = std::move(status);
        return seams.widgets->FilterableMenuModal(
            [state, sessions](const std::string& filter) {
                PickerSort sort;
                {
                    std::lock_guard<std::mutex> rows_lock(state->mtx);
                    sort = state->sort;
                }
                std::vector<client::MenuRow> rows;
                for (const auto& row : BuildPickerRows(sessions, filter, sort)) {
                    rows.push_back({row.label, row.session_id});
                }
                return rows;
            },
            spec,
            {.on_select =
                 [actions = seams.actions, sessions, close](const client::MenuRow& row) {
                     if (row.id.empty()) {
                         actions->NewTab();  // the "+ new session" row
                     } else {
                         bool is_live = false;
                         for (const auto& session : sessions) {
                             if (session.session_id() == row.id) is_live = session.is_live();
                         }
                         actions->OpenSession(row.id, is_live);
                     }
                     close();
                 },
             .on_close = close,
             .on_extra_key =
                 [state, worker, api = seams.api, has_more](
                     const ftxui::Event& event, const client::MenuContext& ctx,
                     const std::function<void()>& rebuild) {
                     using ftxui::Event;
                     if (event == Event::CtrlD) {  // the new-session row has nothing to delete
                         if (!ctx.rows.empty() && !ctx.rows[ctx.selected].id.empty()) {
                             std::lock_guard<std::mutex> key_lock(state->mtx);
                             state->confirm_delete = ctx.rows[ctx.selected].id;
                             state->stale->store(true);
                         }
                         return true;
                     }
                     if (event == Event::Tab) {  // display only: no refetch
                         {
                             std::lock_guard<std::mutex> key_lock(state->mtx);
                             state->sort = state->sort == PickerSort::kRecent ? PickerSort::kName
                                                                              : PickerSort::kRecent;
                         }
                         rebuild();
                         return true;
                     }
                     if (event == Event::CtrlA) {  // changes what the server is asked for
                         {
                             std::lock_guard<std::mutex> key_lock(state->mtx);
                             state->include_all_dirs = !state->include_all_dirs;
                         }
                         FetchPage(state, *worker, api, /*reset=*/true);
                         return true;
                     }
                     // Past the last loaded row, unfiltered: the next page.
                     if (event == Event::ArrowDown && has_more && ctx.filter.empty() &&
                         !ctx.rows.empty() &&
                         ctx.selected == static_cast<int>(ctx.rows.size()) - 1) {
                         FetchPage(state, *worker, api, /*reset=*/false);
                         return true;
                     }
                     return false;
                 }},
            *theme);
    };
    return ftxui::Make<StateView>(state_->stale, std::move(build), [state = state_] {
        std::lock_guard<std::mutex> lock(state->mtx);
        return !state->confirm_delete.empty();
    });
}

// --- subagents -----------------------------------------------------------------

struct SubagentsState {
    std::mutex mtx;
    std::string parent;
    std::vector<v1::SessionSummary> children;
    bool include_ended = false;  // display only: the list is fetched whole
    bool loading = false;
    std::string error;
    std::uint64_t generation = 0;
    std::shared_ptr<client::MenuMemory> memory = std::make_shared<client::MenuMemory>();
    std::shared_ptr<std::atomic<bool>> stale = std::make_shared<std::atomic<bool>>(true);
};

SubagentsView::SubagentsView(Seams seams)
    : seams_(seams),
      state_(std::make_shared<SubagentsState>()),
      worker_(std::make_unique<plugins::TaskWorker>("session-view", [ui = seams.ui] {
          ui->RequestRedraw();
      })) {}

SubagentsView::~SubagentsView() = default;

const plugins::ViewSpec& SubagentsView::spec() const {
    static const plugins::ViewSpec kSpec{kSubagentsViewId, "subagents",
                                         plugins::ViewPlacement::Modal};
    return kSpec;
}

void SubagentsView::OnShow() const {
    // The focused session's children -- a nested subagent's tab lists its
    // own.
    const std::string parent = seams_.focus->FocusedSessionId();
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mtx);
        state_->parent = parent;
        state_->children.clear();
        state_->include_ended = false;
        state_->memory = std::make_shared<client::MenuMemory>();
        state_->error = parent.empty() ? "no session is focused" : "";
        state_->loading = !parent.empty();
        generation = ++state_->generation;
    }
    state_->stale->store(true);
    if (parent.empty()) return;
    worker_->Post([state = state_, api = seams_.api, parent, generation] {
        std::string error;
        auto children = api->ListSubagents(parent, &error);
        std::lock_guard<std::mutex> lock(state->mtx);
        if (generation != state->generation) return;  // reopened meanwhile
        state->loading = false;
        if (children) {
            state->children = std::move(*children);
        } else {
            state->error = error.empty() ? "the subagent list is unavailable" : error;
        }
        state->stale->store(true);
    });
}

ftxui::Component SubagentsView::component() {
    auto build = [seams = seams_, state = state_, theme = &theme_]() -> ftxui::Component {
        bool include_ended = false;
        std::vector<v1::SessionSummary> children;
        std::string parent;
        client::MenuStatus status;
        {
            std::lock_guard<std::mutex> lock(state->mtx);  // a snapshot; released before the kit
            include_ended = state->include_ended;
            children = state->children;
            parent = state->parent;
            status = {state->loading, state->error, state->memory};
        }
        const auto close = [ui = seams.ui] { ui->CloseView(kSubagentsViewId); };
        client::FilterableMenuSpec spec{
            .window_title = "subagents",
            .hint_line = " enter: open  ·  ctrl+a: " +
                         std::string(include_ended ? "hide ended" : "show ended") +
                         "  ·  esc: close",
            .empty_message = include_ended ? " (no subagents)"
                                           : " (no running subagents -- ctrl+a shows ended ones)",
        };
        spec.status = std::move(status);
        return seams.widgets->FilterableMenuModal(
            [children, include_ended](const std::string& filter) {
                std::vector<client::MenuRow> rows;
                for (const auto& row : BuildSubagentPickerRows(children, include_ended, filter)) {
                    rows.push_back({row.label, row.session_id});
                }
                return rows;
            },
            spec,
            {.on_select =
                 [actions = seams.actions, children, parent, close](const client::MenuRow& row) {
                     for (const auto& child : children) {
                         if (child.session_id() != row.id) continue;
                         actions->OpenSubagent(row.id, parent, child.is_live());
                         close();
                         return;
                     }
                 },
             .on_close = close,
             .on_extra_key =
                 [state](const ftxui::Event& event, const client::MenuContext&,
                         const std::function<void()>&) {
                     if (event != ftxui::Event::CtrlA) return false;
                     std::lock_guard<std::mutex> key_lock(state->mtx);
                     state->include_ended = !state->include_ended;
                     state->stale->store(true);  // the hint line changes too
                     return true;
                 }},
            *theme);
    };
    return ftxui::Make<client::RebuildingComponent>(state_->stale, std::move(build));
}

}  // namespace minimal::session_view

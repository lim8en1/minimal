#pragma once

// The widget contracts: what a view hands the widget kit (WidgetKit,
// minimal/client/sdk/widgets.hpp) and gets back -- rows, specs,
// callbacks and state, no FTXUI construction. Shared by every widget kit and
// every view built on one, so a replacement kit renders the same data the
// same views already produce.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <nlohmann/json.hpp>

#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

// --- Modal chrome ---------------------------------------------------------------

// What a modal's frame says about it. Attention is for a question the user
// must answer before work goes on (a permission, an ask, a destructive
// confirm); everything else -- a form, a listing -- wears the focus border,
// like the pickers, so the accent keeps its meaning.
enum class ModalAccent { Focus, Attention };

// --- The filterable menu ------------------------------------------------------

// One row of the filtered list. `id` is caller-defined: session_id for the
// session picker (empty => the synthetic "+ new session" row), preset name
// for the agent picker, "<provider>/<model>" for the model picker.
struct MenuRow {
    std::string label;
    std::string id;
    // A header row is a group label (the model picker's provider names):
    // rendered bold and never prefixed with the cursor, Enter on it is
    // swallowed, and the cursor never RESTS on one -- ResolveSelectableRow
    // (below) walks the selection to the nearest selectable row after every
    // filter change and before every render, so FTXUI's Menu can keep owning
    // the arrow keys without a header ever wearing (or hiding) the cursor.
    bool header = false;
    // Optional "#rrggbb" for the label (the agent picker's preset.color).
    // Anything unparseable renders as the theme default -- a picker row must
    // never throw over cosmetics.
    std::string color = {};
    // Optional annotation rendered muted after the label (the model picker's
    // context window). Empty renders nothing.
    std::string detail = {};
};

// What on_extra_key needs to answer a key the generic handler doesn't
// recognize: the session picker's ctrl+d (delete the highlighted row) and
// arrow-down-at-end (load more, only when unfiltered and already on the
// last row) both need the CURRENT row list, the highlighted index, and the
// filter text, not just the event itself.
struct MenuContext {
    const std::vector<MenuRow>& rows;
    int selected;
    const std::string& filter;
};

struct FilterableMenuCallbacks {
    // Return, with the highlighted row (rows is never empty when this
    // fires -- Return is swallowed unconditionally on an empty list, same
    // as the two pickers already did, so there is nothing to select).
    std::function<void(const MenuRow&)> on_select;
    std::function<void()> on_close;   // Escape

    // Keys the generic handler doesn't recognize (session picker: ctrl+d,
    // tab, ctrl+a, arrow-down-at-end; agent picker: nullptr, it has none).
    // `rebuild` is the shell's OWN rebuild -- call it if the handler
    // mutated something that changes what build_rows(filter) would return
    // (e.g. the session picker's Tab cycling its own external sort state).
    // Returns whether the event was consumed.
    std::function<bool(const ftxui::Event&, const MenuContext&, const std::function<void()>& rebuild)>
        on_extra_key;
};

// The menu input that must survive a component REBUILD. Pickers rebuild
// their component whenever fetched data lands (a page, a load-more, a
// refresh); a fresh component used to start with an empty filter and the
// cursor on row 0, so type-ahead entered while the fetch was in flight --
// and the cursor after "load more" -- were thrown away. The owner keeps one
// per picker, resets it on open, and hands it to every rebuild.
struct MenuMemory {
    std::string filter;
    std::string selected_id;   // the highlighted row's MenuRow::id ("" = none yet)
};

// What the list's data source is doing, rendered in place of the empty
// message: without it an open-while-fetching picker read "(no models
// match)", and a failed fetch said the same thing forever.
struct MenuStatus {
    bool loading = false;
    std::string error;   // the last fetch's failure, "" when none
    std::shared_ptr<MenuMemory> memory;
};

struct FilterableMenuSpec {
    std::string window_title;    // "resume a session" / "switch agent preset"
    std::string hint_line;       // caller-composed key hints, rendered as the footer
    // Shown flush right on the filter line (a leading "  ·  " is dropped) --
    // the session picker adds sort/all-dirs/more-below; the agent picker
    // leaves this empty. Composed fresh each render so it reflects state
    // (sort order, has_more) the shell has no access to.
    std::function<std::string(const std::string& filter)> status_suffix;
    std::string empty_message;   // "(no other stored sessions here)" / "(no agent presets match)"
    // The empty message shows when build_rows(filter).size() <= this. The
    // session picker always has its synthetic "+ new session" row, so it
    // sets this to 1; the agent picker (no synthetic row) leaves it 0.
    std::size_t empty_threshold = 0;
    // If set, the factory stores a closure clearing the filter and the
    // cursor selection. The help overlays are built ONCE (app.cpp), so
    // without this the typed filter survived across opens -- reopening the
    // help presented yesterday's needle as if it were a fresh search. The
    // PICKERS deliberately do not reset (reopening with the filter intact
    // is the feature there).
    std::function<void()>* reset_state = nullptr;
    MenuStatus status;
};

// --- The schema form -----------------------------------------------------------

struct SchemaFormState {
    std::string title;
    nlohmann::json describe;  // null while loading
    std::string status;       // the last edit's outcome, or a load error
    bool status_is_error = false;
    // Bumped whenever `describe` changes: the rows rebuild, keeping the
    // selected field.
    std::uint64_t generation = 0;
    // An edit was sent and its answer has not landed: the shown values may
    // be stale, so the form takes no edit keys (a second toggle would send
    // the same value again) and says it is saving.
    bool busy = false;
    // The field the last refused edit was for ("" = none): an edit typed in
    // the editor reopens with its text, so a refusal does not lose it.
    std::string error_path;
};

// What the form asks its owner to choose a value for (on_pick): the field,
// its current value, and where the choice goes. `deliver` takes the chosen
// value as the field's text, or nullopt to unset it; the form decides what
// that does -- saved at once on the plugin's own form, kept in the draft of
// a list item or map entry being edited. Calling it after the form moved on
// is harmless.
struct PickRequest {
    std::string path;          // the field's path, as its form knows it
    nlohmann::json field;      // its schema field ("choices", "format", ...)
    nlohmann::json current;    // its value now (null: unset)
    std::function<void(std::optional<std::string> text)> deliver;
};

struct SchemaFormCallbacks {
    std::function<void(const std::string& path, const std::string& text)> on_set;
    std::function<void(const std::string& path)> on_unset;
    std::function<void()> on_back;  // optional
    // Optional. Enter on an enum, or on a field with a "format" (a model
    // ref): the owner shows a chooser and sends what is chosen through its
    // own set/unset. Unset: an enum steps to its next choice and a formatted
    // field opens the text editor, as before.
    std::function<void(const PickRequest& request)> on_pick;
};

// --- The model selector --------------------------------------------------------

// A chooser over the server's models, which it fetches itself -- through the
// function the view hands it, so a plugin's ClientApi and core's own client
// feed the same widget.
struct ModelSelectorSpec {
    std::string window_title = "choose a model";
    std::string select_hint = "choose";  // what Enter does, for the hint line
    // Answers the model list, on `post`'s worker -- never the UI thread.
    // `refresh` (ctrl+r) re-runs every provider's discovery first and can
    // take seconds. nullopt: it failed, and `error` says why (shown in place
    // of the list).
    std::function<std::optional<v1::ListModelsResponse>(bool refresh, std::string& error)> fetch;
    // Where `fetch` runs: the caller's own long-lived worker
    // (TaskWorker::Post). The selector owns no thread, so dropping it -- a
    // modal closed mid-refresh -- never waits on a fetch; an answer landing
    // after that goes nowhere. Unset: the selector says it has no worker.
    std::function<void(std::function<void()>)> post;
    // Called from the worker after each answer lands (HostUi::RequestRedraw).
    std::function<void()> redraw;
    // A row above the models that is not one ("off (untagged)", "the
    // session's own model"); on_select receives `leading_row_id` for it.
    // Empty id: no such row.
    std::string leading_row_label;
    std::string leading_row_id;
    // The leading row stands for the server's default model: shown only
    // when the answer names one, and labelled "default (<its ref>)".
    bool leading_row_is_default = false;
    // Offer the typed `<provider>/<model>` itself when the list does not
    // have it (discovery offline, a provider not configured yet).
    bool allow_custom = false;
    // Answers a model's own name (ModelSummary::model_name -- what its
    // provider is sent, and what a per-model setting is keyed by) instead of
    // its `<provider>/<model>` ref; a typed custom value is taken as a name.
    bool choose_model_name = false;
};

struct ModelSelectorCallbacks {
    std::function<void(const std::string& ref)> on_select;  // a ref, or leading_row_id
    std::function<void()> on_close;                         // Escape
};

// --- Escape ownership ----------------------------------------------------------

// Implemented by a plugin modal's root component that has a use for Escape
// of its own while some state lasts (an open inline editor). Asked per key.
class EscapeAware {
public:
    virtual ~EscapeAware();
    virtual bool WantsEscape() const = 0;
};

}  // namespace minimal::client

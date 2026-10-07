#pragma once

// What a plugin adds to the TUI's surface: views (panels and modals), start
// views, prompt renderers (the modal for a prompt kind) and client-side
// /-commands.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

#include "minimal/client/sdk/theme.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client::plugins {

// --- Prompt renderers (keyed by plugin prompt kind) -------------------------

// What a prompt renderer answers with -- the PluginPromptResponse minus the
// request id, which the host fills in. `answer` is sent only with ANSWERED.
struct PromptReply {
    v1::PromptOutcome outcome = v1::PROMPT_OUTCOME_DISMISSED;
    nlohmann::json answer = nullptr;
};

// Everything a prompt renderer builds its modal FROM. Valid only for the
// Make() call: a component copies whatever it keeps.
struct PromptView {
    std::string_view kind;
    // The server plugin's request, parsed from PluginPromptRequest.payload_json.
    const nlohmann::json& payload;
    // Who is asking, for the modal's wording: the tab's agent label ("" for
    // the main agent, e.g. "review" on a subagent's tab).
    std::string_view asker;
};

// Draws one KIND of prompt as a modal: a plugin kind (docs/EXTENSIBILITY.md,
// "Plugin prompts" -- the client half of a server plugin that calls
// ToolContext::prompt_user), or one of core's own two, "permission" and
// "confirmation" (§4.2: their payloads and answers, both kinds required at
// startup). The host shows it in its modal layer, rebuilds it per request id,
// and sends whatever `on_reply` receives. A kind with no renderer, or a Make()
// that throws, is answered at once -- UNAVAILABLE for a plugin kind, deny /
// "do not proceed" for core's -- never left hanging.
class PromptRenderer {
public:
    virtual ~PromptRenderer() = default;
    virtual std::string_view kind() const = 0;
    // `theme` outlives the component. Call `on_reply` once; the host ignores
    // any later call for the same request (the slot is gone by then).
    virtual ftxui::Component Make(const PromptView& prompt, const Theme& theme,
                                  std::function<void(PromptReply)> on_reply) const = 0;
};

// --- Views (additive: panel or modal) ---------------------------------------

enum class ViewPlacement { Panel, Modal };

struct ViewSpec {
    // The canonical id: what the plugin's own code addresses (HostUi view
    // requests). Users address the SURFACE name -- this id, or the
    // client-plugins.json alias that renamed it (keybindings.json, view hints).
    std::string id;
    std::string title;
    ViewPlacement placement = ViewPlacement::Panel;
    // Registered as the DEFAULT binding only if the user's keybindings.json
    // does not already name this id (Keybindings::EnsureDefault).
    std::string default_keybinding = {};
};

// A self-contained view. Panels render in the panel region; modals render in
// the modal layer -- BELOW the quit confirmation and the permission modal,
// always: a plugin modal cannot preempt or mask the security UI (the invariant
// is structural in the host's modal nesting, and tested).
class View {
public:
    virtual ~View() = default;
    virtual const ViewSpec& spec() const = 0;
    virtual ftxui::Component component() = 0;

    // Called once after the theme is loaded (views register before it), the
    // same push ChatSurface gets. Without it the interface's own "colors come
    // from theme roles" contract (see plugin_abi.h's View docs) is
    // unimplementable: a view has no other route to the resolved theme.
    // Default no-op so views that hardcode styles keep compiling. A throw
    // here disables nothing yet (the host catches); render-time calls are
    // contained separately.
    virtual void OnTheme(const Theme& theme) { (void)theme; }

    // Called on the UI thread each time the host makes the view visible (a
    // command's view_hint, or its keybinding toggling it on) -- the moment a
    // view over live server state (the mcp picker) starts a fresh fetch. Must
    // not block: kick background work and return. Default no-op. A throw is
    // contained by the host (logged; the view still opens). Const, like the
    // rest of the dispatch surface (PluginViewEntry holds a const View*): a
    // view keeps whatever it kicks behind its own pointer members.
    virtual void OnShow() const {}
};

// --- Start views (additive: the initial view on start) ----------------------

// The initial view rendered while there is no conversation content yet: the
// app-level empty state (no tab open) and a freshly opened session before its
// first message arrives. Additive -- every registered start view renders,
// stacked in registration order; a duplicate id is fatal through the shared
// name resolution like every other view id.
//
// Element, not Component: a start view is non-interactive decoration, which
// keeps the 20 Hz render contract trivially cheap. The per-render session id
// is what lets a plugin vary its art per session (a generator keyed by
// session_id -- regenerating inside Render() with no key would re-randomize
// every frame at 20 Hz).
class StartView {
public:
    virtual ~StartView() = default;
    virtual const ViewSpec& spec() const = 0;
    // `session_id` empty = the app-level empty state (no tab open). Colors
    // come from theme roles, never hardcoded (the same contract as every
    // other render seam); missing roles fall back to Color::Default.
    virtual ftxui::Element Render(const std::string& session_id, const Theme& theme) const = 0;
};

// A client-side /-command a plugin contributes: it runs in the TUI and never
// reaches the server (compare a server plugin's command, which runs on the
// session worker). Listed in /-autocomplete and the /help overlay beside the
// server's commands; a name the client's own commands already use is ignored
// with a startup warning. Run() executes on the UI thread: cheap, never
// blocking -- hand slow work to the plugin's own thread, then RequestRedraw or
// ShowView through the context's HostUi.
class ClientCommand {
public:
    virtual ~ClientCommand() = default;
    virtual std::string_view name() const = 0;      // without the leading '/'
    virtual std::string_view summary() const = 0;   // the autocomplete line
    virtual bool requires_session() const { return false; }
    // Changes nothing a session holds (it opens a view, switches the theme):
    // offered on a subagent tab, whose command surface is read-only.
    virtual bool read_only() const { return false; }
    // False: the command only intercepts some forms of a SERVER command of
    // the same name (the bare `/model` that opens a picker, while
    // `/model <ref>` stays the server's) -- not listed on its own; the
    // catalog lists the server's entry.
    virtual bool listed() const { return true; }
    // `positional`: whitespace-split arguments; `raw`: everything after the
    // name, untouched. Returns whether it handled the line; false passes it
    // on to the server's command of that name (an intercept declining a
    // form it does not own).
    virtual bool Run(const std::vector<std::string>& positional, const std::string& raw) = 0;
};

}  // namespace minimal::client::plugins

// checklist-view -- the bundled `{todo}` status segment AND the toggleable
// `checklist.panel` panel view: two presentation surfaces over ONE data path.
//
// Paired with the server's bundled `checklist` plugin (plugins/server/checklist),
// which exposes per-session task lists over the generic PluginCall channel as
// "checklist.rpc" ("get"/"list", see checklist_model.hpp for the shapes).
//
// ChecklistPoller (checklist_poller.cpp) owns every fetch: one background
// worker, one cadence, one cache, shared by both hooks below regardless of
// registration order. The segment renders the focused session's counts; the
// panel view renders the same data as either surface -- FOCUSED (the current
// tab's tasklist) or SUMMARY (one line per open tab, display names via the
// FocusProvider seam). Neither surface ever calls PluginCall from a render
// path; the poller's "cheap, never block" contract is the
// StatusSegment::Render discipline this plugin has always run on.

#include <memory>
#include <string>

#include "checklist_panel_view.hpp"
#include "checklist_poller.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/common/abi_tag.hpp"

namespace {

namespace plugins = minimal::client::plugins;
using namespace minimal::checklist_view;

// The {todo} status segment, now thin over the shared poller.
class ChecklistStatusSegment final : public plugins::StatusSegment {
public:
    explicit ChecklistStatusSegment(std::shared_ptr<ChecklistPoller> poller)
        : poller_(std::move(poller)) {}

    std::string_view id() const override { return "todo"; }

    // Same tab-switch discipline as always, now one nudge deep: a focus
    // change clears the poller's focused cache and wakes its worker, so the
    // label never shows the previous tab's checklist.
    std::string Render(const std::string& focused_session_id) const override {
        poller_->OnFocusHint(focused_session_id);
        return poller_->FocusedText();
    }

private:
    std::shared_ptr<ChecklistPoller> poller_;
};

}  // namespace

namespace {
// One poller per process (this .so is never unloaded): whichever hook runs
// first creates it, the second reuses it.
std::shared_ptr<ChecklistPoller> shared_instance;
std::shared_ptr<ChecklistPoller> SharedPoller(plugins::ClientApi* api,
                                              plugins::FocusProvider* focus) {
    if (shared_instance == nullptr) {
        shared_instance = std::make_shared<ChecklistPoller>(api, focus);
    }
    return shared_instance;
}
}  // namespace

extern "C" {

const char* minimal_client_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_client_plugin_name() { return "checklist-view"; }
const char* minimal_client_plugin_version() { return minimal::common::Version(); }

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // No client_api seam offered: degrade to not registering rather than a
    // segment that can never do anything.
    if (context.client_api == nullptr) return;
    registry.Register(std::make_unique<ChecklistStatusSegment>(
        SharedPoller(context.client_api, context.focus)));
}

void minimal_client_plugin_register_views(
    plugins::ViewRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // Both seams or none: the panel renders focus-keyed data it cannot fetch
    // without the api, and cannot even name the focused session without the
    // focus provider. Degrade to not registering, the same rule the segment
    // follows for a missing api.
    if (context.client_api == nullptr || context.focus == nullptr) return;
    registry.Register(std::make_unique<ChecklistPanelView>(
        SharedPoller(context.client_api, context.focus), context.focus));
}

}  // extern "C"

// checklist-view -- the bundled `{todo}` status segment AND the toggleable
// `checklist.panel` panel view: two presentation surfaces over ONE data path.
//
// Paired with the server's bundled `checklist` plugin (plugins/server/checklist),
// which exposes per-session task lists over the generic PluginCall channel as
// "checklist.rpc" ("get"/"list", see checklist_model.hpp for the shapes).
//
// ChecklistFeed (checklist_feed.hpp) owns every call: one background worker
// holding the server's "changes" push stream (with polling as the fallback),
// one per-session cache, shared by both hooks below regardless of
// registration order. The segment renders the focused session's counts; the
// panel view renders the same data as either surface -- FOCUSED (the current
// tab's tasklist) or SUMMARY (one line per open tab, display names via the
// FocusProvider seam). Neither surface ever calls out from a render path:
// the StatusSegment::Render "cheap, never block" discipline.

#include <memory>
#include <string>

#include "checklist_panel_view.hpp"
#include "checklist_feed.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"

namespace {

namespace plugins = minimal::client::plugins;
using namespace minimal::checklist_view;

// The {todo} status segment, thin over the shared feed.
class ChecklistStatusSegment final : public plugins::StatusSegment {
public:
    explicit ChecklistStatusSegment(std::shared_ptr<ChecklistFeed> feed)
        : feed_(std::move(feed)) {}

    std::string_view id() const override { return "todo"; }

    // Keyed by the frame's own session id, so the label can never show the
    // previous tab's checklist; the call doubles as the feed's focus hint.
    std::string Render(const std::string& focused_session_id) const override {
        return feed_->SegmentText(focused_session_id);
    }

private:
    std::shared_ptr<ChecklistFeed> feed_;
};

}  // namespace

namespace {
// One feed for both hooks: whichever runs first creates it, the second
// reuses it. WEAK: the registered segment and view own it, so it dies with
// the host's registries -- while the ClientApi/FocusProvider seams its
// worker calls are still alive. A strong static here kept the worker running
// until static destruction at process exit, calling into seams RunTui had
// already destroyed.
std::weak_ptr<ChecklistFeed> shared_instance;
std::shared_ptr<ChecklistFeed> SharedFeed(plugins::ClientApi* api,
                                          plugins::FocusProvider* focus, plugins::HostUi* ui) {
    auto feed = shared_instance.lock();
    if (feed == nullptr) {
        feed = std::make_shared<ChecklistFeed>(api, focus);
        feed->SetHostUi(ui);
        shared_instance = feed;
    }
    return feed;
}
}  // namespace

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("checklist-view")

extern "C" {

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // No client_api seam offered: degrade to not registering rather than a
    // segment that can never do anything.
    if (context.client_api == nullptr) return;
    registry.Register(std::make_unique<ChecklistStatusSegment>(
        SharedFeed(context.client_api, context.focus, context.ui)));
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
        SharedFeed(context.client_api, context.focus, context.ui), context.focus));
}

}  // extern "C"

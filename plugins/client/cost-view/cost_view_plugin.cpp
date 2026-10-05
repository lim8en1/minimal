// cost-view -- the bundled `{cost}` and `{token_detail}` status-bar segments.
//
// Paired with the server's bundled `cost-tracker` plugin (plugins/server/
// cost-tracker): that plugin computes real cumulative $ costs and per-model
// token totals (input/output plus cache read/write) per session, subagent
// subtree included, and pushes them over the generic PluginSubscribe channel
// as they change. {cost} renders the dollar figure, {token_detail} the four
// token sums (the formatters decide; see cost_view_format.hpp).
//
// StatusSegment::Render() runs on the render thread at ~20 Hz with a "cheap,
// never block" contract (registries.hpp), so the segments only ever read the
// per-session cache TotalsFeed keeps (totals_feed.hpp) -- one stream and
// one cache behind both segments.

#include <memory>
#include <string>
#include <utility>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/common/abi_tag.hpp"
#include "totals_feed.hpp"

namespace {

namespace plugins = minimal::client::plugins;
using minimal::client::cost_view::TotalsFeed;
using minimal::client::cost_view::TotalsField;

class TotalsSegment final : public plugins::StatusSegment {
public:
    TotalsSegment(std::string_view id, TotalsField field, std::shared_ptr<TotalsFeed> feed)
        : id_(id), field_(field), feed_(std::move(feed)) {}

    std::string_view id() const override { return id_; }

    std::string Render(const std::string& focused_session_id) const override {
        return feed_->Render(focused_session_id, field_);
    }
    // Reference detail, but above the session name and version.
    int priority() const override { return 40; }

private:
    std::string_view id_;
    TotalsField field_;
    std::shared_ptr<TotalsFeed> feed_;
};

// Focus and tab closes, pushed: the feed follows the focused session even
// when no {cost} segment is rendered, and drops a closed tab's figures.
class TotalsObserver final : public plugins::Observer {
public:
    explicit TotalsObserver(std::shared_ptr<TotalsFeed> feed) : feed_(std::move(feed)) {}
    void OnFocusChanged(const plugins::TabInfo& focused) override {
        feed_->SetFocus(focused.session_id);
    }
    void OnTabClosed(const plugins::TabInfo& tab) override {
        if (!tab.session_id.empty()) feed_->Forget(tab.session_id);
    }

private:
    std::shared_ptr<TotalsFeed> feed_;
};

// The segments' feed, for the observer hook that runs after them. WEAK: the
// registered segments own it (see the hook below).
std::weak_ptr<TotalsFeed> shared_feed;

}  // namespace

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("cost-view")

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // No client_api seam offered (e.g. a host that hasn't wired one, or a
    // test fixture exercising other registries): degrade to not registering
    // rather than segments that can never do anything.
    if (context.client_api == nullptr) return;
    // shared_ptr: the LAST segment destroyed tears the feed (and its worker
    // thread) down -- the registry owns the segments, and neither outliving
    // nor underliving the other is expressible this way.
    auto feed = std::make_shared<TotalsFeed>(context.client_api);
    feed->SetHostUi(context.ui);
    shared_feed = feed;
    registry.Register(std::make_unique<TotalsSegment>("cost", TotalsField::kCost, feed));
    registry.Register(
        std::make_unique<TotalsSegment>("token_detail", TotalsField::kTokenDetail, feed));
}

// Runs after register_status_segments (the host's hook order): no feed
// there, nothing to observe.
void minimal_client_plugin_register_observers(
    plugins::ObserverRegistry& registry,
    const plugins::ClientPluginRegistrationContext&) {
    if (auto feed = shared_feed.lock()) registry.Register(std::make_unique<TotalsObserver>(feed));
}

}  // extern "C"

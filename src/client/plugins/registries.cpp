#include "minimal/client/plugins/registries.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::client::plugins {

// --- AttentionState ----------------------------------------------------------

void AttentionState::RaiseTab(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    tab_[session_id] = true;
}

void AttentionState::ClearTab(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    tab_[session_id] = false;
}

void AttentionState::RaiseGlobal() {
    std::lock_guard<std::mutex> lock(mtx_);
    global_ = true;
}

void AttentionState::ClearGlobal() {
    std::lock_guard<std::mutex> lock(mtx_);
    global_ = false;
}

bool AttentionState::TabRaised(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = tab_.find(session_id);
    return it != tab_.end() && it->second;
}

bool AttentionState::GlobalRaised() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return global_;
}

std::vector<std::string> AttentionState::RaisedTabs() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> out;
    for (const auto& [session_id, raised] : tab_) {
        if (raised) out.push_back(session_id);
    }
    return out;
}

// --- ExclusiveRecord ----------------------------------------------------------

bool ExclusiveRecord::Claim(std::string_view claimant) {
    if (registered) {
        common::DefaultLogger().warn(
            "client plugin '{}': {} slot already claimed by '{}' -- registration ignored "
            "(the exclusive-slot rule: first registration wins)",
            claimant, plugin_name.empty() ? "the core default" : "the exclusive", plugin_name);
        return false;
    }
    registered = true;
    plugin_name = std::string(claimant);
    return true;
}

// --- Additive registries ------------------------------------------------------

void ViewRegistry::Register(std::unique_ptr<View> view) {
    attributions_.push_back({attribution_, view->spec().id});
    views_.push_back(std::move(view));
}

View* ViewRegistry::Find(std::string_view id) const {
    for (const auto& view : views_) {
        if (view->spec().id == id) return view.get();
    }
    return nullptr;
}

void ToolResultRendererRegistry::Register(std::unique_ptr<ToolResultRenderer> renderer) {
    attributions_.push_back({attribution_, std::string(renderer->tool_name())});
    renderers_.push_back(std::move(renderer));
}

const ToolResultRenderer* ToolResultRendererRegistry::Find(std::string_view tool_name) const {
    for (const auto& renderer : renderers_) {
        if (renderer->tool_name() == tool_name) return renderer.get();
    }
    return nullptr;
}

void MessageRendererRegistry::Register(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer) {
    claims_.emplace_back(kind, Attribution{attribution_, std::string(ToString(kind))});
    // First claimant stays active; the duplicate is visible in claims() and the
    // loader turns it fatal. Silently replacing would make presentation depend
    // on plugin load order -- exactly what the collision rule exists to prevent.
    if (renderers_.find(kind) == renderers_.end()) {
        renderers_.emplace(kind, std::move(renderer));
    } else {
        common::DefaultLogger().warn(
            "client plugin '{}': message-kind '{}' already claimed -- registration recorded "
            "and ignored (first claimant wins; the duplicate is fatal at startup)",
            attribution_, ToString(kind));
    }
}

const MessageRenderer* MessageRendererRegistry::Find(ChatRowKind kind) const {
    const auto it = renderers_.find(kind);
    return it == renderers_.end() ? nullptr : it->second.get();
}

void StatusSegmentRegistry::Register(std::unique_ptr<StatusSegment> segment) {
    attributions_.push_back({attribution_, std::string(segment->id())});
    segments_.push_back(std::move(segment));
}

const StatusSegment* StatusSegmentRegistry::Find(std::string_view id) const {
    for (const auto& segment : segments_) {
        if (segment->id() == id) return segment.get();
    }
    return nullptr;
}

void SyntaxGrammarRegistry::Register(std::unique_ptr<SyntaxGrammar> grammar) {
    attributions_.push_back({attribution_, std::string(grammar->Language())});
    grammars_.push_back(std::move(grammar));
}

// --- Exclusive registries -----------------------------------------------------

void ChatSurfaceRegistry::Register(std::unique_ptr<ChatSurface> candidate) {
    if (candidate == nullptr) return;  // a plugin passing nothing claims nothing
    attributions_.push_back({attribution_, candidate->spec().id});
    if (!record.Claim(attribution_)) return;
    surface = std::move(candidate);
}

void MarkdownEngineRegistry::Register(std::unique_ptr<MarkdownEngine> candidate) {
    if (candidate == nullptr) return;
    attributions_.push_back({attribution_, "markdown_engine"});
    if (!record.Claim(attribution_)) return;
    engine = std::move(candidate);
}

}  // namespace minimal::client::plugins

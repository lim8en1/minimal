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
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    // Same null guard the exclusive registries have: Register(nullptr)
    // compiles at every plugin call site and dereferencing spec() segfaults.
    if (view == nullptr) return;
    attributions_.push_back({attribution_, view->spec().id});
    views_.push_back(std::move(view));
}

View* ViewRegistry::Find(std::string_view id) const {
    for (const auto& view : views_) {
        if (view->spec().id == id) return view.get();
    }
    return nullptr;
}

void ViewRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // views_ and attributions_ are index-aligned (Register pushes to both in
    // lockstep) -- compact both together rather than via std::remove_if on
    // either alone, which would desync them.
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            views_[write] = std::move(views_[read]);
        }
        ++write;
    }
    attributions_.resize(write);
    views_.resize(write);
}

void StartViewRegistry::Register(std::unique_ptr<StartView> view) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (view == nullptr) return;
    attributions_.push_back({attribution_, view->spec().id});
    views_.push_back(std::move(view));
}

StartView* StartViewRegistry::Find(std::string_view id) const {
    for (const auto& view : views_) {
        if (view->spec().id == id) return view.get();
    }
    return nullptr;
}

void StartViewRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            views_[write] = std::move(views_[read]);
        }
        ++write;
    }
    attributions_.resize(write);
    views_.resize(write);
}

void ToolResultRendererRegistry::Register(std::unique_ptr<ToolResultRenderer> renderer) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (renderer == nullptr) return;
    attributions_.push_back({attribution_, std::string(renderer->tool_name())});
    renderers_.push_back(std::move(renderer));
}

const ToolResultRenderer* ToolResultRendererRegistry::Find(std::string_view tool_name) const {
    for (const auto& renderer : renderers_) {
        if (renderer->tool_name() == tool_name) return renderer.get();
    }
    return nullptr;
}

void ToolResultRendererRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            renderers_[write] = std::move(renderers_[read]);
        }
        ++write;
    }
    attributions_.resize(write);
    renderers_.resize(write);
}

void DisplayRendererRegistry::Register(std::unique_ptr<DisplayRenderer> renderer) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (renderer == nullptr) return;
    attributions_.push_back({attribution_, std::string(renderer->kind())});
    renderers_.push_back(std::move(renderer));
}

const DisplayRenderer* DisplayRendererRegistry::Find(std::string_view kind) const {
    for (const auto& renderer : renderers_) {
        if (renderer->kind() == kind) return renderer.get();
    }
    return nullptr;
}

void DisplayRendererRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            renderers_[write] = std::move(renderers_[read]);
        }
        ++write;
    }
    attributions_.resize(write);
    renderers_.resize(write);
}

void PromptRendererRegistry::Register(std::unique_ptr<PromptRenderer> renderer) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (renderer == nullptr) return;
    attributions_.push_back({attribution_, std::string(renderer->kind())});
    renderers_.push_back(std::move(renderer));
}

const PromptRenderer* PromptRendererRegistry::Find(std::string_view kind) const {
    for (const auto& renderer : renderers_) {
        if (renderer->kind() == kind) return renderer.get();
    }
    return nullptr;
}

void PromptRendererRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            renderers_[write] = std::move(renderers_[read]);
        }
        ++write;
    }
    attributions_.resize(write);
    renderers_.resize(write);
}

void MessageRendererRegistry::Register(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (renderer == nullptr) return;   // a stored null would Find() as null and silently disable the kind
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

void MessageRendererRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // claims_ records EVERY claim attempt in order, but only the FIRST claim
    // for a given kind actually won the registry slot (Register(): first
    // claimant stays, a later duplicate is recorded here but never reaches
    // renderers_) -- erase renderers_[kind] only when THIS plugin is that
    // first claimant, never for a losing duplicate that happens to name a
    // kind some earlier plugin still owns.
    std::map<ChatRowKind, bool> owner_decided;
    for (const auto& [kind, attribution] : claims_) {
        if (owner_decided[kind]) continue;
        owner_decided[kind] = true;
        if (attribution.plugin_name == plugin_name) renderers_.erase(kind);
    }
    claims_.erase(std::remove_if(claims_.begin(), claims_.end(),
                                 [&](const auto& claim) {
                                     return claim.second.plugin_name == plugin_name;
                                 }),
                  claims_.end());
}

void StatusSegmentRegistry::Register(std::unique_ptr<StatusSegment> segment) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (segment == nullptr) return;
    attributions_.push_back({attribution_, std::string(segment->id())});
    segments_.push_back(std::move(segment));
}

const StatusSegment* StatusSegmentRegistry::Find(std::string_view id) const {
    for (const auto& segment : segments_) {
        if (segment->id() == id) return segment.get();
    }
    return nullptr;
}

void StatusSegmentRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            segments_[write] = std::move(segments_[read]);
        }
        ++write;
    }
    attributions_.resize(write);
    segments_.resize(write);
}

void SyntaxGrammarRegistry::Register(std::unique_ptr<SyntaxGrammar> grammar) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (grammar == nullptr) return;
    attributions_.push_back({attribution_, std::string(grammar->Language())});
    grammars_.push_back(std::move(grammar));
}

void SyntaxGrammarRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            grammars_[write] = std::move(grammars_[read]);
        }
        ++write;
    }
    attributions_.resize(write);
    grammars_.resize(write);
}

// --- Exclusive registries -----------------------------------------------------

void ChatSurfaceRegistry::Register(std::unique_ptr<ChatSurface> candidate) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (candidate == nullptr) return;  // a plugin passing nothing claims nothing
    attributions_.push_back({attribution_, candidate->spec().id});
    if (!record.Claim(attribution_)) return;
    surface = std::move(candidate);
}

void ChatSurfaceRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // Register() records every claim attempt in attributions_ before checking
    // Claim() -- a LOSING claimant still gets an attributions_ entry even
    // though it never became the holder. Erase that entry unconditionally;
    // only reset the slot itself (surface/record) when this plugin actually
    // holds it, mirroring MessageRendererRegistry::RemoveByAttribution.
    if (record.registered && record.plugin_name == plugin_name) {
        surface.reset();
        record = ExclusiveRecord{};
    }
    attributions_.erase(std::remove_if(attributions_.begin(), attributions_.end(),
                                       [&](const Attribution& a) {
                                           return a.plugin_name == plugin_name;
                                       }),
                        attributions_.end());
}

void MarkdownEngineRegistry::Register(std::unique_ptr<MarkdownEngine> candidate) {
    if (sealed_) {
        common::DefaultLogger().warn(
            "client plugin '{}': registration ignored -- the registries were sealed after "
            "startup (registration-time-only contract)",
            attribution_.empty() ? "<core>" : attribution_);
        return;
    }
    if (candidate == nullptr) return;
    attributions_.push_back({attribution_, "markdown_engine"});
    if (!record.Claim(attribution_)) return;
    engine = std::move(candidate);
}

void MarkdownEngineRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // See ChatSurfaceRegistry::RemoveByAttribution: a losing claimant's
    // attributions_ entry must be erased unconditionally, not only when it
    // holds the slot.
    if (record.registered && record.plugin_name == plugin_name) {
        engine.reset();
        record = ExclusiveRecord{};
    }
    attributions_.erase(std::remove_if(attributions_.begin(), attributions_.end(),
                                       [&](const Attribution& a) {
                                           return a.plugin_name == plugin_name;
                                       }),
                        attributions_.end());
}

}  // namespace minimal::client::plugins

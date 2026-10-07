#pragma once

// The client slot registries (docs/EXTENSIBILITY.md 4.2) -- the TUI's
// pluggable presentation surface, mirroring the server's registry pattern.
// What each slot's entries ARE lives in the seam headers (host, render,
// views, widgets, prompt_editor, chat); this is how they are registered:
//
//   - ADDITIVE registries (views, renderers, status segments, grammars, ...)
//     take many plugins; a duplicate key is recorded through the shared name
//     resolution and is FATAL at startup, naming both claimants. A bundled
//     default may register as a FALLBACK for its key, which any claim of the
//     key replaces.
//
//   - EXCLUSIVE slots (chat surface, markdown engine, widget kit, prompt
//     editor, tab bar) take one implementation. Every claimant is recorded,
//     so two claims are FATAL too: which implementation stands must not
//     depend on load order. Replacing a bundled one means disabling it.
//     Some slots are REQUIRED (startup refuses while they are empty); the
//     markdown engine alone falls back to core's plain one.
//
// CONTRACT: registration-time only, ENFORCED -- every registry is sealed
// once loading finishes, and a later Register warns and is ignored. Still
// single-threaded: nothing concurrent ever registers.

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minimal/client/sdk/chat_model.hpp"
#include "minimal/client/sdk/host.hpp"
#include "minimal/client/sdk/prompt_editor.hpp"
#include "minimal/client/sdk/render.hpp"
#include "minimal/client/sdk/views.hpp"
#include "minimal/client/sdk/widgets.hpp"
#include "minimal/plugins/name_table.hpp"

namespace minimal::client::plugins {

// The chat surface (EXCLUSIVE, REQUIRED) is chat.hpp's: its host hands out
// the renderer registries below.
class ChatSurface;

// --- Registries -------------------------------------------------------------

// Common attribution mechanics, identical to the server's registries: the
// loader opens a window around each plugin's registration callback and
// everything arriving inside it is attributed to that plugin.
struct Attribution {
    std::string plugin_name;    // empty for what core registers itself
    std::string original_name;  // the key as registered
};

// What every registry shares: the attribution window, the record of who
// registered what, rollback, and the seal.
//
// Registration-time-only enforcement, the server's Seal ported: after the
// loader has run every plugin's register_* hook, the host seals each
// registry. A plugin that stashed its registry reference and registered
// LATER used to bypass the collision rule, the attribution windows and (for
// exclusive slots) the core default entirely, silently; a sealed Register
// warns and is ignored instead.
//
// HOST-ONLY: Seal, SetAttribution, ClearAttribution, RemoveByAttribution and
// ApplyAliases are the loader's and the bootstrap's, public for the same
// reason the server's registries keep them public (one pattern on both
// sides, and the tests drive them). A plugin calls Register /
// RegisterFallback and reads; it never calls these.
class RegistryBase {
public:
    virtual ~RegistryBase() = default;

    void Seal() { sealed_ = true; }
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    // Every accepted registration, in order: (plugin, key). An additive
    // registry keeps a duplicate here (the collision scan's input); an
    // exclusive slot records only its holder.
    const std::vector<Attribution>& attributions() const { return attributions_; }

    // Partial-registration rollback (ClientPluginLoader::LoadOne): strips
    // everything `plugin_name` registered here, so a plugin whose LATER hook
    // throws does not leave an earlier hook's registrations live to collide
    // with a later, healthy plugin.
    virtual void RemoveByAttribution(const std::string& plugin_name) = 0;

    // Renames entries per client-plugins.json `aliases` (keyed by stable
    // identity, "plugin.original_name"). Called by the bootstrap only for
    // NAME namespaces, after the collision check passed; a slot registry is
    // never renamed. Default: nothing to rename.
    virtual void ApplyAliases(const std::map<std::string, std::string>& aliases) { (void)aliases; }
    // Nothing registered (a required slot left empty refuses startup).
    virtual bool empty() const = 0;

protected:
    // False, with a warning, once sealed.
    bool Admit() const;

    std::string attribution_;
    std::vector<Attribution> attributions_;
    bool sealed_ = false;
};

// Many plugins, one entry per key. `KeyOf` maps an entry to its key (a view
// id, a tool name, ...). A duplicate key is NOT replaced: it is kept and
// recorded, and the startup collision scan (name resolution) turns the
// duplicate fatal, naming both claimants. Each entry also has a SURFACE name
// -- its key, or the alias ApplyAliases gave it -- and Find() looks up the
// surface.
//
// FALLBACKS (RegisterFallback): a bundled default for a key that yields to
// any claim of it -- Find() answers the claim when there is one, the fallback
// otherwise, and the two never collide. Two fallbacks for one key do (they
// are recorded under "fallback:<key>").
template <typename T, typename KeyOf>
class AdditiveRegistry : public RegistryBase {
public:
    // Takes ownership. Null, or an empty key, registers nothing.
    void Register(std::unique_ptr<T> entry) {
        if (!Admit() || entry == nullptr) return;
        std::string key(KeyOf{}(*entry));
        if (key.empty()) return;
        surfaces_.push_back(key);
        owners_.push_back(attribution_);
        attributions_.push_back({attribution_, std::move(key)});
        entries_.push_back(std::move(entry));
    }

    // A default for its key that any Register() of the same key replaces.
    void RegisterFallback(std::unique_ptr<T> entry) {
        if (!Admit() || entry == nullptr) return;
        std::string key(KeyOf{}(*entry));
        if (key.empty()) return;
        attributions_.push_back({attribution_, std::string(kFallbackPrefix) + key});
        fallback_keys_.push_back(std::move(key));
        fallback_owners_.push_back(attribution_);
        fallbacks_.push_back(std::move(entry));
    }

    // The claims, in registration order (fallbacks are not listed).
    const std::vector<std::unique_ptr<T>>& All() const { return entries_; }
    bool empty() const override { return entries_.empty() && fallbacks_.empty(); }
    // Index-aligned with All(): the name each entry answers to.
    const std::vector<std::string>& surfaces() const { return surfaces_; }
    // Index-aligned with All(): the plugin that registered each entry ("" =
    // core). Not attributions(), which interleaves the fallbacks.
    const std::vector<std::string>& owners() const { return owners_; }

    // The claim for `surface`, else its fallback, else null.
    T* Find(std::string_view surface) const {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (surfaces_[i] == surface) return entries_[i].get();
        }
        for (std::size_t i = 0; i < fallbacks_.size(); ++i) {
            if (fallback_keys_[i] == surface) return fallbacks_[i].get();
        }
        return nullptr;
    }
    // Whether a claim (not a fallback) answers `surface`.
    bool Claimed(std::string_view surface) const {
        return std::find(surfaces_.begin(), surfaces_.end(), surface) != surfaces_.end();
    }

    void ApplyAliases(const std::map<std::string, std::string>& aliases) override {
        std::size_t entry = 0;
        for (const auto& attribution : attributions_) {
            if (IsFallback(attribution)) continue;
            const auto it = aliases.find(minimal::plugins::StableIdentity(
                attribution.plugin_name, attribution.original_name));
            if (it != aliases.end()) surfaces_[entry] = it->second;
            ++entry;
        }
    }

    void RemoveByAttribution(const std::string& plugin_name) override {
        // Claims: entries_ and surfaces_ are index-aligned with the claim
        // attributions (attributions_ minus the fallback ones).
        std::size_t write = 0;
        std::size_t entry = 0;
        for (const auto& attribution : attributions_) {
            if (IsFallback(attribution)) continue;
            if (attribution.plugin_name != plugin_name) {
                if (write != entry) {
                    entries_[write] = std::move(entries_[entry]);
                    surfaces_[write] = std::move(surfaces_[entry]);
                    owners_[write] = std::move(owners_[entry]);
                }
                ++write;
            }
            ++entry;
        }
        entries_.resize(write);
        surfaces_.resize(write);
        owners_.resize(write);
        write = 0;
        for (std::size_t read = 0; read < fallbacks_.size(); ++read) {
            if (fallback_owners_[read] == plugin_name) continue;
            if (write != read) {
                fallbacks_[write] = std::move(fallbacks_[read]);
                fallback_keys_[write] = std::move(fallback_keys_[read]);
                fallback_owners_[write] = std::move(fallback_owners_[read]);
            }
            ++write;
        }
        fallbacks_.resize(write);
        fallback_keys_.resize(write);
        fallback_owners_.resize(write);
        std::erase_if(attributions_, [&](const Attribution& attribution) {
            return attribution.plugin_name == plugin_name;
        });
    }

private:
    static constexpr std::string_view kFallbackPrefix = "fallback:";
    static bool IsFallback(const Attribution& attribution) {
        return attribution.original_name.rfind(kFallbackPrefix, 0) == 0;
    }

    std::vector<std::unique_ptr<T>> entries_;
    std::vector<std::string> surfaces_;
    std::vector<std::string> owners_;
    std::vector<std::unique_ptr<T>> fallbacks_;
    std::vector<std::string> fallback_keys_;
    std::vector<std::string> fallback_owners_;
};

// Warns that `claimant` lost the exclusive slot to `holder`.
void WarnTaken(const std::string& holder, const std::string& claimant);

// One holder. Every claimant is recorded under the slot's own name (KeyOf
// answers a constant), so two plugins claiming the slot collide in the
// startup name resolution -- fatal, naming both: which implementation stands
// must not depend on load order, and bundled plugins load first. Replacing a
// bundled default means disabling it. The first claimant holds the slot
// until then. The core default registers only when no plugin claimed it.
template <typename T, typename KeyOf>
class ExclusiveSlot : public RegistryBase {
public:
    void Register(std::unique_ptr<T> candidate) {
        if (!Admit() || candidate == nullptr) return;
        attributions_.push_back({attribution_, std::string(KeyOf{}(*candidate))});
        if (held_) {
            WarnTaken(holder_, attribution_);
            return;
        }
        held_ = true;
        holder_ = attribution_;
        entry_ = std::move(candidate);
    }

    T* get() const { return entry_.get(); }
    bool held() const { return held_; }
    bool empty() const override { return !held_; }
    // Who holds the slot: a plugin's name, or "" for core.
    const std::string& holder() const { return holder_; }

    // Drops `plugin_name`'s claims, and releases the slot only IF it holds
    // it -- a plugin that failed a LATER hook must not release another's.
    void RemoveByAttribution(const std::string& plugin_name) override {
        std::erase_if(attributions_, [&](const Attribution& attribution) {
            return attribution.plugin_name == plugin_name;
        });
        if (!held_ || holder_ != plugin_name) return;
        entry_.reset();
        held_ = false;
        holder_.clear();
    }

private:
    std::unique_ptr<T> entry_;
    bool held_ = false;
    std::string holder_;
};

struct ViewKey {
    std::string_view operator()(const View& view) const { return view.spec().id; }
};

struct StartViewKey {
    std::string_view operator()(const StartView& view) const { return view.spec().id; }
};

struct ClientCommandKey {
    std::string_view operator()(const ClientCommand& command) const { return command.name(); }
};

struct ToolRendererKey {
    std::string_view operator()(const ToolResultRenderer& r) const { return r.tool_name(); }
};

struct DisplayRendererKey {
    std::string_view operator()(const DisplayRenderer& r) const { return r.kind(); }
};

struct PromptRendererKey {
    std::string_view operator()(const PromptRenderer& r) const { return r.kind(); }
};

struct StatusSegmentKey {
    std::string_view operator()(const StatusSegment& segment) const { return segment.id(); }
};

struct SyntaxGrammarKey {
    std::string_view operator()(const SyntaxGrammar& grammar) const { return grammar.Language(); }
};

// An exclusive slot's key is the slot itself: every claimant collides.
struct ChatSurfaceKey {
    std::string_view operator()(const ChatSurface&) const { return "chat_surface"; }
};

// Observers have no identity to collide on: one constant key.
struct ObserverKey {
    std::string_view operator()(const Observer&) const { return "observer"; }
};

struct MarkdownEngineKey {
    std::string_view operator()(const MarkdownEngine&) const { return "markdown_engine"; }
};

struct WidgetKitKey {
    std::string_view operator()(const WidgetKit&) const { return "widget_kit"; }
};

struct TabBarKey {
    std::string_view operator()(const TabBar&) const { return "tab_bar"; }
};

struct PromptEditorKey {
    std::string_view operator()(const PromptEditorFactory&) const { return "prompt_editor"; }
};

using ViewRegistry = AdditiveRegistry<View, ViewKey>;

using StartViewRegistry = AdditiveRegistry<StartView, StartViewKey>;

using ClientCommandRegistry = AdditiveRegistry<ClientCommand, ClientCommandKey>;

using ToolResultRendererRegistry = AdditiveRegistry<ToolResultRenderer, ToolRendererKey>;

using DisplayRendererRegistry = AdditiveRegistry<DisplayRenderer, DisplayRendererKey>;

using PromptRendererRegistry = AdditiveRegistry<PromptRenderer, PromptRendererKey>;

using StatusSegmentRegistry = AdditiveRegistry<StatusSegment, StatusSegmentKey>;

// The language table the highlighter consults, in registration order.
using SyntaxGrammarRegistry = AdditiveRegistry<SyntaxGrammar, SyntaxGrammarKey>;

using ObserverRegistry = AdditiveRegistry<Observer, ObserverKey>;

// Required: the TUI's body.
using ChatSurfaceRegistry = ExclusiveSlot<ChatSurface, ChatSurfaceKey>;

// Null get(): the host registers core's plain-text engine (the bundled md4c
// engine is a plugin's claim).
using MarkdownEngineRegistry = ExclusiveSlot<MarkdownEngine, MarkdownEngineKey>;

// Required: the bundled ui-kit's, or a replacement. Filled before any view's
// component() is built.
using WidgetKitRegistry = ExclusiveSlot<WidgetKit, WidgetKitKey>;

// Null get(): no tab bar.
using TabBarRegistry = ExclusiveSlot<TabBar, TabBarKey>;

// Required: the TUI has no other way to take input.
using PromptEditorRegistry = ExclusiveSlot<PromptEditorFactory, PromptEditorKey>;

// Keyed by the ChatRowKind a plugin claims, not by the renderer, so it keeps
// its own Register. First claimant stays active; a second claim is recorded
// (attributions(), keyed by ToString(kind)) and the collision scan turns it
// fatal, like every additive key.
class MessageRendererRegistry : public RegistryBase {
public:
    // Claims and fallbacks alike, as every additive registry counts them.
    bool empty() const override { return renderers_.empty() && fallbacks_.empty(); }
    void Register(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer);
    // A default for `kind` that any Register() of it replaces (the bundled
    // chat-renderers). Two fallbacks for one kind collide.
    void RegisterFallback(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer);
    // The claim for a kind, else its fallback, else null.
    const MessageRenderer* Find(ChatRowKind kind) const;
    // Whether a claim (not a fallback) answers `kind`: the chat view routes
    // a claimed kind's LIVE rows to it too.
    bool PluginClaimed(ChatRowKind kind) const;

    void RemoveByAttribution(const std::string& plugin_name) override;

private:
    std::map<ChatRowKind, std::unique_ptr<MessageRenderer>> renderers_;
    std::map<ChatRowKind, std::unique_ptr<MessageRenderer>> fallbacks_;
    // Index-aligned with attributions_: the kind each registration named,
    // and whether it was a fallback.
    std::vector<ChatRowKind> kinds_;
    std::vector<bool> fallback_;
};

}  // namespace minimal::client::plugins

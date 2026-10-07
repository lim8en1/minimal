#include "minimal/client/view/prompt_host.hpp"

#include <algorithm>
#include <exception>

#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

#include "minimal/client/config_file_io.hpp"
#include "minimal/client/policy.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {

namespace {

// What of a submitted line the history may keep. A /-command whose
// arguments may carry a credential (CommandCatalogEntry::sensitive_args) is
// kept as its bare name, and so is any /-command with arguments while the
// catalog is not known yet: the history is persisted in plaintext.
std::string HistoryLine(const std::string& line, const CommandCatalogSource& catalog) {
    if (line.empty() || line.front() != '/') return line;
    const auto end = line.find_first_of(" \t\n");
    if (end == std::string::npos) return line;
    const std::string name = line.substr(1, end - 1);
    const auto entries = catalog != nullptr ? catalog() : nullptr;
    if (entries == nullptr || entries->empty()) return "/" + name;
    for (const auto& entry : *entries) {
        if (entry.name == name) return entry.sensitive_args ? "/" + name : line;
    }
    return line;
}

}  // namespace

// --- PromptHistory -----------------------------------------------------------

void PromptHistory::Record(const std::string& submitted) {
    const std::string line = HistoryLine(submitted, catalog_);
    // Skip a consecutive duplicate of the last entry -- repeatedly
    // submitting the same line, or repeating what recall just brought back,
    // should not grow the list every time.
    if (!entries_.empty() && entries_.back() == line) return;
    entries_.push_back(line);
    // Bounded. A browse position is a sequence number (PromptTimeline), so a
    // front trim moves FirstSeq and no editor's position needs fixing up.
    if (entries_.size() > kMaxPromptHistoryEntries) {
        const std::size_t trimmed = entries_.size() - kMaxPromptHistoryEntries;
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<long>(trimmed));
        first_seq_ += trimmed;
    }
    Save();
}

void PromptHistory::PersistTo(std::filesystem::path path) {
    persist_path_ = std::move(path);
    std::string text;
    if (!TryReadConfigFile(persist_path_, &text)) return;
    const auto doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_array()) return;
    std::vector<std::string> loaded;
    for (const auto& line : doc) {
        if (line.is_string()) loaded.push_back(line.get<std::string>());
    }
    // Loaded lines come BEFORE anything recorded this run, under the same
    // entry bound Record keeps.
    entries_.insert(entries_.begin(), loaded.begin(), loaded.end());
    if (entries_.size() > kMaxPromptHistoryEntries) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<long>(entries_.size() - kMaxPromptHistoryEntries));
    }
}

void PromptHistory::Save() const {
    if (persist_path_.empty()) return;
    // The newest entries_ that fit the byte budget, oldest first on disk.
    std::size_t bytes = 0;
    std::size_t first = entries_.size();
    while (first > 0 && bytes + entries_[first - 1].size() <= kMaxPromptHistoryFileBytes) {
        bytes += entries_[first - 1].size();
        --first;
    }
    nlohmann::json doc = nlohmann::json::array();
    for (std::size_t i = first; i < entries_.size(); ++i) doc.push_back(entries_[i]);
    if (!WriteConfigFileAtomic(persist_path_,
                               doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) {
        common::DefaultLogger().warn("prompt history: could not write {}",
                                     persist_path_.string());
    }
}


// --- PerTabPromptEditors -----------------------------------------------------

void PerTabPromptEditors::SetBuilder(Build build) { build_ = std::move(build); }

PerTabPromptEditors::Entry* PerTabPromptEditors::EntryFor(const std::string& tab) {
    auto it = entries_.find(tab);
    if (it != entries_.end()) return &it->second;
    if (!build_) return nullptr;
    // Plugin code: a throw (or a null editor) leaves the tab without one --
    // logged, retried on the next resolution, never fatal.
    Entry entry;
    try {
        entry.editor = build_();
        if (entry.editor != nullptr) entry.component = entry.editor->component();
    } catch (const std::exception& error) {
        common::DefaultLogger().error("prompt editor: building one threw: {}", error.what());
        return nullptr;
    } catch (...) {
        common::DefaultLogger().error("prompt editor: building one threw");
        return nullptr;
    }
    if (entry.editor == nullptr || entry.component == nullptr) return nullptr;
    return &entries_.emplace(tab, std::move(entry)).first->second;
}

ftxui::Component PerTabPromptEditors::Resolve(const std::string& tab) {
    std::lock_guard lock(mtx_);
    Entry* entry = EntryFor(tab);
    return entry != nullptr ? entry->component : nullptr;
}

plugins::PromptEditor* PerTabPromptEditors::Editor(const std::string& tab) {
    std::lock_guard lock(mtx_);
    Entry* entry = EntryFor(tab);
    return entry != nullptr ? entry->editor.get() : nullptr;
}

bool PerTabPromptEditors::Built(const std::string& tab) const {
    std::lock_guard lock(mtx_);
    return entries_.count(tab) != 0;
}

bool PerTabPromptEditors::WantsFrames(const std::string& tab) const {
    std::lock_guard lock(mtx_);
    // find, not EntryFor: a tab the pump peeked at must not have an editor
    // built for it from another thread; absent means "nothing armed".
    const auto it = entries_.find(tab);
    return it != entries_.end() && it->second.editor->WantsFrames();
}

void PerTabPromptEditors::ForgetTab(const std::string& tab) {
    std::lock_guard lock(mtx_);
    // The "" (pre-tab) entry is never forgotten -- it IS the app's no-tab
    // editor.
    if (!tab.empty()) entries_.erase(tab);
}

// The per-tab editor host. A ComponentBase subclass, NOT a
// Renderer+CatchEvent composition -- for the reason modal_host.cpp
// documents at length: a plain composition never registers the swapped-in
// child as a real ftxui child, so Focusable()/ActiveChild() fall back to
// ComponentBase's defaults (no children), Focused() is false along the whole
// container chain above, and every keystroke is dropped before it can reach
// the editor. Overriding the pair to defer to the LIVE slot is what keeps
// the host (and therefore the editor, and therefore the Input) visible to
// ftxui's own focus machinery.
class PerTabEditorHost : public ftxui::ComponentBase {
public:
    PerTabEditorHost(PerTabPromptEditors& tabs, std::function<std::string()> focused_tab)
        : tabs_(tabs), focused_tab_(std::move(focused_tab)) {}

private:
    ftxui::Component resolve() { return tabs_.Resolve(focused_tab_()); }

    ftxui::Element OnRender() override {
        ftxui::Component editor = resolve();
        // emptyElement (not a default-constructed Element): the frame always
        // reserves the editor's row (kCriticalHeight), so the no-editor
        // frame must render an empty node, not hand ftxui a null pointer.
        return editor ? editor->Render() : ftxui::emptyElement();
    }

    bool OnEvent(ftxui::Event event) override {
        // Resolved LIVE: a key arriving in the same frame as a tab switch
        // lands in the tab focused at event time -- the same rule the submit
        // paths follow via focused_or_new_tab. Unhandled events return false
        // and keep propagating up (to with_keys' HandleGlobalKey and the
        // outer containers), the same posture as any leaf component.
        ftxui::Component editor = resolve();
        return editor != nullptr && editor->OnEvent(std::move(event));
    }

    // The ModalHost contract: being the focused tab's editor IS being the
    // active thing. Both defer to the live slot, so Focused()'s walk from
    // the root (through the modal stack, with_keys, session_view) terminates
    // in the Input that should hold the keyboard.
    bool Focusable() const override { return tabs_.Built(focused_tab_()); }

    ftxui::Component ActiveChild() override { return resolve(); }

    PerTabPromptEditors& tabs_;
    std::function<std::string()> focused_tab_;
};

ftxui::Component MakePerTabPromptEditor(PerTabPromptEditors& tabs,
                                        std::function<std::string()> focused_tab) {
    return ftxui::Make<PerTabEditorHost>(tabs, std::move(focused_tab));
}

// --- MentionQuerySlots -------------------------------------------------------

std::optional<std::vector<std::string>> MentionQuerySlots::Poll(const std::string& tab,
                                                                const std::string& prefix) {
    std::lock_guard lock(mtx_);
    Slot& slot = slots_[tab];
    if (slot.prefix == prefix) {
        if (slot.ready) {
            // Consume: this answer belongs to exactly the query the caller
            // just asked about, and the MentionSuggestor contract is
            // deliver-once (the render gate stops polling after a present
            // result).
            slot.ready = false;
            slot.in_flight = false;
            return std::move(slot.result);
        }
        if (slot.in_flight) return std::nullopt;   // still waiting; poll again next frame
    }
    // A new (tab, prefix) pair: arm it. Whatever was in flight for this tab
    // under a different prefix is superseded -- its completion, if it arrives
    // late, fails Complete's prefix guard below and is dropped rather than
    // clobbering the newer query. An answer that landed but was never
    // consumed (the caller re-armed before the next frame) is discarded the
    // same way: ready/result are overwritten here.
    slot.prefix = prefix;
    slot.in_flight = true;
    slot.ready = false;
    slot.result.clear();
    return std::nullopt;
}

void MentionQuerySlots::Complete(const std::string& tab, const std::string& prefix,
                                 std::vector<std::string> paths) {
    std::lock_guard lock(mtx_);
    const auto it = slots_.find(tab);
    // No slot: the query died with the tab (ForgetTab) -- nothing to deliver.
    // A different prefix: the worker's answer is for a query the user has
    // already moved past -- the staleness drop the shared-slot design could
    // not make per-tab, which is what let one tab's answer poison another's.
    if (it == slots_.end() || it->second.prefix != prefix) return;
    it->second.result = std::move(paths);
    it->second.ready = true;
    it->second.in_flight = false;
}

void MentionQuerySlots::Forget(const std::string& tab) {
    std::lock_guard lock(mtx_);
    slots_.erase(tab);
}

// --- MakeMentionSuggestor ----------------------------------------------------

MentionSuggestor MakeMentionSuggestor(
    AppState* app, MentionQuerySlots& slots,
    std::function<std::string(const std::string& session_id)> session_root_fallback) {
    // `session_root_fallback` maps a session id to the project root a
    // SESSIONLESS-flavored query would use. Production passes the empty
    // function (sessioned tabs query BY SESSION -- the server resolves the
    // root); tests pass a table so no RPC worker is needed. Kept a parameter
    // rather than a AppState method probe because the fallback's correct
    // answer differs per caller and the seam documents that in one place.
    return [app, &slots, session_root_fallback = std::move(session_root_fallback)](
               const std::string& prefix) -> std::optional<std::vector<std::string>> {
        if (app == nullptr) return std::vector<std::string>{};
        const std::string tab = app->FocusedTabId();
        const std::string sid = app->FocusedSessionId();

        // Per-tab slot, keyed by the tab id the scope used to embed. The
        // slot handoff (nullopt = armed-and-waiting) comes FIRST: the
        // subscription is what makes a later Complete() deliverable, so it
        // must be registered before the RPC can answer, whatever the
        // submission path below does.
        auto armed = slots.Poll(tab, prefix);
        if (armed.has_value()) return armed;   // consume the resolved answer

        // Submission. Empty prefix INCLUDED: a bare `@` is a live query whose
        // empty prefix asks the server for the project-root listing (the
        // resolver answers "" with the root's entries, directories
        // '/'-suffixed -- bash-completion shape). The old client-side
        // `if (prefix.empty()) return {}` short-circuit made a bare `@` a
        // permanent no-op -- the list could never open. The server scopes
        // every request (session id or project root, validated & clamped
        // server-side), so nothing here is unscoped even for "".
        if (!sid.empty()) {
            app->RequestSuggestPaths(sid, prefix, kMentionSuggestionCount, [&slots, tab, prefix](
                                                          std::vector<std::string> paths) {
                slots.Complete(tab, prefix, std::move(paths));
            });
        } else {
            // Sessionless tab: scope to the project root the tab's first
            // message would create the session with. Production passes
            // CurrentProjectRootOrEmpty(); a test's fallback answers from
            // its table.
            const std::string root =
                session_root_fallback ? session_root_fallback(sid) : std::string();
            if (root.empty()) {
                // No project context at all (a stray cwd failure): the
                // historical empty answer. Complete() is deliberately NOT
                // called: Poll armed the slot, so feeding it an empty answer
                // would open-then-close the list on the next frame; leaving
                // it in-flight keeps the armed state honest (nothing is
                // coming, but nothing was asked either) and the next prefix
                // change re-arms cleanly.
                return std::nullopt;
            }
            app->RequestSuggestPathsForRoot(root, prefix, kMentionSuggestionCount, [&slots, tab, prefix](
                                                                  std::vector<std::string> paths) {
                slots.Complete(tab, prefix, std::move(paths));
            });
        }
        return std::nullopt;
    };
}

}  // namespace minimal::client

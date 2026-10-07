#pragma once

// Core's side of the prompt editor slot (registries.hpp PromptEditorFactory):
// the app-scoped submitted-line timeline, one editor instance per tab swapped
// in for the focused tab, and the @-mention RPC behind
// PromptEditorHost::Mentions. The editor itself is a plugin (bundled:
// prompt-editor).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/command_catalog.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// Submitted-line recall, APP-scoped by design -- "what did I just type",
// regardless of which tab it went to: one shared timeline is what keeps
// up-arrow continuity when a user fires the same command across sessions.
// Each editor keeps its own browse position into it (a sequence number).
class PromptHistory final : public plugins::PromptTimeline {
public:
    const std::vector<std::string>& Entries() const override { return entries_; }
    std::uint64_t FirstSeq() const override { return first_seq_; }
    // Keeps what HistoryLine allows of `line` (credential-bearing command
    // arguments are dropped, judged against the catalog SetCatalog gave),
    // skips a repeat of the newest entry, and bounds the timeline.
    void Record(const std::string& line) override;

    // The catalog Record judges command lines against; unset = every
    // command line with arguments is kept as its bare name.
    void SetCatalog(CommandCatalogSource catalog) { catalog_ = std::move(catalog); }

    // Keeps the timeline across launches: loads `path` (a JSON array of
    // lines) now, and every Record() rewrites it atomically -- the newest
    // entries up to kMaxPromptHistoryFileBytes. Never set (the default):
    // memory only. A missing or unreadable file starts empty; a failed write
    // is logged, not fatal.
    void PersistTo(std::filesystem::path path);

private:
    void Save() const;
    std::vector<std::string> entries_;
    std::uint64_t first_seq_ = 0;
    CommandCatalogSource catalog_;
    std::filesystem::path persist_path_;
};

// One prompt editor per tab, made by the slot's factory on the tab's first
// resolution and dropped with the tab. "" is the PRE-TAB editor (the frame
// before any tab exists): typing there goes through the same submit paths,
// which open a tab on submit. A factory that throws or answers null leaves
// that tab without an editor (logged; retried on the next resolution).
// Loop thread, except WantsFrames (the redraw pump; the mutex exists for it).
class PerTabPromptEditors {
public:
    using Build = std::function<std::unique_ptr<plugins::PromptEditor>()>;
    void SetBuilder(Build build);

    // The tab's editor component, built on first use.
    ftxui::Component Resolve(const std::string& tab);
    // The tab's editor, built on first use; null when it cannot be built.
    plugins::PromptEditor* Editor(const std::string& tab);
    // Whether the tab's editor exists already (a peek: never builds).
    bool Built(const std::string& tab) const;
    // The pump thread's read of the focused tab's editor. Never builds.
    bool WantsFrames(const std::string& tab) const;
    // Tab close. The "" pre-tab entry is never forgotten.
    void ForgetTab(const std::string& tab);

private:
    struct Entry {
        std::unique_ptr<plugins::PromptEditor> editor;
        ftxui::Component component;
    };
    Entry* EntryFor(const std::string& tab);   // mtx_ held

    mutable std::mutex mtx_;
    std::map<std::string, Entry> entries_;
    Build build_;
};

// A stable component in the frame that is always the FOCUSED tab's editor,
// resolved live at render and event time (FTXUI binds an input's text by
// pointer at construction, so one editor can never follow tab switches).
ftxui::Component MakePerTabPromptEditor(PerTabPromptEditors& tabs,
                                        std::function<std::string()> focused_tab);

// PromptEditorHost::Mentions' shape: nullopt = still waiting, ask again next
// frame; a value (possibly empty) = the answer, handed out once.
using MentionSuggestor =
    std::function<std::optional<std::vector<std::string>>(const std::string& prefix)>;

// Per-tab in-flight slots for the @-mention suggest RPC. One slot per tab: a
// single shared slot let a second tab's query overwrite the first before its
// answer was consumed, and the staleness guard then dropped that answer.
// Cross-thread: the loop thread calls Poll, AppState's RPC worker Complete;
// one mutex guards the map and every slot.
class MentionQuerySlots {
public:
    // The first call for a (tab, prefix) arms the slot and returns nullopt;
    // later calls with the SAME prefix return nullopt while in flight, then
    // the answer once Complete ran. A DIFFERENT prefix supersedes (its late
    // answer is dropped by Complete's guard) and arms anew.
    std::optional<std::vector<std::string>> Poll(const std::string& tab,
                                                 const std::string& prefix);
    // The RPC worker's answer for `tab`'s most recent query: recorded only
    // when the slot's prefix still matches.
    void Complete(const std::string& tab, const std::string& prefix,
                  std::vector<std::string> paths);
    // Tab close.
    void Forget(const std::string& tab);

private:
    struct Slot {
        std::string prefix;
        bool in_flight = false;
        bool ready = false;
        std::vector<std::string> result;
    };

    mutable std::mutex mtx_;
    std::map<std::string, Slot> slots_;
};

// The TUI's MentionSuggestor: per-tab slots in front of AppState's async
// SuggestPaths RPCs, for the focused tab. A sessioned tab queries by session
// (the server resolves the root); a sessionless one scopes to
// `session_root_fallback`'s answer.
MentionSuggestor MakeMentionSuggestor(
    AppState* app, MentionQuerySlots& slots,
    std::function<std::string(const std::string& session_id)> session_root_fallback);

}  // namespace minimal::client

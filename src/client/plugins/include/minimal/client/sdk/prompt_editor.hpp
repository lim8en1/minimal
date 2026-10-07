#pragma once

// The prompt editor slot (EXCLUSIVE, REQUIRED): one editor per tab over
// the host's submit paths, completion sources and the shared timeline.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/sdk/command_catalog_entry.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::client::plugins {

// --- Prompt editor (EXCLUSIVE, REQUIRED) -----------------------------------

// The submitted-line timeline, app-scoped (every tab's editor records into
// and recalls from the same one) and persisted by core. Entries are numbered
// by a sequence that never repeats: entry i is FirstSeq() + i, and the
// oldest entries fall off the front, so an editor's browse position is a
// sequence number -- one below FirstSeq() has been trimmed away.
class PromptTimeline {
public:
    virtual ~PromptTimeline() = default;
    virtual const std::vector<std::string>& Entries() const = 0;
    virtual std::uint64_t FirstSeq() const = 0;
    // A submitted line. Core decides what of it is kept (a command whose
    // arguments may carry a credential is kept as its bare name) and skips a
    // repeat of the newest entry.
    virtual void Record(const std::string& line) = 0;
};

// What core gives the prompt editor. Every call on the UI thread; every
// call acts on the FOCUSED tab -- the editor shown is always that tab's.
class PromptEditorHost {
public:
    virtual ~PromptEditorHost() = default;
    // The two submit paths: a turn, or a /-command line (the leading '/'
    // included). Which one a line is, is the editor's call -- SubmitLine
    // below is the usual rule.
    virtual void SubmitChat(const std::string& text) = 0;
    virtual void SubmitCommand(const std::string& line) = 0;
    // The /-commands usable on the focused tab now (core has applied the
    // session and subagent gates), or null before the catalog is known. The
    // pointer changes exactly when the list does: cheap to poll per frame.
    virtual std::shared_ptr<const std::vector<CommandCatalogEntry>> Commands() = 0;
    // @-mention completion: the paths matching `prefix`. nullopt = still
    // being fetched -- ask again on a later frame; a value (possibly empty)
    // is the answer, handed out once.
    virtual std::optional<std::vector<std::string>> Mentions(const std::string& prefix) = 0;
    virtual PromptTimeline& History() = 0;
    // Whether `event` is bound to `action` -- one of the editor's own
    // (PromptEditorFactory::Actions), as keybindings.json resolved it.
    virtual bool Matches(std::string_view action, const ftxui::Event& event) const = 0;
    // Asks for a frame (any thread): a timer the editor runs from render
    // needs frames to keep coming.
    virtual void RequestRedraw() = 0;
};

// The usual routing: a line starting with '/' is a command.
inline void SubmitLine(PromptEditorHost& host, const std::string& line) {
    if (!line.empty() && line.front() == '/') {
        host.SubmitCommand(line);
    } else {
        host.SubmitChat(line);
    }
}

// One tab's editor. Core keeps one per tab, made on the tab's first frame
// and dropped with the tab.
class PromptEditor {
public:
    virtual ~PromptEditor() = default;
    // Built once; the host swaps the focused tab's in.
    virtual ftxui::Component component() = 0;
    virtual std::string Text() const = 0;
    // Replaces the text, caret at the end (a help entry inserted, a message
    // that failed to send handed back).
    virtual void SetText(const std::string& text) = 0;
    // Asked BEFORE core's global key bindings: true keeps `event` from any
    // binding, so it reaches the editor (an open suggestion list's keys, a
    // pasted byte that looks like a bound key, a modal editor's Escape).
    virtual bool Claims(const ftxui::Event& event) const = 0;
    // Any thread: the editor is waiting on time to pass (a debounce) and
    // needs frames meanwhile.
    virtual bool WantsFrames() const = 0;
    // A modal took the keyboard: drop state a key sequence left half-done (a
    // paste whose end marker the modal will swallow).
    virtual void OnFocusLost() {}
};

// A key action a slot implementation declares (the prompt editor's, the chat
// surface's): registered into the keymap at startup -- `default_key` only
// where keybindings.json binds nothing -- and listed in the keymap help with
// `description`. Ids are prefixed by convention ("prompt.", "chat.").
struct DeclaredAction {
    std::string id;
    std::string default_key;
    std::string description;
};

class PromptEditorFactory {
public:
    virtual ~PromptEditorFactory() = default;
    virtual std::vector<DeclaredAction> Actions() const = 0;
    // `host` and `theme` outlive the editor.
    virtual std::unique_ptr<PromptEditor> Make(PromptEditorHost& host,
                                               const Theme& theme) const = 0;
};

}  // namespace minimal::client::plugins

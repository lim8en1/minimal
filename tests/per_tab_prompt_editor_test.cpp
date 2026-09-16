// PerTabPromptEditors / MakePerTabPromptEditor: the per-tab prompt editor
// host. The mechanics it exists for -- FTXUI's InputOption binds content/
// cursor_position by POINTER at construction, so one shared editor can never
// follow tab switches -- are exercised here at the same rendered-screen tier
// as prompt_editor_test.cpp: two tabs, two states, one stable host component.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "minimal/client/view/prompt_editor.hpp"

namespace minimal::client {
namespace {

namespace ftx = ftxui;   // shorthand alias, like view_factory_test's ftx::Event

const Theme& TestTheme() {
    static const Theme theme =
        Theme::Parse(R"({"name": "test", "colors": {"error": "#f00"}})",
                     ColorDepth::Truecolor);
    return theme;
}

std::string ToText(ftxui::Component& component, int width, int height) {
    auto element = component->Render();
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                        ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen.ToString();
}

// The catch2 test binary links client_core; a test-local focused-tab source
// stands in for AppState::FocusedTabId (the host's resolution rule is what
// is under test, not AppState's focus tracking).
struct Harness {
    PerTabPromptEditors tabs;
    std::string focused;
    std::vector<std::string> submitted;
    ftxui::Component host;

    explicit Harness(Theme theme_) : theme(std::move(theme_)) {
        host = MakePerTabPromptEditor(
            tabs, [this] { return focused; },
            [this, t = &theme](PromptEditorState& state) {
                built.push_back("built:" + focused);
                return MakePromptEditor(
                    *t, state,
                    [this](const std::string& text) { submitted.push_back(text); },
                    [](const std::string&) {},
                    [](const std::string&) { return std::vector<std::string>{}; });
            });
    }

    void Type(const std::string& text) {
        for (const char c : text) {
            REQUIRE(host->OnEvent(ftx::Event::Character(std::string(1, c))));
        }
    }

    Theme theme;
    std::vector<std::string> built;   // "built:<tab>" per factory invocation
};

}  // namespace

TEST_CASE("PerTabPromptEditors: drafts are per-tab", "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    h.focused = "A";

    h.Type("tab a draft");
    // No frame has rendered yet: lazy build happens on the first event/render
    // resolution, and the event MUST have landed in A's fresh state.
    REQUIRE(h.tabs.CurrentState("A").content == "tab a draft");

    h.focused = "B";   // switch tabs: A's draft must survive untouched
    h.Type("tab b");
    CHECK(h.tabs.CurrentState("A").content == "tab a draft");
    CHECK(h.tabs.CurrentState("B").content == "tab b");

    h.focused = "A";   // and back: the draft is exactly where typing left it
    CHECK(h.tabs.CurrentState("A").content == "tab a draft");
}

TEST_CASE("PerTabPromptEditors: one factory invocation per tab, lazily",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());

    // Nothing built before first use -- a tab that never shows a prompt
    // never pays for one.
    CHECK(h.built.empty());
    ToText(h.host, 40, 3);   // a render resolves the pre-tab "" entry
    REQUIRE(h.built == std::vector<std::string>{"built:"});

    h.focused = "A";
    h.Type("x");
    REQUIRE(h.built == std::vector<std::string>{"built:", "built:A"});

    h.focused = "B";
    h.Type("y");
    REQUIRE(h.built == std::vector<std::string>{"built:", "built:A", "built:B"});

    // Re-resolving an already-built tab builds nothing new.
    h.focused = "A";
    CHECK(ToText(h.host, 40, 3).find("x") != std::string::npos);
    REQUIRE(h.built == std::vector<std::string>{"built:", "built:A", "built:B"});
}

TEST_CASE("PerTabPromptEditors: submit routes through the focused tab's state",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    h.focused = "A";
    h.Type("from a");
    h.focused = "B";
    h.Type("from b");
    REQUIRE(h.host->OnEvent(ftx::Event::Return));

    // The submit callback received ONLY B's content -- A's draft stayed a
    // draft (per-tab content bound by pointer, the whole point of the host).
    REQUIRE(h.submitted == std::vector<std::string>{"from b"});
    CHECK(h.tabs.CurrentState("B").content.empty());
    CHECK(h.tabs.CurrentState("A").content == "from a");
}

TEST_CASE("PerTabPromptEditors: the shared history is app-scoped, the cursor per editor",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    PromptHistory hist;
    auto own = MakePerTabPromptEditor(
        h.tabs, [&h] { return h.focused; },
        [&](PromptEditorState& state) {
            return MakePromptEditor(h.theme, state, [](const std::string&) {},
                                    [](const std::string&) {},
                                    [](const std::string&) {
                                        return std::vector<std::string>{};
                                    },
                                    nullptr, nullptr, nullptr, nullptr, &hist);
        });

    h.focused = "A";
    own->OnEvent(ftx::Event::Character("o"));
    REQUIRE(own->OnEvent(ftx::Event::Return));   // records into the shared timeline
    h.focused = "B";
    // B's editor has never typed anything, but up-arrow recalls the
    // app-scoped timeline: one history, not one per tab.
    REQUIRE(own->OnEvent(ftx::Event::ArrowUp));
    CHECK(h.tabs.CurrentState("B").content == "o");
}

TEST_CASE("PerTabPromptEditors: browsing in one tab does not clobber another "
          "tab's draft",
          "[prompt_editor][per_tab][regression]") {
    // THE BUG: PromptHistory's cursor/draft used to live on the shared
    // PromptHistory instance itself, even though it is read/written against
    // a per-tab PromptEditorState::content. Browsing in A, switching to B,
    // and typing a fresh draft there used to leave A's browse cursor armed --
    // arrowing in B then overwrote B's untouched draft with A's history
    // state instead of B's own.
    Harness h(TestTheme());
    PromptHistory hist;
    auto own = MakePerTabPromptEditor(
        h.tabs, [&h] { return h.focused; },
        [&](PromptEditorState& state) {
            return MakePromptEditor(h.theme, state, [](const std::string&) {},
                                    [](const std::string&) {},
                                    [](const std::string&) {
                                        return std::vector<std::string>{};
                                    },
                                    nullptr, nullptr, nullptr, nullptr, &hist);
        });

    h.focused = "A";
    own->OnEvent(ftx::Event::Character("o"));
    REQUIRE(own->OnEvent(ftx::Event::Return));   // seed the shared timeline
    REQUIRE(own->OnEvent(ftx::Event::ArrowUp));  // A starts browsing
    CHECK(h.tabs.CurrentState("A").content == "o");

    h.focused = "B";   // switch tabs mid-browse, without arrowing back down
    own->OnEvent(ftx::Event::Character("b"));
    REQUIRE(h.tabs.CurrentState("B").content == "b");

    // B never browsed: Up/Down here must walk B's own (fresh) browse state,
    // not resume A's -- and must never discard "b".
    REQUIRE(own->OnEvent(ftx::Event::ArrowUp));
    CHECK(h.tabs.CurrentState("B").content == "o");   // recalls the shared entry
    REQUIRE(own->OnEvent(ftx::Event::ArrowDown));
    CHECK(h.tabs.CurrentState("B").content == "b");   // restores B's own draft

    // A's browse state is untouched by B's activity.
    h.focused = "A";
    CHECK(h.tabs.CurrentState("A").content == "o");
}

TEST_CASE("PerTabPromptEditors: ForgetTab drops the state and the editor together",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    h.focused = "A";
    h.Type("doomed draft");
    h.tabs.ForgetTab("A");

    // The state is gone -- and re-resolving the tab rebuilds a FRESH one
    // (no resurrected draft from a stale editor whose closures had captured
    // the erased node's address).
    h.focused = "A";
    CHECK(h.tabs.CurrentState("A").content.empty());
    CHECK(h.built.size() == 1);   // rebuilt exactly once more
}

TEST_CASE("PerTabPromptEditors: the pre-tab editor serves the empty app",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    h.focused = "";   // no tab at all -- the startup frame's posture

    h.Type("pre-tab typing");
    REQUIRE(h.tabs.CurrentState("").content == "pre-tab typing");
    REQUIRE(h.host->OnEvent(ftx::Event::Return));
    CHECK(h.submitted == std::vector<std::string>{"pre-tab typing"});
}

TEST_CASE("PerTabPromptEditors: QueryPendingHint reads only the asked tab",
          "[prompt_editor][per_tab]") {
    PerTabPromptEditors tabs;
    // Nothing built: absent means "not armed", and no state is minted by a
    // cross-thread peek.
    CHECK_FALSE(tabs.QueryPendingHint("nowhere"));

    PromptEditorState& s = tabs.CurrentState("t");
    s.query_pending_hint.store(true, std::memory_order_release);
    CHECK(tabs.QueryPendingHint("t"));
    CHECK_FALSE(tabs.QueryPendingHint("other"));
}

}  // namespace minimal::client

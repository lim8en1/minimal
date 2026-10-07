// PerTabPromptEditors / MakePerTabPromptEditor: the per-tab prompt editor
// host. The mechanics it exists for -- FTXUI's InputOption binds content/
// cursor_position by POINTER at construction, so one shared editor can never
// follow tab switches -- are exercised here at the same rendered-screen tier
// as prompt_editor_test.cpp: two tabs, two states, one stable host component.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "prompt_editor_factory.hpp"
#include "prompt_editor_fixture.hpp"
#include "temp_dir.hpp"

#include "minimal/client/policy.hpp"
#include "minimal/client/view/prompt_host.hpp"

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

// A test-local focused-tab source stands in for AppState::FocusedTabId (the
// host's resolution rule is what is under test, not AppState's focus
// tracking); the editors are the bundled plugin's, over one fake host.
struct Harness {
    PerTabPromptEditors tabs;
    std::string focused;
    std::vector<std::string> submitted;
    minimal_test::FakeEditorHost& fake = minimal_test::NewFakeEditorHost();
    ftxui::Component host;

    explicit Harness(Theme theme_) : theme(std::move(theme_)) {
        fake.on_text = [this](const std::string& text) { submitted.push_back(text); };
        tabs.SetBuilder([this] {
            built.push_back("built:" + focused);
            return prompt_editor::Factory().Make(fake, theme);
        });
        host = MakePerTabPromptEditor(tabs, [this] { return focused; });
    }

    std::string Text(const std::string& tab) { return tabs.Editor(tab)->Text(); }

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
    REQUIRE(h.Text("A") == "tab a draft");

    h.focused = "B";   // switch tabs: A's draft must survive untouched
    h.Type("tab b");
    CHECK(h.Text("A") == "tab a draft");
    CHECK(h.Text("B") == "tab b");

    h.focused = "A";   // and back: the draft is exactly where typing left it
    CHECK(h.Text("A") == "tab a draft");
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
    CHECK(h.Text("B").empty());
    CHECK(h.Text("A") == "from a");
}

TEST_CASE("PerTabPromptEditors: undo is per tab", "[prompt_editor][per_tab][undo]") {
    Harness h(TestTheme());
    h.focused = "A";
    h.Type("alpha");
    h.focused = "B";
    h.Type("beta");
    // Undo in B reverts only B's draft.
    REQUIRE(h.host->OnEvent(ftx::Event::Character("\x1bz")));   // alt+z
    CHECK(h.Text("B").empty());
    CHECK(h.Text("A") == "alpha");
}

TEST_CASE("PerTabPromptEditors: the shared history is app-scoped, the cursor per editor",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    PromptHistory hist;
    h.fake.history = &hist;
    auto& own = h.host;

    h.focused = "A";
    own->OnEvent(ftx::Event::Character("o"));
    REQUIRE(own->OnEvent(ftx::Event::Return));   // records into the shared timeline
    h.focused = "B";
    // B's editor has never typed anything, but up-arrow recalls the
    // app-scoped timeline: one history, not one per tab.
    REQUIRE(own->OnEvent(ftx::Event::ArrowUp));
    CHECK(h.Text("B") == "o");
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
    h.fake.history = &hist;
    auto& own = h.host;

    h.focused = "A";
    own->OnEvent(ftx::Event::Character("o"));
    REQUIRE(own->OnEvent(ftx::Event::Return));   // seed the shared timeline
    REQUIRE(own->OnEvent(ftx::Event::ArrowUp));  // A starts browsing
    CHECK(h.Text("A") == "o");

    h.focused = "B";   // switch tabs mid-browse, without arrowing back down
    own->OnEvent(ftx::Event::Character("b"));
    REQUIRE(h.Text("B") == "b");

    // B never browsed: Up/Down here must walk B's own (fresh) browse state,
    // not resume A's -- and must never discard "b".
    REQUIRE(own->OnEvent(ftx::Event::ArrowUp));
    CHECK(h.Text("B") == "o");   // recalls the shared entry
    REQUIRE(own->OnEvent(ftx::Event::ArrowDown));
    CHECK(h.Text("B") == "b");   // restores B's own draft

    // A's browse state is untouched by B's activity.
    h.focused = "A";
    CHECK(h.Text("A") == "o");
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
    CHECK(h.Text("A").empty());
    CHECK(h.built.size() == 2);   // rebuilt exactly once more
}

TEST_CASE("PerTabPromptEditors: the pre-tab editor serves the empty app",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    h.focused = "";   // no tab at all -- the startup frame's posture

    h.Type("pre-tab typing");
    REQUIRE(h.Text("") == "pre-tab typing");
    REQUIRE(h.host->OnEvent(ftx::Event::Return));
    CHECK(h.submitted == std::vector<std::string>{"pre-tab typing"});
}

TEST_CASE("PerTabPromptEditors: WantsFrames peeks without building",
          "[prompt_editor][per_tab]") {
    Harness h(TestTheme());
    // Nothing built: absent means "not armed", and no editor is built by a
    // cross-thread peek.
    CHECK_FALSE(h.tabs.WantsFrames("nowhere"));
    CHECK_FALSE(h.tabs.Built("nowhere"));
    CHECK(h.built.empty());
}

TEST_CASE("PerTabPromptEditors: a factory that throws leaves the tab without an editor",
          "[prompt_editor][per_tab]") {
    PerTabPromptEditors tabs;
    int attempts = 0;
    tabs.SetBuilder([&]() -> std::unique_ptr<plugins::PromptEditor> {
        ++attempts;
        throw std::runtime_error("boom");
    });
    CHECK(tabs.Resolve("A") == nullptr);
    CHECK(tabs.Editor("A") == nullptr);
    CHECK_FALSE(tabs.Built("A"));
    CHECK(tabs.Failed("A"));
    // Once per tab: every render and key resolves the editor, and a retry
    // ran the failing factory (and logged) at frame rate.
    CHECK(attempts == 1);

    // The host's row says so instead of rendering blank.
    auto host = MakePerTabPromptEditor(tabs, [] { return std::string("A"); });
    CHECK(ToText(host, 60, 1).find("failed to start") != std::string::npos);
    CHECK(attempts == 1);

    // Forgetting the tab forgets the failure; another tab tries afresh.
    tabs.ForgetTab("A");
    CHECK_FALSE(tabs.Failed("A"));
    CHECK(tabs.Resolve("B") == nullptr);
    CHECK(attempts == 2);
}

TEST_CASE("PerTabPromptEditors: a browse position trimmed away restarts browsing",
          "[prompt_editor][per_tab][history]") {
    // Seq-numbered positions: another tab recording past the bound cannot
    // leave this tab pointing at an unrelated line.
    Harness h(TestTheme());
    PromptHistory hist;
    h.fake.history = &hist;
    hist.Record("oldest");
    h.focused = "A";
    REQUIRE(h.host->OnEvent(ftx::Event::ArrowUp));
    REQUIRE(h.Text("A") == "oldest");
    for (std::size_t i = 0; i < minimal::client::kMaxPromptHistoryEntries; ++i) {
        hist.Record("newer " + std::to_string(i));
    }
    // "oldest" is gone: Up starts over from the newest entry.
    REQUIRE(h.host->OnEvent(ftx::Event::ArrowUp));
    CHECK(h.Text("A") ==
          "newer " + std::to_string(minimal::client::kMaxPromptHistoryEntries - 1));
}

}  // namespace minimal::client

TEST_CASE("prompt history persists across instances, newest last", "[prompt_editor][history]") {
    minimal::testing::TempDir dir;
    const auto path = dir.path() / "prompt_history.json";
    {
        minimal::client::PromptHistory first;
        first.PersistTo(path);   // no file yet: starts empty
        CHECK(first.Entries().empty());
        first.Record("one");
        first.Record("two\nlines");
    }
    minimal::client::PromptHistory second;
    second.Record("typed before load");   // this run's entries follow the loaded ones
    second.PersistTo(path);
    REQUIRE(second.Entries().size() == 3);
    CHECK(second.Entries()[0] == "one");
    CHECK(second.Entries()[1] == "two\nlines");
    CHECK(second.Entries()[2] == "typed before load");
}

TEST_CASE("prompt history: a front trim moves FirstSeq, never renumbers",
          "[prompt_editor][history]") {
    minimal::client::PromptHistory hist;
    for (std::size_t i = 0; i < minimal::client::kMaxPromptHistoryEntries + 3; ++i) {
        hist.Record("line " + std::to_string(i));
    }
    CHECK(hist.Entries().size() == minimal::client::kMaxPromptHistoryEntries);
    CHECK(hist.FirstSeq() == 3);
    CHECK(hist.Entries().front() == "line 3");
}


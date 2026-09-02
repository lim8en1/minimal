// Regression tier for src/client/view/modal_host.{hpp,cpp}: the modal-layer
// host that app.cpp's four swappable-child modals (permission prompt, wire
// confirmation, session picker, plugin modals) are built on.
//
// The bug this locks out: the first handlers captured the visibility flag BY
// VALUE, latching the startup `false` forever. The modal still rendered
// (ftxui's Modal() reads the flag through a pointer) but the handler gated
// every event on the stale copy -- a permission prompt no key could answer,
// a picker that could not be closed: the "UI froze" class. The decisive
// assertions here therefore flip the flag AFTER the component is built, the
// only ordering that catches latching.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "ftxui_harness.hpp"
#include "minimal/client/view/modal_host.hpp"

#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

using minimal::client::MakeModalHost;
using minimal_test::RenderFrame;
using minimal_test::SendEvent;
using ftxui::Modal;

namespace {

// A child that records every event it is offered and always consumes it.
ftxui::Component MakeRecordingChild(std::vector<std::string>& seen) {
    using namespace ftxui;
    auto inner = Renderer([] { return text("modal body"); });
    return CatchEvent(inner, [&seen](const Event& event) {
        seen.push_back(event.input());   // raw bytes: unique per key
        return true;
    });
}

}  // namespace

TEST_CASE("MakeModalHost: events reach the child flipped on AFTER construction",
          "[modal_host][regression]") {
    auto child_slot = std::make_shared<ftxui::Component>();
    bool show = false;
    auto host = MakeModalHost(child_slot, show);

    std::vector<std::string> seen;
    *child_slot = MakeRecordingChild(seen);

    // The latching bug lived exactly here: show flips long after the
    // handler's captures were fixed in place.
    show = true;

    CHECK(SendEvent(host, ftxui::Event::Return));
    CHECK(SendEvent(host, ftxui::Event::Escape));
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == ftxui::Event::Return.input());
    CHECK(seen[1] == ftxui::Event::Escape.input());
}

TEST_CASE("MakeModalHost: hidden host swallows nothing and forwards nothing",
          "[modal_host]") {
    auto child_slot = std::make_shared<ftxui::Component>();
    bool show = false;
    auto host = MakeModalHost(child_slot, show);

    std::vector<std::string> seen;
    *child_slot = MakeRecordingChild(seen);

    CHECK_FALSE(SendEvent(host, ftxui::Event::Return));
    CHECK(seen.empty());
}

TEST_CASE("MakeModalHost: child slot swapped after construction is the one that runs",
          "[modal_host]") {
    auto child_slot = std::make_shared<ftxui::Component>();
    bool show = false;
    auto host = MakeModalHost(child_slot, show);

    std::vector<std::string> seen;
    *child_slot = MakeRecordingChild(seen);
    show = true;

    // Rebuild the inner modal in place, the way a new request_id does.
    std::vector<std::string> second_seen;
    *child_slot = MakeRecordingChild(second_seen);

    CHECK(SendEvent(host, ftxui::Event::ArrowDown));
    REQUIRE(second_seen.size() == 1);
    CHECK(seen.empty());
}

TEST_CASE("MakeModalHost: reachable through ftxui::Modal(), the actual call site",
          "[modal_host][regression]") {
    // RunTui never calls host->OnEvent() directly -- it hands the host to
    // ftxui::Modal(), which routes keyboard events through a Container::Tab
    // gated on Focused(). A host built from a bare Renderer (the shape this
    // file had before) never registers its swapped-in child as a real ftxui
    // child, so Focusable()/ActiveChild() see nothing, Focused() comes back
    // false the instant the modal opens, and the Container silently drops
    // every keystroke -- Escape included, no key can close it. Only a test
    // that goes through Modal() the way app.cpp does can catch that; SendEvent
    // straight on the host (the other cases here) bypasses the gate entirely
    // and stays green even when this is broken.
    auto child_slot = std::make_shared<ftxui::Component>();
    bool show = false;
    auto host = MakeModalHost(child_slot, show);

    auto main = ftxui::Renderer([] { return ftxui::text("main"); });
    auto modal = Modal(main, host, &show);

    std::vector<std::string> seen;
    *child_slot = MakeRecordingChild(seen);
    show = true;

    CHECK(SendEvent(modal, ftxui::Event::Escape));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == ftxui::Event::Escape.input());
}

TEST_CASE("MakeModalHost: render mirrors the child slot", "[modal_host]") {
    auto child_slot = std::make_shared<ftxui::Component>();
    bool show = false;
    auto host = MakeModalHost(child_slot, show);

    // Empty slot renders nothing (no stray text for Modal() to stack).
    CHECK(RenderFrame(host).find("modal body") == std::string::npos);

    std::vector<std::string> seen;
    *child_slot = MakeRecordingChild(seen);
    CHECK_THAT(RenderFrame(host), Catch::Matchers::ContainsSubstring("modal body"));
}

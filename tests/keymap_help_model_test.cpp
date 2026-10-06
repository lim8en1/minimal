// Core's key-action vocabulary (keymap_help_model.hpp): what HostUi::KeyBindings
// tells a plugin about each binding. The listing built from it is
// help-view's (help_view_test.cpp).

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/keymap_help_model.hpp"

TEST_CASE("core describes its key actions for plugins: words, group, and the listing order",
          "[keymap_help]") {
    using minimal::client::DescribeKeyAction;
    const auto next = DescribeKeyAction("tab.next");
    CHECK(next.description == "next tab");
    CHECK(next.group == "tabs");
    // Groups in display order, and the curated order within one -- not the alphabet.
    CHECK(next.order < DescribeKeyAction("tab.previous").order);
    CHECK(DescribeKeyAction("tab.goto.9").order < DescribeKeyAction("tab.close").order);
    CHECK(DescribeKeyAction("tab.close").order < DescribeKeyAction("chat.scroll_up").order);
    CHECK(DescribeKeyAction("chat.scroll_up").group == "chat");
    // Unknown to core (a plugin view's toggle): grouped last, named by its id.
    const auto view = DescribeKeyAction("config");
    CHECK(view.group == "views");
    CHECK(view.description == "config");
    CHECK(DescribeKeyAction("app.quit").order < view.order);
}

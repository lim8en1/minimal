// ModalStack: the TUI's modal layer order and flags, in one place.

#include <catch2/catch_test_macros.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "ftxui_harness.hpp"
#include "minimal/client/view/modal_stack.hpp"

using minimal::client::ModalId;
using minimal::client::ModalStack;

namespace {
ftxui::Component Label(const std::string& text) {
    return ftxui::Renderer([text] { return ftxui::text(text); });
}
}  // namespace

TEST_CASE("modal stack: AnyOpen reflects every flag", "[modal_stack]") {
    ModalStack stack;
    CHECK_FALSE(stack.AnyOpen());
    stack.Flag(ModalId::kThemePicker) = true;
    CHECK(stack.AnyOpen());
    CHECK(stack.Open(ModalId::kThemePicker));
    stack.Flag(ModalId::kThemePicker) = false;
    CHECK_FALSE(stack.AnyOpen());
}

TEST_CASE("modal stack: the permission prompt draws above everything else",
          "[modal_stack]") {
    ModalStack stack;
    stack.Set(ModalId::kPluginModal, Label("plugin-modal"));
    stack.Set(ModalId::kSessionPicker, Label("session-picker"));
    stack.Set(ModalId::kPermission, Label("permission-prompt"));
    auto tree = stack.Build(Label("frame"));

    stack.Flag(ModalId::kPluginModal) = true;
    stack.Flag(ModalId::kPermission) = true;
    const auto frame = minimal_test::RenderFrame(tree);
    // Modal() centers the topmost layer over the rest; the permission prompt
    // must be visible while the lower plugin modal is also flagged.
    CHECK(frame.find("permission-prompt") != std::string::npos);
    // Events route to the topmost open layer only.
    stack.Flag(ModalId::kPermission) = false;
    CHECK(minimal_test::RenderFrame(tree).find("plugin-modal") != std::string::npos);
}

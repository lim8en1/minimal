#pragma once

// Headless FTXUI component harness.
//
// "FTXUI render output isn't unit-tested" (ARCHITECTURE.md, Testing) excludes
// VISUALS, not wiring: view factories take plain data and return components,
// so they can be constructed, event-driven, and rendered to a string without a
// terminal. That is the tier where track E's crashes lived -- factory closures
// capturing dead locals, components bound to freed snapshots -- and it is
// fully automatable. These helpers give an invariant test three verbs:
//
//     RenderFrame(component)  -- the current frame as text (assert on content)
//     SendEvent(component,ev) -- one event, as the keyboard would deliver it
//     Screen capture          -- pixel-level assertions if ever needed
//
// Components are driven exactly as ScreenInteractive would: OnEvent dispatches
// through the container tree to the focused child; Render rebuilds the frame
// from current state. Anything a factory gets wrong at CONSTRUCTION time
// (dangling captures, wrong state ownership) shows up here as a sanitizer
// finding or a wrong-frame assertion instead of an EXC_BAD_ACCESS in the smoke
// run.

#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace minimal_test {

// Render a component's current frame into a string. Deterministic; no terminal.
inline std::string RenderFrame(const ftxui::Component& component, int width = 80,
                               int height = 24) {
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, component->Render());
    return screen.ToString();
}

// Deliver one event; true when the component consumed it.
inline bool SendEvent(const ftxui::Component& component, const ftxui::Event& event) {
    return component->OnEvent(event);
}

// Render a bare Element (ChatView::Render's return type) into a string.
inline std::string RenderElement(const ftxui::Element& element, int width = 80, int height = 24) {
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, element);
    return screen.ToString();
}

}  // namespace minimal_test

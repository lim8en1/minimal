#pragma once

// Host-side (not SDK): a registered view's runtime dispatch entry.

#include <string>

#include <ftxui/component/component.hpp>

#include "minimal/client/sdk/views.hpp"

namespace minimal::client::plugins {

// A registered view's runtime dispatch entry: the view's own spec, its
// (already-constructed) component, and whether it is currently visible
// (panels toggle; modals stage into the host's modal layer on open). `view`
// is a non-owning pointer into a ViewRegistry's heap-owned Views -- safe
// regardless of the registry's own address, since a registry only ever
// relocates its OWN vector of unique_ptrs, never the pointees. Lives here
// (not in the client's bootstrap/dispatch code) so both the startup sequence
// that builds the list and the global-keybinding dispatch that reads it can
// share one definition without one including the other.
struct PluginViewEntry {
    const View* view;
    ftxui::Component component;
    bool visible = false;
    // The panel-focus rule (keybinding_dispatch.cpp): at most ONE visible
    // panel is focused; it alone receives key events, and the frame renders
    // its chrome (border + title). Opening a panel takes focus; closing the
    // focused one moves focus to the first remaining visible panel. Modals
    // never use it (they stage into the modal layer whole).
    bool focused = false;
    // What keybindings and view hints address: the client-plugins.json alias
    // that renamed the view, else spec().id.
    std::string surface_id;
    // The registering plugin ("" for core): with spec().id, what that plugin's
    // own view requests address (ViewRequest::plugin).
    std::string plugin;
};

}  // namespace minimal::client::plugins

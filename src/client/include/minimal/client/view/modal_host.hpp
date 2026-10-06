#pragma once

#include <functional>
#include <memory>

#include <ftxui/component/component.hpp>

namespace minimal::client {

// A modal layer's host: a STABLE component in the FTXUI tree whose inner
// modal component is swapped at runtime (per request_id, per open) and whose
// event forwarding is gated on a visibility flag read LIVE at event time.
// TuiApp builds one per layer (permission prompt, wire confirmation, plugin
// prompt, pickers; plugin views use MakePluginModalHost below) and passes the same flag to ftxui's Modal()
// decorator, which reads it through a pointer.
//
// The signature takes the flag by reference ON PURPOSE. The returned
// component's handler must observe the flag as it is when an EVENT ARRIVES,
// not as it was when the handler was built: a by-value capture of a plain
// bool latched the startup value (false) forever -- the modal still rendered
// but swallowed every event, giving a permission prompt no key could answer
// and a picker that could not be closed. Binding the caller's own bool
// (which Modal() also aliases through &flag) keeps the two views consistent
// by construction; the child slot is a shared_ptr, whose by-value capture
// aliases the same component slot the owner reassigns.

ftxui::Component MakeModalHost(const std::shared_ptr<ftxui::Component>& child,
                               const bool& show);

// The plugin modal layer's host: MakeModalHost's focus behaviour (the shown
// host is what ftxui::Modal() routes keys to) plus the two things plugin code
// needs around it. A throw from the plugin's component -- render or event --
// is contained and logged, never fatal (a throwing render draws nothing that
// frame). And Escape is the HOST's: it calls `close` before the plugin sees
// the key, because Modal() gives the layer exclusive input while it is shown,
// so a plugin that never handles Escape would otherwise trap every key until
// the terminal is killed.
//
// The one exception is opt-in and momentary: a plugin modal's root component
// that also implements EscapeAware gets Escape while WantsEscape() says so --
// to cancel an inline edit -- and the host closes on Escape otherwise.
ftxui::Component MakePluginModalHost(const std::shared_ptr<ftxui::Component>& child,
                                     const bool& show, std::function<void()> close);

// Implemented by a plugin modal's root component that has a use for Escape
// of its own while some state lasts (an open inline editor). Asked per key.
class EscapeAware {
public:
    virtual ~EscapeAware();
    virtual bool WantsEscape() const = 0;
};

}  // namespace minimal::client

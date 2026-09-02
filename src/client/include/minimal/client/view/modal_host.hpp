#pragma once

#include <memory>

#include <ftxui/component/component.hpp>

namespace minimal::client {

// A modal layer's host: a STABLE component in the FTXUI tree whose inner
// modal component is swapped at runtime (per request_id, per open) and whose
// event forwarding is gated on a visibility flag read LIVE at event time.
// RunTui builds one per layer (permission prompt, wire confirmation, session
// picker, plugin modals) and passes the same flag to ftxui's Modal()
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

}  // namespace minimal::client

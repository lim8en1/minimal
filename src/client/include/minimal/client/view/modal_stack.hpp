#pragma once

// The TUI's modal layer, owned in one place: which modals exist, their
// stacking order, and their visibility flags. RunTui used to declare thirteen
// `show_*` bools, chain thirteen FTXUI Modal() calls by hand, and repeat the
// whole flag list again wherever it needed "is any modal up" -- adding a
// modal meant finding all three.
//
// Layers are declared BOTTOM to TOP (ModalId order). The order is the
// never-preempted invariant made structural: the security prompts (the
// permission request above the wire confirmation) are the topmost layers, and
// plugin modals sit below every core modal.

#include <array>
#include <cstddef>

#include <ftxui/component/component.hpp>

namespace minimal::client {

enum class ModalId : std::size_t {
    kPluginModal,      // a plugin view with placement Modal (one slot)
    kDeleteConfirm,    // the session picker's delete confirmation
    kSessionPicker,
    kAgentPicker,
    kModelPicker,
    kSubagentPicker,
    kThemePicker,
    kKeymapHelp,
    kCommandsHelp,
    kQuitConfirm,      // quit / tab-close while work is in flight
    kPluginPrompt,     // a server tool's plugin prompt (the pending kinds are
    kConfirmation,     // mutually exclusive in SessionState)
    kPermission,       // topmost: nothing may preempt the security prompt
    kCount,
};

class ModalStack {
public:
    // The visibility flag, by reference: components and controllers that
    // open and close a modal hold it (MakeModalHost binds it by reference).
    bool& Flag(ModalId id) { return flags_[Index(id)]; }
    bool Open(ModalId id) const { return flags_[Index(id)]; }
    bool AnyOpen() const;

    // The component shown while `id`'s flag stands. Set every layer before
    // Build().
    void Set(ModalId id, ftxui::Component component) {
        components_[Index(id)] = std::move(component);
    }

    // `base` wrapped in one FTXUI Modal() per layer that has a component,
    // bottom to top -- the outermost wrapper is the topmost modal.
    ftxui::Component Build(ftxui::Component base);

private:
    static constexpr std::size_t Index(ModalId id) { return static_cast<std::size_t>(id); }
    static constexpr std::size_t kSize = static_cast<std::size_t>(ModalId::kCount);
    std::array<bool, kSize> flags_{};
    std::array<ftxui::Component, kSize> components_{};
};

}  // namespace minimal::client

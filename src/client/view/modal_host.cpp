#include "minimal/client/view/modal_host.hpp"

#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/common/logger.hpp"

namespace minimal::client {

namespace {

using namespace ftxui;

// A plain Renderer+CatchEvent pair (the previous shape of this file) never
// registers the swapped-in child as a real ftxui child: Focusable() and
// ActiveChild() then fall back to ComponentBase's defaults, which see no
// children at all. ftxui::Modal() routes keyboard events through a
// Container::Tab, and ContainerBase::OnEvent refuses to forward anything
// until Focused() -- computed by walking Active()/ActiveChild() up to the
// root -- says the whole chain is active. With the child invisible to that
// walk, Focused() is false the instant a modal opens, and every keystroke
// (Escape, Enter, arrows, filter text) is dropped for as long as it's shown.
// Overriding Focusable()/ActiveChild() here to defer to the live slot is
// what makes this host, and therefore the modal above it, look active to
// ftxui's own machinery.
class ModalHostImpl : public ComponentBase {
public:
    ModalHostImpl(std::shared_ptr<Component> slot, const bool* show)
        : slot_(std::move(slot)), show_(show) {}

private:
    Element OnRender() override {
        return *slot_ ? (*slot_)->Render() : emptyElement();
    }
    bool OnEvent(Event event) override {
        return *show_ && *slot_ && (*slot_)->OnEvent(std::move(event));
    }
    // Deliberately NOT `*slot_ && (*slot_)->Focusable()`: that would make a
    // modal invisible to Focused() again whenever its content isn't built
    // from an ftxui primitive that self-reports focusable (Menu, Input) --
    // e.g. a plain CatchEvent(Renderer(...), ...) modal, which is a real
    // shape (see the confirmation/permission modals). Being shown IS being
    // the active thing; that alone is what should make the host focusable.
    bool Focusable() const override { return *show_ && (bool)*slot_; }
    Component ActiveChild() override { return *show_ ? *slot_ : nullptr; }

    std::shared_ptr<Component> slot_;
    const bool* show_;
};

// Same focus overrides as ModalHostImpl (see there for why they are what
// makes keys reach the modal at all); the plugin modal used to be a plain
// Renderer+CatchEvent, so with it open every key -- Escape and ctrl+q
// included -- was dropped and the UI read as frozen.
class PluginModalHostImpl : public ComponentBase {
public:
    PluginModalHostImpl(std::shared_ptr<Component> slot, const bool* show,
                        std::function<void()> close)
        : slot_(std::move(slot)), show_(show), close_(std::move(close)) {}

private:
    Element OnRender() override {
        if (!*slot_) return emptyElement();
        try {
            return (*slot_)->Render();
        } catch (const std::exception& error) {
            common::DefaultLogger().error("plugin modal threw: {}", error.what());
        } catch (...) {
            common::DefaultLogger().error("plugin modal threw a non-standard exception");
        }
        return emptyElement();
    }
    bool OnEvent(Event event) override {
        if (!*show_ || !*slot_) return false;
        if (event == Event::Escape) {
            // dynamic_cast across the plugin boundary is sound: EscapeAware's
            // key function (its destructor) lives in client core, so there
            // is one typeinfo however many plugins implement it.
            const auto* aware = dynamic_cast<const EscapeAware*>(slot_->get());
            if (aware == nullptr || !aware->WantsEscape()) {
                if (close_) close_();
                return true;
            }
        }
        // Plugin component code: a throw escaping into the ftxui loop would
        // std::terminate the process. Logged, and the event treated as
        // consumed.
        try {
            return (*slot_)->OnEvent(std::move(event));
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("plugin modal view threw on event: {}", error.what());
        } catch (...) {
            common::DefaultLogger().warn("plugin modal view threw on event (non-standard exception)");
        }
        return true;
    }
    bool Focusable() const override { return *show_ && (bool)*slot_; }
    Component ActiveChild() override { return *show_ ? *slot_ : nullptr; }

    std::shared_ptr<Component> slot_;
    const bool* show_;
    std::function<void()> close_;
};

}  // namespace

EscapeAware::~EscapeAware() = default;

ftxui::Component MakePluginModalHost(const std::shared_ptr<ftxui::Component>& child,
                                     const bool& show, std::function<void()> close) {
    return Make<PluginModalHostImpl>(child, &show, std::move(close));
}

ftxui::Component MakeModalHost(const std::shared_ptr<ftxui::Component>& child,
                               const bool& show) {
    // By value: the shared_ptr copy aliases the SAME slot the owner
    // reassigns per request; copying it decouples the handler's lifetime
    // from RunTui's locals while keeping the pointee shared.
    auto slot = child;
    // The flag travels as a pointer to the CALLER's bool -- the exact object
    // Modal(...) reads. A copied bool here is the bug this helper exists to
    // make unrepresentable (see the header).
    const bool* show_ptr = &show;
    return Make<ModalHostImpl>(std::move(slot), show_ptr);
}

}  // namespace minimal::client

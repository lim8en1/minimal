#pragma once

// A component rebuilt from scratch whenever it is marked stale -- a view
// that reads the host afresh on every open (OnShow marks it; the build reads
// HostUi and makes a new widget, filter and cursor reset). The rebuild
// happens at the start of the next render or event, never mid-frame: ftxui
// asks ActiveChild() while drawing, and freeing the child being drawn there
// would leave the frame writing into freed memory.

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

namespace minimal::client {

class RebuildingComponent final : public ftxui::ComponentBase {
public:
    // `stale` starts true in the usual case: the first frame builds.
    RebuildingComponent(std::shared_ptr<std::atomic<bool>> stale,
                        std::function<ftxui::Component()> build)
        : stale_(std::move(stale)), build_(std::move(build)) {}

private:
    void Sync() {
        if (child_ && !stale_->exchange(false)) return;
        stale_->store(false);
        if (child_) DetachAllChildren();
        child_ = build_();
        Add(child_);
    }
    ftxui::Element OnRender() override {
        Sync();
        return child_->Render();
    }
    bool OnEvent(ftxui::Event event) override {
        Sync();
        const ftxui::Component target = child_;  // a callback may mark it stale
        return target->OnEvent(std::move(event));
    }
    bool Focusable() const override { return true; }
    ftxui::Component ActiveChild() override { return child_; }  // no side effects

    std::shared_ptr<std::atomic<bool>> stale_;
    std::function<ftxui::Component()> build_;
    ftxui::Component child_;
};

}  // namespace minimal::client

#include "minimal/client/view/chat_scroll_region.hpp"

#include <algorithm>

#include <ftxui/screen/screen.hpp>
#include <ftxui/util/autoreset.hpp>

namespace minimal::client {

namespace {

class ScrollRegionNode : public ftxui::Node {
public:
    ScrollRegionNode(ftxui::Element child, std::shared_ptr<ScrollState> state)
        : ftxui::Node(ftxui::unpack(std::move(child))), state_(std::move(state)) {}

    // Base Node::ComputeRequirement is exactly the pass-through wanted here
    // (requirement_ = the single child's, focused-requirement propagation
    // included for completeness) -- the chat column no longer plants any
    // focus() marker, but a plugin row renderer conceivably could.
    void ComputeRequirement() override { Node::ComputeRequirement(); }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        const int viewport = box.y_max - box.y_min + 1;
        // Short transcripts report a content requirement BELOW the viewport
        // (the top filler collapses to zero); the child is still assigned
        // the full viewport height by the flex layout, so measure content as
        // at least the viewport or the clamp below would invent scroll room.
        const int content = std::max(children_[0]->requirement().min_y, viewport);
        const int max_top = std::max(0, content - viewport);
        const int from_bottom =
            std::clamp(state_->desired_offset_from_bottom, 0, max_top);
        const int top = max_top - from_bottom;
        // Same child-box shape FTXUI's Frame assigns: the full inner extent,
        // shifted up by the scroll offset -- which is what keeps RowBoxSpy
        // boxes true screen coordinates for the click hit-test.
        ftxui::Box child_box = box;
        child_box.y_min = box.y_min - top;
        child_box.y_max = box.y_min + content - top - 1;
        children_[0]->SetBox(child_box);
        state_->applied_offset_from_bottom = from_bottom;
        state_->viewport_height = viewport;
        state_->content_height = content;
        state_->viewport_top_screen = box.y_min;
    }

    void Render(ftxui::Screen& screen) override {
        const ftxui::AutoReset<ftxui::Box> stencil(
            &screen.stencil, ftxui::Box::Intersection(box_, screen.stencil));
        children_[0]->Render(screen);
    }

private:
    std::shared_ptr<ScrollState> state_;
};

}  // namespace

ftxui::Element ScrollRegion(ftxui::Element child,
                            std::shared_ptr<ScrollState> state) {
    return std::make_shared<ScrollRegionNode>(std::move(child), std::move(state));
}

}  // namespace minimal::client

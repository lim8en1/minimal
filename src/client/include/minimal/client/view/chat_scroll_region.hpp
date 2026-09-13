#pragma once

// An explicit-offset replacement for FTXUI's `yframe` for the chat body.
//
// WHY NOT yframe: yframe scrolls to whatever subtree element carries
// `focus()`, and its Frame::SetBox CENTERS that element in the viewport
// (dy = focused.y_min - external/2 + focused_height/2). Scrolling therefore
// moved one RENDER CHUNK per wheel step -- a chunk capped at 20 *source*
// lines but wrapping to far more *screen* lines (dense thinking prose) --
// and every step overshot to put the target chunk mid-viewport. A multi-line
// thinking block could leap the transcript first-to-last in a few ticks.
//
// This node applies an explicit scroll offset in SCREEN LINES instead, so
// granularity is a line regardless of how tall any single element is. The
// offset is expressed FROM THE BOTTOM and lives in the shared ScrollState:
// ChatView writes the desired value before building its Element for the
// frame; this node clamps it against the REAL content/viewport sizes during
// layout (SetBox) and writes the applied value + measured heights back.
// The write-back lets ChatView translate the row cursor (chat.row_up /
// chat.row_down) and keyboard scroll steps into lines without guessing, and
// keeps the position stable when content grows underneath (streaming): an
// offset-from-bottom is immune to appends, unlike an absolute offset.
//
// Layout semantics mirror FTXUI's own Frame (children box shifted by -top,
// Render through an intersected stencil), minus the focus-driven dy: one
// Node subclass, requirement pass-through, base Node::Check inherited so the
// standard two-iteration layout convergence still runs.

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <memory>

namespace minimal::client {

// Shared between ChatView (the input side) and the ScrollRegionNode (the
// layout side). Both fields the node writes are measurements of the LAST
// layout pass -- read them the way ChatView already reads RowBoxSpy boxes:
// as one-frame-stale ground truth that converges on the next pass.
struct ScrollState {
    // INPUT (ChatView writes each frame, before Render returns): how many
    // lines the newest content line sits above the viewport's bottom line.
    // 0 pins to the newest content (the historical default).
    int desired_offset_from_bottom = 0;
    // OUTPUT (node writes at SetBox): the offset actually applied after
    // clamping to [0, content - viewport].
    int applied_offset_from_bottom = 0;
    // OUTPUT: viewport height and total content height in lines, as laid
    // out last pass (content is at least the viewport -- short transcripts
    // stretch via the top filler).
    int viewport_height = 0;
    int content_height = 0;
    // OUTPUT: the viewport's screen-space y_min last pass. RowBoxSpy boxes
    // are screen coordinates; converting a row's box into content
    // coordinates (for row-cursor reveal) needs the viewport's own origin.
    int viewport_top_screen = 0;
};

// Wraps `child` in the scroll region driven by `state`. Ownership is shared:
// the element tree holds the node alive for the frames it renders, ChatView
// holds the state alive across frames.
ftxui::Element ScrollRegion(ftxui::Element child,
                            std::shared_ptr<ScrollState> state);

}  // namespace minimal::client

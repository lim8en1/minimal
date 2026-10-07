#pragma once

// The status bar's arrangement: which segments show, on which side, in what
// order. Every segment is a plugin's and places itself
// (plugins::StatusSegment::placement()); ui.json's `status_segments` hides
// or moves one by id; ComposeStatusBar turns the two into the ordered pieces
// the renderer resolves (view/status_bar.hpp). Core names no segment.
// `minimal status-bar` prints the same composition (status_bar_command.hpp).

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "minimal/client/sdk/registries.hpp"
#include "minimal/client/sdk/render.hpp"

namespace minimal::client {

// One piece of the composed bar, in render order: a segment (by its surface
// id), a literal separator between two, or the split ("{>}"): everything
// after it right-aligns.
struct StatusSegment {
    bool is_segment = false;  // false: literal text
    std::string text;         // the literal, or the segment id
};

// ui.json's word on one segment: hidden, or moved -- a field left unset
// keeps the segment's own.
struct StatusSegmentOverride {
    bool hidden = false;
    std::optional<plugins::StatusPlacement::Side> side;
    std::optional<int> order;
};

// One segment and where it ended up. `placement` null: shown nowhere (the
// segment places nowhere and nothing placed it, or the override hid it).
struct PlacedStatusSegment {
    std::string id;
    std::optional<plugins::StatusPlacement> placement;
    bool overridden = false;
};

struct ComposedStatusBar {
    std::vector<StatusSegment> format;  // what ResolveStatusBar takes
    // Every registered segment: the shown ones in render order (left, then
    // right), then the rest by id.
    std::vector<PlacedStatusSegment> segments;
    // Override ids no registered segment answers to.
    std::vector<std::string> unknown;
};

// Between two shown segments on one side.
inline constexpr std::string_view kStatusSeparator = " · ";

// Sorts each side by (order, id) and joins it with kStatusSeparator, the
// left side, then the split, then the right. A segment whose placement()
// throws places nowhere (plugin code is contained, as when it renders).
ComposedStatusBar ComposeStatusBar(const plugins::StatusSegmentRegistry& registry,
                                   const std::map<std::string, StatusSegmentOverride>& overrides);

}  // namespace minimal::client

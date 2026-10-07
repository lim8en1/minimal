#include "minimal/client/status_layout.hpp"

#include <algorithm>
#include <tuple>

#include "minimal/client/view/status_bar.hpp"

namespace minimal::client {

namespace {

using Side = plugins::StatusPlacement::Side;

std::optional<plugins::StatusPlacement> OwnPlacement(const plugins::StatusSegment& segment) {
    try {
        return segment.placement();
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

ComposedStatusBar ComposeStatusBar(const plugins::StatusSegmentRegistry& registry,
                                   const std::map<std::string, StatusSegmentOverride>& overrides) {
    ComposedStatusBar out;
    const auto& entries = registry.All();
    const auto& surfaces = registry.surfaces();
    std::vector<PlacedStatusSegment> shown;
    std::vector<PlacedStatusSegment> unplaced;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        PlacedStatusSegment placed{surfaces[i], OwnPlacement(*entries[i]), false};
        if (const auto it = overrides.find(placed.id); it != overrides.end()) {
            const StatusSegmentOverride& override = it->second;
            placed.overridden = true;
            if (override.hidden) {
                placed.placement.reset();
            } else if (override.side || override.order) {
                plugins::StatusPlacement moved = placed.placement.value_or(plugins::StatusPlacement{});
                if (override.side) moved.side = *override.side;
                if (override.order) moved.order = *override.order;
                placed.placement = moved;
            }
        }
        (placed.placement ? shown : unplaced).push_back(std::move(placed));
    }
    for (const auto& [id, override] : overrides) {
        (void)override;
        if (std::find(surfaces.begin(), surfaces.end(), id) == surfaces.end()) {
            out.unknown.push_back(id);
        }
    }

    std::sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) {
        return std::tuple(a.placement->side == Side::kRight, a.placement->order, a.id) <
               std::tuple(b.placement->side == Side::kRight, b.placement->order, b.id);
    });
    std::sort(unplaced.begin(), unplaced.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });

    bool split = false;
    bool first_on_side = true;
    for (const auto& placed : shown) {
        if (!split && placed.placement->side == Side::kRight) {
            out.format.push_back({true, std::string(kStatusSplitId)});
            split = true;
            first_on_side = true;
        }
        if (!std::exchange(first_on_side, false)) {
            out.format.push_back({false, std::string(kStatusSeparator)});
        }
        out.format.push_back({true, placed.id});
    }
    out.segments = std::move(shown);
    out.segments.insert(out.segments.end(), std::make_move_iterator(unplaced.begin()),
                        std::make_move_iterator(unplaced.end()));
    return out;
}

}  // namespace minimal::client

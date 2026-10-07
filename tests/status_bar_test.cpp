// The status bar's width decision (src/client/view/status_bar.{hpp,cpp}),
// tested pure (ARCHITECTURE.md "Testing": render output itself is not
// unit-tested, the logic behind it is). Two regressions this covers:
//
//   1. An unfilled segment used to leave its queued literal separator
//      dangling, so consecutive skipped segments produced doubled " | "
//      runs (e.g. "0.1.0 |  |  | 682/262144 tok | ..." when {activity} was
//      empty).
//   2. With no width awareness at all, FTXUI's hbox shrink pass truncated
//      EVERY segment proportionally at narrow widths, including the token
//      count -- rendering "186/26214" at 80 columns instead of the real
//      "186/262144", a wrong number presented as fact.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>

#include "minimal/client/sdk/utf8.hpp"
#include "minimal/client/view/status_bar.hpp"

using minimal::client::ResolveStatusBar;
using minimal::client::StatusBarPart;
using minimal::client::StatusLayout;

namespace {
// Core knows no segment by name: what a segment declares -- its priority,
// its column cap -- comes with the call, as the frame passes it. These are
// session-status's own for its segments.
const StatusLayout kLayout = {
    .priorities = {{"permission_mode", 100}, {"activity", 90}, {"model", 80}, {"tokens", 70},
                   {"agent", 60}, {"paused", 50}, {"session", 20}, {"version", 10}},
    .caps = {{"session", 24}}};

// The pieces ComposeStatusBar would hand the renderer, written the short
// way: "{id}" a segment, "{>}" the split, anything else a literal.
std::vector<minimal::client::StatusSegment> ParseStatusFormat(std::string_view format) {
    std::vector<minimal::client::StatusSegment> out;
    std::size_t i = 0;
    while (i < format.size()) {
        const std::size_t open = format.find('{', i);
        if (open != i) {
            const std::size_t end = open == std::string_view::npos ? format.size() : open;
            out.push_back({false, std::string(format.substr(i, end - i))});
            i = end;
            continue;
        }
        const std::size_t close = format.find('}', open);
        out.push_back({true, std::string(format.substr(open + 1, close - open - 1))});
        i = close + 1;
    }
    return out;
}

std::vector<std::string> Texts(const std::vector<StatusBarPart>& parts) {
    std::vector<std::string> out;
    for (const auto& part : parts) out.push_back(part.text);
    return out;
}
}  // namespace

TEST_CASE("all segments filled renders every value with literals between", "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {tokens} | {model}");
    const std::map<std::string, std::string> values = {
        {"version", "0.1.0"}, {"tokens", "551/262144 tok"}, {"model", "local/x"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0", " | ", "551/262144 tok", " | ", "local/x"});
}

TEST_CASE("an unfilled segment renders nothing and does not orphan its separator",
          "[status_bar]") {
    // Regression: {activity} empty used to leave a bare " | " where it sat,
    // rather than being absorbed into the run around it.
    const auto format = ParseStatusFormat("{version} | {activity} | {tokens}");
    const std::map<std::string, std::string> values = {{"version", "0.1.0"},
                                                        {"tokens", "682/262144 tok"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0", " | ", "682/262144 tok"});
}

TEST_CASE("two consecutive unfilled segments still leave exactly one separator",
          "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {activity} | {agent} | {tokens}");
    const std::map<std::string, std::string> values = {{"version", "0.1.0"},
                                                        {"tokens", "682/262144 tok"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0", " | ", "682/262144 tok"});
}

TEST_CASE("an unfilled segment first in the format renders no leading literal",
          "[status_bar]") {
    const auto format = ParseStatusFormat("{session} | {version}");
    const std::map<std::string, std::string> values = {{"version", "0.1.0"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0"});
}

TEST_CASE("max_width <= 0 never clamps {session}", "[status_bar]") {
    const auto format = ParseStatusFormat("{session}");
    const std::map<std::string, std::string> values = {
        {"session", std::string(200, 'x')}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0, kLayout);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].text.size() == 200);
}

TEST_CASE("a capped segment is cut to its cap even with plenty of room", "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {session}");
    const std::map<std::string, std::string> values = {
        {"version", "0.1.0"}, {"session", std::string(200, 'x')}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/500, kLayout);
    REQUIRE(parts.size() == 3);
    CHECK(minimal::client::Utf8ColumnWidth(parts[2].text) == 24);
    CHECK(parts[2].text.back() != 'x');  // ellipsis, not a raw cut
}

TEST_CASE("uncapped segments are NEVER truncated, only a capped one gives way", "[status_bar]") {
    // The regression this guards: at 80 columns the old renderer shrank
    // EVERY child proportionally, including the token count. This asserts
    // the model/tokens/version/agent/permission_mode values survive intact
    // even when {session} alone is far too wide to fit.
    const auto format = ParseStatusFormat(
        "{version} | {session} | {tokens} | {model} | {agent} | {permission_mode}");
    const std::map<std::string, std::string> values = {
        {"version", "0.1.0"},
        {"session", "Reply with only a fenced code block tagged cpp containing: int main(){}"},
        {"tokens", "186/262144 tok"},
        {"model", "local/ornith-1.5-9b"},
        {"agent", "build"},
        {"permission_mode", "normal"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/80, kLayout);
    const auto texts = Texts(parts);
    CHECK(std::find(texts.begin(), texts.end(), "0.1.0") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "186/262144 tok") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "local/ornith-1.5-9b") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "build") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "normal") != texts.end());
}

TEST_CASE("a capped segment is dropped, with its leading separator, when there is no room "
          "at all", "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {session} | {tokens}");
    const std::map<std::string, std::string> values = {
        {"version", std::string(30, 'v')},
        {"session", "anything"},
        {"tokens", std::string(28, 't')}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/61, kLayout);
    CHECK(Texts(parts) == std::vector<std::string>{std::string(30, 'v'), " | ", std::string(28, 't')});
}

TEST_CASE("overflow drops whole segments by priority, never clips a value", "[status_bar]") {
    // The tail used to be clipped by the terminal edge -- and the tail is
    // where permission_mode sits. Now version (lowest) goes first, with the
    // separator that joined it, and the mode survives intact.
    const auto format = ParseStatusFormat("{version} | {tokens} | {permission_mode}");
    const std::map<std::string, std::string> values = {
        {"version", "0.1.0-long-build-tag"}, {"tokens", "62k/200k · 31%"},
        {"permission_mode", "bypass"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/30, kLayout);
    CHECK(Texts(parts) == std::vector<std::string>{"62k/200k · 31%", " | ", "bypass"});

    // Narrower still: tokens (70) goes before permission_mode (100).
    const auto tighter = ResolveStatusBar(format, values, /*max_width=*/10, kLayout);
    CHECK(Texts(tighter) == std::vector<std::string>{"bypass"});
}

TEST_CASE("{>} splits the bar; the right group's first value takes no separator",
          "[status_bar]") {
    const auto format = ParseStatusFormat("{activity} · {tokens}{>}{model} · {agent}");
    const std::map<std::string, std::string> values = {
        {"activity", "thinking…"}, {"tokens", "1k tok"}, {"model", "a/x"}, {"agent", "build"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    REQUIRE(parts.size() == 7);
    CHECK(parts[3].is_split);
    CHECK(Texts(parts) ==
          std::vector<std::string>{"thinking…", " · ", "1k tok", "", "a/x", " · ", "build"});
    CHECK(parts[4].segment_id == "model");

    // An empty first value on the right does not leave a leading separator.
    const std::map<std::string, std::string> no_model = {{"activity", "thinking…"},
                                                         {"agent", "build"}};
    CHECK(Texts(ResolveStatusBar(format, no_model, 0)) ==
          std::vector<std::string>{"thinking…", "", "build"});
}

TEST_CASE("a plugin segment gives way by its own declared priority", "[status_bar]") {
    // Core knows no plugin segment by name: {low} and {high} are both plugin
    // ids, ranked only by the priority each declared.
    const auto format = ParseStatusFormat("{low} | {model} | {high}");
    const std::map<std::string, std::string> values = {
        {"low", "aaaaaaaa"}, {"model", "local/x"}, {"high", "bbbbbbbb"}};
    const std::map<std::string, int> priorities = {{"low", 5}, {"high", 95}};
    // Room for model + one plugin value: the lower-priority one goes.
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/22, {.priorities = priorities});
    CHECK(Texts(parts) == std::vector<std::string>{"local/x", " | ", "bbbbbbbb"});
}

// --- composition: segments place themselves, ui.json overrides ---------------

namespace {

using minimal::client::ComposeStatusBar;
using minimal::client::StatusSegmentOverride;
using Side = minimal::client::plugins::StatusPlacement::Side;

class Placed final : public minimal::client::plugins::StatusSegment {
public:
    Placed(std::string id, std::optional<minimal::client::plugins::StatusPlacement> placement,
           bool throws = false)
        : id_(std::move(id)), placement_(placement), throws_(throws) {}
    std::string_view id() const override { return id_; }
    minimal::client::plugins::StatusValue Render(
        const minimal::client::plugins::TabStatus&) const override {
        return {id_, ""};
    }
    std::optional<minimal::client::plugins::StatusPlacement> placement() const override {
        if (throws_) throw std::runtime_error("placement broke");
        return placement_;
    }

private:
    std::string id_;
    std::optional<minimal::client::plugins::StatusPlacement> placement_;
    bool throws_;
};

std::string Shape(const minimal::client::ComposedStatusBar& bar) {
    std::string out;
    for (const auto& piece : bar.format) out += piece.is_segment ? "{" + piece.text + "}" : piece.text;
    return out;
}

void Fill(minimal::client::plugins::StatusSegmentRegistry& registry) {
    registry.Register(std::make_unique<Placed>("model", minimal::client::plugins::StatusPlacement{Side::kRight, 10}));
    registry.Register(std::make_unique<Placed>("tokens", minimal::client::plugins::StatusPlacement{Side::kLeft, 20}));
    registry.Register(std::make_unique<Placed>("activity", minimal::client::plugins::StatusPlacement{Side::kLeft, 10}));
    registry.Register(std::make_unique<Placed>("cost", minimal::client::plugins::StatusPlacement{Side::kLeft, 20}));
    registry.Register(std::make_unique<Placed>("version", std::nullopt));
    registry.Register(std::make_unique<Placed>("broken", minimal::client::plugins::StatusPlacement{Side::kLeft, 1}, /*throws=*/true));
}

}  // namespace

TEST_CASE("the bar composes from placements: side, order, ties by id, unplaced left out",
          "[status_bar][status_layout]") {
    minimal::client::plugins::StatusSegmentRegistry registry;
    Fill(registry);
    const auto bar = ComposeStatusBar(registry, {});
    // cost and tokens tie at left 20: by id. A throwing placement places nowhere.
    CHECK(Shape(bar) == "{activity} · {cost} · {tokens}{>}{model}");
    std::vector<std::string> order;
    for (const auto& placed : bar.segments) order.push_back(placed.id);
    CHECK(order == std::vector<std::string>{"activity", "cost", "tokens", "model", "broken", "version"});
    CHECK(bar.unknown.empty());
}

TEST_CASE("ui.json overrides hide, move one field, place an unplaced segment, and name unknowns",
          "[status_bar][status_layout]") {
    minimal::client::plugins::StatusSegmentRegistry registry;
    Fill(registry);
    const std::map<std::string, StatusSegmentOverride> overrides = {
        {"cost", {.side = Side::kRight, .order = 5}},
        {"tokens", {.order = 5}},              // side kept: left
        {"model", {.hidden = true}},
        {"version", {.side = Side::kRight}},   // placed nowhere before: order 0
        {"nope", {.hidden = true}}};
    const auto bar = ComposeStatusBar(registry, overrides);
    CHECK(Shape(bar) == "{tokens} · {activity}{>}{version} · {cost}");
    CHECK(bar.unknown == std::vector<std::string>{"nope"});
    for (const auto& placed : bar.segments) {
        INFO(placed.id);
        CHECK(placed.overridden == (placed.id != "activity" && placed.id != "broken"));
    }
}

TEST_CASE("a bar with one side only takes no split", "[status_bar][status_layout]") {
    minimal::client::plugins::StatusSegmentRegistry registry;
    registry.Register(std::make_unique<Placed>("model", minimal::client::plugins::StatusPlacement{Side::kRight, 10}));
    CHECK(Shape(ComposeStatusBar(registry, {})) == "{>}{model}");
    CHECK(Shape(ComposeStatusBar(registry, {{"model", {.hidden = true}}})).empty());
}

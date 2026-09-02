// Pure-logic tier for the agent picker's row-building, mirroring
// picker_model_test.cpp -- no FTXUI, no AppState, no network.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/agent_picker_model.hpp"

using minimal::client::AgentPickerRow;
using minimal::client::BuildAgentPickerRows;

namespace {

minimal::v1::AgentPresetSummary Preset(std::string name, std::string description) {
    minimal::v1::AgentPresetSummary p;
    p.set_name(std::move(name));
    p.set_description(std::move(description));
    return p;
}

}  // namespace

TEST_CASE("agent picker rows: no filter returns every preset in order") {
    const std::vector<minimal::v1::AgentPresetSummary> presets = {
        Preset("build", "Builds things."), Preset("plan", "Plans work.")};

    const auto rows = BuildAgentPickerRows(presets, "");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "build");
    CHECK(rows[1].name == "plan");
}

TEST_CASE("agent picker rows: filter matches name or description case-insensitively") {
    const std::vector<minimal::v1::AgentPresetSummary> presets = {
        Preset("build", "Runs the build."), Preset("plan", "Plans work.")};

    SECTION("matches on name") {
        const auto rows = BuildAgentPickerRows(presets, "BUI");
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].name == "build");
    }
    SECTION("matches on description") {
        const auto rows = BuildAgentPickerRows(presets, "plans");
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].name == "plan");
    }
}

TEST_CASE("agent picker rows: a filter matching nothing returns an empty list") {
    const std::vector<minimal::v1::AgentPresetSummary> presets = {Preset("build", "")};

    CHECK(BuildAgentPickerRows(presets, "zzz-no-match").empty());
}

TEST_CASE("agent picker rows: an empty preset list returns an empty list") {
    CHECK(BuildAgentPickerRows({}, "").empty());
}

TEST_CASE("agent picker rows: label falls back to the bare name when description is empty") {
    const std::vector<minimal::v1::AgentPresetSummary> presets = {Preset("build", "")};

    const auto rows = BuildAgentPickerRows(presets, "");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].label == "build");
}

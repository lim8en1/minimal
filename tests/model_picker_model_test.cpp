// Pure-logic tier for the model picker's row-building, mirroring
// picker_model_test.cpp / agent_picker_model_test.cpp -- no FTXUI, no
// AppState, no network. The grouping rules are the point: a discovered
// catalog (OpenRouter serves hundreds) must collapse whole providers away
// under a filter, headers included.

#include <catch2/catch_test_macros.hpp>

#include "model_picker_model.hpp"

using minimal::client::ui_kit::BuildModelPickerRows;
using minimal::client::ui_kit::ModelPickerRow;
using minimal::v1::ModelSummary;

namespace {

ModelSummary Model(std::string ref, int64_t ctx = 0) {
    ModelSummary m;
    m.set_ref(std::move(ref));
    m.set_context_window_tokens(ctx);
    return m;
}

}  // namespace

TEST_CASE("model picker rows: no filter groups by provider with headers") {
    // Server delivery order is refs ascending, which groups providers.
    const std::vector<ModelSummary> models = {
        Model("local/some-model"), Model("local/another-model"),
        Model("openrouter/claude-opus-5"),
    };

    const auto rows = BuildModelPickerRows(models, "");
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].header);
    CHECK(rows[0].label == "local");
    CHECK(rows[0].ref.empty());
    CHECK_FALSE(rows[1].header);
    CHECK(rows[1].ref == "local/another-model");
    CHECK(rows[1].label == "another-model");   // the model id, not the ref
    CHECK(rows[2].label == "some-model");
    CHECK(rows[3].header);
    CHECK(rows[3].label == "openrouter");
    CHECK(rows[4].ref == "openrouter/claude-opus-5");
}

TEST_CASE("model picker rows: filter matches model id or provider, omits empty providers") {
    const std::vector<ModelSummary> models = {
        Model("local/some-model"), Model("local/another-model"),
        Model("openrouter/anthropic/claude-opus-5"),
        Model("openrouter/openai/gpt-5"),
    };

    SECTION("matching the model id keeps its group header and its siblings out") {
        const auto rows = BuildModelPickerRows(models, "some-model");
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].header);
        CHECK(rows[0].label == "local");
        CHECK(rows[1].ref == "local/some-model");
    }

    SECTION("matching the provider keeps the whole group") {
        const auto rows = BuildModelPickerRows(models, "openrouter");
        REQUIRE(rows.size() == 3);
        CHECK(rows[0].header);
        CHECK(rows[0].label == "openrouter");
        CHECK(rows[1].ref == "openrouter/anthropic/claude-opus-5");
        CHECK(rows[2].ref == "openrouter/openai/gpt-5");
    }

    SECTION("matching a nested id segment works (openrouter ids carry slashes)") {
        const auto rows = BuildModelPickerRows(models, "claude");
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].header);
        CHECK(rows[1].ref == "openrouter/anthropic/claude-opus-5");
    }

    SECTION("a provider with zero hits vanishes entirely, header and all") {
        const auto rows = BuildModelPickerRows(models, "gpt-5");
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].label == "openrouter");
        CHECK(rows[1].ref == "openrouter/openai/gpt-5");
    }
}

TEST_CASE("model picker rows: filter matching nothing returns an empty list") {
    const std::vector<ModelSummary> models = {Model("local/some-model")};
    CHECK(BuildModelPickerRows(models, "zzz").empty());
}

TEST_CASE("model picker rows: an empty list yields no headers either") {
    CHECK(BuildModelPickerRows({}, "").empty());
}

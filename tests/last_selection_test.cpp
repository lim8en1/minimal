#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "minimal/client/last_selection.hpp"
#include "temp_dir.hpp"

using minimal::client::LastSelectionStore;

TEST_CASE("last selection: per project, with a global fallback", "[last_selection]") {
    minimal::testing::TempDir dir;
    const LastSelectionStore store(dir.path() / "last_selection.json");

    CHECK(store.For("/repo/a").agent.empty());   // no file yet: nothing remembered

    store.Record("/repo/a", std::string("plan"), std::string("local/qwen4"));
    CHECK(store.For("/repo/a").agent == "plan");
    CHECK(store.For("/repo/a").model == "local/qwen4");
    // Another project with no entry of its own gets the global last pick.
    CHECK(store.For("/repo/b").agent == "plan");

    store.Record("/repo/b", std::string("build"), std::nullopt);
    CHECK(store.For("/repo/b").agent == "build");
    CHECK(store.For("/repo/b").model == "local/qwen4");  // the global model, untouched
    CHECK(store.For("/repo/a").agent == "plan");         // a's own entry wins over global
}

TEST_CASE("last selection: an empty model is back to the default", "[last_selection]") {
    minimal::testing::TempDir dir;
    const LastSelectionStore store(dir.path() / "last_selection.json");
    store.Record("/repo", std::string("plan"), std::string("local/qwen4"));
    store.Record("/repo", std::nullopt, std::string());
    CHECK(store.For("/repo").model.empty());
    CHECK(store.For("/repo").agent == "plan");
}

TEST_CASE("last selection: Forget drops what the server refused", "[last_selection]") {
    minimal::testing::TempDir dir;
    const LastSelectionStore store(dir.path() / "last_selection.json");
    store.Record("/repo", std::string("gone"), std::string("old/model"));
    store.Forget("/repo", /*agent=*/false, /*model=*/true);
    CHECK(store.For("/repo").agent == "gone");
    CHECK(store.For("/repo").model.empty());
    CHECK(store.For("/elsewhere").model.empty());   // the global copy too
}

TEST_CASE("last selection: a broken file reads as empty and is replaced", "[last_selection]") {
    minimal::testing::TempDir dir;
    const auto path = dir.path() / "last_selection.json";
    std::ofstream(path) << "{ not json";
    const LastSelectionStore store(path);
    CHECK(store.For("/repo").agent.empty());
    store.Record("/repo", std::string("plan"), std::nullopt);
    CHECK(store.For("/repo").agent == "plan");
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "minimal/common/config_reader.hpp"

using namespace minimal::config;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;

namespace {

std::string ErrorOf(const std::function<void()>& run) {
    try {
        run();
    } catch (const ConfigError& error) {
        return error.what();
    }
    return "";
}

}  // namespace

TEST_CASE("Reader: absent keys fall back, present keys are typed", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"flag": true, "n": 7, "s": "x", "d": 0.5})");
    Reader reader(doc, "f.json");
    CHECK(reader.Bool("flag", false));
    CHECK(reader.Int("n", 0) == 7);
    CHECK(reader.String("s", "") == "x");
    CHECK(reader.Double("d", 0.0) == 0.5);
    CHECK(reader.Int("missing", 42) == 42);
    reader.Finish();
}

TEST_CASE("Reader: a wrong type names file and full path", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"memory": {"auto_compact": "yes"}})");
    Reader root(doc, "settings.json");
    Reader memory = root.Object("memory");
    CHECK_THAT(ErrorOf([&] { memory.Bool("auto_compact", true); }),
               Equals("settings.json: memory.auto_compact: must be true or false, got \"yes\""));
}

TEST_CASE("Reader: a float is never truncated into an integer", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"max_tokens": 4096.7})");
    Reader reader(doc, "providers.json", "providers.l");
    CHECK_THAT(ErrorOf([&] { reader.Int("max_tokens", 1); }),
               ContainsSubstring("providers.l.max_tokens: must be an integer"));
}

TEST_CASE("Reader: ranges are enforced, including huge unsigned values", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"a": 0, "b": 18446744073709551615})");
    Reader reader(doc, "f.json");
    CHECK_THAT(ErrorOf([&] { reader.Int("a", 1, 1, 10); }), ContainsSubstring("at least 1"));
    CHECK_THAT(ErrorOf([&] { reader.Int("b", 1, 1, 10); }), ContainsSubstring("at most 10"));
}

TEST_CASE("Reader: an enum lists its choices", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"mode": "Trusted"})");
    Reader reader(doc, "settings.json", "project_local");
    CHECK_THAT(ErrorOf([&] { reader.Enum("mode", "trusted", {"trusted", "all", "none"}); }),
               Equals("settings.json: project_local.mode: must be one of \"trusted\", \"all\", "
                      "\"none\", got \"Trusted\""));
}

TEST_CASE("Reader: Finish reports an unknown key with the known set", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"defualt_model": "x", "// note": "comment"})");
    Reader reader(doc, "settings.json");
    reader.String("default_model", "");
    reader.Bool("background_wake", false);
    CHECK_THAT(ErrorOf([&] { reader.Finish(); }),
               Equals("settings.json: defualt_model: unknown field (known: background_wake, "
                      "default_model)"));
}

TEST_CASE("Reader: Finish in warn mode reports through the sink", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"typo": 1})");
    std::vector<std::string> warnings;
    Reader reader(doc, "ui.json", "", [&](const std::string& text) { warnings.push_back(text); });
    reader.Finish(UnknownKeys::kWarn);
    REQUIRE(warnings.size() == 1);
    CHECK_THAT(warnings.front(), ContainsSubstring("ui.json: typo: unknown field"));
}

TEST_CASE("Reader: a non-object section is an error, not defaults", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"memory": "oops"})");
    Reader root(doc, "settings.json");
    CHECK_THAT(ErrorOf([&] { root.Object("memory"); }),
               Equals("settings.json: memory: must be an object, got \"oops\""));
}

TEST_CASE("Reader: string lists and maps are validated element-wise", "[config_reader]") {
    const auto doc = nlohmann::json::parse(
        R"({"list": "read", "mixed": ["a", 1], "map": {"X": "a\nb"}})");
    Reader reader(doc, "f.json");
    CHECK_THAT(ErrorOf([&] { reader.StringList("list"); }),
               ContainsSubstring("must be a list of strings"));
    CHECK_THAT(ErrorOf([&] { reader.StringList("mixed"); }),
               ContainsSubstring("must contain only strings"));
    CHECK_THAT(ErrorOf([&] { reader.MaybeStringMap("map"); }), ContainsSubstring("CR or LF"));
}

TEST_CASE("Reader: comment keys are skipped by Entries", "[config_reader]") {
    const auto doc = nlohmann::json::parse(R"({"models": {"//": "c", "a": {}}})");
    Reader reader(doc, "f.json");
    const auto entries = reader.Entries("models");
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().first == "a");
    reader.Finish();
}

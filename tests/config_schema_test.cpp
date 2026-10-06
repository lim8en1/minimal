// Describe mode (config::SchemaRecording): a plugin's schema recorded from
// its own parser, and dry-run validation through the same function.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>

#include "bash_settings.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_loader.hpp"

using namespace minimal;
using nlohmann::json;

namespace {

// Every recorded default, placed at its dotted path: the config a user who
// wrote nothing effectively has.
json DefaultsOf(const json& schema) {
    json out = json::object();
    for (const auto& field : schema.at("fields")) {
        if (!field.contains("default")) continue;
        json* node = &out;
        const std::string path = field.at("path");
        std::size_t start = 0;
        for (std::size_t dot; (dot = path.find('.', start)) != std::string::npos; start = dot + 1) {
            node = &(*node)[path.substr(start, dot - start)];
        }
        (*node)[path.substr(start)] = field.at("default");
    }
    return out;
}

const json* FieldAt(const json& schema, const std::string& path) {
    for (const auto& field : schema.at("fields")) {
        if (field.at("path") == path) return &field;
    }
    return nullptr;
}

// A parser with one read of each kind, nested, plus a second file it also
// reads (a bundled defaults file) that must not leak into the schema.
void ParseSample(const json& config) {
    static const json kBundled = {{"other", 1}};
    config::Reader bundled(kBundled, "bundled.json");
    (void)bundled.Int("other", 0);
    bundled.Finish();

    config::Reader root(config, "plugins.d/sample.json");
    root.Describe("limit", {"How many.", config::ApplyPolicy::kLive});
    (void)root.Int("limit", 5, 1, 100);
    (void)root.Bool("flag", true);
    (void)root.Enum("mode", "fast", {"fast", "slow"});
    root.Describe("api_key", {"Key reference.", config::ApplyPolicy::kRestart, true});
    (void)root.String("api_key", "");
    (void)root.MaybeDouble("ratio", 0.0, 1.0);
    root.Default("ratio", 0.5);
    config::Reader nested = root.Object("nested");
    (void)nested.StringList("names");
    (void)nested.Raw("blob");
    nested.Finish();
    root.Finish();
}

}  // namespace

TEST_CASE("describe mode records each field the parser reads", "[config_schema]") {
    const json schema = config::DescribeConfig("plugins.d/sample.json", ParseSample);
    CHECK(schema.at("file") == "plugins.d/sample.json");
    CHECK(schema.at("fields").size() == 7);  // bundled.json's "other" is not one

    const json* limit = FieldAt(schema, "limit");
    REQUIRE(limit != nullptr);
    CHECK(limit->at("type") == "integer");
    CHECK(limit->at("default") == 5);
    CHECK(limit->at("minimum") == 1);
    CHECK(limit->at("maximum") == 100);
    CHECK(limit->at("doc") == "How many.");
    CHECK(limit->at("apply") == "live");

    const json* mode = FieldAt(schema, "mode");
    REQUIRE(mode != nullptr);
    CHECK(mode->at("type") == "enum");
    CHECK(mode->at("choices") == json::array({"fast", "slow"}));
    CHECK(mode->at("default") == "fast");
    CHECK(mode->at("apply") == "restart");  // undeclared: the safe default

    CHECK(FieldAt(schema, "flag")->at("default") == true);
    CHECK(FieldAt(schema, "api_key")->at("secret") == true);
    CHECK(FieldAt(schema, "ratio")->at("default") == 0.5);  // Maybe* + Default()
    CHECK(FieldAt(schema, "ratio")->at("maximum") == 1.0);
    CHECK(FieldAt(schema, "nested.names")->at("type") == "string_list");
    CHECK(FieldAt(schema, "nested.blob")->at("type") == "json");
    CHECK(FieldAt(schema, "other") == nullptr);
}

TEST_CASE("outside describe mode a reader records nothing and Describe is inert",
          "[config_schema]") {
    REQUIRE_NOTHROW(ParseSample(json::object()));
    // A recording for a different file sees none of this one's reads.
    config::SchemaRecording other("plugins.d/other.json");
    ParseSample(json::object());
    CHECK(other.Schema().at("fields").empty());
}

TEST_CASE("ValidateConfig answers with the parser's own message", "[config_schema]") {
    CHECK(config::ValidateConfig(ParseSample, json{{"limit", 7}}).empty());
    CHECK(config::ValidateConfig(ParseSample, json{{"limit", 0}}) ==
          "plugins.d/sample.json: limit: must be at least 1, got 0");
    CHECK(config::ValidateConfig(ParseSample, json{{"limt", 7}})
              .find("limt: unknown field") != std::string::npos);
}

TEST_CASE("a real plugin's schema: tools-bash", "[config_schema]") {
    const json schema = config::DescribeConfig(
        "plugins.d/tools-bash.json",
        [](const json& config) { (void)tools_bash::ParseBashSettings(config); });
    const json* timeout = FieldAt(schema, "timeout_s");
    REQUIRE(timeout != nullptr);
    CHECK(timeout->at("default") == 120);  // seconds, the unit the file uses
    CHECK(timeout->contains("doc"));
    CHECK(FieldAt(schema, "inherit_env")->at("choices") == json::array({"allowlist", "all"}));
    for (const auto& field : schema.at("fields")) {
        INFO(field.dump());
        CHECK(field.contains("default"));  // every tools-bash field has one
        CHECK(field.contains("doc"));
    }
}

TEST_CASE("every bundled plugin's schema agrees with its own parser", "[config_schema][bundled]") {
    // decision-advisor's parse reads its bundled defaults file from the config
    // dir; the source tree's bundled/ is that file's origin.
    ::setenv("MINIMAL_CONFIG_DIR", MINIMAL_BUNDLED_DIR, /*overwrite=*/1);

    int described = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(MINIMAL_BUNDLED_SERVER_PLUGIN_DIR)) {
        if (entry.path().extension() != ".so" && entry.path().extension() != ".dylib") continue;
        const auto inspected = plugins::InspectPluginConfig(entry.path());
        INFO(entry.path().string() << ": " << inspected.error);
        REQUIRE(inspected.error.empty());
        if (inspected.config.schema == nullptr) continue;
        REQUIRE(inspected.config.validate != nullptr);  // the macro defines both
        ++described;

        json schema;
        inspected.config.schema(schema);
        INFO(schema.dump(2));
        CHECK(schema.at("file") == "plugins.d/" + inspected.name + ".json");
        REQUIRE_FALSE(schema.at("fields").empty());

        bool any_live = false;
        for (const auto& field : schema.at("fields")) {
            CHECK(field.contains("type"));
            CHECK(field.contains("default"));
            if (inspected.name != "decision-advisor") CHECK(field.contains("doc"));
            any_live = any_live || field.at("apply") == "live";
        }
        // A plugin that declares a live field must be able to take one.
        if (any_live) CHECK(inspected.config.reconfigure != nullptr);

        // What the schema says the defaults are, the parser accepts; and the
        // parser still refuses what the schema does not list.
        std::string error;
        inspected.config.validate(DefaultsOf(schema), error);
        CHECK(error.empty());
        inspected.config.validate(json{{"no_such_field", 1}}, error);
        CHECK(error.find("no_such_field: unknown field") != std::string::npos);
    }
    // tools-fs, tools-search, tools-bash, tools-python, tools-web, memory,
    // mcp, cost-tracker, session-titler, project-context, decision-advisor.
    CHECK(described == 11);
}

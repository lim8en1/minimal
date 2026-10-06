// Editing a plugins.d file against its schema (plugin_config_store.hpp), and
// the `minimal-server plugin ...` CLI built on it, end to end.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <sys/wait.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "minimal/common/plugin_config_store.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using nlohmann::json;

namespace {

const json kSchema = json::parse(R"({"file": "plugins.d/sample.json", "fields": [
    {"path": "limit", "type": "integer", "default": 5, "minimum": 1, "maximum": 100,
     "apply": "live"},
    {"path": "mode", "type": "enum", "choices": ["fast", "slow"], "default": "fast",
     "apply": "restart"},
    {"path": "net.timeout_ms", "type": "integer", "default": 1000, "apply": "live"},
    {"path": "net.url", "type": "string", "default": "", "apply": "restart"},
    {"path": "flag", "type": "boolean", "default": true, "apply": "restart"},
    {"path": "ratio", "type": "number", "default": 0.5, "maximum": 1.0, "apply": "restart"},
    {"path": "names", "type": "string_list", "default": [], "apply": "restart"},
    {"path": "key", "type": "string", "default": "", "secret": true, "apply": "restart"}
]})");

const json& Field(const std::string& path) { return *common::SchemaField(kSchema, path); }

}  // namespace

TEST_CASE("dotted paths: set creates, get finds, unset prunes emptied parents",
          "[plugin_config_store]") {
    json config = json::object();
    common::ConfigSet(config, "net.timeout_ms", 30);
    CHECK(config == json{{"net", {{"timeout_ms", 30}}}});
    CHECK(*common::ConfigGet(config, "net.timeout_ms") == 30);
    CHECK(common::ConfigGet(config, "net.url") == nullptr);
    CHECK(common::ConfigGet(config, "net.timeout_ms.deeper") == nullptr);

    CHECK(common::ConfigUnset(config, "net.timeout_ms"));
    CHECK(config == json::object());  // "net" went with its last key
    CHECK_FALSE(common::ConfigUnset(config, "net.timeout_ms"));

    json scalar = {{"net", 1}};
    CHECK_THROWS_AS(common::ConfigSet(scalar, "net.timeout_ms", 2), std::invalid_argument);
}

TEST_CASE("command-line text parses by the field's type, with the schema's limits",
          "[plugin_config_store]") {
    CHECK(common::ParseFieldValue(Field("limit"), "7") == 7);
    CHECK_THROWS_WITH(common::ParseFieldValue(Field("limit"), "0"),
                      "limit: must be at least 1, got \"0\"");
    CHECK_THROWS_WITH(common::ParseFieldValue(Field("limit"), "7x"),
                      "limit: must be an integer, got \"7x\"");
    CHECK(common::ParseFieldValue(Field("mode"), "slow") == "slow");
    CHECK_THROWS(common::ParseFieldValue(Field("mode"), "medium"));
    CHECK(common::ParseFieldValue(Field("flag"), "false") == false);
    CHECK_THROWS(common::ParseFieldValue(Field("flag"), "no"));
    CHECK(common::ParseFieldValue(Field("ratio"), "0.25") == 0.25);
    CHECK_THROWS(common::ParseFieldValue(Field("ratio"), "2"));
    CHECK(common::ParseFieldValue(Field("net.url"), "http://x?a=1") == "http://x?a=1");
    CHECK(common::ParseFieldValue(Field("names"), R"(["a","b"])") == json::array({"a", "b"}));
    CHECK_THROWS(common::ParseFieldValue(Field("names"), "a,b"));
}

TEST_CASE("the effective config is the file over the defaults", "[plugin_config_store]") {
    const auto rows = common::EffectiveConfig(kSchema, json{{"mode", "slow"}});
    REQUIRE(rows.size() == 8);
    CHECK(rows[0].path == "limit");
    CHECK(rows[0].value == 5);
    CHECK_FALSE(rows[0].overridden);
    CHECK(rows[1].value == "slow");
    CHECK(rows[1].overridden);
}

TEST_CASE("a change is live only when every changed field is", "[plugin_config_store]") {
    const json before = {{"mode", "fast"}};
    auto change = common::DiffConfig(kSchema, before, json{{"mode", "fast"}, {"limit", 9}});
    CHECK(change.paths == std::vector<std::string>{"limit"});
    CHECK(change.all_live);

    change = common::DiffConfig(kSchema, before, json{{"mode", "slow"}, {"limit", 9}});
    CHECK(change.paths.size() == 2);
    CHECK_FALSE(change.all_live);

    // Writing a default explicitly changes nothing.
    change = common::DiffConfig(kSchema, json::object(), json{{"limit", 5}});
    CHECK(change.paths.empty());

    // A key the schema does not know is never live.
    change = common::DiffConfig(kSchema, json::object(), json{{"stray", 1}});
    CHECK(change.paths == std::vector<std::string>{"stray"});
    CHECK_FALSE(change.all_live);
}

TEST_CASE("a secret's literal is masked; a reference is shown", "[plugin_config_store]") {
    CHECK(common::DisplayValue(Field("key"), "sk-live-123") == "\"****\"");
    CHECK(common::DisplayValue(Field("key"), "secret:openai") == "\"secret:openai\"");
    CHECK(common::DisplayValue(Field("key"), "env:KEY") == "\"env:KEY\"");
    CHECK(common::DisplayValue(Field("limit"), 5) == "5");
}

TEST_CASE("the file: missing reads as {}, an empty config removes it", "[plugin_config_store]") {
    TempDir dir;
    const auto path = dir.path() / "plugins.d" / "sample.json";
    CHECK(common::ReadPluginConfigFile(path, "plugins.d/sample.json") == json::object());

    CHECK(common::WritePluginConfigFile(path, json{{"limit", 9}}).empty());  // creates the dir
    CHECK(common::ReadPluginConfigFile(path, "plugins.d/sample.json") == json{{"limit", 9}});
    CHECK(common::WritePluginConfigFile(path, json::object()).empty());
    CHECK_FALSE(std::filesystem::exists(path));

    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "[1]";
    CHECK_THROWS_WITH(common::ReadPluginConfigFile(path, "plugins.d/sample.json"),
                      "plugins.d/sample.json: must be a JSON object");
}

// --- the CLI, through the real server binary ---------------------------------

namespace {

struct CliResult {
    int code = -1;
    std::string output;
};

CliResult RunCli(const std::filesystem::path& config_dir, const std::string& args) {
    const std::string command = "MINIMAL_CONFIG_DIR='" + config_dir.string() + "' '" +
                                MINIMAL_SPAWN_TEST_SERVER + "' plugin " + args + " 2>&1";
    CliResult result;
    FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buffer[4096];
    while (const std::size_t n = std::fread(buffer, 1, sizeof(buffer), pipe)) {
        result.output.append(buffer, n);
    }
    const int status = ::pclose(pipe);
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

}  // namespace

TEST_CASE("minimal-server plugin: set, get, unset, and refusals", "[plugin_cli]") {
    TempDir dir;
    const auto file = dir.path() / "plugins.d" / "tools-web.json";

    auto result = RunCli(dir.path(), "set tools-web fetch.timeout_ms 30000");
    INFO(result.output);
    CHECK(result.code == 0);
    CHECK(result.output.find("live") != std::string::npos);
    CHECK(common::ReadPluginConfigFile(file, "x") == json{{"fetch", {{"timeout_ms", 30000}}}});

    result = RunCli(dir.path(), "set tools-web search.api_url 'http://localhost:8888/search?format=json'");
    CHECK(result.code == 0);
    CHECK(result.output.find("restart") != std::string::npos);

    result = RunCli(dir.path(), "get tools-web search.api_url");
    CHECK(result.code == 0);
    CHECK(result.output == "http://localhost:8888/search?format=json\n");

    result = RunCli(dir.path(), "get tools-web fetch.max_redirects");  // a default
    CHECK(result.output == "5\n");

    // The plugin's own parser refuses; the file is untouched.
    result = RunCli(dir.path(), "set tools-web fetch.timeout_ms 0");
    CHECK(result.code == 1);
    CHECK(result.output.find("must be at least 1") != std::string::npos);
    result = RunCli(dir.path(), "set tools-web fetch.timeot 5");
    CHECK(result.code == 1);
    CHECK(result.output.find("unknown key 'fetch.timeot'") != std::string::npos);
    // Cross-field rules live in the parser, and still apply.
    result = RunCli(dir.path(), "set tools-web fetch.max_bytes 1");
    CHECK(result.code == 1);
    CHECK(result.output.find("must not be below fetch.default_max_bytes") != std::string::npos);

    result = RunCli(dir.path(), "unset tools-web fetch.timeout_ms");
    CHECK(result.code == 0);
    result = RunCli(dir.path(), "unset tools-web search.api_url");
    CHECK(result.code == 0);
    CHECK_FALSE(std::filesystem::exists(file));  // nothing configured: no file

    result = RunCli(dir.path(), "tool-ask");
    CHECK(result.code == 1);
    CHECK(result.output.find("has no configuration") != std::string::npos);
    result = RunCli(dir.path(), "set tools-web");
    CHECK(result.code == 2);
}

TEST_CASE("minimal-server plugin --json: list and show are machine-readable", "[plugin_cli]") {
    TempDir dir;
    REQUIRE(RunCli(dir.path(), "set tools-bash shell /bin/zsh").code == 0);

    auto result = RunCli(dir.path(), "--json");
    REQUIRE(result.code == 0);
    const json list = json::parse(result.output);
    bool found = false;
    for (const auto& row : list) {
        if (row.at("name") != "tools-bash") continue;
        found = true;
        CHECK(row.at("set") == 1);
        CHECK(row.at("configurable") == true);
    }
    CHECK(found);

    result = RunCli(dir.path(), "tools-bash --json");
    REQUIRE(result.code == 0);
    const json show = json::parse(result.output);
    CHECK(show.at("file") == "plugins.d/tools-bash.json");
    CHECK(show.at("effective").at("shell") == "/bin/zsh");
    CHECK(show.at("effective").at("timeout_s") == 120);
    CHECK(show.at("schema").at("fields").size() == show.at("effective").size());
}

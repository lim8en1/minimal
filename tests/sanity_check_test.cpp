#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "minimal/common/config_paths.hpp"
#include "commands_core_fixture.hpp"
#include "minimal/config/sanity_check.hpp"
#include "minimal/plugins/name_resolution.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::testing;
using config::CheckResult;
using config::CheckSeverity;
using config::StartupSanityCheck;

namespace {

// RAII env override (same convention as the other config tests): DataDir() is
// resolved when the check RUNS, so scoping it per test keeps every database the
// check opens inside the temp dir.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_old_ = true;
            old_ = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string old_;
    bool had_old_ = false;
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

// A configuration that passes every check: the baseline each broken fixture is
// one mutation away from.
void WriteGoodConfig(const std::filesystem::path& dir) {
    WriteFile(dir / "settings.json", R"({"default_agent_preset":"build"})");
    WriteFile(dir / "permissions.json", R"({})");
    WriteFile(dir / "providers.json",
              R"({"providers":{"fixture-main":{"backend":"fixture",)"
              R"("url":"http://localhost:1/v1","models":{"test-model":{}}}}})");
    WriteFile(dir / "agents/build.md",
              "---\nname: build\nmodel: fixture-main/test-model\nallowed_tools:\n  - bash\n---\n"
              "You are a test preset.\n");
    WriteFile(dir / "prompts/compression.md", "# compression\nSummarize the earlier messages.\n");
}

struct Fixture {
    TempDir tmp;
    std::filesystem::path config_dir = tmp.path() / "config";
    // XDG_DATA_HOME itself, and what DataDir() resolves to beneath it -- the
    // check under test calls common::DataDir(), so the fixture must mirror its
    // resolution rather than pretend to be it.
    std::filesystem::path xdg_data = tmp.path() / "xdg";
    std::filesystem::path data_dir = xdg_data / "minimal";
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_factories;
    config::SkillRegistry skills;
    plugins::ServiceRegistry service_registry;

    Fixture() {
        std::filesystem::create_directories(config_dir);
        std::filesystem::create_directories(data_dir);
        provider_factories.Register("fixture", [](const llm::ProviderConfig&) {
            return std::make_unique<FakeLlmProvider>();
        });
    }

    void Write(const std::string& rel, const std::string& content) { WriteFile(config_dir / rel, content); }

    // One tool registration under a plugin attribution -- the shape the loader
    // produces, which is what collision detection and permission keys feed on.
    void RegisterTool(const std::string& plugin, const std::string& name,
                      tools::ToolCategory category = tools::ToolCategory::Execute) {
        tool_registry.SetAttribution(plugin);
        tool_registry.Register(std::make_unique<FakeTool>(name, category));
        tool_registry.ClearAttribution();
    }

    // One service registration under a plugin attribution -- the shape the
    // loader produces for minimal_plugin_register_services.
    void ProvideService(const std::string& plugin, const std::string& name) {
        static int dummy;  // identity only; nothing calls through it here
        service_registry.SetAttribution(plugin);
        service_registry.Provide(name, &dummy);
        service_registry.ClearAttribution();
    }

    config::SanityCheckInputs Inputs(const std::vector<plugins::PluginRecord>& records,
                                     const std::map<std::string, std::string>& aliases,
                                     const commands::CommandRegistry* command_registry,
                                     const tools::PermissionAdvisorRegistry* advisors = nullptr,
                                     const plugins::ServiceRegistry* services = nullptr) {
        // ApplyAliases runs after ALL registration, as it does in main.
        tool_registry.ApplyAliases(aliases);
        auto resolution =
            plugins::ResolveRegistrationNames(tool_registry, aliases, command_registry, {},
                                              services);
        plugins::PluginConfig plugin_config;
        plugin_config.command_aliases = aliases;
        return config::SanityCheckInputs{config_dir,         records, tool_registry,
                                         provider_factories, skills,  plugin_config,
                                         resolution,         command_registry, advisors,
                                         services};
    }
};

std::vector<CheckResult> RunChecks(Fixture& f,
                                   const std::vector<plugins::PluginRecord>& records = {},
                                   const std::map<std::string, std::string>& aliases = {},
                                   const commands::CommandRegistry* commands = nullptr,
                                   const tools::PermissionAdvisorRegistry* advisors = nullptr) {
    // The fixture's service registry is passed always: it is empty unless a
    // test populates it, which is also the shape main() sees for every other
    // registry.
    return StartupSanityCheck(f.Inputs(records, aliases, commands, advisors,
                                       &f.service_registry)).RunAll();
}

std::vector<const CheckResult*> ByCheck(const std::vector<CheckResult>& results, const std::string& name) {
    std::vector<const CheckResult*> out;
    for (const auto& result : results) {
        if (result.check_name == name) out.push_back(&result);
    }
    return out;
}

bool HasFatal(const std::vector<CheckResult>& results) {
    return std::any_of(results.begin(), results.end(),
                       [](const CheckResult& r) { return r.severity == CheckSeverity::Fatal; });
}

plugins::PluginRecord Record(std::filesystem::path file, std::string name, plugins::Provenance provenance,
                             plugins::LoadStatus status, std::string error = {}) {
    plugins::PluginRecord record;
    record.file = std::move(file);
    record.name = std::move(name);
    record.version = "1.0";
    record.abi_tag = "tag";
    record.provenance = provenance;
    record.status = status;
    record.error = std::move(error);
    return record;
}

}  // namespace

// --- Baseline -------------------------------------------------------------------

TEST_CASE("a good configuration passes every check", "[sanity_check]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");

    const auto results = RunChecks(f);

    for (const auto& result : results) {
        FAIL("unexpected check result: " << result.check_name << ": " << result.message);
    }
    CHECK(results.empty());
}

// --- config_parse ----------------------------------------------------------------

TEST_CASE("each core config file must parse, and a broken one is fatal", "[sanity_check][config_parse]") {
    for (const std::string& file : {"settings.json", "permissions.json", "providers.json"}) {
        Fixture f;
        WriteGoodConfig(f.config_dir);
        ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
        f.Write(file, "{ not json");

        const auto results = RunChecks(f);
        const auto hits = ByCheck(results, "config_parse");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->severity == CheckSeverity::Fatal);
        CHECK(hits[0]->message.find(file) != std::string::npos);
    }
}

TEST_CASE("a malformed plugins.json warns but does not block startup", "[sanity_check][config_parse]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("plugins.json", "{ not json");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "config_parse");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK_FALSE(HasFatal(results));
}

TEST_CASE("a malformed mcps.json warns and disables all MCP servers", "[sanity_check][config_parse]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("mcps.json", "{ not json");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "config_parse");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("mcps.json") != std::string::npos);
    CHECK_FALSE(HasFatal(results));
}

// --- usable_provider ---------------------------------------------------------------

TEST_CASE("no usable provider entry is fatal; a bad kind alone is a warning", "[sanity_check][usable_provider]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    // The only entry names a kind no loaded plugin registers.
    f.Write("providers.json",
            R"({"providers":{"ghost":{"backend":"anthropic",)"
            R"("url":"http://localhost:1/v1","models":{"m":{}}}}})");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "usable_provider");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0]->severity == CheckSeverity::Warning);   // names the entry and the kind
    CHECK(hits[0]->message.find("anthropic") != std::string::npos);
    CHECK(hits[1]->severity == CheckSeverity::Fatal);     // nothing usable remains
}

TEST_CASE("a default_model naming no configured model warns", "[sanity_check][usable_provider]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("settings.json", R"({"default_agent_preset":"build","default_model":"ghost/nope"})");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "usable_provider");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("ghost") != std::string::npos);
}

TEST_CASE("two plugins registering one provider kind warns", "[sanity_check][provider_kinds]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());

    std::vector<plugins::PluginRecord> records = {
        Record("a.so", "plugin-a", plugins::Provenance::User, plugins::LoadStatus::Loaded),
        Record("b.so", "plugin-b", plugins::Provenance::User, plugins::LoadStatus::Loaded)};
    records[0].registered_provider_kinds = {"fixture"};
    records[1].registered_provider_kinds = {"fixture"};

    const auto results = RunChecks(f, records);
    const auto hits = ByCheck(results, "provider_kinds");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("plugin-a") != std::string::npos);
    CHECK(hits[0]->message.find("plugin-b") != std::string::npos);
}

// --- presets -----------------------------------------------------------------------

TEST_CASE("a cold start with no presets is fatal", "[sanity_check][presets]") {
    Fixture f;
    // Everything except agents/: the bare minimum a fresh install has.
    f.Write("settings.json", R"({})");
    f.Write("permissions.json", R"({})");
    f.Write("providers.json",
            R"({"providers":{"fixture-main":{"backend":"fixture",)"
            R"("url":"http://localhost:1/v1","models":{"m":{}}}}})");
    f.Write("prompts/compression.md", "# compression\nSummarize.\n");
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "presets");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Fatal);
    CHECK(hits[0]->message.find("no agent preset") != std::string::npos);
}

TEST_CASE("the default preset must resolve end to end", "[sanity_check][presets]") {
    // Missing file.
    {
        Fixture f;
        WriteGoodConfig(f.config_dir);
        ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
        f.Write("settings.json", R"({"default_agent_preset":"ghost"})");

        const auto results = RunChecks(f);
        const auto hits = ByCheck(results, "presets");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->severity == CheckSeverity::Fatal);
        CHECK(hits[0]->message.find("ghost") != std::string::npos);
    }
    // Present, but its provider chain is broken.
    {
        Fixture f;
        WriteGoodConfig(f.config_dir);
        ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
        f.Write("agents/build.md", "---\nname: build\nmodel: no-such/model\n---\nBody.\n");

        const auto results = RunChecks(f);
        const auto hits = ByCheck(results, "presets");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->severity == CheckSeverity::Fatal);
        CHECK(hits[0]->message.find("no-such/model") != std::string::npos);
    }
}

TEST_CASE("a broken non-default preset warns", "[sanity_check][presets]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    // A malformed model reference. `provider` used to be required and its
    // absence was the break here; a preset may now legally omit `model` and take
    // settings.default_model, so the broken case is one that names a model
    // WRONGLY rather than not at all.
    f.Write("agents/broken.md", "---\nname: broken\nmodel: no-slash-here\n---\nBody.\n");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "presets");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("broken.md") != std::string::npos);
    CHECK_FALSE(HasFatal(results));
}

TEST_CASE("an absent compression prompt is loud", "[sanity_check][presets]") {
    // The default prompt: absence degrades every session's compaction silently.
    {
        Fixture f;
        WriteGoodConfig(f.config_dir);
        ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
        std::filesystem::remove(f.config_dir / "prompts" / "compression.md");

        const auto results = RunChecks(f);
        const auto hits = ByCheck(results, "presets");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->severity == CheckSeverity::Warning);
        CHECK(hits[0]->message.find("compression.md") != std::string::npos);
    }
    // A preset override pointing at a missing file. The frontmatter value is
    // the stem -- ".md" is appended by the resolver (see ResolveUncached).
    {
        Fixture f;
        WriteGoodConfig(f.config_dir);
        ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
        f.Write("agents/build.md",
                "---\nname: build\nmodel: fixture-main/test-model\ncompression_prompt: custom\n---\nBody.\n");

        const auto results = RunChecks(f);
        const auto hits = ByCheck(results, "presets");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->message.find("custom.md") != std::string::npos);
    }
}

// --- mcp_servers --------------------------------------------------------------------

TEST_CASE("mcp entries are checked per transport", "[sanity_check][mcp_servers]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("mcps.json",
            R"([{"name":"good-stdio","transport":"stdio","command":"/bin/echo"},)"
            R"({"name":"bad-stdio","transport":"stdio","command":"definitely-not-a-real-mcp-binary"},)"
            R"({"name":"good-http","transport":"http","url":"https://mcp.example.com/sse"},)"
            R"({"name":"bad-http","transport":"http","url":"ftp://nope"}])");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "mcp_servers");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0]->message.find("bad-stdio") != std::string::npos);
    CHECK(hits[1]->message.find("bad-http") != std::string::npos);
    CHECK_FALSE(HasFatal(results));
}

TEST_CASE("disabled mcp entries are not checked", "[sanity_check][mcp_servers]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("mcps.json",
            R"([{"name":"off","transport":"stdio","command":"definitely-not-a-real-mcp-binary",)"
            R"("enabled":false}])");

    CHECK(ByCheck(RunChecks(f), "mcp_servers").empty());
}

// --- directories / session_db ---------------------------------------------------------

TEST_CASE("an unwritable data dir is fatal", "[sanity_check][directories]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    // POSIX stat/chmod: this libc++ ships no std::filesystem::permissions query
    // overload, only the setters.
    struct stat st;
    REQUIRE(::stat(f.data_dir.c_str(), &st) == 0);
    const auto previous_mode = st.st_mode;
    REQUIRE(::chmod(f.data_dir.c_str(), 0555) == 0);  // drop owner write

    const auto results = RunChecks(f);
    REQUIRE(::chmod(f.data_dir.c_str(), previous_mode) == 0);  // restore before TempDir teardown

    const auto hits = ByCheck(results, "directories");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Fatal);
    CHECK(hits[0]->message.find("data dir") != std::string::npos);
}

TEST_CASE("a database newer than the binary is fatal", "[sanity_check][session_db]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());

    // Hand-craft a database claiming schema version 99 (same pattern as the
    // session-store migration test).
    const auto db_path = f.data_dir / "sessions.db";
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(db_path.string().c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 99;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "session_db");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Fatal);
    CHECK(hits[0]->message.find("99") != std::string::npos);
}

// --- plugins ---------------------------------------------------------------------------

TEST_CASE("bundled plugin failures are fatal, user failures warn", "[sanity_check][plugins]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());

    std::vector<plugins::PluginRecord> records = {
        Record("tools-fs.so", "tools-fs", plugins::Provenance::Bundled, plugins::LoadStatus::OpenFailed,
               "image not found"),
        Record("custom.so", "custom", plugins::Provenance::User, plugins::LoadStatus::AbiMismatch,
               "built for 'x', host is 'y' -- rebuild the plugin"),
        // A deliberately disabled bundled plugin: the documented replacement flow,
        // not a failure.
        Record("tools-bash.so", "tools-bash", plugins::Provenance::Bundled, plugins::LoadStatus::Disabled),
    };

    const auto results = RunChecks(f, records);
    const auto hits = ByCheck(results, "plugins");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0]->severity == CheckSeverity::Fatal);
    CHECK(hits[0]->message.find("tools-fs") != std::string::npos);
    CHECK(hits[1]->severity == CheckSeverity::Warning);
    CHECK(hits[1]->message.find("custom") != std::string::npos);
}

// --- name collisions / aliases -----------------------------------------------------------

TEST_CASE("a tool-name collision is fatal and names both plugins", "[sanity_check][name_collisions]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");
    f.RegisterTool("bash_over_ssh", "bash");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "name_collisions");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Fatal);
    CHECK(hits[0]->message.find("tools-bash") != std::string::npos);
    CHECK(hits[0]->message.find("bash_over_ssh") != std::string::npos);
}

TEST_CASE("an alias resolves the collision before the check runs", "[sanity_check][name_collisions]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");
    f.RegisterTool("bash_over_ssh", "bash");

    const auto results = RunChecks(f, {}, {{"bash_over_ssh.bash", "ssh_bash"}});
    CHECK(ByCheck(results, "name_collisions").empty());
    CHECK_FALSE(HasFatal(results));
    // Both implementations are alive under their surface names.
    CHECK(f.tool_registry.Find("bash") != nullptr);
    CHECK(f.tool_registry.Find("ssh_bash") != nullptr);
}

TEST_CASE("a service-name collision is fatal and names both plugins, with no alias escape",
          "[sanity_check][name_collisions]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.ProvideService("tools-bash", "tools-bash.session_state");
    f.ProvideService("bash_over_ssh", "tools-bash.session_state");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "name_collisions");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Fatal);
    CHECK(hits[0]->message.find("service name") != std::string::npos);
    CHECK(hits[0]->message.find("tools-bash") != std::string::npos);
    CHECK(hits[0]->message.find("bash_over_ssh") != std::string::npos);
}

TEST_CASE("a service name shared with a tool name is not a collision",
          "[sanity_check][name_collisions]") {
    // Separate namespaces: services are addressed by code through Find, never
    // by the model or the user, so there is nothing to be ambiguous about.
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");
    f.ProvideService("tools-bash", "bash");

    const auto results = RunChecks(f);
    CHECK(ByCheck(results, "name_collisions").empty());
    CHECK_FALSE(HasFatal(results));
}

TEST_CASE("a dangling alias warns", "[sanity_check][aliases]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());

    const auto results = RunChecks(f, {}, {{"no-such-plugin.tool", "x"}});
    const auto hits = ByCheck(results, "aliases");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("no-such-plugin.tool") != std::string::npos);
}

// --- naming model ------------------------------------------------------------------------

TEST_CASE("per_tool_overrides keys that resolve to nothing warn", "[sanity_check][naming_model]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");
    f.Write("permissions.json", R"({"per_tool_overrides":{"edti":"deny"}})");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "naming_model");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("edti") != std::string::npos);
}

TEST_CASE("an ambiguous bare permission key warns", "[sanity_check][naming_model]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    // Two plugins provide "bash", so a bare override on it governs nothing.
    f.RegisterTool("tools-bash", "bash");
    f.RegisterTool("bash_over_ssh", "bash");
    f.Write("permissions.json", R"({"per_tool_overrides":{"bash":"deny"}})");

    // The good-config preset's allowed_tools: [bash] also stops matching once
    // the name is ambiguous -- expect that second warning too.
    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "naming_model");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0]->message.find("ambiguous") != std::string::npos);
    CHECK(hits[1]->message.find("allowed_tools") != std::string::npos);
}

TEST_CASE("unknown allowed_skills entries are reported", "[sanity_check][naming_model]") {
    // The known gap this check closes: resolution silently drops them, which is
    // correct for lookup but must be loud at startup.
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("agents/build.md",
            "---\nname: build\nmodel: fixture-main/test-model\nallowed_skills:\n  - ghost-skill\n---\nBody.\n");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "naming_model");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("ghost-skill") != std::string::npos);
}

TEST_CASE("every not-loaded skill warns with its reason", "[sanity_check][naming_model]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    // Frontmatter without the required `description`.
    f.Write("skills/broken/SKILL.md", "---\nname: broken\n---\nBody.\n");
    f.skills = config::SkillRegistry::Load(f.config_dir);
    // Register bash so the preset's allowed_tools:[bash] resolves and does not
    // produce an unrelated naming_model warning.
    f.RegisterTool("tools-bash", "bash");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "naming_model");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Warning);
    CHECK(hits[0]->message.find("broken") != std::string::npos);
    CHECK(hits[0]->message.find("not loaded") != std::string::npos);
}

TEST_CASE("allowed_mcp_servers naming no configured server warns", "[sanity_check][naming_model]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("agents/build.md",
            "---\nname: build\nmodel: fixture-main/test-model\nallowed_mcp_servers:\n  - ghost-mcp\n---\nBody.\n");

    const auto results = RunChecks(f);
    const auto hits = ByCheck(results, "naming_model");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->message.find("ghost-mcp") != std::string::npos);
}

// --- subagent limit ------------------------------------------------------------------------

TEST_CASE("max_concurrent_subagents out of range warns", "[sanity_check][subagent_limit]") {
    for (const std::string& value : {"9", "0"}) {
        Fixture f;
        WriteGoodConfig(f.config_dir);
        ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
        f.Write("settings.json", R"({"default_agent_preset":"build","max_concurrent_subagents":)" + value + R"(})");

        const auto results = RunChecks(f);
        const auto hits = ByCheck(results, "subagent_limit");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0]->severity == CheckSeverity::Warning);
    }
}

// --- The linter property ---------------------------------------------------------------------

TEST_CASE("independent failures all appear in one RunAll result", "[sanity_check][linter]") {
    // THE property: a misconfigured setup yields one complete diagnostic, not a
    // fix-one-rerun-find-the-next loop. A regression to fail-fast would return at
    // the first Fatal and drop everything after it -- so the assertions below
    // require results from checks that run AFTER the first fatal one fires.
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("settings.json", "{ not json");  // Fatal, first check to fire
    f.Write("mcps.json", R"([{"name":"bad","transport":"stdio","command":"no-such-mcp-binary"}])");

    std::vector<plugins::PluginRecord> records = {
        Record("tools-fs.so", "tools-fs", plugins::Provenance::Bundled, plugins::LoadStatus::OpenFailed,
               "image not found")};  // Fatal, runs after mcp_servers

    const auto results = RunChecks(f, records);

    CHECK(ByCheck(results, "config_parse").size() == 1);
    CHECK(ByCheck(results, "mcp_servers").size() == 1);
    CHECK(ByCheck(results, "plugins").size() == 1);
    REQUIRE(HasFatal(results));
    // The warning survives alongside the fatals -- nothing short-circuited.
    const auto mcp = ByCheck(results, "mcp_servers");
    REQUIRE(mcp.size() == 1);
    CHECK(mcp[0]->severity == CheckSeverity::Warning);
}

TEST_CASE("a colliding COMMAND name is fatal and names both plugins",
          "[sanity_check][name_collisions][invariant]") {
    // CROSS-FEATURE: the sanity check predates the command system, so its
    // collision reporter walked tool attributions only -- a command collision
    // would have been reported with an empty claimant list, naming nobody to
    // disable. Tools and commands are separate namespaces, so the kind is part
    // of the answer.
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());

    commands::CommandRegistry registry;
    registry.SetAttribution("plugin-a");
    registry.Register(std::make_unique<testing::ThrowingCommand>());
    registry.ClearAttribution();
    registry.SetAttribution("plugin-b");
    registry.Register(std::make_unique<testing::ThrowingCommand>());
    registry.ClearAttribution();

    const auto results = RunChecks(f, {}, {}, &registry);
    const auto hits = ByCheck(results, "name_collisions");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0]->severity == CheckSeverity::Fatal);
    INFO(hits[0]->message);
    CHECK(hits[0]->message.find("command name '/boom'") != std::string::npos);
    CHECK(hits[0]->message.find("plugin-a") != std::string::npos);
    CHECK(hits[0]->message.find("plugin-b") != std::string::npos);
}

TEST_CASE("a command_aliases key naming a COMMAND is not reported dangling",
          "[sanity_check][aliases][invariant]") {
    // The routing in ResolveRegistrationNames. Applying every alias as a Tool
    // alias -- as it did when commands did not exist -- makes a perfectly good
    // command alias look like a typo, which is a diagnostic pointing at nothing.
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.Write("plugins.json",
            R"({"plugins":[],"command_aliases":{"plugin-a.boom":"kaboom"}})");

    commands::CommandRegistry registry;
    registry.SetAliases({{"plugin-a.boom", "kaboom"}});
    registry.SetAttribution("plugin-a");
    registry.Register(std::make_unique<testing::ThrowingCommand>());
    registry.ClearAttribution();

    const auto results = RunChecks(f, {}, {{"plugin-a.boom", "kaboom"}}, &registry);
    CHECK(ByCheck(results, "aliases").empty());
    CHECK(ByCheck(results, "name_collisions").empty());
    CHECK(registry.Find("kaboom") != nullptr);
}

// --- advisor ---------------------------------------------------------------------

namespace {

// Minimal answering advisor for the registry-resolution cases.
class StubAdvisor : public tools::PermissionAdvisor {
public:
    std::optional<tools::PermissionResponse> Advise(const tools::PermissionRequest&,
                                                    const tools::AdvisorContext&) override {
        return std::nullopt;  // abstain: these tests never run a tool
    }
};

}  // namespace

TEST_CASE("an advisor naming no loaded plugin warns", "[sanity_check][advisor]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");
    f.Write("permissions.json", R"({"advisor": "auto-approve"})");

    const auto results = RunChecks(f);

    const auto advisor = ByCheck(results, "advisor");
    REQUIRE(advisor.size() == 1);
    CHECK(advisor.front()->severity == CheckSeverity::Warning);
    CHECK(advisor.front()->message.find("auto-approve") != std::string::npos);
}

TEST_CASE("a configured advisor that resolves produces no finding", "[sanity_check][advisor]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");
    f.Write("permissions.json", R"({"advisor": "auto-approve"})");

    tools::PermissionAdvisorRegistry advisors;
    advisors.SetAttribution("auto-approve");
    advisors.Register(std::make_shared<StubAdvisor>());
    advisors.ClearAttribution();

    const auto results = RunChecks(f, {}, {}, nullptr, &advisors);
    CHECK(ByCheck(results, "advisor").empty());
}

TEST_CASE("advisor_inherit without an advisor is inert and warned", "[sanity_check][advisor]") {
    Fixture f;
    WriteGoodConfig(f.config_dir);
    ScopedEnv data("XDG_DATA_HOME", f.xdg_data.string().c_str());
    f.RegisterTool("tools-bash", "bash");
    f.Write("permissions.json", R"({"advisor_inherit": true})");

    const auto results = RunChecks(f);

    const auto advisor = ByCheck(results, "advisor");
    REQUIRE(advisor.size() == 1);
    CHECK(advisor.front()->severity == CheckSeverity::Warning);
    CHECK(advisor.front()->message.find("advisor_inherit") != std::string::npos);
}

// minimal-server entry point.
//
// Startup ordering is load-bearing and must not be rearranged casually:
//   config load -> plugin load -> sanity check -> serve
// Providers and tools are plugin-registered, so "is there at least one usable
// provider?" cannot be answered before plugins are in. See ARCHITECTURE.md.
//
// The sanity check is still absent; everything else is wired.

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/skill_tool.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace {

std::string ReadFileOrEmpty(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (out) out << contents;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path socket_path = minimal::rpc::DefaultSocketPath();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("minimal-server %s\n", minimal::common::Version());
            return 0;
        }
    }

    const auto config_dir = minimal::common::ConfigDir();

    std::printf("minimal-server %s\n", minimal::common::Version());
    std::printf("  abi tag:    %s\n", minimal::common::AbiTag());
    std::printf("  config dir: %s\n", config_dir.c_str());
    std::printf("  data dir:   %s\n", minimal::common::DataDir().c_str());

    // --- Stage 1: config -----------------------------------------------------
    // Nothing is seeded here. `cmake --install` populates the config dir (see
    // cmake/SeedConfig.cmake); by the time the server runs, those files are the
    // user's and this process only reads them.
    const auto plugins_json = config_dir / "plugins.json";
    auto plugin_config = minimal::plugins::ParsePluginConfig(ReadFileOrEmpty(plugins_json));

    // Frontmatter-only scan; bodies stay on disk until a skill is invoked.
    const auto skills = minimal::config::SkillRegistry::Load(config_dir);

    // --- Stage 2: plugins ----------------------------------------------------
    const auto bundled_dir = minimal::plugins::BundledPluginDir();
    const auto user_dir = config_dir / "plugins";
    std::printf("  plugin dir: %s\n\n", bundled_dir.c_str());

    auto candidates =
        minimal::plugins::DiscoverPlugins(bundled_dir, user_dir, plugin_config);

    // Persist anything newly discovered, so its enablement is user-editable.
    WriteFile(plugins_json, minimal::plugins::SerializePluginConfig(plugin_config));

    minimal::tools::ToolRegistry tools;
    minimal::llm::ProviderFactoryRegistry providers;
    minimal::agent::SessionObserverRegistry observers(minimal::common::DefaultLogger());

    // The tools slice of settings.json is forwarded verbatim to every plugin;
    // core imposes no schema on it. Loaded defensively because a missing or
    // malformed settings.json must not stop plugins from loading -- the startup
    // sanity check is what will report it.
    nlohmann::json tools_settings = nlohmann::json::object();
    try {
        tools_settings =
            nlohmann::json::parse(ReadFileOrEmpty(config_dir / "settings.json")).value("tools",
                                                                                       nlohmann::json::object());
    } catch (const std::exception&) {
    }

    minimal::plugins::PluginLoader loader(tools, providers, tools_settings, &observers);
    const auto records = loader.LoadAll(candidates);

    std::printf("plugins (%zu discovered):\n", records.size());
    for (const auto& record : records) {
        std::printf("  %-18s %-12s %s\n",
                    record.name.empty() ? record.file.filename().c_str() : record.name.c_str(),
                    std::string(minimal::plugins::ToString(record.status)).c_str(),
                    record.error.c_str());
        for (const auto& tool : record.registered_tools) {
            std::printf("      tool     %s\n", tool.c_str());
        }
        for (const auto& kind : record.registered_provider_kinds) {
            std::printf("      provider %s\n", kind.c_str());
        }
        if (record.registered_session_observer) {
            std::printf("      observer session lifecycle\n");
        }
    }

    // Core tools are registered by the harness itself, not by a plugin: the
    // empty attribution marks them as built-ins for collision reporting.
    tools.Register(std::make_unique<minimal::tools::SkillTool>(skills));

    std::printf("skills (%zu):\n", skills.All().size());
    // An empty registry is normal; an unreadable skills/ is not, and the two are
    // otherwise indistinguishable from here.
    if (!skills.scan_error().empty()) {
        std::printf("  WARNING: %s\n", skills.scan_error().c_str());
    }
    for (const auto& skill : skills.All()) {
        if (skill.loaded) {
            std::printf("  %-16s %s\n", skill.name.c_str(), skill.description.c_str());
        } else {
            std::printf("  %-16s NOT LOADED: %s\n", skill.name.c_str(), skill.load_error.c_str());
        }
    }

    std::printf("\nregistered: %zu tool(s), %zu provider kind(s)\n", tools.size(),
                providers.kinds().size());

    // TODO: stage 3 sanity check (bundled plugin failures are Fatal, user
    // failures Warning).

    // --- Stage 4: serve ------------------------------------------------------
    // Config that only the serving stage needs is loaded here rather than at
    // stage 1, because the loaders throw on a missing file and plugin loading
    // must not be held hostage to a config problem it does not depend on.
    minimal::config::SettingsConfig settings;
    minimal::config::PermissionsConfig permissions;
    std::unique_ptr<minimal::agent::AgentPresetRegistry> presets;
    std::unique_ptr<minimal::session::SessionDatabase> session_db;
    try {
        settings = minimal::config::LoadSettingsConfig(config_dir / "settings.json");
        permissions = minimal::config::LoadPermissionsConfig(config_dir / "permissions.json");
        presets = std::make_unique<minimal::agent::AgentPresetRegistry>(config_dir, skills);
        // Opening migrates, and refuses a database this binary is too old to
        // understand. That is a fatal startup condition, not a degraded mode:
        // running on would silently drop every transcript on the floor.
        session_db = std::make_unique<minimal::session::SessionDatabase>(
            minimal::session::ResolveSessionDbPath(settings.session_db_path));
    } catch (const std::exception& e) {
        // Until the sanity check lands this is the only diagnostic for a broken
        // or absent config, so it names the directory as well as the error.
        std::fprintf(stderr, "\nconfiguration error (%s): %s\n", config_dir.c_str(), e.what());
        return 1;
    }

    minimal::agent::LoopConfig loop_config;
    loop_config.auto_compact = settings.memory.auto_compact;
    loop_config.compaction_trigger = settings.memory.context_compaction_trigger;
    loop_config.compaction_keep_recent_tokens = settings.memory.compaction_keep_recent_tokens;

    minimal::agent::SessionRegistry sessions(*presets, tools, providers, observers, *session_db,
                                             loop_config, minimal::common::DefaultLogger());
    minimal::rpc::SessionServiceImpl service(sessions, *session_db, settings, permissions,
                                             minimal::common::DefaultLogger());
    minimal::rpc::Server server(service, socket_path);

    if (!server.listening()) {
        std::fprintf(stderr, "\ncannot listen on %s (is a server already running?)\n",
                     socket_path.c_str());
        return 1;
    }

    // NO DAEMON: refcounted by attached clients, gone when the last one leaves.
    // That is what lets a client auto-spawn a server without anyone tuning an
    // idle timeout or reaping a stray background process.
    // RequestShutdown, not Shutdown: this fires at the tail of OpenSession, on
    // the handler thread, and the blocking form would wait for that very RPC.
    service.set_on_last_client_detached([&server] { server.RequestShutdown(); });

    std::printf("\nlistening on %s\n", server.target().c_str());
    std::fflush(stdout);
    server.Wait();

    sessions.TerminateAll();
    return 0;
}

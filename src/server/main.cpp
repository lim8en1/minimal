// minimal-server entry point.
//
// Startup ordering is load-bearing and must not be rearranged casually:
//   config load -> plugin load -> sanity check -> serve
// Providers and tools are plugin-registered, so "is there at least one usable
// provider?" cannot be answered before plugins are in. See ARCHITECTURE.md.

#include <algorithm>
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
#include "minimal/agent/spawn_subagent_tool.hpp"
#include "minimal/agent/subagent_manager.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/sanity_check.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/plugins/name_resolution.hpp"
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
    minimal::commands::CommandRegistry commands;
    // Applied at registration, BEFORE the collision check below, so aliasing is
    // itself a way to resolve a collision rather than only a way to avoid one.
    commands.SetAliases({plugin_config.command_aliases.begin(),
                         plugin_config.command_aliases.end()});
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

    minimal::plugins::PluginLoader loader(tools, providers, tools_settings, &observers, &commands);
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
        for (const auto& command : record.registered_commands) {
            std::printf("      command  /%s\n", command.c_str());
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

    // --- Stage 3: sanity check -------------------------------------------------
    // Aliases apply at registration, before collision checking: a tool whose
    // "plugin.original_name" has an entry in plugins.json command_aliases is
    // served under its surface name, and permission keys / allowed_tools keep
    // referencing the non-alias name (ARCHITECTURE.md, "Name collisions and
    // aliases").
    tools.ApplyAliases(plugin_config.command_aliases);
    // Commands alias at REGISTRATION rather than in a second pass: a tool has to
    // be re-keyed and wrapped so spec() advertises its surface name, which cannot
    // happen until every registration is in, whereas a command's Name() IS its
    // surface. Same rule, different amount of machinery to enforce it.
    auto name_resolution =
        minimal::plugins::ResolveRegistrationNames(tools, plugin_config.command_aliases, &commands);
    tools.SetPermissionKeys(name_resolution.permission_keys);

    // A linter, not a fail-fast validator: RunAll() surfaces every problem at
    // once, then a single Fatal result aborts startup. Warnings degrade and
    // continue.
    minimal::config::StartupSanityCheck sanity(minimal::config::SanityCheckInputs{
        config_dir,
        records,
        tools,
        providers,
        skills,
        plugin_config,
        std::move(name_resolution),
        &commands,
    });
    const auto check_results = sanity.RunAll();

    bool any_fatal = false;
    if (check_results.empty()) {
        std::printf("\nsanity check: ok\n");
    } else {
        std::printf("\nsanity check:\n");
        for (const auto& result : check_results) {
            const char* tag =
                result.severity == minimal::config::CheckSeverity::Fatal ? "FATAL  " : "warning";
            std::printf("  %s [%s] %s\n", tag, result.check_name.c_str(), result.message.c_str());
            if (result.severity == minimal::config::CheckSeverity::Fatal) any_fatal = true;
        }
    }
    if (any_fatal) {
        std::fprintf(stderr, "\nstartup aborted: fix the fatal problem(s) above and retry\n");
        return 1;
    }

    // --- Stage 4: serve ------------------------------------------------------
    // Settings, permissions and the session database were already loaded and
    // validated by the sanity check; take them from there rather than re-reading
    // the same files. The preset registry still loads its own copies -- it is
    // constructed from the config dir -- which cannot fail now that parsing is
    // proven; the catch remains defense-in-depth.
    minimal::config::SettingsConfig settings = sanity.settings();
    minimal::config::PermissionsConfig permissions = sanity.permissions();
    std::unique_ptr<minimal::agent::AgentPresetRegistry> presets;
    auto session_db = sanity.take_session_db();
    try {
        presets = std::make_unique<minimal::agent::AgentPresetRegistry>(config_dir, skills);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nconfiguration error (%s): %s\n", config_dir.c_str(), e.what());
        return 1;
    }

    minimal::agent::LoopConfig loop_config;
    loop_config.auto_compact = settings.memory.auto_compact;
    loop_config.compaction_trigger = settings.memory.context_compaction_trigger;
    loop_config.compaction_keep_recent_tokens = settings.memory.compaction_keep_recent_tokens;

    minimal::agent::SessionRegistry sessions(*presets, tools, providers, observers, *session_db,
                                             loop_config, minimal::common::DefaultLogger());
    sessions.set_commands(&commands);

    // Registered by the harness itself, like SkillTool, but only NOW: the
    // manager needs the SessionRegistry to exist, and no session can build a
    // tool set before serving starts, so registering here is still before
    // first use. Core surface by design -- making it a plugin would expose
    // session CREATION across the ABI to relocate a thin wrapper.
    auto subagents = std::make_unique<minimal::agent::SubagentManager>(
        sessions, *session_db, *presets, settings.max_concurrent_subagents,
        minimal::common::DefaultLogger());
    minimal::agent::SubagentManager& subagent_manager = *subagents;
    tools.Register(std::make_unique<minimal::agent::SpawnSubagentTool>(
        [&subagent_manager](const minimal::agent::SubagentRequest& request,
                            const minimal::tools::ToolContext& ctx) {
            return subagent_manager.Spawn(request, ctx);
        }));

    minimal::rpc::SessionServiceImpl service(sessions, *session_db, settings, permissions,
                                             minimal::common::DefaultLogger());
    service.set_commands(&commands);
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

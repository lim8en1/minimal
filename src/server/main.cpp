// minimal-server entry point.
//
// Startup ordering is load-bearing and must not be rearranged casually:
//   config load -> plugin load -> sanity check -> serve
// Providers and tools are plugin-registered, so "is there at least one usable
// provider?" cannot be answered before plugins are in. See ARCHITECTURE.md.

#include <algorithm>
#include <chrono>
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
#include "minimal/common/logging_setup.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/sanity_check.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/plugins/name_resolution.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/atomic_write.hpp"
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

// ATOMIC, and only when the content actually changed.
//
// This is the user's plugin-enablement file and it was rewritten on EVERY start
// with a plain truncating ofstream -- so a crash, a full disk or a killed
// process during that write left it truncated, and the tree already had
// tools::write_file (same-directory temp, fsync, rename, mode preserved, added
// in §11.7) doing this correctly for model-written files. The operator's own
// config deserved at least what a tool call gets.
//
// Skipping the write when nothing changed is the other half: discovery usually
// finds exactly what the file already says, so the common case now touches
// nothing at all.
void WriteConfigIfChanged(const std::filesystem::path& path, const std::string& contents,
                          const std::string& previous) {
    if (contents == previous) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto outcome = minimal::tools::write_file(path, contents);
    if (!outcome.ok) {
        std::fprintf(stderr, "warning: could not update %s: %s\n", path.c_str(),
                     outcome.error.c_str());
    }
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
    const auto plugins_json_before = ReadFileOrEmpty(plugins_json);
    auto plugin_config = minimal::plugins::ParsePluginConfig(plugins_json_before);

    // LOGGING FIRST, before anything logs. DefaultLogger() caches spdlog's
    // default on first use for the process lifetime, so this has to land ahead
    // of it -- and ahead of plugin load, which is the first thing with anything
    // to say. Read here with a tolerant parse rather than from the sanity
    // check's SettingsConfig, because the sanity check runs two stages later and
    // its own findings are among the things that need somewhere to go.
    {
        minimal::common::LoggingOptions logging;
        try {
            const auto settings = minimal::config::LoadSettingsConfig(config_dir / "settings.json");
            logging.level = settings.logging.level;
            logging.path = settings.logging.path;
            logging.max_size_mb = settings.logging.max_size_mb;
            logging.max_files = settings.logging.max_files;
        } catch (const std::exception&) {
            // Malformed settings.json is the sanity check's to report, at stage
            // 3, with a proper message. Console defaults until then.
        }
        if (logging.path.empty()) {
            // The documented home is the DATA dir, beside session_db_path --
            // not the config dir, which is for things the user edits.
            logging.path = (minimal::common::DataDir() / "minimal.log").string();
        }
        if (const auto problem = minimal::common::ConfigureLogging(logging); !problem.empty()) {
            std::fprintf(stderr, "warning: %s\n", problem.c_str());
        }
        std::printf("  log file:   %s\n", logging.path.c_str());
    }

    // Frontmatter-only scan; bodies stay on disk until a skill is invoked.
    const auto skills = minimal::config::SkillRegistry::Load(config_dir);

    // --- Stage 2: plugins ----------------------------------------------------
    const auto bundled_dir = minimal::plugins::BundledPluginDir();
    const auto user_dir = config_dir / "plugins";
    std::printf("  plugin dir: %s\n\n", bundled_dir.c_str());

    auto candidates =
        minimal::plugins::DiscoverPlugins(bundled_dir, user_dir, plugin_config);

    // Persist anything newly discovered, so its enablement is user-editable.
    WriteConfigIfChanged(plugins_json, minimal::plugins::SerializePluginConfig(plugin_config),
                         plugins_json_before);

    minimal::tools::ToolRegistry tools;
    minimal::llm::ProviderFactoryRegistry providers;
    minimal::commands::CommandRegistry commands;
    // Applied at registration, BEFORE the collision check below, so aliasing is
    // itself a way to resolve a collision rather than only a way to avoid one.
    commands.SetAliases({plugin_config.aliases.begin(), plugin_config.aliases.end()});
    minimal::agent::SessionObserverRegistry observers(minimal::common::DefaultLogger());
    // Exclusive: at most one plugin provides the connection policy. An empty
    // registry is the pass-through default -- no authentication, matching the
    // local-only single-user topology.
    minimal::rpc::AuthProviderRegistry auth_providers;
    minimal::agent::MentionResolverRegistry mention_resolvers(minimal::common::DefaultLogger());
    // Additive across plugins: each may contribute one advisor keyed by its
    // name; permissions.json's "advisor" selects one. An empty registry is the
    // default -- every Ask reaches the human.
    minimal::tools::PermissionAdvisorRegistry permission_advisors;
    // Additive across plugins, keyed by the provider kind string: a backend can
    // be asked to LIST its models ("discover": true in providers.json), fetched
    // lazily at first resolve and never written back to the file.
    minimal::llm::ModelCatalogRegistry model_catalogs;
    // The sanctioned cross-plugin channel (pull; SessionObserver is push).
    // Additive: many plugins may each provide services, and a duplicate name is
    // fatal at startup via the name resolution below. Also handed to every
    // plugin as the consumer side, through PluginRegistrationContext.
    minimal::plugins::ServiceRegistry services;

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

    minimal::plugins::PluginLoader loader(tools, providers, tools_settings, &observers, &commands,
                                          &auth_providers, &permission_advisors, &model_catalogs,
                                          &services, &mention_resolvers);
    const auto records = loader.LoadAll(candidates);
    // The load thread is the only writer; from here the map is read-only, which
    // is what makes unguarded Find from session workers and the command pool
    // safe. A runtime Provide is logged and ignored (the registry's own rule).
    services.Seal();

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
        for (const auto& kind : record.registered_model_catalog_kinds) {
            std::printf("      catalog  %s\n", kind.c_str());
        }
        for (const auto& command : record.registered_commands) {
            std::printf("      command  /%s\n", command.c_str());
        }
        if (record.registered_session_observer) {
            std::printf("      observer session lifecycle\n");
        }
        if (record.registered_auth_provider) {
            std::printf("      auth provider registered\n");
        }
        if (record.registered_mention_resolver) {
            std::printf("      mention resolver\n");
        }
        if (record.registered_permission_advisor) {
            std::printf("      advisor  permission prompts\n");
        }
        for (const auto& service : record.registered_services) {
            std::printf("      service  %s\n", service.c_str());
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
    // "plugin.original_name" has an entry in plugins.json aliases is
    // served under its surface name, and permission keys / allowed_tools keep
    // referencing the non-alias name (ARCHITECTURE.md, "Name collisions and
    // aliases").
    tools.ApplyAliases(plugin_config.aliases);
    // Commands alias at REGISTRATION rather than in a second pass: a tool has to
    // be re-keyed and wrapped so spec() advertises its surface name, which cannot
    // happen until every registration is in, whereas a command's Name() IS its
    // surface. Same rule, different amount of machinery to enforce it.
    auto name_resolution = minimal::plugins::ResolveRegistrationNames(
        tools, plugin_config.aliases, &commands,
        /*core_tool_names=*/{minimal::agent::kSpawnSubagentToolName}, &services);
    // The name above is RESERVED here but the tool object itself registers at
    // serve stage (main registers SpawnSubagentTool only once subagents exist).
    // The reservation is what puts it inside the fatal-collision scan.
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
        &permission_advisors,
        &services,
        &model_catalogs,
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
    // the same files. The preset registry used to load settings.json a THIRD time --
    // main ran the loop on the sanity check's parse while the registry resolved
    // default_model from its own, so the sanity check could validate content
    // the server was not running. It receives the sanity check's parse now;
    // providers.json remains its own to load, and cannot fail
    // now that its parsing is proven -- the catch is defense-in-depth.
    minimal::config::SettingsConfig settings = sanity.settings();
    minimal::config::PermissionsConfig permissions = sanity.permissions();
    std::unique_ptr<minimal::agent::AgentPresetRegistry> presets;
    auto session_db = sanity.take_session_db();
    try {
        presets = std::make_unique<minimal::agent::AgentPresetRegistry>(config_dir, skills,
                                                                        settings);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nconfiguration error (%s): %s\n", config_dir.c_str(), e.what());
        return 1;
    }

    // Inject discovery (track N): providers.json "discover": true entries list
    // their models through the catalog the backend kind names. AFTER plugin
    // load and BEFORE any Resolve() -- presets resolve lazily, so this is the
    // last point at which the hook can be set before first use. The hook never
    // throws: a failed List is cached as a failed discovery and reported
    // through the miss reporting and /providers refresh, not here.
    presets->SetDiscoveryHook(
        [&model_catalogs, &logger = minimal::common::DefaultLogger()](
            const std::string& kind,
            const minimal::llm::ProviderConfig& connection) -> minimal::config::DiscoveryOutcome {
            minimal::config::DiscoveryOutcome outcome;
            auto* catalog = model_catalogs.Find(kind);
            if (catalog == nullptr) return outcome;  // catalog_present stays false
            outcome.catalog_present = true;
            try {
                outcome.models = catalog->List(connection);
                outcome.ok = true;
            } catch (const std::exception& error) {
                outcome.error = error.what();
                logger.warn("model discovery for backend kind '{}' failed: {}", kind, outcome.error);
            }
            return outcome;
        });

    minimal::agent::LoopConfig loop_config;
    loop_config.auto_compact = settings.memory.auto_compact;
    loop_config.compaction_trigger = settings.memory.context_compaction_trigger;
    loop_config.compaction_keep_recent_tokens = settings.memory.compaction_keep_recent_tokens;

    minimal::agent::SessionRegistry sessions(*presets, tools, providers, observers, *session_db,
                                             loop_config, minimal::common::DefaultLogger());
    sessions.set_commands(&commands);

    // The shared MCP server processes: one connection per enabled mcps.json
    // entry, shared across every session. Core, like the MCP client itself.
    // Nothing spawns here -- liveness checks fire when a session whose preset
    // allows a server actually starts (ARCHITECTURE.md, MCP client). A missing
    // mcps.json is the normal no-MCP state, not an error.
    minimal::mcp::McpLimits mcp_limits;
    mcp_limits.connect = std::chrono::milliseconds(settings.tools.mcp.connect_timeout_ms);
    mcp_limits.request = std::chrono::milliseconds(settings.tools.mcp.request_timeout_ms);
    mcp_limits.max_list_pages = settings.tools.mcp.max_list_pages;
    auto mcp_manager = std::make_unique<minimal::mcp::McpClientManager>(
        minimal::config::LoadMcpsConfig(config_dir / "mcps.json"), tools, mcp_limits,
        minimal::common::DefaultLogger());
    minimal::mcp::McpClientManager& mcp = *mcp_manager;

    // Registered by the harness itself, like SkillTool, but only NOW: the
    // manager needs the SessionRegistry to exist, and no session can build a
    // tool set before serving starts, so registering here is still before
    // first use. Core surface by design -- making it a plugin would expose
    // session CREATION across the ABI to relocate a thin wrapper.
    //
    // The tool captures a SHARED_PTR, not the bare reference this used to hold:
    // `tools` is declared before `subagents`, so it is DESTROYED after it, and
    // the registered tool briefly held a reference into a destroyed manager
    // during teardown -- safe only because sessions.TerminateAll() runs first
    // on the normal path, which is exactly the kind of "safe" a reordered
    // return path forgets. Ownership in the capture makes the order not matter.
    //
    // The advisor is resolved ONCE from the config against the loaded plugins;
    // an unset advisor or a name that resolved to nothing (warned by the sanity
    // check) both leave it null, which is the no-advisor default everywhere.
    std::shared_ptr<minimal::tools::PermissionAdvisor> advisor =
        permissions.advisor ? permission_advisors.Find(*permissions.advisor) : nullptr;
    auto subagents = std::make_shared<minimal::agent::SubagentManager>(
        sessions, *session_db, *presets, settings.max_concurrent_subagents,
        minimal::common::DefaultLogger(), &mcp, advisor, permissions.advisor_inherit,
        std::chrono::milliseconds(permissions.advisor_timeout_ms));
    tools.Register(std::make_unique<minimal::agent::SpawnSubagentTool>(
        [subagents](const minimal::agent::SubagentRequest& request,
                    const minimal::tools::ToolContext& ctx) {
            return subagents->Spawn(request, ctx);
        }));

    minimal::rpc::SessionServiceImpl service(sessions, *session_db, settings, permissions,
                                             minimal::common::DefaultLogger(),
                                             &permission_advisors);
    service.set_commands(&commands);
    service.set_auth_providers(&auth_providers);
    service.set_mention_resolvers(&mention_resolvers);
    service.set_mcp_manager(&mcp);
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

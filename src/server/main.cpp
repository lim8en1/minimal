// minimal-server entry point.
//
// Startup ordering is load-bearing and must not be rearranged casually:
//   config load -> plugin load -> sanity check -> serve
// Providers and tools are plugin-registered, so "is there at least one usable
// provider?" cannot be answered before plugins are in. See ARCHITECTURE.md.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <unistd.h>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/agent/session_support.hpp"
#include "minimal/agent/signal_router.hpp"
#include "minimal/agent/signal_service.hpp"
#include "minimal/agent/spawn_subagent_tool.hpp"
#include "minimal/agent/structured_compaction.hpp"
#include "minimal/agent/subagent_manager.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/fd_hygiene.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/logging_setup.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/config_bundle.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/hooks/hook_observer_bridge.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/plugins/name_resolution.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/host_registries.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/plugins/plugin_rpc.hpp"
#include "minimal/plugins/plugin_storage.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/server_housekeeping.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/rpc/session_service.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/startup/sanity_check.hpp"
#include "minimal/tools/atomic_write.hpp"
#include "minimal/tools/skill_tool.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace {

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

// The self-pipe behind SIGINT/SIGTERM (see main): the handler writes a byte,
// the watcher thread turns it into a graceful RequestShutdown. The write end's
// fd lives in a file-scope volatile so the handler needs no process state
// beyond it; both ends are O_CLOEXEC.
int shutdown_pipe_fds[2] = {-1, -1};
volatile sig_atomic_t shutdown_signal_fd = -1;

void OnShutdownSignal(int) {
    const char byte = 1;
    const int fd = shutdown_signal_fd;
    if (fd >= 0) {
        const ssize_t ignored = ::write(fd, &byte, 1);
        (void)ignored;  // a full pipe means a shutdown is already queued
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Process-wide, deliberate, and UNCONDITIONAL: the hook executor and the
    // MCP stdio shim both write payloads into pipes a child may close without
    // reading. The shim used to install this lazily -- on first stdio-MCP
    // initialization -- and the hook executor implicitly leaned on it, so
    // server survival depended on whether an unrelated subsystem had happened
    // to initialize. Children inherit SIG_IGN (the shim's comment already
    // documents that as the hosted-runtime trade); a script that wants
    // SIGPIPE semantics back can trap it.
    std::signal(SIGPIPE, SIG_IGN);

    // SIGINT/SIGTERM route through the same graceful shutdown the idle reaper
    // uses (RequestShutdown), so `kill` and Ctrl+C run TerminateAll, the
    // session_ended hooks and a clean database close instead of skipping all
    // of them. The handler itself does the only async-signal-safe thing --
    // write one byte to a self-pipe; a watcher thread reads it and drives the
    // shutdown machinery, which takes locks and must never run IN a handler.
    // (Caught dispositions reset to default on exec, so children are
    // unaffected -- unlike a process-wide blocked mask, which they would
    // inherit.) The pipe is close-on-exec for the same reason.
    if (::pipe(shutdown_pipe_fds) != 0) {
        std::perror("pipe");
        return 1;
    }
    for (int fd : shutdown_pipe_fds) {
        ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
    // Assigned BEFORE the handlers are installed: OnShutdownSignal reads
    // shutdown_signal_fd, and a signal delivered between installing the
    // handler and this assignment would see it still at its initial -1 --
    // `if (fd >= 0)` then silently drops the shutdown byte, so the
    // operator's first Ctrl+C/kill appears to do nothing until a second one
    // arrives after the assignment.
    shutdown_signal_fd = shutdown_pipe_fds[1];
    std::signal(SIGINT, OnShutdownSignal);
    std::signal(SIGTERM, OnShutdownSignal);

    std::filesystem::path socket_path = minimal::rpc::DefaultSocketPath();
    bool refresh_cost_rates = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (std::strcmp(argv[i], "--refresh-cost-rates") == 0) {
            refresh_cost_rates = true;
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
    // ReadFileOrEmpty can throw (an oversize or unreadable file) ahead of the
    // first try/catch in this function -- uncaught here, that aborts the
    // process with no diagnostic at all, before even the version banner
    // above's config/data dir lines print. Folded into the same malformed-file
    // path ParsePluginConfig already has: both are "plugins.json could not be
    // used as written", and the existing warning plus skip-the-rewrite
    // discipline below is the right response to either.
    minimal::plugins::PluginConfig plugin_config;
    std::string plugins_json_before;
    try {
        plugins_json_before = minimal::common::ReadFileOrEmpty(plugins_json);
        plugin_config = minimal::plugins::ParsePluginConfig(plugins_json_before);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "minimal: warning: cannot read %s: %s\n", plugins_json.c_str(),
                     e.what());
        plugin_config.malformed = true;
    }
    // A malformed file degrades to an empty config; silently rewriting the
    // user's bytes over that default at stage 2 would destroy every enablement
    // decision and per-plugin config in a file they are explicitly invited to
    // edit. Warn now and skip the rewrite below -- the client host applies the
    // same discipline (tui_bootstrap.cpp).
    if (plugin_config.malformed) {
        std::fprintf(stderr,
                     "minimal: warning: %s is malformed -- it was NOT rewritten; fix or "
                     "remove it to enable plugin config management\n",
                     plugins_json.c_str());
    }

    // Read settings.json ONCE, tolerantly, ahead of the sanity check's own
    // authoritative parse two stages later -- both the logging setup below
    // and the tools-slice extraction in stage 2 need pieces of it before then.
    // A malformed file (or one with a schema violation anywhere in it) leaves
    // this nullopt, and both sites fall back to their pre-Phase-3 defaults
    // exactly as if settings.json were absent: the sanity check is what
    // reports the real problem, at stage 3, with a proper message. A process
    // that reaches stage 3 with a bad settings.json aborts there regardless
    // (see the any_fatal check below) -- the narrow window where plugin load
    // (stage 2) sees an empty tools-slice instead of a slice from a
    // file that fails validation elsewhere never reaches serving traffic.
    std::optional<minimal::config::SettingsConfig> settings_preload;
    try {
        settings_preload = minimal::config::LoadSettingsConfig(config_dir / "settings.json");
    } catch (const std::exception&) {
    }

    // LOGGING FIRST, before anything logs -- ahead of plugin load, which is
    // the first thing with anything to say. (DefaultLogger() no longer
    // caches: it resolves spdlog's default per call, so ConfigureLogging's
    // swap is picked up by every later call. Ordering still matters for
    // "nothing logs before the sinks exist", not for caching.)
    {
        minimal::common::LoggingOptions logging;
        if (settings_preload) {
            logging.level = settings_preload->logging.level;
            logging.path = settings_preload->logging.path;
            logging.max_size_mb = settings_preload->logging.max_size_mb;
            logging.max_files = settings_preload->logging.max_files;
        }
        if (logging.path.empty()) {
            // The documented home is the DATA dir, beside session_db_path --
            // not the config dir, which is for things the user edits.
            logging.path = (minimal::common::DataDir() / "minimal.log").string();
        } else if (std::filesystem::path(logging.path).is_relative()) {
            // A relative logging.path resolved against the cwd -- which for
            // an auto-spawned server is "/" (SpawnDetached chdir's there),
            // landing the log at /<name>. Anchor it to the data dir instead.
            logging.path = (minimal::common::DataDir() / logging.path).string();
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
    // Skipped on a malformed source file -- see the warning at stage 1: writing
    // the degraded-to-empty default back would erase the user's actual config.
    if (!plugin_config.malformed) {
        WriteConfigIfChanged(plugins_json, minimal::plugins::SerializePluginConfig(plugin_config),
                             plugins_json_before);
    }

    // Per-plugin persistent KV storage. DECLARED BEFORE every registry that can
    // hold plugin-registered objects (tools, commands, services, ...) on
    // purpose: reverse-order destruction frees later declarations FIRST, so
    // from here it is destroyed LAST -- after the registries' own destructors
    // (flushes, observer drains) are done with anything that references
    // plugin-owned state. Declared after the registries, it was freed before
    // them -- the one teardown ordering the "process lifetime, like the
    // registries" contract got backwards.
    std::unique_ptr<minimal::plugins::PluginStorage> plugin_storage;
    minimal::tools::ToolRegistry tools;
    minimal::llm::ProviderFactoryRegistry providers;
    minimal::commands::CommandRegistry commands;
    // Applied at registration, BEFORE the collision check below, so aliasing is
    // itself a way to resolve a collision rather than only a way to avoid one.
    commands.SetAliases({plugin_config.aliases.begin(), plugin_config.aliases.end()});
    // The hook mechanism (track O). Owns the point registry -- the harness's
    // eleven conversation-path points are registered in its constructor,
    // reserved before any plugin loads -- and, after Configure below, the
    // hooks.json action map. One object, deliberately: registration and
    // dispatch are the two halves of the same seam, and a HookPoint handle a
    // plugin captured at load must reach the dispatch surface without a second
    // indirection.
    //
    // Declared BEFORE `observers` so reverse destruction leaves it standing
    // while the registry drains: the bridge registered below holds a
    // HookDispatcher&, and a pending bridge task must never EmitAsync on an
    // already-destroyed dispatcher.
    minimal::hooks::HookDispatcher hook_dispatcher(minimal::common::DefaultLogger());
    minimal::agent::SessionObserverRegistry observers(minimal::common::DefaultLogger());
    // The observer-to-hook bridge. NOTE on ordering: the old comment claimed
    // "registered FIRST so a user's hook action runs before plugin observers"
    // -- but the bridge converts events onto EmitAsync, which only ENQUEUES
    // onto the dispatcher's own thread, while plugin observers run inline in
    // the registry. No registration order can guarantee script execution
    // lands first; a full hook queue drops the emission entirely. The
    // registration order is therefore merely conventional. It converts
    // session_created/session_ended/post_tool_use/stop onto their EmitAsync
    // points -- the observe-only hook points need no dispatch machinery of
    // their own, by design.
    observers.Register(std::make_shared<minimal::hooks::HookObserverBridge>(hook_dispatcher));
    // Exclusive: at most one plugin provides the connection policy. An empty
    // registry is the pass-through default -- no authentication, matching the
    // local-only single-user topology.
    minimal::rpc::AuthProviderRegistry auth_providers;
    minimal::agent::MentionResolverRegistry mention_resolvers(minimal::common::DefaultLogger());
    // Additive across plugins: sections pre-filled into each session's system
    // context (context_contributor.hpp). Configured from settings.json below,
    // once settings are parsed; asked per session by the SessionRegistry.
    minimal::agent::ContextContributorRegistry context_contributors(
        minimal::common::DefaultLogger());
    // Additive across plugins: each may contribute one advisor keyed by its
    // name; permissions.json's "advisor" selects one. An empty registry is the
    // default -- every Ask reaches the human.
    minimal::tools::PermissionAdvisorRegistry permission_advisors;
    // Additive across plugins, keyed by the provider kind string: a backend can
    // be asked to LIST its models ("discover": true in providers.json), fetched
    // lazily at first resolve and never written back to the file.
    minimal::llm::ModelCatalogRegistry model_catalogs;
    // Enrichment defaults (docs/EXTENSIBILITY.md §2.13): a models.dev-style
    // catalog fills missing context windows, output caps and effort ladders
    // for models the config already addresses -- automatically, no per-entry
    // opt-in; providers.json "metadata": false declines per entry. A second
    // registry beside model_catalogs because that one's kind slots are the
    // listing catalogs' (one catalog per backend kind, first-wins) and an
    // enrichment source serves several backends at once.
    minimal::llm::MetadataCatalogRegistry metadata_catalogs;
    // Additive across plugins, keyed by the SAME provider kind string: the
    // wire-shape a backend wants for one "@effort" variant (relocated out of
    // core -- ARCHITECTURE.md, "Core vs. plugin surface"). An unregistered
    // kind is not degraded state -- it is the ordinary case for every backend
    // that needs no per-kind table of its own (see variant_params.hpp).
    minimal::llm::VariantParamsRegistry variant_params;
    // S4: additive across plugins, keyed by name -- memory.compaction_strategy
    // selects one, with agent-preset frontmatter overriding per preset. Core's
    // own strategies are registered directly here, BEFORE plugin load, so the
    // default path is never plugin-dependent -- a plugin adds a kind core has
    // never heard of (drop-oldest, tool-result-elide, the deferred
    // llamacpp-kv's external-KV strategy), it does not replace these two.
    // "structured" is the settings.json DEFAULT: the summarizer's output is
    // constrained to a forced record_summary tool call parsed into typed
    // sections; "summarize" is the free-text format, reachable globally by
    // settings or per agent by declaring `compaction_strategy: summarize` in
    // the preset's frontmatter.
    minimal::agent::CompactionStrategyRegistry compaction_strategies;
    compaction_strategies.Register("summarize", minimal::agent::MakeSummarizeCompactionStrategy());
    compaction_strategies.Register("structured",
                                   minimal::agent::MakeStructuredCompactionStrategy());
    // The sanctioned cross-plugin channel (pull; SessionObserver is push).
    // Additive: many plugins may each provide services, and a duplicate name is
    // fatal at startup via the name resolution below. Also handed to every
    // plugin as the consumer side, through PluginRegistrationContext.
    minimal::plugins::ServiceRegistry services;

    // Per-plugin persistent KV storage, constructed BEFORE plugin load: the
    // pointer rides PluginRegistrationContext into every registration hook.
    // Declared above, before the registries -- see the destruction-ordering
    // note there. Open failure
    // is fatal -- a plugin store a newer binary wrote must not be silently
    // rebuilt over -- and there is no degraded harness to serve anyway, since
    // bundled plugins may depend on the seam.
    const auto plugin_data_root = minimal::common::DataDir() / "plugins";
    try {
        plugin_storage =
            std::make_unique<minimal::plugins::PluginStorage>(minimal::common::DataDir() /
                                                              "plugins.db");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nplugin storage error: %s\n", e.what());
        return 1;
    }

    // The core-provided support service (the one pull seam a session-lifecycle
    // plugin cannot do without: transcript reads, the session's own provider,
    // and rename-and-notify). Constructed and Provided BEFORE Seal so the
    // startup collision scan sees the name -- a plugin registering
    // "core.session_support" is a diagnosed fatal duplicate, not a silent
    // shadow. Its dependencies are serve-stage objects, so the actual wiring
    // happens in stage 4 via Wire(); nothing can call through the unwired
    // service because the RPC server is not listening yet.
    minimal::agent::SessionSupport session_support(minimal::common::DefaultLogger());
    services.Provide(minimal::agent::kSessionSupportServiceName, &session_support);

    // The second core-provided service: the signal router, Provided as
    // core.signals (the interface is SignalService; the router implements it).
    // Same LATE WIRING shape as session_support: the object exists now (it has
    // no dependencies at construction) so the collision scan sees the name,
    // while its channel registry fills in after the sanity check parses
    // channels.json -- still before serving, hence before any Find(). The
    // bundled message_session tool is the first consumer; any plugin producer
    // (timers, file watchers) would Find the same seam.
    minimal::agent::SignalRouter signal_router;
    services.Provide(minimal::agent::kSignalServiceName, &signal_router);

    // The tools slice of settings.json is forwarded verbatim to every plugin;
    // core imposes no schema on it. Falls back to {} exactly as if
    // settings.json were absent when the stage-1 preload above failed --
    // a missing or malformed settings.json must not stop plugins from
    // loading, the startup sanity check is what will report it.
    const nlohmann::json tools_settings =
        settings_preload ? settings_preload->tools.raw : nlohmann::json::object();

    // Every registry above, handed as one bundle to each consumer: the loader
    // here, and the session registry and RPC service at serve stage.
    const minimal::plugins::HostRegistries registries{
        .tools = &tools,
        .providers = &providers,
        .commands = &commands,
        .observers = &observers,
        .auth_providers = &auth_providers,
        .permission_advisors = &permission_advisors,
        .model_catalogs = &model_catalogs,
        .metadata_catalogs = &metadata_catalogs,
        .variant_params = &variant_params,
        .compaction_strategies = &compaction_strategies,
        .context_contributors = &context_contributors,
        .mention_resolvers = &mention_resolvers,
        .services = &services,
        .hooks = &hook_dispatcher,
    };
    minimal::plugins::PluginLoader loader(registries, tools_settings, plugin_storage.get(),
                                          plugin_data_root);
    const auto records = loader.LoadAll(candidates);
    // The load thread is the only writer; from here the map is read-only, which
    // is what makes unguarded Find from session workers and the command pool
    // safe. A runtime Provide is logged and ignored (the registry's own rule).
    services.Seal();

    // One-shot rates refresh: load plugins exactly as a normal start does, ask
    // cost-tracker's .rpc service to fetch its pricing snapshots, print the
    // outcome, and exit before listening. This is the manual escape hatch the
    // old /cost-update-rates command was (cost-tracker's startup auto-refresh
    // gates itself on the snapshot's age; this flag always fetches). Reaching
    // the plugin through the generic PluginCall channel -- the same sealed
    // registry the RPC transport dispatches through -- keeps core from
    // depending on any bundled plugin: the name is a runtime string, and a
    // disabled or failed cost-tracker is an error, not a link dependency.
    if (refresh_cost_rates) {
        void* raw = services.Find(minimal::plugins::PluginRpcServiceName("cost-tracker"));
        if (raw == nullptr) {
            std::fprintf(stderr, "cost-tracker is not loaded or disabled; nothing to refresh\n");
            return 1;
        }
        // The .rpc convention's handler type (see plugin_rpc.hpp): the name
        // registry guarantees what sits under "cost-tracker.rpc" is one.
        const auto reply = static_cast<minimal::plugins::PluginRpcHandler*>(raw)->Handle(
            "refresh_rates", "{}");
        if (!reply.ok) {
            std::fprintf(stderr, "%s\n", reply.error.c_str());
            return 1;
        }
        std::printf("%s\n", reply.payload.c_str());
        return 0;
        // The exit cascade joins cost-tracker's startup auto-refresh worker
        // (its observer-owner) with plugin storage still alive.
    }

    // The project-scoped half of the /-command catalog (docs/COMMANDS_HOWTO.md
    // §2): unlike `commands` above, this is never populated here -- there is
    // no project root at startup -- only handed the now-fully-loaded `loader`
    // so it can ask each plugin's optional per-root hook later, lazily, once
    // a session names a real one.
    minimal::commands::ProjectCommandSource project_commands(loader);

    std::printf("plugins (%zu discovered):\n", records.size());
    for (const auto& record : records) {
        std::printf("  %-18s %-12s %s\n",
                    record.name.empty() ? record.file.filename().c_str() : record.name.c_str(),
                    std::string(minimal::plugins::ToString(record.status)).c_str(),
                    record.error.c_str());
        for (const auto& [label, names] : record.registered) {
            for (const auto& name : names) {
                std::printf("      %-8s %s\n", label.c_str(), name.c_str());
            }
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
        /*core_tool_names=*/{minimal::agent::kSpawnSubagentToolName}, &services,
        &hook_dispatcher);
    // The name above is RESERVED here but the tool object itself registers at
    // serve stage (main registers SpawnSubagentTool only once subagents exist).
    // The reservation is what puts it inside the fatal-collision scan.
    tools.SetPermissionKeys(name_resolution.permission_keys);

    // Every config file, loaded once; what failed to load is already a
    // problem. Then the linter: every problem surfaces at once, a single Fatal
    // aborts startup, warnings degrade and continue.
    minimal::config::ConfigBundle bundle = minimal::config::LoadConfigBundle(
        config_dir, minimal::config::MakeVariantParamsHook(&variant_params));
    std::vector<minimal::config::CheckResult> check_results = bundle.problems;
    auto session_db = minimal::startup::OpenSessionDatabase(bundle, check_results);
    {
        minimal::startup::StartupSanityCheck sanity(
            bundle, minimal::startup::SanityCheckInputs{config_dir, records, skills, plugin_config,
                                                        std::move(name_resolution), registries});
        auto validation = sanity.RunAll();
        check_results.insert(check_results.end(), std::make_move_iterator(validation.begin()),
                             std::make_move_iterator(validation.end()));
    }

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
    // Everything below takes its config from the bundle rather than re-reading
    // the same files. providers.json is the exception, still: the live,
    // discovery-hooked instance the preset registry builds is not
    // interchangeable with the bundle's un-hooked one (see ProvidersConfig's
    // discovery cache).
    minimal::config::SettingsConfig settings = bundle.settings;
    minimal::config::PermissionsConfig permissions = bundle.permissions;
    std::unique_ptr<minimal::agent::AgentPresetRegistry> presets;
    // Resolved once, after ApplyAliases/SetPermissionKeys above, so preset
    // parsing and permission-default resolution see the live tool vocabulary
    // rather than a hardcoded read/write/edit/bash/glob/grep literal list
    // (ARCHITECTURE.md, "Core vs. plugin surface").
    const minimal::agent::ToolPermissionKeys tool_permission_keys{
        tools.PermissionKeys(), tools.DotenvProtectedPermissionKeys()};
    try {
        presets = std::make_unique<minimal::agent::AgentPresetRegistry>(
            config_dir, skills, settings, permissions, tool_permission_keys, &variant_params);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nconfiguration error (%s): %s\n", config_dir.c_str(), e.what());
        return 1;
    }

    // Inject discovery (track N): providers.json "discover": true entries list
    // their models through the catalog the backend kind names. AFTER plugin
    // load and BEFORE any Resolve() -- presets resolve lazily, so this is the
    // last point at which the hook can be set before first use. The hook never
    // throws: a failed List is cached as a failed discovery and reported
    // through the miss reporting and ListModels(refresh_discovery) -- the
    // client model picker's ctrl+r -- not here.
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
            } catch (...) {
                outcome.error = "unknown error";
                logger.warn("model discovery for backend kind '{}' failed: {}", kind, outcome.error);
            }
            return outcome;
        });

    // Inject metadata enrichment (docs/EXTENSIBILITY.md §2.13), the same
    // AFTER-plugin-load / BEFORE-first-Resolve point the discovery hook uses.
    // The hook consults the metadata registry: the kind's own catalog, else
    // the fallback chain. Every failure inside Lookup is the catalog's own
    // degraded nullopt or a caught throw: an enrichment miss must never fail
    // a resolve.
    presets->SetMetadataHook(
        [&metadata_catalogs, &logger = minimal::common::DefaultLogger()](
            const minimal::llm::MetadataQuery& query, minimal::llm::MetadataMatch match)
            -> std::optional<minimal::llm::DiscoveredModel> {
            auto* catalog = metadata_catalogs.FindForQuery(query);
            if (catalog == nullptr) return std::nullopt;
            try {
                return catalog->Lookup(query, match);
            } catch (const std::exception& error) {
                logger.warn("metadata lookup for '{}' (kind '{}') failed: {}", query.model_id,
                            query.backend, error.what());
                return std::nullopt;
            } catch (...) {
                logger.warn("metadata lookup for '{}' (kind '{}') failed: unknown error",
                            query.model_id, query.backend);
                return std::nullopt;
            }
        });

    // Inject the DISCOVERED-variant params hook (ResolveVariant), the same
    // AFTER-plugin-load / BEFORE-first-Resolve point the two hooks above use.
    // The STATIC "variants": [...] list-form path already resolved its
    // parameters above, during the LoadProvidersConfig call inside
    // AgentPresetRegistry's own constructor -- it needed the registry
    // earlier than any post-construction setter could supply it, so it took
    // the same &variant_params pointer directly as a constructor argument
    // instead (see AgentPresetRegistry's constructor comment).
    presets->SetVariantParamsHook(minimal::config::MakeVariantParamsHook(&variant_params));

    // Preset-scoped tool definitions: the builders run once per resolved
    // preset (inside the registry's resolve cache) and their results ride the
    // frozen preset into AgentLoop::BuildSystemAndTools. Registered HERE
    // rather than inside the registry so the registry stays a config reader
    // and each tool owns its own presentation -- and here rather than later
    // because every builder must exist before the first Resolve(), the same
    // deadline the discovery hook above is under.
    presets->SetDefinitionBuilder(minimal::agent::kSkillToolName,
                                  minimal::agent::AgentPresetRegistry::MakeSkillDefinitionBuilder());
    presets->SetDefinitionBuilder(minimal::agent::kSpawnSubagentToolName,
                                  minimal::agent::MakeSpawnDefinitionBuilder());

    minimal::agent::LoopConfig loop_config;
    loop_config.auto_compact = settings.memory.auto_compact;
    loop_config.compaction_trigger = settings.memory.context_compaction_trigger;
    loop_config.compaction_keep_recent_tokens = settings.memory.compaction_keep_recent_tokens;
    loop_config.compaction_replace_percent = settings.memory.compaction_replace_percent;
    loop_config.offer_summary_tool = true;
    loop_config.max_parallel_tool_calls = settings.tools.max_parallel_tool_calls;
    loop_config.max_tool_input_bytes = settings.tools.max_input_bytes;
    loop_config.max_tool_result_bytes = settings.tools.max_result_bytes;
    loop_config.max_tool_display_bytes = settings.tools.max_display_bytes;
    loop_config.estimate_zero_usage = settings.memory.estimate_zero_usage;
    loop_config.hooks = &hook_dispatcher;

    // S4: resolve memory.compaction_strategy ONCE here and copy the whole
    // strategy object into every session's LoopConfig -- it survives to the
    // consumer, rather than dissolving into two loose std::functions at this
    // producer. An unresolved name (typo, or the providing plugin is
    // disabled -- the sanity check already warned) leaves it default-
    // constructed, which AgentLoop treats as "use the built-in behavior".
    // The registry ALSO reaches every session whole (below): an agent preset's
    // `compaction_strategy:` frontmatter resolves against it per compaction
    // attempt, outranking this settings-level choice for that preset.
    if (const auto* strategy = compaction_strategies.Find(settings.memory.compaction_strategy)) {
        loop_config.compaction_strategy = *strategy;
    }

    // Configure hooks (track O) now that every point -- core's and plugins' --
    // is registered: hooks.json actions resolve onto points, filter fields are
    // validated, and the point map seals (making unguarded Emit from session
    // workers safe). The sanity check validated the same file structurally and
    // reports the same registry-relative findings; its list is what the
    // deduplication below subtracts, so a user sees each warning once. What
    // Configure adds beyond the check is nothing today -- but it OWNS the
    // resolution (the check reads registered points only), so its return value
    // stays the backstop rather than being assumed empty.
    {
        std::unordered_set<std::string> already_reported;
        for (const auto& result : check_results) {
            if (result.check_name == "hooks") already_reported.insert(result.message);
        }
        for (const auto& warning : hook_dispatcher.Configure(bundle.hooks)) {
            if (already_reported.count(warning) != 0) continue;
            std::printf("  warning [hooks] %s\n", warning.c_str());
        }
    }

    // Contributors are configured before the registry exists, so no session
    // can render against unconfigured settings.
    context_contributors.Configure(settings.context_contributors);
    minimal::agent::SessionRegistry sessions(*presets, minimal::rpc::FinishSessionWiring,
                                             registries, *session_db, loop_config,
                                             minimal::common::DefaultLogger());
    sessions.set_project_commands(&project_commands);
    sessions.set_signal_router(&signal_router);
    // Project context (AGENTS.md/CLAUDE.md): each session loads its own, from
    // its own project_root, at creation/resume/spawn -- never at startup, since
    // the server serves many project roots and no file belongs to all of them.
    sessions.set_project_context(settings.project_context);
    if (!settings.small_model.empty()) sessions.set_small_model(settings.small_model);
    sessions.set_prompt_timeout(
        std::chrono::seconds(settings.sessions.unattended_prompt_timeout_seconds));

    // Stage-4 wiring for the core support service Provided above (see the
    // comment there): the dependencies only exist now. Serving has not
    // started, so this is still ahead of any Find().
    session_support.Wire(sessions, *session_db, *presets);

    // The signal router's channel registry, from the startup-parsed
    // channels.json: subscriptions validate against declared channels, not
    // connected ones. Populated below, once the MCP manager exists -- see
    // there for why the enabled/reserved/collision filtering happens through
    // it rather than by re-deriving the same rules here (declared here
    // first, so it outlives the manager's wiring).

    // The shared MCP server processes: one connection per enabled mcps.json
    // entry, shared across every session. Core, like the MCP client itself.
    // Nothing spawns here -- liveness checks fire when a session whose preset
    // allows a server actually starts (ARCHITECTURE.md, MCP client). A missing
    // mcps.json is the normal no-MCP state, not an error. Channel entries
    // (channels.json) join the same map and lifecycle; their notifications
    // route into the router above, their instructions feed session context.
    minimal::mcp::McpLimits mcp_limits;
    mcp_limits.connect = std::chrono::milliseconds(settings.tools.mcp.connect_timeout_ms);
    mcp_limits.request = std::chrono::milliseconds(settings.tools.mcp.request_timeout_ms);
    mcp_limits.max_list_pages = settings.tools.mcp.max_list_pages;
    mcp_limits.restart_cooldown = std::chrono::milliseconds(settings.tools.mcp.restart_cooldown_ms);
    minimal::mcp::ChannelWiring channel_wiring;
    channel_wiring.config = bundle.channels;
    channel_wiring.on_notification = [&signal_router](const std::string& channel,
                                                      const std::string& raw) {
        // Runs on the channel's read thread: the router's contract is
        // parse, look up, inject -- no blocking.
        signal_router.OnMcpNotification(channel, raw);
    };
    channel_wiring.on_connected = [&signal_router](const std::string& channel,
                                                   const std::string& instructions) {
        signal_router.SetChannelInstructions(channel, instructions);
    };
    auto mcp_manager = std::make_unique<minimal::mcp::McpClientManager>(
        bundle.mcps, tools, mcp_limits, minimal::common::DefaultLogger(),
        std::move(channel_wiring));
    minimal::mcp::McpClientManager& mcp = *mcp_manager;

    // Register with the router only the channels the manager actually built
    // a handle for -- asking the manager directly, rather than re-deriving
    // its enabled/reserved/collision rules here, is what keeps this in sync
    // with EnsureAlive's own view: a subscription to anything else would pass
    // CheckSelector and then either fail at EnsureAlive or, worse, silently
    // resolve to an unrelated mcps.json server handle of the same name,
    // producing "subscribed but no signal ever arrives" with no error
    // anywhere. The sanity check already warns about both skip cases from
    // the config side; this is the enforcement.
    {
        const auto enabled_channels = mcp.EnabledChannelNames();
        for (const auto& entry : bundle.channels.entries) {
            if (std::find(enabled_channels.begin(), enabled_channels.end(), entry.name) !=
                enabled_channels.end()) {
                signal_router.RegisterChannel(entry);
            }
        }
    }

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
        settings.default_subagent_model, minimal::common::DefaultLogger(), &mcp, advisor,
        permissions.advisor_inherit,
        std::chrono::milliseconds(permissions.advisor_timeout_ms), &hook_dispatcher);
    tools.Register(std::make_unique<minimal::agent::SpawnSubagentTool>(
        [subagents](const minimal::agent::SubagentRequest& request,
                    const minimal::tools::ToolContext& ctx) {
            return subagents->Spawn(request, ctx);
        }));

    minimal::rpc::SessionServiceImpl service(sessions, *session_db, settings, permissions,
                                             minimal::common::DefaultLogger(), registries);
    service.set_project_commands(&project_commands);
    service.set_mcp_manager(&mcp);
    service.set_presets(presets.get());
    service.set_signal_router(&signal_router);

    minimal::rpc::Server server(service, socket_path);

    if (!server.listening()) {
        std::fprintf(stderr, "\ncannot listen on %s (is a server already running?)\n",
                     socket_path.c_str());
        return 1;
    }

    // Every long-lived descriptor this process will ever own is open by NOW
    // (logging, the session database, plugin storage, the signal pipe, and --
    // since the line above -- gRPC's listen socket): mark them all
    // close-on-exec so a bash command, MCP stdio server or hook action
    // inherits only what its spawner explicitly dups. The sweep used to run
    // BEFORE Server construction, so gRPC's socket escaped it -- the exact
    // descriptor class the sweep exists for. Retires the §7 fd-hygiene gap
    // structurally instead of chasing each spawner's fds. (Accepted
    // connections and post-sweep opens remain per-spawner obligations.)
    const auto cloexec_marked = minimal::common::MarkInheritedFdsCloseOnExec();
    if (cloexec_marked != 0) {
        std::printf("  fd hygiene: %zu descriptor(s) marked close-on-exec\n", cloexec_marked);
    }

    // The session reaper: never-attached sessions after the attach guard,
    // detached-quiescent ones after the idle TTL.
    minimal::agent::SessionRegistry::ReaperConfig reaper_config;
    reaper_config.initial_attach_guard = std::chrono::seconds(settings.sessions.initial_attach_guard_seconds);
    reaper_config.detached_idle_ttl = std::chrono::seconds(settings.sessions.detached_idle_ttl_seconds);
    sessions.set_reaper_config(std::move(reaper_config));

    // The process's own periodic duties. S5: a session outlives the client
    // that opened it, so "last client detached" does not mean "safe to exit"
    // -- the idle exit waits for zero clients, no recent Ping, and no session
    // holding the process up. RequestShutdown, not Shutdown: this fires from
    // the housekeeping thread, and the blocking form would wait for itself.
    minimal::rpc::ServerHousekeeping::Config housekeeping_config;
    housekeeping_config.idle_shutdown = std::chrono::seconds(settings.server.idle_shutdown_seconds);
    housekeeping_config.attached_clients = [&service] { return service.attached_clients(); };
    housekeeping_config.client_idle_for = [&service] { return service.client_idle_for(); };
    housekeeping_config.sessions_hold = [&sessions] { return sessions.AnySessionHolds(); };
    housekeeping_config.on_idle_expired = [&server] { server.RequestShutdown(); };
    // wal_autocheckpoint=0 moved checkpoint I/O off the append path onto this
    // thread.
    housekeeping_config.periodic_checkpoint = [&session_db] {
        using CheckpointOutcome = minimal::session::SessionDatabase::CheckpointOutcome;
        const CheckpointOutcome outcome = session_db->MaybeRunPassiveCheckpoint();
        if (outcome != CheckpointOutcome::kFailed) return;
        // Log at 1 then every doubling (1, 2, 4, 8...) so a persistently
        // pinned WAL is visible without a line per tick.
        const auto failures = session_db->checkpoint_failures();
        if ((failures & (failures - 1)) == 0) {
            minimal::common::DefaultLogger().warn(
                "session database: WAL checkpoint failed ({} consecutive); the WAL keeps "
                "growing until one succeeds -- an out-of-process reader may be pinning it",
                failures);
        }
    };
    minimal::rpc::ServerHousekeeping housekeeping(std::move(housekeeping_config));

    std::printf("\nlistening on %s\n", server.target().c_str());
    std::fflush(stdout);

    // Discovery warm-up (server.discovery_warmup_timeout_seconds): providers.json
    // "discover": true entries list their models lazily, so a freshly started
    // server's /model picker would show static models only until the user hits
    // ctrl+r. This warms the runtime cache in the background right after
    // listening starts -- the server serves immediately either way; startup
    // never waits on it. RefreshDiscovery() is const and thread-safe (per-
    // provider gates, providers fetched in parallel) and each catalog fetch is
    // bounded by its own transport timeout (10 s for the OpenAI-shape catalog),
    // so the budget below is a backstop for a catalog that ignores its
    // transport timeout, not the primary bound. A concurrent ctrl+r refresh
    // serializes per provider on the same gates -- no corruption, a possible
    // wait. On expiry the worker keeps running (still transport-bounded) and
    // its late results publish into the same cache, appearing at the next
    // ListModels; the detached worker can only dangle if the process exits
    // while a catalog has already blown past its own 10 s bound AND its
    // deadline -- a window the deadline exists to shrink, not to close.
    std::thread discovery_warmup;
    if (settings.server.discovery_warmup_timeout_seconds > 0) {
        std::promise<std::vector<minimal::config::DiscoveryRefreshOutcome>> warmup_promise;
        auto warmup_result = warmup_promise.get_future();
        std::thread warmup_worker(
            [&presets, promise = std::move(warmup_promise)]() mutable {
                try {
                    promise.set_value(presets->RefreshDiscovery());
                } catch (...) {
                    promise.set_exception(std::current_exception());
                }
            });
        discovery_warmup = std::thread(
            [&logger = minimal::common::DefaultLogger(),
             budget = std::chrono::seconds(settings.server.discovery_warmup_timeout_seconds),
             result = std::move(warmup_result), worker = std::move(warmup_worker)]() mutable {
                if (result.wait_for(budget) != std::future_status::ready) {
                    logger.warn("model discovery warm-up exceeded {}s; /model lists static "
                                "models until the fetch completes or ctrl+r refreshes",
                                budget.count());
                    worker.detach();
                    return;
                }
                try {
                    for (const auto& outcome : result.get()) {
                        if (outcome.ok) {
                            logger.info("model discovery: '{}' -> {} model(s)", outcome.provider,
                                        outcome.model_count);
                        } else {
                            logger.warn("model discovery: '{}' failed: {}", outcome.provider,
                                        outcome.error);
                        }
                    }
                } catch (const std::exception& error) {
                    logger.warn("model discovery warm-up failed: {}", error.what());
                } catch (...) {
                    logger.warn("model discovery warm-up failed: unknown error");
                }
                worker.join();
            });
    }

    // The signal watcher turns the self-pipe byte (written by the signal
    // handler, which must stay async-signal-safe) into the same graceful
    // RequestShutdown the idle exit drives. Joined right after Wait()
    // returns, so it cannot touch `server` during teardown; its one blocking
    // read happens outside any lock, and its RequestShutdown call happens
    // before Wait() can return -- `server` is alive throughout.
    std::thread signal_watcher([&server] {
        char byte = 0;
        while (::read(shutdown_pipe_fds[0], &byte, 1) < 0 && errno == EINTR) {
        }
        std::fprintf(stderr, "\nshutting down (signal received)\n");
        server.RequestShutdown();
    });

    server.Wait();
    // Wake the watcher: a normal idle exit produces no signal byte, and the
    // pipe's write end stays open for the process's whole life. The
    // RequestShutdown the watcher then issues is idempotent.
    const char wake = 1;
    (void)!::write(shutdown_pipe_fds[1], &wake, 1);
    signal_watcher.join();
    // Bounded by the warm-up budget (or a no-op when disabled), so a catalog
    // that wedged past its transport timeout cannot stall process exit.
    if (discovery_warmup.joinable()) discovery_warmup.join();

    // Stop both periodic threads before the exit cascade: housekeeping's
    // callbacks capture `service`, `server`, `sessions` and `session_db` by
    // reference, and the reaper terminates sessions. MCP teardown between
    // server.Wait() and the registry's destruction takes seconds -- a real
    // window, not a formality.
    housekeeping.Stop();
    sessions.StopReaper();
    sessions.TerminateAll();
    // Drain and release the observers NOW, not in ~SessionObserverRegistry:
    // `observers` is declared before `services`, `session_support`,
    // `session_db` and `sessions`, so its destructor runs after all of them --
    // and the backlog it drains (TerminateAll's OnSessionEnded among it) plus
    // any observer-owned worker (session-titler's rename) reach exactly those.
    observers.Shutdown();
    return 0;
}

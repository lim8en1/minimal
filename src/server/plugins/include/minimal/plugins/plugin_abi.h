#pragma once

// The server plugin ABI. This is the header a plugin author includes.
//
// COMPATIBILITY MODEL: source, not binary. Plugins are compiled from the same
// tree with the same pinned toolchain as the harness, so an interface change is
// a compile error you fix by rebuilding -- not a silent break. The ABI tag below
// is therefore a "you forgot to rebuild" guard, not a stability promise, which
// is what makes it safe to ship core functionality as plugins while these
// interfaces are still evolving.
//
// The header has two halves, and both are frozen surface:
//
//   1. EXTENSION POINTS a plugin implements -- the extern "C" symbols below.
//      Symbol presence *is* the capability declaration; the loader dlsym's each
//      optional name and calls it only if found, so there is no manifest to
//      drift out of sync with the code.
//
//   2. UTILITIES a plugin consumes -- included below. A plugin tool must call
//      path_confinement::confine rather than reimplementing path safety, which
//      is precisely why these are ABI surface too, just inbound. One utility is
//      reached through ServiceRegistry::Find rather than a link-time symbol:
//      SessionSupportService, provided by CORE itself under
//      kSessionSupportServiceName -- the pull seam for plugins that react to
//      session lifecycle and need the transcript, the session's own provider,
//      or the rename-and-notify path. ConfigDir() is here for the same reason:
//      a plugin whose data ships as a seeded config file (the providers.json
//      treatment -- user-editable text, never compiled in) needs the same
//      directory resolution the server uses.

#include <filesystem>
#include <string>

#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"

// --- Half 2: utilities plugins consume -------------------------------------
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/context_contributor.hpp"
#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/session_observer.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/agent/session_support_service.hpp"
#include "minimal/commands/command.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/rpc/auth_provider.hpp"
#include "minimal/llm/metadata_catalog.hpp"
#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/llm/variant_params.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/net/sse_parser.hpp"
#include "minimal/plugins/plugin_rpc.hpp"
#include "minimal/plugins/plugin_storage.hpp"
#include "minimal/session/history_window.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/tools/atomic_write.hpp"
#include "minimal/tools/file_access.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/permission_model.hpp"
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "minimal/plugins/service_registry.hpp"

// --- Half 1: extension points plugins implement -----------------------------

namespace minimal::plugins {

// Everything a plugin needs at REGISTRATION time, for every registration seam.
// A struct rather than loose parameters so a later addition stays
// source-compatible instead of forcing another signature change on every
// plugin author. (Per-invocation needs like the Logger travel in ToolContext,
// not here.)
struct PluginRegistrationContext {
    // The `tools` object from settings.json, verbatim. Core deliberately does
    // not impose a schema on it: each plugin owns the shape of its own slice,
    // which is why config::ToolsSettings keeps it as raw JSON. Carried on the
    // shared context rather than only at tool registration so a plugin that
    // registers several seam kinds sees the same world at each.
    nlohmann::json tools_settings = nlohmann::json::object();

    // This plugin's own configuration, verbatim: the optional "config" object
    // on the plugin's plugins.json entry --
    //   {"plugins": [{"file": "tools-bash.so", "config": {"timeout_s": 30}}]}
    // -- and only this plugin's. Core imposes no schema here either; the same
    // rule as tools_settings. Empty when the entry carries none (a runtime
    // condition, not an error). Configure a plugin where you enable it:
    // plugins.json is the one file a user already edits for that purpose.
    nlohmann::json config = nlohmann::json::object();

    // The cross-plugin service registry, for plugins that CONSUME another
    // plugin's services as well as for the provider itself. Capture the pointer
    // at registration and resolve with Find() lazily at use time -- never at
    // registration: plugin load order is file order, so a consumer can load
    // before its provider. Null when the host does not offer the seam (a test
    // exercising only other registries), which a consumer treats as "no
    // services", not as an error.
    ServiceRegistry* services = nullptr;

    // Per-plugin persistent KV storage, namespaced by this plugin's name --
    // pass the namespace as the first argument of every call; no plugin can
    // reach another's rows. Null when the host does not offer the seam, which a
    // plugin treats as "storage unavailable" and DEGRADES with a clear message,
    // not as a fatal condition. Values are opaque strings (serialize your own
    // JSON). For state that does not fit a KV shape, data_dir below is the
    // escape hatch.
    PluginStorage* storage = nullptr;

    // This plugin's own directory under the host's data dir, created by the
    // host before registration: <DataDir()>/plugins/<plugin_name>/. Empty when
    // the host does not provision one. The documented escape hatch for state
    // that does not fit the KV store -- a plugin may create its own database or
    // files here, inheriting the concurrency discipline as its own obligation
    // (WAL, a single-writer mutex).
    std::filesystem::path data_dir;
};

// --- The generic client channel (track E5; docs/EXTENSIBILITY.md 2.5) -------
//
// A UI plugin is usually a PAIR: a server-side data source plus a client-side
// view. The wire carries two generic RPCs (PluginCall / PluginSubscribe);
// server-side, they dispatch by plugin_name through the ServiceRegistry to the
// PluginEndpoint the plugin Provided under PluginEndpointServiceName(name).
// Core defines neither payload nor semantics: the JSON both ways is the
// plugin's own.

// "<plugin_name>.endpoint" -- the ServiceRegistry name a plugin's PluginCall/
// PluginSubscribe data source is Provided under. Mirrors PluginRpcServiceName
// (plugin_rpc.hpp) for the same reason: ServiceRegistry is one flat, generic
// void*-typed namespace shared with plain §2.2 services (a plugin may Provide
// both a PluginEndpoint and, say, a BashSessionStateService), so a bare
// plugin_name key would let ANY interface registered under it get
// static_cast to PluginEndpoint by PluginCall's lookup -- a real vtable call
// through the wrong type, not a theoretical mismatch. The suffix makes that
// collision unexpressible (NameRegistry would refuse two registrations under
// the same key), the same way ".rpc" already does for PluginRpcHandler.
inline std::string PluginEndpointServiceName(std::string_view plugin_name) {
    return std::string(plugin_name) + ".endpoint";
}

// The push side of a subscription. Implemented by the RPC layer; a plugin's
// push source holds it by shared_ptr. Thread-safe. Push returns false once the
// subscriber is gone (stream closed or cancelled) -- a push source must stop
// and drop its reference at that point, which is what makes teardown safe.
class PluginSubscriptionSink {
public:
    virtual ~PluginSubscriptionSink() = default;
    virtual bool Push(std::string json_payload) = 0;
};

// What a plugin Provides under PluginEndpointServiceName(its own name) to
// serve the generic channel. Abstract, header-only, pure virtual: the RPC
// layer static_casts ServiceRegistry::Find(...) to this and dispatches
// through the vtable -- no symbol resolution, so RTLD_LOCAL costs nothing.
class PluginEndpoint {
public:
    virtual ~PluginEndpoint() = default;

    // One call, one JSON answer. A throw is contained by the RPC layer and
    // answered as an error to the caller -- plugin code is not trusted to
    // return normally, like every other entry point.
    virtual std::string Call(const std::string& method, const std::string& json_payload) = 0;

    // Starts a subscription: the endpoint pushes payloads to `sink` (from any
    // thread it owns) until the subscriber is gone -- Push returning false.
    // Returns false to refuse (unknown method, no such subscription): the RPC
    // answers NOT_FOUND. The sink's lifetime is shared; holding it past the
    // subscriber's disconnect is safe but pointless, since every Push fails.
    virtual bool Subscribe(const std::string& method, const std::string& json_payload,
                           std::shared_ptr<PluginSubscriptionSink> sink) = 0;
};

}  // namespace minimal::plugins

extern "C" {

// REQUIRED. A plugin missing any of these is skipped with MissingRequiredSymbol.
//
// Implement these with MINIMAL_PLUGIN_ABI_SYMBOLS below, not by hand: a
// handwritten `return minimal::common::AbiTag();` LOOKS like it reports the
// toolchain the plugin was built with, but does not. minimal_common is a
// STATIC library linked PUBLIC into the SHARED server/client core, which
// re-exports minimal::common::AbiTag(); a plugin's undefined reference to it
// binds at dlopen time to the ALREADY-RUNNING HOST's copy, not to anything
// baked into the plugin -- so the guard compares the host's tag against
// itself and always passes, even for a plugin built against a since-changed
// harness revision whose stale .so sits in the plugin dir. The macro instead
// expands the compile-time MINIMAL_ABI_TAG/MINIMAL_VERSION macros (PUBLIC
// compile definitions on minimal_common, see src/common/CMakeLists.txt),
// which the compiler bakes into the plugin's own object code, so the reported
// tag is the plugin's, not the host's.
const char* minimal_plugin_abi_tag();
const char* minimal_plugin_name();
const char* minimal_plugin_version();

}  // extern "C"

// Defines the three REQUIRED symbols above. Every server plugin should use
// this instead of writing them by hand -- see the comment above for why a
// handwritten abi_tag is silently wrong. Invoke once, at file scope, in the
// plugin's .cpp (the three declarations above already gave these C linkage,
// so no `extern "C"` is needed at the call site).
#define MINIMAL_PLUGIN_ABI_SYMBOLS(plugin_name_literal)                                 \
    const char* minimal_plugin_abi_tag() { return MINIMAL_ABI_TAG; }                    \
    const char* minimal_plugin_name() { return (plugin_name_literal); }                 \
    const char* minimal_plugin_version() { return MINIMAL_VERSION; }

extern "C" {

// OPTIONAL, additive: many plugins may each contribute tools. The context
// carries the verbatim `tools` object from settings.json AND this plugin's own
// `config` object from its plugins.json entry.
void minimal_plugin_register_tools(minimal::tools::ToolRegistry&,
                                   const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes provider kinds addressable from providers.json.
void minimal_plugin_register_provider_factories(
    minimal::llm::ProviderFactoryRegistry&, const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: reacts to session lifecycle events.
//
// This is how a plugin holding per-session state releases it. Without it that
// state grows for the process lifetime -- tools-bash's cwd/env map is the
// concrete case. Observers are dispatched fire-and-forget and cannot veto
// anything; see SessionObserver for the constraints they run under.
void minimal_plugin_register_session_observer(minimal::agent::SessionObserverRegistry&,
                                              const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes /-commands.
//
// Separate from register_tools on purpose. A plugin owns the commands that touch
// its own state -- tools-bash registers both a `bash` tool and `/restart-shell`,
// because the per-session cwd/env they share lives inside that .so. (Other
// plugins may now ASK for that state through the service registry, which is the
// sanctioned channel; a command registered by a plugin that does not own the
// state it touches would still be the wrong shape.) Two independent symbols is
// what expresses ownership, rather than one "kind" of plugin that has to be
// either/or.
void minimal_plugin_register_commands(minimal::commands::CommandRegistry&,
                                      const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes /-commands scoped to ONE project root.
//
// Unlike every other registration hook above -- including register_commands
// itself -- this one is NOT called once at startup. There is no project root
// yet then: commands register before any session exists, and the server
// serves many project roots over its life. Instead it is called LAZILY, the
// first time a session rooted at `project_root` needs its command catalog
// (PluginLoader::RegisterProjectCommands, driven by
// minimal::commands::ProjectCommandSource's per-root cache), and the result is
// cached for that root -- rebuilt only when the plugin's OPTIONAL
// minimal_plugin_project_commands_fingerprint (below) reports a change.
//
// `registry` is a FRESH, project-scoped CommandRegistry -- never the global
// one register_commands populates, and never shared between roots. A name
// registered here may freely shadow a global command of the same name for
// sessions rooted at `project_root`: dispatch consults the project registry
// first, falling back to the global one, matching how a project's own
// agents/<name>.md already shadows a global preset by name. A throwing
// implementation is caught and logged by the caller; it must not fail
// session creation for the root that triggered it.
void minimal_plugin_register_project_commands(
    minimal::commands::CommandRegistry& registry, const std::filesystem::path& project_root,
    const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, meaningful only beside register_project_commands: a cheap
// summary of whatever this plugin's project commands for `project_root` are
// built FROM (commands-md: its directories' and files' mtimes). The
// ProjectCommandSource cache calls it on every lookup for that root and
// rebuilds the root's registry -- asking every plugin's
// register_project_commands again -- when any plugin's value changed, so an
// edited command file takes effect without a server restart. Must be cheap
// (stat-level, no parsing) and must not throw; it writes its value into
// `out` (empty on entry; leaving it empty is a valid value -- an out-param
// rather than a return, since a C-linkage function cannot return
// std::string). A plugin without it never triggers a rebuild.
void minimal_plugin_project_commands_fingerprint(const std::filesystem::path& project_root,
                                                 std::string& out);

// OPTIONAL, additive: contributes services other plugins may reach. Each
// registration is an (name, abstract-interface pointer) pair; the interface
// header the provider ships is the contract, and a consumer static_casts
// ServiceRegistry::Find's result to it -- see service_registry.hpp for the
// rules (abstract interfaces only, lazy lookup, no bus). Two plugins providing
// the same service name is FATAL at startup, naming both.
void minimal_plugin_register_services(minimal::plugins::ServiceRegistry&,
                                      const minimal::plugins::PluginRegistrationContext&);

void minimal_plugin_register_mention_resolver(minimal::agent::MentionResolverRegistry&,
                                              const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes CONTEXT SECTIONS pre-filled into every
// session's system prompt (after AGENTS.md and signal-channel context). Asked
// once per session at creation/resume/spawn and frozen for the session, so
// the prompt-cache prefix never shifts. Contribute() runs synchronously on
// the session-creation path -- keep it local and cheap. See
// context_contributor.hpp for the full contract; settings.json
// context_contributors.disabled turns one plugin's sections off.
void minimal_plugin_register_context_contributors(
    minimal::agent::ContextContributorRegistry&,
    const minimal::plugins::PluginRegistrationContext&);


// Exclusive: one global choice. First registration wins; a second logs a
// warning and is ignored (the registry enforces this). The default is NO
// authentication -- an empty registry passes everything through, matching the
// local-only single-user topology (ARCHITECTURE.md).
void minimal_plugin_register_auth_provider(minimal::rpc::AuthProviderRegistry&,
                                           const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes a permission advisor, keyed by the plugin's
// name. permissions.json's "advisor" field selects one by that name; an
// unselected advisor is inert. The advisor answers Ask-gated tool calls
// automatically and abstains (or throws) to the wire checker -- the human.
// It can only NARROW what reaches a person: ToolExecutor has already applied
// the mode lattice, precedence chains, subagent ceilings, and run modes before
// the checker runs, and the grant filter at the call site holds for an
// advisor's answer exactly as for a human's.
void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry&,
    const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, one per plugin: contributes the PERMISSION MODEL for this
// plugin's own tools, keyed by the plugin's name (tools/permission_model.hpp).
// The model claims its tools and replaces core resolution stages for them
// (the base mode, the network fold); core keeps everything around those
// stages -- operator per-tool denies, subagent ceilings, out_of_workdir, run
// modes, grants, the advisor and the human. Its configuration is opaque to
// core: permissions.json `plugins.<plugin>` and a preset's
// `plugin_permissions.<plugin>`, handed to BuildLayer. Claiming a tool
// another plugin registered is FATAL at startup; a model that throws at
// resolve time is answered with Deny.
void minimal_plugin_register_permission_models(
    minimal::tools::PermissionModelRegistry&,
    const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes a model catalog, keyed by the SAME kind
// string the provider factory registry uses -- a providers.json entry's
// `backend` selects both how to chat and how to list. A catalog serves
// "discover": true entries: List() is fetched lazily at first resolve (never
// at startup), results are runtime-only and never written back to
// providers.json, and a failed fetch is negative-cached and degrades to the
// entry's static models.
void minimal_plugin_register_model_catalogs(minimal::llm::ModelCatalogRegistry&,
                                             const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes an enrichment catalog, keyed by the kind
// string(s) whose wire shapes it knows models for -- NOT exclusively one kind,
// and not displacing the listing catalogs registered under the same strings
// (this is a SECOND registry: ModelCatalogRegistry stays the discover/list
// seam). An enrichment catalog answers Lookup(model_id) with field defaults
// for models the config already addresses -- a providers.json entry uses its
// answers automatically (no per-entry opt-in) whenever a model is missing
// `context_window_tokens`/`max_tokens`, and, through the advertised effort
// ladder, declares no `variants` of its own. A metadata answer can only FILL
// FIELDS of an existing model, never add an addressable one -- that invariant
// is core-enforced (Lookup is only ever called for a resolved or listed id)
// and is what lets auto-use be the default. See metadata_catalog.hpp for the
// Lookup contract and the fallback chain (RegisterFallback) for kinds nobody
// named.
void minimal_plugin_register_metadata_catalogs(
    minimal::llm::MetadataCatalogRegistry&,
    const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes the wire-shape a backend kind wants for one
// "@effort" variant (ARCHITECTURE.md, "Core vs. plugin surface" -- relocated
// out of core's providers_config.cpp), keyed by the SAME kind string
// ProviderFactoryRegistry/ModelCatalogRegistry use. A providers.json entry's
// "variants": [<effort names>] list form (and a discovered model's
// autodetected effort ladder) derives its wire parameters through whichever
// kind's provider is registered here; an unregistered kind falls back to
// core's generic {"reasoning_effort": "<word>"} shape. See variant_params.hpp
// for the DeriveParameters contract.
void minimal_plugin_register_variant_params(minimal::llm::VariantParamsRegistry&,
                                             const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes a compaction strategy (S4), keyed by name --
// memory.compaction_strategy in settings.json selects one. Core registers its
// own built-in "summarize" strategy directly in main.cpp (not through this
// hook), so the default path is never plugin-dependent; a plugin adds a kind
// core has never heard of (drop-oldest, tool-result-elide, the deferred
// llamacpp-kv's external-KV strategy). See compaction_strategy.hpp: a
// strategy proposes a candidate cut, but core ALWAYS snaps it to a safe
// tool-call boundary before using it -- that invariant is not a contract a
// strategy author can get wrong.
void minimal_plugin_register_compaction_strategies(
    minimal::agent::CompactionStrategyRegistry&,
    const minimal::plugins::PluginRegistrationContext&);

// OPTIONAL, additive: contributes HOOK POINTS -- named, typed events this
// plugin emits from its own code, which hooks.json actions can answer. Core
// owns the mechanism (dispatch, filters, the shell executor, fail-open
// semantics); a plugin owns the points it declares and the meaning a Block
// has for them. Registration is startup-only: keep the returned HookPoint
// handle and Emit from your own code later. A duplicate point name is a
// FATAL startup collision, named by the sanity check. See
// hook_dispatcher.hpp for the full contract and docs/EXTENSIBILITY.md for
// the worked example (tools-bash's before_command).
void minimal_plugin_register_hook_points(minimal::hooks::HookDispatcher&,
                                          const minimal::plugins::PluginRegistrationContext&);

// Frozen-surface note (track M): ToolContext::history is a
// session::HistoryWindow -- a bound, windowed slice of SessionReader exposing
// only keyset-paged GetMessages. Both headers above are the surface; a plugin
// tool reading the conversation reads bounded pages through it and has no
// full-materialization path (LoadAll stays on SessionReader, the resume path,
// which no tool holds).
}  // extern "C"

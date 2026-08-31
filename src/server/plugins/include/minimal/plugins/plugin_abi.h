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
//      is precisely why these are ABI surface too, just inbound.

#include "minimal/common/abi_tag.hpp"

// --- Half 2: utilities plugins consume -------------------------------------
#include "minimal/agent/session_observer.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/commands/command.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/rpc/auth_provider.hpp"
#include "minimal/llm/http_client.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/llm/sse_parser.hpp"
#include "minimal/tools/atomic_write.hpp"
#include "minimal/tools/file_access.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "minimal/tools/permission_advisor.hpp"
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
    //   {"plugins": [{"file": "tools-bash.so", "config": {"timeout_ms": 30000}}]}
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
};

}  // namespace minimal::plugins

extern "C" {

// REQUIRED. A plugin missing any of these is skipped with MissingRequiredSymbol.
//
// Implement abi_tag by returning minimal::common::AbiTag() -- returning a
// literal defeats the guard, since the point is to report the toolchain this
// binary was actually built with.
const char* minimal_plugin_abi_tag();
const char* minimal_plugin_name();
const char* minimal_plugin_version();

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

// OPTIONAL, additive: contributes services other plugins may reach. Each
// registration is an (name, abstract-interface pointer) pair; the interface
// header the provider ships is the contract, and a consumer static_casts
// ServiceRegistry::Find's result to it -- see service_registry.hpp for the
// rules (abstract interfaces only, lazy lookup, no bus). Two plugins providing
// the same service name is FATAL at startup, naming both.
void minimal_plugin_register_services(minimal::plugins::ServiceRegistry&,
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

// Not yet wired -- declared nowhere until the subsystem it registers into
// exists, so that this ABI is not frozen before it has consumers:
//   minimal_plugin_register_mention_resolver(MentionResolverRegistry&)

}  // extern "C"

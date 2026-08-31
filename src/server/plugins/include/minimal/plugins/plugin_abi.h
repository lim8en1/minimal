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
#include "minimal/agent/mention_resolver.hpp"
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
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_registry.hpp"

// --- Half 1: extension points plugins implement -----------------------------

namespace minimal::plugins {

// Everything a tool needs at REGISTRATION time. A struct rather than loose
// parameters so a later addition stays source-compatible instead of forcing
// another signature change on every plugin author. (Per-invocation needs like
// the Logger travel in ToolContext, not here.)
struct ToolRegistrationContext {
    // The `tools` object from settings.json, verbatim. Core deliberately does
    // not impose a schema on it: each plugin owns the shape of its own slice,
    // which is why config::ToolsSettings keeps it as raw JSON.
    nlohmann::json tools_settings = nlohmann::json::object();
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

// OPTIONAL, additive: many plugins may each contribute tools.
void minimal_plugin_register_tools(minimal::tools::ToolRegistry&,
                                   const minimal::plugins::ToolRegistrationContext&);

// OPTIONAL, additive: contributes provider kinds addressable from providers.json.
void minimal_plugin_register_provider_factories(minimal::llm::ProviderFactoryRegistry&);

// OPTIONAL, additive: reacts to session lifecycle events.
//
// This is how a plugin holding per-session state releases it. Without it that
// state grows for the process lifetime -- tools-bash's cwd/env map is the
// concrete case. Observers are dispatched fire-and-forget and cannot veto
// anything; see SessionObserver for the constraints they run under.
void minimal_plugin_register_session_observer(minimal::agent::SessionObserverRegistry&);

// OPTIONAL, additive: contributes /-commands.
//
// Separate from register_tools on purpose. A plugin owns the commands that touch
// its own state -- tools-bash registers both a `bash` tool and `/restart-shell`,
// because the per-session cwd/env they share lives inside that .so and no other
// plugin can reach it. Two independent symbols is what expresses that, rather
// than one "kind" of plugin that has to be either/or.
void minimal_plugin_register_commands(minimal::commands::CommandRegistry&);

// OPTIONAL, exclusive: one @-mention suggestion strategy. First plugin wins; a
// second registration logs a warning and is ignored. The core default
// (PrefixMentionResolver) stays in force until a plugin registers.
void minimal_plugin_register_mention_resolver(minimal::agent::MentionResolverRegistry&);

// Exclusive: one global choice. First registration wins; a second logs a
// warning and is ignored (the registry enforces this). The default is NO
// authentication -- an empty registry passes everything through, matching the
// local-only single-user topology (ARCHITECTURE.md).
void minimal_plugin_register_auth_provider(minimal::rpc::AuthProviderRegistry&);

}  // extern "C"

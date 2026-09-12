# minimal — Plugin extensibility

Companion to `ARCHITECTURE.md`, which remains the single source of truth for *design*. This document covers only *extensibility*: the plugin surface as it actually exists in the tree (verified against the code, not against other documents), the places it stops short, and the designed answer for each gap. Where this document and `ARCHITECTURE.md` disagree, that document wins and this one is stale.

## 1. The surface as it exists

**Server ABI** (`src/server/plugins/include/minimal/plugins/plugin_abi.h`) — thirteen optional `extern "C"` registration hooks (plus the required tag/name/version symbols), each `dlsym`'d by `plugin_loader.cpp`:

| Symbol | Status | What it grants |
|---|---|---|
| `minimal_plugin_abi_tag` / `_name` / `_version` | required | load gate -- define via `MINIMAL_PLUGIN_ABI_SYMBOLS(name)`, not by hand: a handwritten `minimal::common::AbiTag()` call binds to the host's copy at `dlopen` time, not the plugin's own build, so the rebuild guard always passes |
| `minimal_plugin_register_tools(ToolRegistry&, const PluginRegistrationContext&)` | live | concrete tools; the context carries the verbatim `tools.*` JSON from `settings.json` plus the plugin's own `config` from `plugins.json` |
| `minimal_plugin_register_provider_factories(ProviderFactoryRegistry&)` | live | provider kinds addressable from `providers.json` |
| `minimal_plugin_register_session_observer(SessionObserverRegistry&)` | live | fire-and-forget lifecycle reactions; cannot veto anything |
| `minimal_plugin_register_commands(CommandRegistry&)` | live | `/`-commands, registered once at startup into the process-wide registry |
| `minimal_plugin_register_project_commands(CommandRegistry&, const std::filesystem::path&)` | **live** | project-scoped `/`-commands (`commands-md`'s own project-local discovery is the only caller): unlike every other hook, called LAZILY per project root (`ProjectCommandSource`, cached per root) rather than once at startup -- there is no project root yet then. `registry` is a fresh, per-root instance, never the global one; a name here may shadow a global command for sessions rooted there |
| `minimal_plugin_register_services(ServiceRegistry&)` | **live (track L)** | pull-side access to another plugin's services; consumer side rides `PluginRegistrationContext::services` |
| `minimal_plugin_register_mention_resolver(...)` | **live** — `plugin_loader.cpp` dlsym's and calls it; `SessionServiceImpl` resolves every `@`-mention through `MentionResolverRegistry::Active()` | pluggable `@`-mention resolution; `PrefixMentionResolver` is the core default, exclusive first-wins |
| `minimal_plugin_register_auth_provider(...)` | live | exclusive connection policy; pass-through default |
| `minimal_plugin_register_permission_advisor(PermissionAdvisorRegistry&)` | **live (track J)** | answers `Ask` automatically; abstains/throws to the wire checker — the human |
| `minimal_plugin_register_model_catalogs(ModelCatalogRegistry&)` | **live (track N)** | `"discover": true` providers list their models from the backend, lazily at first resolve; keyed by the same kind string as the provider factory |
| `minimal_plugin_register_compaction_strategies(CompactionStrategyRegistry&)` | **live (S4)** | plugin compaction strategies keyed by name — `memory.compaction_strategy` in settings.json selects one. Core registers its built-in `"summarize"` strategy directly in `main.cpp` (never through this hook), and always snaps a strategy's proposed cut to a safe tool-call boundary before using it |
| `minimal_plugin_register_hook_points(HookDispatcher&)` | **live (track O)** | named, veto-capable or observe-only events the plugin emits; hooks.json actions answer them — see §5 |

Interfaces a plugin implements today: `Tool` (with `Disclose` / `category_for` / `ExtractPath`), `Command` → `HarnessCommand`/`LLMCommand`, `LlmProvider` plus a factory, `SessionObserver`, `PermissionAdvisor` (composing above the wire checker — see §3), and the service interfaces a provider ships (abstract, header-only — see §2.2).

**Utilities half** — included from `plugin_abi.h`: `path_confinement`, `file_access`, `atomic_write`, `HttpClient`, `SseParser`, `Logger`, `SessionReader` + `HistoryWindow` (landed with track M), `PluginStorage` (per-plugin KV, plus a per-plugin `data_dir` on the context) and `plugin_rpc.hpp` (the `PluginCall` dispatch convention), plus the registry headers. The frontmatter parser remains reachable through ordinary core linkage (plugins link `minimal_server_core`, a shared library) but is not declared surface.

**Client ABI** (`src/client/plugins/include/minimal/client/plugins/client_plugin_abi.h`) — **live (track E5)**: `minimal_client_plugin_abi_tag` / `_name` / `_version` plus eight optional `minimal_client_plugin_register_*` hooks (views, start views, tool-result renderers, message-kind renderers, status segments, syntax grammars, and the exclusive chat-surface and markdown-engine slots), each taking `(Registry&, const ClientPluginRegistrationContext&)` — the context carries the per-plugin `config` slice, an `AttentionSink`, and the `ClientApi` data path (GetMessages paging + the PluginCall/PluginSubscribe channel). Loaded by `client_plugin_loader.cpp` (the server loader's twin: `RTLD_NOW|RTLD_LOCAL`, tag gate, guarded registration, attribution, never dlclosed) against `client-plugins.json` — a separate file from the server's `plugins.json`, so neither writer can stomp the other's entries. Client plugins link `minimal::client_core` (SHARED, like the server core) and install to `plugins/client/`. The generic channel dispatches server-side through the ServiceRegistry to a `PluginEndpoint` the pair plugin Provides under `PluginEndpointServiceName(name)` ("`<plugin_name>.endpoint`") -- suffixed, like `PluginRpcServiceName`, so a bare plugin_name key can never let PluginCall's lookup `static_cast` some other §2.2 service the same plugin registered.

`tests/support/` is an **in-tree INTERFACE target** (`minimal::test_support`) used by the test suite only — it is not installed and not exported, so a plugin author cannot link the `FakeLlmProvider`/`ScriptedProvider`/permission fakes; they exist for this repo's tests.

## 2. Extensibility holes

Ordered roughly by the size of the ceiling each imposes. Each entry states the hole with evidence, the suggested architecture, and the cost. None is scheduled except where § 4 says otherwise.

### 2.1 A tool cannot see the conversation

**Status: landed as track M.** Implemented with two refinements over the sketch below — the handle is typed `session::HistoryWindow`, not `const SessionReader*`, which turns the two stated constraints into properties of the type:

- **The window.** `history_window.hpp` (frozen surface in `plugin_abi.h`, with `SessionReader`) is a header-only, non-owning wrapper holding the reader pointer **and the session id it is bound to**, exposing only keyset-paged `GetMessages(before_seq, limit)` with the page size **clamped to a hard cap** (1000) — a tool asking for a whole transcript in one call gets a bounded page, so "one call cannot materialize the history" is structural too. There is no `LoadAll` on it — full materialization stays the resume path's job on `SessionReader`, which no tool holds — so "no path a tool can reach materializes the full history" is structural, not a documented rule. Binding is also what isolates subagents: the registry is the one funnel every session passes through, so injecting the reader there binds each session's window to its own id with no extra code.
- **Ownership.** As planned below: `SessionParams` gained a reader pointer, owned by `Session` alongside the window built from it (declared before the loop, so the loop's borrowed pointer never outlives it); the agent loop sets `ctx.history` from it per invocation. Stores that are not SQLite-backed (test fakes) leave it null — `history == nullptr` is the natural "unavailable" case, not an error path.

The windowed-access and 512k–1M notes below still describe the semantics; the type now enforces the first.

**Hole.** `ToolContext` (`tool.hpp:29-60`) carries `session_id`, `project_root`, `tool_use_id`, `allowed_skills`, `is_cancelled`, `emit_output`, `logger` — and nothing else. A tool cannot inspect the conversation it runs inside. Anything context-aware — a "summarize the thread so far" tool, a retrieval tool keyed to recent turns, a wrapper that references what was last edited — is inexpressible.

**Suggested architecture.** An optional read-only handle on the context:

```cpp
// tool.hpp -- set by the agent loop per invocation; null when unavailable
const session::SessionReader* history = nullptr;  // read-only
```

Constraints that keep it safe:

- **Windowed access only.** `SessionReader` answers keyset-paged ranges. A tool must never call `LoadAll()` — that path materializes a whole transcript and exists solely for resume (§ *Session storage*). The constraint is stated on the member; the pointer type already communicates read-only.
- **Filled per invocation, not at construction.** `ToolContext` is built per tool call; the agent loop sets the member from the session's reader. A subagent sees only its own history — the parent's is deliberately out of reach.
- **The 512k–1M goal is not violated**, because nothing is held: a tool reads what it needs in bounded windows, exactly like `GetMessages` does.

**Cost.** Low — one member, one producer, tests against an in-memory or SQLite reader.

**Postscript — the pattern held.** This section's "optional member, filled per invocation by the agent loop, null when unavailable" became the template for every later `ToolContext` growth: background bash's three lifetime-safe handles (`schedule_injection` / `session_lifetime` / `emit_task_event`), and now `ask_user` — the interactive ask the `tool-ask` plugin blocks on (ARCHITECTURE.md, "Asking the user"). Same three properties each time: plain-data types cross no agent-layer boundary, the handle is produced in the ctx-builder from session-owned wiring, and null is a degrade path the tool surfaces, never a hang.

### 2.2 No sanctioned cross-plugin channel

**Status: landed as track L.** Implemented as specified, with the interface shaped as below and three implementation findings:

- **The consumer channel is `PluginRegistrationContext::services`** (a `ServiceRegistry*` alongside the config from 2.3). A consumer captures the pointer at registration and resolves with `Find` lazily at use time — plugin load order is file order, so a consumer can load before its provider. Null when the host offers no seam (a test), treated as "no services".
- **The provider's interface must be abstract** (header-only, pure virtual). The concrete store's symbols live inside the providing `.so`, loaded `RTLD_LOCAL` — a consumer calling them by symbol fails at `dlopen(RTLD_NOW)`. Virtual dispatch needs no symbols. `tools-bash` ships `bash_state_service.hpp` (`BashSessionStateService`: `Erase`/`Reset`) and `BashSessionStateStore` implements it.
- **The first consumer stayed where it was.** `/restart-shell` remains in `tools-bash` — a command belongs to the plugin that owns the state it touches — but it now resolves the store through `Find` (the extracted `RestartShellCommand` answers a clear `kError` when the service is absent), so the service path is what runs, not a plugin-internal static.
- **Core can be a provider too.** `SessionSupportService` (`agent/session_support_service.hpp`, provided as `core.session_support`) is the first CORE-provided service: the pull seam a session-lifecycle plugin needs and cannot otherwise reach — the session's own `LlmProvider` + model (shared through `ProviderInstanceCache`, or a configured `<provider>/<model>` override), its `SessionReader`, and `RenameSession` (persist + live in-memory name + `SessionMetadataEvent`). It is Provided BEFORE `Seal()` so the name is inside the startup collision scan — a plugin claiming `core.session_support` is a diagnosed fatal duplicate — but its dependencies (`SessionRegistry`, the database, the preset registry) are serve-stage objects, so the concrete `SessionSupport` is constructed early and `Wire()`d in stage 4, before the RPC server listens. First consumer: the bundled `session-titler` plugin (shipped disabled — ARCHITECTURE.md, "Session naming, layered").
- **The second core-provided service is `core.signals`** (`agent/signal_service.hpp`, implemented BY `SignalRouter`): the push seam for INTERNAL signal producers. A plugin tool (the bundled `signals-message`'s `message_session` is the reference) delivers a cross-session message to a target session's "sessions" subscription — addressed, not broadcast, outcome-returning (delivered / not-subscribed / refused), with the router owning provenance framing, wake, and the drop-when-no-subscriber rule. Subscriptions, channels.json, and the receiver side stay core: a plugin can PRODUCE signals, never change who RECEIVES them. The trust split this encodes: producing is a permission-gated tool call; receiving is the operator's subscription decision at session start. Third-party producers (timers, file watchers) would ship exactly the `signals-message` shape.

**Hole.** One `.so` cannot reach another's state. For bash that is by design (a plugin owning its own state is the point), but the design stopped at ownership without providing the sanctioned way *to ask*. `SessionObserver` covers ordered notification, not queries; `CommandContext` deliberately hides other plugins' state (`command_context.hpp` says so explicitly).

**Suggested architecture.** A service registry, in the same shape as every other registry:

```cpp
// src/server/plugins/include/minimal/plugins/service_registry.hpp
class ServiceRegistry {
public:
    // Register an abstract interface under a name. Ownership stays with the
    // plugin; the registry holds a non-owning pointer for the process lifetime
    // (plugin handles are never dlclosed -- § Server plugins).
    void Provide(std::string name, void* service);
    // Named lookup. Null when absent: the provider plugin may be disabled,
    // which is a runtime condition, not an error.
    void* Find(std::string_view name) const;
};

// plugin_abi.h
void minimal_plugin_register_services(minimal::plugins::ServiceRegistry&);
```

Rules that keep it from becoming an event bus or a hidden dependency:

- **Typed at the edges, named in the middle.** The provider plugin declares its interface header (e.g. `bash_state.hpp` shipping with `tools-bash`); a consumer `static_cast`s the result of `Find` to it. The name is the only coupling; the header is the contract, under the same source-compatibility model as everything else.
- **Additive, like tools.** Many plugins may each provide services. Name collisions resolve through the existing `NameRegistry` (a new kind), so they are fatal, not shadowed.
- **Registration-time only.** The registry is sealed after plugin load (`main.cpp`, right after `LoadAll`); from then on the map is read-only, which is what makes unguarded `Find` from session workers and the command pool safe without a lock. A runtime `Provide` — a plugin holding its registration context and calling in later — is logged and ignored, not raced.
- **No bus, no subscribe.** Push already exists (`SessionObserver`); this is pull, deliberately. An event bus would put the ordering/concurrency burden back on plugin authors — exactly what the serialized observer dispatch was designed to remove.

The first consumer justifies the pattern: `/restart-shell` lives in `tools-bash` because bash state is unreachable; a service lets others build on that state without `tools-bash` exposing its internals. It is also the dispatch target the server half of 2.5 needs.

**Cost.** Moderate — small code, one new design rule (`NameRegistry` kind, sanity-check wiring).

### 2.3 No per-plugin config beyond one verbatim blob

**Status: landed as track K.** Implemented as specified below, with one deviation: the bundled plugins' knobs did **not** stay in `tools_settings` — `tools-bash` and `tools-fs` moved their knobs to their own `plugins.json` `config` objects with no `tools.*` fallback (the "optional per plugin" migration exercised immediately, strict rather than backward-compatible). The `tools_settings` channel itself is unchanged and still carries the whole verbatim `tools` object.

**Hole.** Only tool-registering plugins receive configuration: `ToolRegistrationContext::tools_settings` is the verbatim `tools` object from `settings.json`, and it reaches only `minimal_plugin_register_tools`. A provider, observer, or command plugin has no sanctioned way to read its own settings, and a tool plugin cannot get anything but the `tools.*` slice.

**Suggested architecture.** Generalize the context and give it a per-plugin slice:

```cpp
// plugin_abi.h
struct PluginRegistrationContext {
    // This plugin's own config, verbatim. Source: an optional "config" object
    // on the plugin's plugins.json entry:
    //   {"plugins": [{"file": "tools-bash.so", "config": {"timeout_s": 30}}]}
    // Core imposes no schema -- the same rule as tools_settings today.
    nlohmann::json config = nlohmann::json::object();
};
void minimal_plugin_register_tools(ToolRegistry&, const PluginRegistrationContext&);
void minimal_plugin_register_provider_factories(ProviderFactoryRegistry&,
                                                const PluginRegistrationContext&);
void minimal_plugin_register_session_observer(SessionObserverRegistry&,
                                              const PluginRegistrationContext&);
void minimal_plugin_register_commands(CommandRegistry&,
                                      const PluginRegistrationContext&);
void minimal_plugin_register_services(ServiceRegistry&,
                                      const PluginRegistrationContext&);
```

`plugins.json` is already self-maintaining and the natural home: the user already edits it to enable a plugin, so enabling *and* configuring in one file is one mental model. Malformed `config` degrades to an empty object with a warning, matching the file's existing degrade-don't-throw rule. Source compatibility makes the signature sweep a rebuild, not a break.

**Cost.** Low — loader plumbing plus the config loader; one ABI signature change across all `register_*` symbols.

### 2.4 No MCP lifecycle participation

**Hole.** MCP liveness, discovery, and tool registration are core; the finest-grained signal a plugin gets is `SessionObserver` (session created / turn completed / tool executed). A plugin cannot react to an MCP server coming up, going down, or exposing a tool set — the natural home for per-server usage tracking, tool auditing, or mirroring MCP tools elsewhere.

**Suggested architecture.** A dedicated observer registry, dispatched like `SessionObserver` (one ordered thread, fire-and-forget, throw-contained):

```cpp
// src/server/mcp/mcp_observer.hpp
class McpObserver {
public:
    virtual void OnServerStarted(const std::string& server_name) {}
    virtual void OnServerFailed(const std::string& server_name, std::string reason) {}
    virtual void OnToolsDiscovered(const std::string& server_name,
                                   std::vector<llm::ToolDefinition> tools) {}
    virtual void OnToolInvoked(const std::string& server_name,
                               const std::string& tool_name, bool is_error) {}
};

// plugin_abi.h
void minimal_plugin_register_mcp_observer(minimal::mcp::McpObserverRegistry&);
```

Payloads are owned values (copies) for the same reason `SessionObserver`'s are — dispatch runs on another thread. Discovery is cached per shared subprocess (§ *MCP client*), so `OnToolsDiscovered` fires once per process start, not per session; the registry is therefore process-scoped, not per-session.

**Cost.** Low, but **sequenced after track G** — there is nothing to observe until the MCP client exists.

### 2.5 No UI surface for server-plugin data

**Status: LANDED.** The unary `PluginCall` RPC and the streaming `PluginSubscribe` RPC are both implemented in `session_service.cpp` and dispatch through §2.2's registry. Two dispatch conventions share the channel, and they cannot collide: a plugin can Provide a `PluginEndpoint` under **`<plugin_name>.endpoint`** (`PluginEndpointServiceName` — `Call` answers unary requests, `Subscribe` pushes to a `PluginSubscriptionSink` until the subscriber disconnects), or a `PluginRpcHandler` under **`<plugin_name>.rpc`** (`PluginRpcServiceName`; unary only, dispatched through `DispatchPluginCall`). `PluginCall` tries the `.endpoint` first, then falls back to `.rpc`; `PluginSubscribe` dispatches to `.endpoint` providers only. Both suffixes keep the dispatch target clear of plain §2.2 service names. Core defines the convention, never the payloads; `DispatchPluginCall` is grpc-free and unit-testable, and the transport only maps outcomes to statuses (`NOT_FOUND` = plugin disabled/absent, `INVALID_ARGUMENT` = bad request or handler refusal, `INTERNAL` = handler threw). No bundled plugin ships a `.endpoint` yet — `checklist` and `cost-tracker` are `.rpc` handlers, and their client views poll — so the in-tree exercise of `PluginSubscribe` awaits a first push-source consumer. *[Postscript: `checklist-view` became the first in-tree Panel **view** — `checklist.panel`, two surfaces over one poller — fed by a new client `FocusProvider` seam (which tab/session the user is on, the component-side twin of the status segments' per-render session id); it still polls, so the push-source statement stands.]* The client half is unchanged, below. **Hole remaining.** A plugin command's result has exactly one sanctioned presentation (since §4.4: a status line or a named view — never a chat row), and a plugin tool renders only as a generic collapsible row unless the plugin ships a client-side renderer.

**Suggested architecture.** Two halves, split because they land in different tracks:

- **Server half (with 2.2):** ~~the generic `PluginCall`/`PluginSubscribe` RPCs from § *Client plugins*~~ — both done as above (`PluginCall` serving `.endpoint` and `.rpc`; `PluginSubscribe` serving `.endpoint` push).
- **Client half (track E):** `ViewRegistry` + `ToolResultRendererRegistry` per § *Client plugins* — the renderer slot first, views and other slots later.

The design intent is unchanged from `ARCHITECTURE.md`: one generic channel, plugin-owned JSON, renderers keyed by tool name with fallback to default rendering. What this document adds is the server-side dispatch story — 2.2's registry is what `PluginCall` routes *to* — which `ARCHITECTURE.md` leaves implicit.

**Cost.** Moderate; sequenced with track E.

### 2.6 No model auto-discovery

**Status: landed.** Implemented with three decisions: the sanity check is **network-free** (structural warnings only — "unreachable" means no catalog registered for the kind, never a startup probe); **no persisted cache and no TTL** (the server is short-lived, so a runtime cache needs neither, and a month-old cached window is the exact staleness this seam exists to eliminate); and **`/providers refresh` shipped in v1** (evict + refetch now, per-provider outcomes, `ReadOnly()` so it runs beside live turns — fetches run under per-provider gates, never the global config mutex, and `Resolve` returns a snapshot, so a hung catalog costs its own 10 s and a refresh never dangles a concurrent reader). The implementation reads one more field than the sketch below: vLLM reports its window as `max_model_len`. Resume after a backend's model list changed needs no mechanism — it degrades to today's fail-loud session-creation error, now distinguishing "discovery succeeded, model not served" from "discovery failed: <reason>". *[Update: the `/providers` command was later removed — its listing role moved to the client's `/model` picker over the session-free `ListModels` RPC, and its refresh role to `ListModels(refresh_discovery)` (the picker's ctrl+r).]*

**Hole.** Every `<provider>/<model>` a session can use must be hand-written into `providers.json`, including `context_window_tokens` — the field window-relative compaction triggers resolve against (§ *Context compaction*). Yet the servers themselves report most of it: OpenRouter's `GET /models` carries `context_length` and pricing; **llama.cpp's `/v1/models` carries model metadata including context size**; vLLM serves `/v1/models` (ids, no context). Nothing consumes any of it. The result is that the one config field with real consequences for correctness (compaction firing at all) is also the one most likely to be stale or absent — and a provider serving fifty models is unconfigurable by hand in any practical sense.

**This is not an OpenRouter feature.** The discovery shape is identical across OpenAI-shaped servers — a GET on the same base URL — so it belongs in the plugin surface, not in one provider plugin. llama.cpp is the proof: with the seam below, llama.cpp gets discovery from the *existing* `provider-openai` plugin with zero new code, and OpenRouter becomes a small increment on top (pricing, richer metadata) rather than a from-scratch provider.

**Suggested architecture.** A catalog registry keyed by the same kind string the provider factory uses — a second registry, not a capability query on `LlmProvider` (the two-method interface stays two methods; note `ARCHITECTURE.md` § *LLM provider adapter* deleted a capability-query layer for having no users — this is its user, arriving as a registry instead):

```cpp
// src/server/llm/model_catalog.hpp
struct DiscoveredModel {
    std::string id;   // the <model> half of <provider>/<model>
    std::optional<std::int64_t> context_window_tokens;  // feeds compaction + status bar
    nlohmann::json metadata;  // pricing, quantization, ... -- opaque to core
};

// Implemented by a plugin; fetched from the provider entry's connection.
// ProviderConfig::model_name is unused here -- a catalog lists, it does not chat.
class ModelCatalog {
public:
    virtual std::vector<DiscoveredModel> List(const ProviderConfig&) = 0;
};

// plugin_abi.h -- optional, keyed by the same kind string as the provider factory:
void minimal_plugin_register_model_catalogs(minimal::llm::ModelCatalogRegistry&);
```

Core side — the seam this needs:

- **`providers.json` gains `"discover": true`**, and the parse stops rejecting a provider with no `models` object when it is set (`providers_config.hpp` currently throws on exactly that shape).
- **`ProvidersConfig` resolves static ∪ discovered.** `Resolve` for a model absent from the static map under a discover-entry consults a per-entry discovery cache; a miss reports both the static alternatives and whether discovery itself failed, so a typo and a dead catalog are distinguishable errors.
- **The cache is runtime-only.** Discovered models are never written back to `providers.json` — the config-read-once and never-overwrite decisions (§ *Config*, § *Bundled assets*) stand. The cache keys on the config-generation counter, like `ProviderInstanceCache`.
- **Failure posture.** Discovery is a live network call: fetched lazily at first resolve, not at startup. The sanity check *warns* when a discover-entry's catalog is unreachable (the entry may still have static models) and only stays fatal through the existing "at least one usable provider" rule — a discover entry with zero static and zero discovered models fails it as today.
- **Consumers of the static list** learn the union: the `/model` picker's listing (originally the `/providers` command, since removed), the "no such model" suggestions, and — the real payoff — `context_window_tokens` resolves from discovery when the static entry omits it, so compaction tracks the model actually served instead of a number someone typed.

The one OpenAI-shape catalog implementation (`GET <url>/models`, read `context_length` / `meta.n_ctx_train` when present) ships beside `provider-openai` and covers llama.cpp, vLLM, and OpenRouter alike. Config knobs for it (model filters, refresh) ride the per-plugin config of 2.3 when that lands; none are required for v1 of the seam.

**Cost.** Low-moderate — the plugin half is small; the work is the `ProvidersConfig` lazy path and the four consumers. Independent of E and G.

## 3. Permission advisor — "auto" mode as a plugin

**Status: landed.** The composition below is implemented as specified: `AdvisedPermissionChecker` over the wire checker, substrate untouched, `permissions.json` `"advisor"`/`"advisor_inherit"`, sanity-check warning for an unresolvable name — the full test list lives with the tests, including the E2E through the real plugin loader and the three §3.3 invariant proofs. The one implementation addition is the `SessionChannels` extraction.

**The request.** A plugin that answers tool-permission prompts automatically — rules, an LLM judge, a remote policy service — falling back to a human only when it abstains.

**The line this draws.** `ARCHITECTURE.md` currently calls the permission model "core, and never plugin-replaceable: it is the substrate everything calls into." That stays true for the **substrate**: the mode lattice, precedence chains, out-of-workdir fold, subagent ceilings, run modes, and grant-set ownership remain core and unexposed. What becomes pluggable is narrower — **the decision source for `Ask`**: who answers when the substrate has resolved a mode to `Ask` and no grant satisfies it. Substrate core; decision pluggable. (§ *Permission model* there is amended to say exactly this.)

### 3.1 Why the interface is ready and the wiring is not

`PermissionChecker` is already a one-method virtual interface (`permission_checker.hpp`), consumed polymorphically by `ToolExecutor` (`tool_executor.cpp:47`) — the same shape as the fakes in `tests/support/`. The design explicitly anticipated alternative implementations.

The blocker is one member declaration: `Session` holds `rpc::WirePermissionChecker checker_;` as a **concrete by-value member** (`session.hpp:276`), constructed in its member-init list (`session.cpp`). There is no seam.

### 3.2 Design: an advisor, not a replacement

Do **not** let a plugin implement `PermissionChecker` outright. The wire checker is load-bearing (registration-before-publish ordering, cancel → Deny mapping, grant filtering); replacing it loses the human path, which is the one thing the permission gate exists to guarantee. Instead a second, narrower interface composes *above* it:

```cpp
// src/server/tools/permission_advisor.hpp

// Answers an Ask automatically, or abstains. nullopt falls through to the wire
// checker -- the human. An advisor therefore narrows what reaches a person; it
// can never widen what runs, because everything below it holds (§ 3.3).
class PermissionAdvisor {
public:
    virtual std::optional<PermissionResponse> Advise(const PermissionRequest&) = 0;
};

// plugin_abi.h
void minimal_plugin_register_permission_advisor(minimal::tools::PermissionAdvisorRegistry&);
```

Cancellation reaches the advisor the way it reaches every blocking component on the worker — an `is_cancelled` predicate delivered alongside the request (a small `AdvisorContext`, or one field on `PermissionRequest`; either is additive). Track J settles the shape.

Composition in `Session`, after the checker-injection change below:

```
Ask resolved by the substrate (modes, ceilings, run mode, grants)  -- unchanged
  → advisor registered AND enabled for this session?
      → Advise(request)
          Allow/Deny → done (grants filtered exactly as today)
          nullopt    → wire checker → the human
      → (advisor throws) → wire checker -- one plugin's failure is not a lost turn
```

### 3.3 Why the hard invariants already hold — the reason the cost is low

An advisor sits **below** `ResolveMode` and **above** the wire checker. Everything dangerous is enforced substrate-side, before the advisor is ever consulted:

| Invariant | Where enforced | Effect on an advisor |
|---|---|---|
| `AlwaysDeny` short-circuit | `tool_executor.cpp:175-178` | Never sees a denied tool at all |
| Policy + out-of-workdir fold | `tool_executor.cpp:47-100` | Cannot overturn `file_write: deny` |
| Subagent ceilings | `tool_executor.cpp:86-96` | A child's advisor cannot exceed its parent |
| Run mode (`AllowOnly`/`Bypass`) | `tool_executor.cpp:99` | Under `AllowOnly`, `Ask` → `AlwaysDeny` short-circuits *before* the advisor — an auto-approver cannot smuggle approvals into headless runs |
| Grant minting | Double filter: `tool_executor.cpp:199-205` + `proto_convert.cpp:493-498` | May grant only keys the disclosure offered — already guaranteed, no new code |
| Cancel semantics | Template exists: `wire_permission_checker.cpp` maps cancel/no-answer → Deny | Must map cancellation → abstain or Deny; never block through a cancel |

Zero changes to `clamp_run_mode`, `most_restrictive`, the precedence chains, or subagent spawn. The advisor inherits all of it by position.

### 3.4 What actually has to change

1. **Checker injection in `Session`** — the only structural change, with one prerequisite: `WirePermissionChecker` is built from `Session`'s `queue_`/`pending_`, so a *composed* checker cannot be built until those exist outside `Session`. They are extracted into a per-session `SessionChannels` block, created by the params producer and owned by `Session` through a `shared_ptr`. `SessionParams` gains `std::unique_ptr<tools::PermissionChecker> permission_checker` (null → `Session` constructs the wire default from the channels); producers compose `AdvisedPermissionChecker(advisor, wire)` when an advisor is configured. Small, mechanical, testable with the existing fakes.
2. **`PermissionAdvisorRegistry` + ABI symbol + loader wiring** — the pattern exists four times live and twice prepared-but-unwired (`plugin_abi.h:94-97`).
3. **Composition** — an `AdvisedPermissionChecker` decorator wrapping {advisor, wire checker}: try advisor, fall through on abstain or throw. `ToolExecutor` is untouched; it still sees one `PermissionChecker`.
4. **Config + selection.** Not a new `PermissionRunMode` — adding `Auto` to the ordered enum would touch `apply_run_mode`, `clamp_run_mode`, the wire enum, and subagent clamping for no gain over selection. Instead: `permissions.json` gains `"advisor": "<plugin-name>"`. The named advisor applies to **top-level sessions by default and to subagents only if opted in** (`"advisor_inherit": true`) — an auto-approver silently governing children violates the spirit of "a child is never more permissive than its parent" even though the ceilings technically hold. The startup sanity check reports an advisor naming no loaded plugin (warning, the `naming_model` pattern).

### 3.5 Contract obligations on the advisor

- **Blocking-safe by construction, cancel-aware by contract, budgeted by the host.** `Advise` runs off the session worker (a bounded check spins it up on its own thread) with `permissions.json` `advisor_timeout_ms` as its budget — default 30 s, `0` disables the budget and restores fully synchronous `Advise`. A well-behaved advisor answers inside the budget; one that hangs — an external decider that ignores both its cancellation predicate and the clock — costs one abandoned, self-contained thread, and the wire checker (the human) is prompted at the budget's edge. The wait polls the cancel source, so a cancel or `Terminate` abandons the wait before the budget is spent: a wedged advisor can never wedge the worker join, the reaper, or process shutdown. Without the budget, an advisor violating its contract would deadlock all three.
- **Owned values.** `PermissionRequest` arrives by value into the advise thread; an advisor must not stash references (the same rule as `SessionObserver` payloads — the worker's stack unwinds after the call).
- **Grants are advisory.** Returning granted keys is permitted — an auto-approver *should* silence future prompts, that is the point of auto mode — and the double filter makes it safe: only disclosed keys land.
- **Source compatibility.** The advisor interface is frozen surface the moment it ships; the same rebuild-guard model applies.

### 3.6 Difficulty

**Low-moderate.** The substrate needs nothing; the semantics dominate the code.

| Piece | Size |
|---|---|
| Checker injection in `Session` (`SessionParams` + member change) | small |
| Registry + ABI symbol + loader + `main.cpp` wiring | small (pattern exists ×4) |
| `AdvisedPermissionChecker` decorator + abstain composition | small |
| `permissions.json` `advisor` field + loader + sanity check | small |
| Selection semantics (subagent default-off, inherit opt-in) | moderate — decisions, not code |
| Tests: unit + integration + E2E over a real socket | moderate |

Depends on nothing unbuilt.

## 4. TUI: core vs. pluggable

Track E is building the client now; this section is the spec it leaves the seams against. It revises the `ARCHITECTURE.md` § *Client plugins* table in one direction: **more of the TUI is pluggable than that table allowed.** The principle is unchanged — the frame, the state machine, and the transport are the client's equivalent of core tools, and presentation is plugin surface — but presentation is most of the TUI, and most of it is now slotted.

### 4.1 What stays core

- **Frame + shell**: `app.cpp` assembly, `ResizableSplit` proportions, the modal stack, the tab model, tab states/attention *derivation from wire events*, ordering key, group lifecycle, navigation.
- **Transport + state**: `SessionClient` (demux by `session_id`, drain-before-`Finish`, attach obligations), `SessionState`/`AppState`, scrollback virtualization against `GetMessages`, the streaming buffer lifecycle.
- **Input**: the prompt-editor shell (multiline input, keyboard-protocol handling). Completion *content* is pluggable (see below); the editor itself is not.
- **Security UI**: the permission/confirmation modal — never pluggable, under any circumstance. The consent-scoping wording obligations are core-owned.
- **The plugin host**: registries, slot routing, fallback rendering when a slot's plugin is disabled.

The "write a different frontend" tier shrinks to exactly this list.

### 4.2 The slot surface

| Slot | Kind | Default |
|---|---|---|
| Tool-result renderer (keyed by tool name) | additive | generic collapsible row |
| Display-kind renderer (keyed by display-payload kind) | additive | core renderers for the core kinds |
| Message-kind presentation (agent/thinking/tool-activity/system) | additive | core defaults (§ *Message-kind rendering*) |
| Chat surface (the whole chat view) | **exclusive**, first registration wins | core chat view |
| Status-bar segments (keyed by segment id) | additive; **format string orders/selects** | core segments |
| Markdown engine | **exclusive** | md4c adapter |
| Syntax-highlight languages (keyed by language) | additive, collisions fatal | vendored grammars |
| Views (Panel/Modal) | additive | — |
| MentionResolver (server-side) | exclusive | `PrefixMentionResolver` — live: `SessionServiceImpl` resolves through `MentionResolverRegistry::Active()` |
| Attention source | additive | core event derivation |
| Theme sections (config) | namespaced roles | core roles |

Details for the new slots:

**Message-kind presentation.** Renderers keyed by message kind, same shape as `ToolResultRenderer`; core supplies the defaults the message-kind table already specifies. The data plane — event demux, virtualization, streaming buffering — stays core regardless of who renders a row.

Rows from an assistant message carry `ChatRow::agent` / `ChatRow::model` — the agent preset and `<provider>/<model>` that produced it, persisted per message (`StoredMessage.agent_preset`/`model`, schema v9; empty = unknown). `AssistantLabel()` formats the core speaker label (`build (sonnet-4-5)`). The synthetic `ChatRowKind::Switch` row sits before the first assistant message whose agent or model differs from the previous one's (`switch_from_agent`/`switch_from_model` hold the old pair); it is never collapsible, and a plugin may claim it like any other kind.

**Chat surface — the one exclusive client slot.**

```cpp
// src/client/plugins/chat_surface.hpp
class ChatSurface {
public:
    virtual const ChatSurfaceSpec& spec() const = 0;  // id (keybindings address it), title
    virtual ftxui::Component component() = 0;         // the tab body
    // The host pushes every ServerEvent for the session; the surface owns its
    // own presentation and buffering. Scrollback pages are fetched through the
    // same client API core uses -- exposed to client plugins as a utility. A
    // surface never opens its own connection.
    virtual void OnEvent(const v1::ServerEvent& event) = 0;
};
```

Exclusive because the interface is deliberately big — streaming lifecycle, scrollback, focus and attention integration — and this repo's rule is that an interface which grew that big needs a reason; a wholesale chat replacement is the reason, taken once. Message-kind slots cover the common case; this covers "a whole different chat UI". Second registration warns and is ignored (the `MentionResolver`/`AuthProvider` pattern); with none registered, the core chat view stands.

**Status-bar segments.**

```cpp
class StatusSegment {
public:
    virtual std::string_view id() const = 0;  // what {id} addresses in the format string
    virtual std::string Render() const = 0;   // text; color via theme roles, never hardcoded
};
```

The format string (`"{version} | {tokens} | {git_branch}"`) is the ordering/selection config; a plugin adds content, the user keeps layout control. An unknown id renders nothing with a startup warning — the `keybindings.json` pattern for names unknown at compile time.

**Markdown engine.** The md4c adapter becomes the default registrant of an exclusive slot. The contract carries the streaming semantics explicitly — complete-text render *and* the `AppendDelta`/`Finalize` lifecycle, whose debounce-and-finalize property ("partial input cannot soundly parse"; `Finalize()` always reparses authoritatively) is the **implementer's** obligation, not the host's.

**Syntax grammars.** The highlighter takes additive, language-keyed registrations; the vendored set registers as the defaults; two plugins claiming `python` is fatal (a `NameRegistry` kind).

**Attention sink.** Plugins receive an `AttentionSink` at registration and may raise/clear tab or global attention; core event derivation remains the default source and a plugin raise wins until cleared. The `!` glyph stays the only color-independent signal, whatever raised it.

**Theme sections.** `themes/*.json` gains optional namespaced sections:

```json
{ "name": "dark",
  "colors": { "...core roles..." },
  "sections": { "git_plugin": { "branch": "#7dd3fc" } } }
```

A section's roles are addressable only as `git_plugin.branch`, merged **after** core roles so a plugin can never shadow one; two sections with the same name, or roles naming no known plugin, are reported by the sanity check (the `naming_model` pattern).

**Tool-result display payloads.** A tool result carries its text (what the model reads) and, optionally, a display payload for clients: `ToolResult::display`, a JSON object `{"kind": "<kind>", ...}`. The tool owns *what* its result is; the client owns *how* it looks.

- **Never shown to the model.** Providers serialize `ToolResultBlock` fields by name and skip `display`; `anthropic_request_test.cpp` and `openai_request_test.cpp` enforce it.
- **Self-contained.** A renderer never parses the result text, which stays the model's input and the fallback for a client with no renderer for the kind. The one sanctioned exception is `markdown`, whose `text` may be omitted to mean "render the result text" (identical content, so a copy would buy nothing).
- **Dropped, never truncated.** Over `tools.max_display_bytes` (default 128 KiB) the agent loop drops the payload and the client shows the text; a cut diff would misrepresent the change.
- **Versioned by name.** An incompatible shape change gets a new kind (`diff2`); unknown fields are ignored.
- **Carried uninterpreted.** Persisted inside the message's `content_json`, sent as `ToolResultBlock.display_json` (a string, not a `Struct`, which would turn every number into a double).

A result row tries, most specific first: a tool-name `ToolResultRenderer`, then a `DisplayRenderer` for the payload's kind, then the default row. A renderer that throws is contained and the next tier draws the row. Kinds rather than tool names are the key because `plugins.json` aliases rename tools, and because kinds are shared: a third-party or MCP tool that emits a core kind gets the core rendering without shipping a client plugin.

Core kinds (v1):

| kind | fields | emitted by |
|---|---|---|
| `diff` | `path`, `created`, `hunks[]{old_start, old_count, new_start, new_count, lines[]{op, text}}`; `op` is `" "`, `"-"` or `"+"`, and an empty side's start is the line before the hunk | `edit` (str_replace, insert, create), `write` |
| `file` | `path`, `start_line`, `end_line`, `total_lines`, `truncated`, `lines[]` (raw, unnumbered) | `read`, `edit view` |
| `process` | `command`, `cwd`, `duration_ms`, `exit_code` or `signal` (neither when unknown), `cancelled`, `timed_out`, `output` (a tail of at most 16 KiB), `output_truncated` | `bash` |
| `markdown` | `text` (optional; absent means the result text) | `checklist`, `skill` |

A `diff` row starts expanded (it is the review surface for a change the model just made); every other tool row keeps the collapsed default.

### 4.3 Rulings from the architecture review

1. **Command enumeration is a wire gap.** `ListCommands` — unary, returning name/summary/usage/kind/display/read-only per live command. Fetched once at connect: plugins load at startup and config is read once, so the list cannot change mid-connection — no invalidation machinery. Without it, plugin commands are invisible to `/`-autocomplete.
2. **Redefining bundled tools is allowed, and rendering core tools is allowed.** Bundled tools are themselves plugins — disabling and replacing via `plugins.json` is the designed path — and a third-party renderer for a *core* tool (a diff view for `edit`) is as legitimate as one for a plugin tool. Duplicate registrations are fatal and alias-resolvable like every other name.
3. **Client plugins own their data path.** A plugin view wanting live data subscribes itself through the generic `PluginCall`/`PluginSubscribe` channel; core transports and never interprets plugin payloads.
4. **Plugin modals never preempt the permission modal.** Permission/confirmation prompts are always topmost; a plugin modal cannot mask one or steal its input. Invariant, tested.

### 4.4 Command results never enter the chat

**The ruling:** the results of `/` commands must not be rendered as chat messages and must not reach the model. `git tree` or a session list fed to an LLM makes no sense. One clarification of current state: command results are *already* wire-events only — appended to no history and sent to no provider, because commands bypass the agent loop by construction. The violation was client-side: output routed to "inline system message in chat". The fix is a display mode, plus a taxonomy refinement:

```cpp
enum class CommandDisplay { Status, View, Silent };
struct CommandResult {
    enum class Status { kOk, kError } status = Status::kOk;
    std::string message;
    CommandDisplay display = CommandDisplay::Status;  // Status: ephemeral line (errors too,
                                                      //   styled as errors — never persisted).
                                                      // View: open/refresh the view named by
                                                      //   view_hint. Silent: status bar only.
    std::string view_hint;  // a client view id; a pair plugin registers both sides
};
```

Taxonomy:

```
Command (the abstract -- already named Command; no rename)
├── HarnessCommand      -- never touches the conversation path; never enters the chat.
│                         Outcome: a view, a config change (status line), or an error
│                         (transient, never a chat row).
│                         The Harness/LLM boundary is the CONVERSATION PATH -- the turn
│                         queue and chat-completions requests -- not "network vs. no
│                         network": /model-info is a read-only HarnessCommand that does
│                         provider-adjacent HTTP and renders a view, safely beside a
│                         live turn.
└── LLMCommand          -- may issue provider requests or submit turns
    ├── direct          -- its own provider request (/compact); displays like a HarnessCommand
    └── PromptShorthand -- submits an interpolated prompt as a user turn (review, roast).
                          The ONLY command kind whose content reaches the model, and it
                          reaches it as the turn itself -- entering the chat exactly as a
                          normal user turn would, never as a result message.
```

`PromptShorthand` is its own subcategory precisely so a prompt macro is an honest thing to register, not a `HarnessCommand` that smuggles a turn. Finer inheritance under `Command` stays open.

**Invariants (each tested):**

- No command path ever appends a message to the store — except a prompt shorthand, which appends exactly the interpolated user turn.
- No `CommandResult` is ever rendered as a chat row or persisted to the session store.
- Subagent completion injections are **not** command results — they stay real messages, because the model must see them at the turn boundary.

`ListCommands` carries `kind`, `display`, and `read_only`, so the prompt editor can annotate autocomplete — including flagging prompt shorthands, the one kind that will submit a prompt.

## 5. Hooks — config-mapped actions on named points

**Status: landed.** ARCHITECTURE.md § *Hooks* is the design source of truth; this section records the surface and the worked example.

**The shape.** Core owns the mechanism (`src/server/hooks/`): a `HookDispatcher` that is both the point registry (registration-time, sealed at `Configure`) and the dispatch surface. `RegisterPoint({name, veto_capable, filterable})` returns a `HookPoint` handle the emitter keeps; `Emit(payload, is_cancelled)` is synchronous and returns the first block reason (nullopt = passed), `EmitAsync` posts to one ordered thread modeled on `SessionObserverRegistry`. Config-declared actions (`hooks.json`) map `/bin/sh -c` commands onto points: JSON payload on stdin, exit 0 pass / 2 block / anything else **fails open, enforced by the dispatcher**. Plugins register ADDITIONAL points — they never see user hook config, and the shell executor never sees plugin code.

**The eleven core points** (registered in the dispatcher constructor, reserved before plugin load): `pre_tool_use` (veto, from `ToolExecutor::RunGuarded` after the `AlwaysDeny` short-circuit and before grants/Ask), `user_prompt_submit` (veto, from the **worker at turn-pop** — never `SubmitTurn`, the reader must not block; top-level sessions only), `post_tool_use` and `stop` (via a core bridge over `SessionObserver`), `session_created`/`session_ended` (same bridge), `pre_compact`/`post_compact` (both the auto trigger and `/compact`, with a filterable `trigger` field), `subagent_started`/`subagent_completed` (the latter from the child's every-exit-path completion hook), and `permission_prompted` (fired the moment an Ask reaches the checker — the desktop-notification feed).

**The worked example: `tools-bash.before_command`.** Veto-capable, filterable on `program` — the disclosure-time RESOLVED program path (session `PATH` included), which is a finer grain than `pre_tool_use`'s tool name. The loader calls `register_hook_points` **before** `register_tools` precisely so the plugin can mint its handle and capture it into the tool at construction; what a Block MEANS is declared by the plugin (bash refuses the invocation) — core owns dispatch and fail-open, the plugin owns its events and their consequences.

**What the surface guarantees (each tested):**

1. Hooks only narrow: `AllowOnly` short-circuits before the hook seam; no hook can raise what the substrate denied.
2. Fail-open on every action failure mode; the permission substrate is untouched and remains the boundary.
3. Filters are exact equality on declared string fields; model-controlled input is never filterable; a pinning test freezes each point's field list.
4. Duplicate point names are fatal via `NameKind::HookPoint` — the attribution feed is `ResolveRegistrationNames`'s `hook_points` parameter.
5. A disabled plugin's points degrade to startup warnings on their actions; core points are unaffected.

**Deliberately deferred:** `pre_compact` veto (bricks the session as occupancy climbs), pre/post LLM-call points (payload = the whole request, prompt-cache-hostile), prompt rewriting and stdout-context injection, per-preset hook scoping.

## 6. Sequencing

| Item | Depends on | Status |
|---|---|---|
| 2.1 Tool history access | nothing | **done (track M)**; the tool-facing type is `session::HistoryWindow` |
| 2.2 Cross-plugin services | nothing | **done (track L)**; first consumer: `/restart-shell` |
| 2.3 Per-plugin config | nothing | **done (track K)** |
| 2.4 MCP lifecycle | track G | designed, waits on G |
| 2.5 Plugin UI surface | track E + 2.2 | **done (track E5)**; `PluginCall` dispatches to a `PluginEndpoint` (`<name>.endpoint`) or a `PluginRpcHandler` (`<name>.rpc`), `PluginSubscribe` to the `.endpoint` push half; the client half is the `ClientApi` in the registration context |
| 2.6 Model auto-discovery | nothing | **done (track N)**; lazy discovery via `ModelCatalogRegistry`, refresh via the `/model` picker's ctrl+r (originally `/providers refresh`, since removed), network-free sanity checks |
| 3 Permission advisor | nothing | **done (track J)** |
| 5 Hooks (track O) | nothing | **done (track O)**; core mechanism + 11 core points + `tools-bash.before_command` worked example |
| 4.2 TUI slots | E's frame first | **done (track E5)**; the full §4.2 slot table ships — additive views/tool-renderers/message-kinds/status-segments/grammars plus the exclusive chat-surface and markdown-engine slots, host `src/client/plugins/`, fixtures proving the loader through a real dlopen |
| 4.3 `ListCommands` | nothing (server half trivial) | **done (track E5a)**; unary, fetched once at connect, feeds `/`-autocomplete |
| 4.4 Command display modes | nothing | **done (track E5a/E5b)**; `CommandDisplay` on the wire, dispatcher downgrades errors to Status, results never enter the chat (`CommandNotice` is type-disjoint from `ChatRow`) |

Track order: **J → K → L → M → N**. K preceded L so the new `register_services` ABI symbol shipped with `PluginRegistrationContext` and never needed a second signature sweep; M and N are independent of everything and each other. 2.1–2.3 were independent of everything and small; 2.5's server half and 2.2 are the same mechanism — the registry was built once by L, and `PluginCall` wires to it when E lands the client half. N added the eleventh `extern "C"` symbol (`minimal_plugin_register_model_catalogs`); discovery reaches the config layer as an injected hook precisely so the config layer never links plugins. O (§5) added the twelfth (`minimal_plugin_register_hook_points`), additive with no signature sweep, and its `hooks.json` reaches the dispatcher the same way `permissions.json` reaches the permission model: config loads before plugins, the dispatcher is core, and the sanity check validates the file against the points both have registered. The mention-resolver registration (`E3`) completed the current set of twelve optional hooks.

## 7. Deferred ideas (recorded, not scheduled)

Carried over from the completed opencode-compatibility effort's deferral table; none is scheduled. Permission glob-pattern rules and per-turn model/agent overrides have since **landed** (`pattern_overrides` in CONFIGURATION.md; command frontmatter `agent:`/`model:`), as has session forking (`ForkSession`).

| Item | Note |
|---|---|
| File-edit snapshots / undo (`/undo` reverting tool effects) | conversation rewind/redo exists; a snapshot journal is the missing half |
| Share/export | local-first analogue: an export-to-markdown/HTML command; a share server is a bigger decision |
| LSP + formatters | natural fit: a server plugin + a `post_tool_use` hook |
| TUI diff rendering | the renderer *slot* exists (§4.2), no implementation ships |
| `@`-mention subagent invocation | prompt-editor autocomplete listing agent presets → `spawn_subagent` shorthand |
| `.rpc`-addressable computed templates | md placeholders calling any plugin's `.rpc` op (`{{checklist list}}`) — makes `.rpc` payloads a de-facto cross-plugin API; needs a stability note first |
| Typed `TemplateDataSource` registry | new ABI symbol + `NameRegistry` kind; only if computed templates become an ecosystem pattern |
| Models.dev-style catalog plugin | fits `ModelCatalogRegistry` (§2.6); a local-first design may prefer `discover: true` |
| Provider breadth & OAuth | beyond the two bundled chat providers; the `auth.json` credential chain covers static keys only |
| ACP/IDE/SDK surfaces | an editor-protocol endpoint over the same gRPC core; not planned |
| Plugin compatibility | opencode plugins are TS/Bun, minimal's are native C++ shared libraries — no bridge by construction (a sidecar or MCP shim would be the only path) |

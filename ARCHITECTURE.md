# minimal — Architecture

Local-first, memory/CPU-efficient coding-agent harness. C++ server/TUI-client split over gRPC. Distributed to a team and public on GitHub — not a single-user personal tool, which shapes the decisions marked *(distribution)* below.

**Product philosophy: build-it-yourself, customize-as-much-as-you-want.** Core owns *mechanisms* and the minimum default needed to function; nearly everything interesting ships as a **plugin using the same shape a third-party plugin would**. Read/write/edit/bash, the OpenAI provider, and the built-in commands are all bundled plugins, not privileged code paths.

## Goals

- Low memory/CPU footprint; must handle 512k–1M token conversation histories without holding full history in memory or re-rendering it.
- Read/write/edit/bash tools, MCP client, skills, agent presets ("declarations" — config bundles, not subagents), an extensible `/`-command system, and a native plugin mechanism on both server and client.

## Distribution

- **License: MIT.**
- **Server topology: local-only, one server per user.** Each team member installs and runs the binary on their own machine — no shared/remote server in v1. This is a *security* assumption, not just a deployment default: the bash tool, path confinement, and permission checker all assume the server acts on behalf of one trusted local user on their own filesystem. A shared/remote server (multiple people connecting to one instance) would need per-connection auth and a much stricter tool-sandboxing model, and is explicitly out of scope until revisited. A single local server does serve multiple concurrent clients.
- **Dependency management**: vcpkg manifest mode (`vcpkg.json`) for the well-known deps already in its registry — grpc, protobuf, curl, sqlite3, nlohmann-json, ftxui, yaml-cpp, md4c, tree-sitter (core), spdlog — so `cmake --build .` works for any contributor without bespoke local setup. `hkr04/cpp-mcp` isn't in the vcpkg registry (niche library); pull it via `FetchContent`/git submodule instead — same fallback for `md4c`/`tree-sitter` core if either turns out not to be present. The curated `tree-sitter-<lang>` grammar repos (see TUI rendering) are individually vendored via `FetchContent` regardless — they aren't vcpkg-packaged per-language.
- **Config paths must be platform-aware** — see Config section below.

## Repository layout

```
minimal/
  CMakeLists.txt              # options, vcpkg toolchain, proto codegen, subdirs
  CMakePresets.json
  vcpkg.json                  # manifest-mode dependencies
  ARCHITECTURE.md  LICENSE  .gitignore
  docs/                       # design notes beyond this file
  proto/minimal/v1/{common,session_service}.proto
  src/
    common/                   # shared by server AND client
      config_paths.{hpp,cpp}      # the client reads themes/ + keybindings.json too
      abi_tag.{hpp,cpp}           # both plugin loaders must compute an identical tag
      version.hpp
    server/
      agent/    # agent_loop, compaction, presets, provider_instance_cache
      llm/      # unified model, provider iface, http_client, sse_parser
      tools/    # Tool iface, registry, executor, permission model, path_confinement
      commands/ # Command iface, registry, dispatcher
      plugins/  # loader, plugin_abi.h, observer/mention/auth registries
      rpc/      # SessionService impl, pending_requests, auth_interceptor
      config/   # loaders, frontmatter, skill_registry, seeding, sanity_check
      mcp/      # cpp-mcp client wrapper, shared server processes
      session/  # SQLite store, migrations, SessionReader
    client/
      view/     # tab_bar, chat_view, prompt_editor, status_bar, session_picker, tools_panel
      render/   # markdown_renderer, streaming_markdown_view, syntax_highlighter, theme
      plugins/  # client loader, view/renderer registries
      session_client.{hpp,cpp}  app.cpp
    plugins/                  # bundled plugins, built in-tree, installed as MODULE libs
      tools-fs/  tools-bash/  provider-openai/  commands-core/
  tests/
    support/                  # FakeLlmProvider etc. -- exported for plugin authors
```

`common/` is narrowly justified rather than a catch-all: the client resolves the *same* config directory as the server (it reads `themes/*.json` and `keybindings.json`), and both plugin loaders must compute an identical ABI tag. Everything else stays server- or client-private.

## Testing

Catch2, deliberately without a mocking framework. **The design already makes fakes cheap** — `LlmProvider` is two methods, `PermissionChecker` / `ConfirmationBroker` / `MentionResolver` are one each — so hand-written fakes suffice, and that property is worth protecting: an interface that needs GMock to test is an interface that grew too big.

Four tiers, in value order:

1. **Pure logic**, no I/O or network: `path_confinement` (security-critical — `..`, symlink escape, encoded traversal, embedded NUL), the `most_restrictive` lattice and both permission precedence chains, `SseParser` frame-splitting across write-callback boundaries, frontmatter parsing, alias/collision resolution, compaction boundary snapping to tool-call pairs, occupancy-vs-cumulative token accounting.
2. **Agent loop against a scripted `FakeLlmProvider`**: the full tool-call cycle, retry-on-`is_retryable`, cancellation, compaction trigger, subagent blocking and non-blocking paths, `max_tokens` surfacing. Deterministic, no network.
3. **Tools against a temp `project_root`**: read/write/edit on a tmpdir; bash allowlist enforcement, metacharacter rejection, rlimit and timeout behavior.
4. **End-to-end**: server plus the headless `-p` client over a real gRPC channel on a unix socket. The `-p` client is the natural E2E harness — a second reason to build it before the TUI.

Not unit-tested: FTXUI render output (visual, low yield) and live provider calls.

`tests/support/` is a build target, not just test-local code, so **plugin authors can link the same fakes** — which matters when most functionality ships as plugins.

## High-level architecture

- **Server** (C++): owns the agent turn loop, MCP client (`hkr04/cpp-mcp`), tool-calling machinery, skills, LLM provider dispatch, session persistence, command dispatch, plugin loading.
- **TUI client** (C++, FTXUI): renders streamed conversation events; virtualizes the scroll window against the server's SQLite-backed session store rather than holding full history in memory.
- **Headless client** (`minimal -p`): a second, non-interactive client against the same service — see "Non-interactive client".
- **Transport**: gRPC. `proto/minimal/v1/{common,session_service}.proto` defines `SessionService`: a `stream ClientEvent -> stream ServerEvent` bidirectional `OpenSession` RPC carries user turns, streamed model output, and permission/confirmation/command round-trips, multiplexed on one connection per open session — bidi specifically because tool-permission gating and command confirmation both need to pause server-side execution mid-turn, ask the client a question, and block until answered. `PermissionRequest`/`ConfirmationRequest` are server-generated and carry a `request_id` the client echoes back. Scrollback (`GetMessages`, keyset-paginated by `seq`), session lifecycle (`ListSessions`/`CreateSession`), path suggestion (`SuggestPaths`), and the generic plugin channel (`PluginCall`/`PluginSubscribe`) are separate unary/streaming RPCs, deliberately outside the bidi stream so paging through a 512k–1M-token history never competes with live turn traffic. The client never sees `ChatRequest` (tools, system prompt, sampling parameters) — that's assembled server-side from the active agent preset each turn.
- **Project/binary name**: `minimal`.

## Concurrency model — `src/server/rpc/`

Foundational, and the thing every "blocks until answered" interface elsewhere in this doc silently depends on (`PermissionChecker::check`, `ConfirmationBroker::RequestConfirmation`, blocking `SpawnSubagentTool::execute`). Designing those components in isolation hid a guaranteed deadlock: gRPC's sync API gives one thread per RPC, so an agent loop running on the `OpenSession` handler thread would have `check()` block that thread waiting for a `PermissionResponse` that can only arrive via `Read()` — on the very thread it just blocked.

**Three threads per open session:**

| Thread | Owns | Blocking behavior |
|---|---|---|
| **Reader** — the gRPC handler thread itself | the `stream->Read()` loop | Never blocks on anything but `Read()`. Dispatches each `ClientEvent` and immediately returns to reading. |
| **Worker** — one per session | the agent loop, tool execution, compaction | Blocks freely: permission waits, LLM calls, bash subprocesses. |
| **Writer** — one per attached stream | `stream->Write()`, and is its **only** caller | Blocks on the outbound queue. |

gRPC permits one concurrent reader and one concurrent writer on a `ServerReaderWriter`, but **not** concurrent `Write()` calls. Three independent sources produce outbound events (agent loop, subagent completion, read-only commands running alongside a turn), so a single writer draining one queue is required — a mutex at each call site would be slower *and* easier to get wrong.

**The outbound queue belongs to the `Session`, not to the stream.** Events produced before a client attaches buffer instead of being dropped — this is what lets a subagent's loop start producing events before its tab attaches (see Subagents), and means the worker never has to know whether anyone is currently listening.

`OpenSession` attaches a writer to the named session's queue and flushes the backlog.

**Session lifetime**: stream close **terminates** the session — see "Session lifetime" below. There is no detached state in v1, which is why `PendingRequests` has no grace-timer machinery.

**Inbound dispatch** (reader thread — every branch is non-blocking):

| `ClientEvent` | Action |
|---|---|
| `UserChatTurn` | Push onto the session's turn queue; wake the worker. |
| `CommandInvocation` | Read-only `HarnessCommand` → execute inline on a short-lived task; anything else → queue for the worker at a turn boundary. |
| `PermissionResponse` / `ConfirmationResponse` | `PendingRequests::Resolve(request_id, choice)` — signals the blocked worker. |
| `CancelTurn` | Set the session's atomic cancel flag. |

```cpp
// src/server/rpc/pending_requests.hpp
class PendingRequests {
public:
    // Worker thread: publishes the outbound request, then blocks here until answered.
    // A client is present by construction (no detached sessions -- see Session lifetime),
    // so this waits indefinitely: a human may simply be away, and auto-denying their
    // work is worse than waiting.
    std::string AwaitResponse(const std::string& request_id);
    void Resolve(const std::string& request_id, std::string chosen_option);  // reader thread
};
```

**Cancellation** — `CancelTurn` sets one atomic flag, read in two places:

- `ToolContext::is_cancelled` — checked before each tool runs, and polled by long-running tools (bash);
- the stream sink, which is why **`StreamEventSink` returns `bool`** (`false` = abort) rather than `void`. `HttpClient`'s libcurl write callback returns a short byte count on `false`, aborting the transfer with `CURLE_WRITE_ERROR`. Deliberately *not* an exception — throwing through libcurl's C stack frames is undefined behavior.

**Tool dispatch is sequential in v1.** An assistant turn requesting five tool calls executes them one at a time. This costs wall-clock latency on parallel-friendly batches, but avoids a per-session thread pool, keeps permission prompts arriving one at a time (better UX than five simultaneous modals), and needs no ordering rules for interleaved `ToolExecutionEvent`s. `request_id` correlation is already in place, so parallel dispatch is a later change rather than a rewrite.

## Session lifetime — `src/server/agent/`

**Two outcomes, not three.** There is no detach in v1:

| Action | Compute | Transcript |
|---|---|---|
| **Terminate** — close a tab, or the stream drops | stopped | kept (SQLite) |
| **Delete** — session view, confirmed | stopped | removed |

Rationale: a detached state would be the sole reason `PendingRequests` needs attach/detach notification, a grace timer, and fail-closed-on-timeout semantics — machinery that exists only to answer "what happens to a permission prompt when nobody is listening?". **No session ever runs unattached**, so the question is unaskable rather than answered. It also removes the need for worker parking and, most significantly, **the daemon requirement**: with nothing outliving its client, the server is refcounted by attached clients and exits when the last one leaves — auto-spawned by the client, no idle-timeout tuning, no persistent background process.

What this gives up: "close the terminal, the agent keeps working, come back later." The transcript survives regardless; only the in-flight turn is lost. Purely additive to reintroduce later (an `EndSession{DETACH}` message slots back in), so it is deferred, not foreclosed.

**One carve-out.** The rule is *not* "unattached ⇒ terminate," because a subagent session emits events before its tab attaches. Terminate fires on stream drop **after having been attached**; a never-yet-attached session gets a short initial-attach timeout (~10 s) purely as a guard against a non-conforming client. Attaching to child sessions is a **client protocol obligation** (see Non-interactive client), so this is a safety net, not a designed flow.

**Termination reuses the cancellation path** (atomic cancel flag → `bool` stream sink aborts the provider request → tools observe `is_cancelled`), then joins and reaps the worker. No new mechanism.

- **Confirm when work is in flight.** Closing a tab mid-turn destroys running work, so it routes through the existing `ConfirmationBroker`; closing an idle tab needs no prompt.
- **Terminating a blocking subagent must unblock its parent** — the parent's `SpawnSubagentTool` is synchronously waiting and receives an error `ToolResultBlock` ("subagent terminated by user") rather than hanging.
- **Client quit** = closing every tab: terminates all attached sessions, with one confirmation if any have work in flight.

## LLM provider adapter — `src/server/llm/`

Provider-agnostic interface. v1 ships one implementation (OpenAI chat-completions shape) **as a bundled plugin** (`provider-openai`). The Anthropic Messages shape is deferred; the unified model was checked against it and needs only additive changes.

**Unified content model** — block-array content for every message, no `System`/`Tool` roles (Anthropic can't express either; OpenAI's are derived views the provider explodes/folds on serialize):

```cpp
enum class Role { User, Assistant };

struct TextBlock       { std::string text; };
struct ThinkingBlock    { std::string text; std::optional<std::string> signature; }; // reserved
struct ToolUseBlock     { std::string id, name; nlohmann::json input; };
struct ToolResultBlock  { std::string tool_use_id; std::vector<ContentBlock> content; bool is_error = false; };
using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock>;

struct Message { Role role; std::vector<ContentBlock> content; };

struct ToolDefinition { std::string name, description; nlohmann::json input_schema; };

enum class ToolChoiceKind { Auto, None, Required, Specific };
struct ToolChoiceSpec {
    ToolChoiceKind kind = ToolChoiceKind::Auto;
    std::optional<std::string> tool_name;   // set iff kind == Specific
};

struct ChatRequest {
    std::optional<std::string> system;
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    ToolChoiceSpec tool_choice;
    std::string model; std::optional<int> max_tokens; bool stream = false;
    nlohmann::json parameters = nlohmann::json::object();  // arbitrary sampling/generation knobs
        // (temperature, top_p, reasoning_effort, id_slot, ...) sourced from the resolved
        // providers.json entry -- opencode-style pass-through rather than a named C++ field
        // per parameter, since backends support different knobs and new ones shouldn't
        // require a code change.
};
struct Usage { int input_tokens=0, output_tokens=0, cache_read_input_tokens=0, cache_creation_input_tokens=0; };
struct ChatResponse { std::string id, model; std::vector<ContentBlock> content; StopReason stop_reason; Usage usage; };
```

**`parameters` merge order**: `parameters`'s keys are written into the outgoing JSON body first, then the structural fields (`model`, `messages`, `tools`, `tool_choice`, `stream`) are always (re)written from `ChatRequest`'s own members afterward, unconditionally overwriting anything of the same name. A misconfigured `providers.json` `parameters` object can tweak generation behavior but can never corrupt conversation/tool-calling mechanics.

**Streaming** — `StreamEvent` variant modeled at Anthropic's event granularity, not OpenAI's flatter chunk shape; the OpenAI provider absorbs the extra bookkeeping (reassembling tool-call fragments keyed by index) so a future Anthropic provider is a near-transliteration:

```cpp
using StreamEvent = std::variant<
    MessageStartEvent, TextDeltaEvent, ToolUseStartEvent, ToolUseInputDeltaEvent,
    ThinkingDeltaEvent, ContentBlockStopEvent, MessageDeltaEvent, UsageEvent, MessageStopEvent>;
using StreamEventSink = std::function<bool(const StreamEvent&)>;  // false = abort (see Concurrency)
```

**Provider interface** — deliberately two methods. An earlier `QueryExtension`/`ProviderExtension` capability-query layer was **deleted**: its only consumer (llama.cpp KV-cache slots) moved to a plugin, so the abstraction had no users left.

```cpp
struct ProviderConfig {
    std::string base_url, api_key, model_name;
    std::chrono::milliseconds connect_timeout{5000}, request_timeout{120000}, stream_idle_timeout{60000};
};

enum class LlmErrorCategory { Transport, RateLimit, ServerError, AuthError, RequestError, ParseError };
class LlmError : public std::runtime_error {
public:
    LlmErrorCategory category() const;
    std::optional<int> http_status() const;
    std::optional<std::chrono::milliseconds> retry_after() const;
    bool is_retryable() const;  // Transport/RateLimit/ServerError -> true
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual ChatResponse Complete(const ChatRequest&) = 0;
    virtual void StreamComplete(const ChatRequest&, const StreamEventSink&) = 0;
};
```

**Thread-safety contract** (load-bearing, since instances are shared across concurrent sessions): implementations must hold **no mutable state across calls** — one CURL handle per call, per-request stream-reassembly state local to the call. Anything else is a data race the instance cache would silently expose.

**Two registries, deliberately distinct** (they were previously conflated under one name):

```cpp
// src/server/llm/provider_factory_registry.hpp -- kind-string -> factory function.
// Plugin-registered; there is no ProviderKind enum. providers.json already carries
// `provider` as a string, so an enum would be a redundant translation layer that
// also couldn't express plugin-supplied providers.
class ProviderFactoryRegistry {
public:
    void Register(std::string kind, std::function<std::unique_ptr<LlmProvider>(const ProviderConfig&)>);
    std::unique_ptr<LlmProvider> Create(std::string_view kind, const ProviderConfig&) const;
};

// src/server/agent/provider_instance_cache.hpp -- constructed instances, shared across sessions.
class ProviderInstanceCache {
public:
    LlmProvider& GetOrCreate(std::string_view provider_entry_name);
private:
    std::mutex mu_;   // GetOrCreate mutates from multiple session workers
    std::unordered_map<ProviderCacheKey, std::unique_ptr<LlmProvider>> instances_;
};
```

`ProviderCacheKey` is the `providers.json` entry **name** plus a config-generation counter — entries are immutable at runtime, so this is sufficient, and it avoids hashing an API key into a map key. `parameters` is not part of the key: it lives on `ChatRequest`, populated fresh per call.

**Token accounting — two distinct quantities, never conflated:**

- **Context occupancy** (status-bar `used/max`, compaction trigger) = `last_turn.input_tokens + last_turn.output_tokens`. Each turn's `input_tokens` already contains the whole history, so *summing* across turns double-counts quadratically and would fire compaction far too early.
- **Cumulative totals** (genuinely additive) = running sum of `output_tokens`, plus summed `input_tokens` if cost display is added later.

Counts come from the backend's own reported usage; there is no separate tokenizer. Some OpenAI-compatible proxies report `0` — no fallback estimate is designed for that, flagged as a known gap.

**Transport**: no `openai-cpp` dependency (researched and rejected — blocking-only, no streaming, no typed message/tool modeling beyond raw JSON passthrough, unmaintained since April 2024, TLS verification appears off by default). Hand-rolled instead: `HttpClient` (blocking POST + streaming POST over libcurl) and `SseParser` (generic incremental SSE frame parser handling frames split across libcurl write-callback invocations). Both are exposed to plugins as consumable utilities. TLS verification left at libcurl defaults (on).

Retries are **not** performed inside the provider; it throws a classified `LlmError` and the agent loop owns backoff/max-attempts policy. A single error type with a category enum (not a hierarchy) crosses two boundaries cleanly: the retry decision, and translation into a gRPC error payload.

## Core tools — `src/server/tools/`

The `Tool` interface and its registry/executor are **core**; the concrete tools are bundled plugins (`tools-fs`, `tools-bash`).

```cpp
enum class ToolCategory { ReadOnly, FileWrite, Execute };
struct ToolSpec { std::string name; ToolCategory category; llm::ToolDefinition definition; };
struct ToolResult { bool is_error = false; std::string content_text; };

struct ToolContext {
    std::string session_id;
    std::filesystem::path project_root;
    Logger& logger;
    std::function<bool()> is_cancelled;
    std::function<void(std::string_view)> emit_output;  // incremental output -> ToolOutputDeltaEvent
};

class Tool {
public:
    virtual ~Tool() = default;
    virtual const ToolSpec& spec() const = 0;
    virtual ToolResult execute(const nlohmann::json& input, ToolContext& ctx) = 0;

    // Declarative opt-in: a tool whose input carries a filesystem path sets this true
    // and implements ExtractPath(). It never calls path_confinement itself for
    // classification. Bash leaves both at their defaults.
    virtual bool path_confinement_supported() const { return false; }
    virtual std::optional<std::string> ExtractPath(const nlohmann::json& input) const { return std::nullopt; }

    // Default: spec().category. Overridden only where one tool spans categories --
    // e.g. Edit's `view` reports ReadOnly though the tool's static category is FileWrite.
    virtual std::optional<ToolCategory> category_for(const nlohmann::json& input) const { return std::nullopt; }
};
```

`ToolExecutor::run()` is the only call site the agent loop uses: resolve category → resolve permission (below) → execute → wrap into `llm::ToolResultBlock`. Permission gating lives entirely outside each tool's code — `Tool::execute()` implementations never reference `PermissionChecker`.

**Shared utilities** (core, and exposed to plugins — a plugin tool must *call* these, never reimplement path safety):

- `path_confinement::confine(project_root, user_supplied, must_exist)` → `{absolute_path, ok, outside_project_root, error}`. Canonicalizes symlinks/`..` and *reports* whether the result escapes `project_root` rather than hard-rejecting; still rejects paths that cannot be safely resolved at all (symlink loops, embedded NUL, encoded traversal resisting canonicalization).
- `file_access::read_window(path, offset, limit, max_line_length)` — streams line-by-line, memory proportional to `limit` not file size. Shared by Read and Edit's `view`.
- `atomic_write::write_file(path, content)` — temp file + fsync + rename. Shared by Write and Edit's mutating commands.

**Read**: `{path, offset, limit}` → line-numbered windowed text + truncation footer (textual pagination hint, since the model only sees text).

**Write**: `{path, content}` → full create/overwrite; requires parent dir to exist (no silent mkdir).

**Edit**: matches Anthropic's built-in `text_editor_20250728` vocabulary — `view` / `create` / `str_replace` (exact match, error unless exactly one occurrence) / `insert` — so an Anthropic provider can later be handed the native schema-less tool without changing file-mutation semantics.

**Bash**: allowlist (not blocklist) of executables; shell metacharacters (`&& | ; \` $()`) rejected outright, never sanitized; no `sh -c` — commands are tokenized and exec'd directly via `posix_spawn`/argv. **Persistent session state (cwd + env only), stateless process-per-call**: persistent state is what makes operator-rejection tolerable UX-wise (so `cd`/`export` chain across calls) without reintroducing an interpreter that could smuggle metacharacters. `{"restart": true}` resets cwd/env, and `/restart-shell` — registered by the *same* plugin — does the same from the command side. Resource limits via `setrlimit` + `killpg` on timeout. **Streams stdout incrementally through `ToolContext::emit_output`** rather than buffering to `max_output_bytes` — the cap still bounds the persisted result. Every invocation logged (command, resolved argv or rejection reason, decision, exit code, duration).

That cwd/env map lives **inside the `tools-bash` plugin**, keyed by session id — core holds no bash state. The plugin therefore also registers a `SessionObserver` purely to implement `OnSessionEnded` and free the entry; without it the map would grow for the process lifetime. This is the canonical example of why plugin-owned per-session state needs a lifecycle signal.

## Permission model — `src/server/tools/`

Core, and never plugin-replaceable: it is the substrate everything calls into, not a strategy with alternatives.

```cpp
enum class PermissionDecision { Allow, Deny };
enum class PermissionMode { AlwaysDeny, Ask, AlwaysAllow };   // ordered: Deny < Ask < Allow
struct PermissionRequest { std::string session_id, tool_name, summary; ToolCategory category; nlohmann::json tool_input; };
class PermissionChecker { public: virtual PermissionDecision check(const PermissionRequest&) = 0; };  // blocks

// The lattice meet. allow+ask=ask, ask+deny=deny, deny+allow=deny.
PermissionMode most_restrictive(PermissionMode a, PermissionMode b);

struct PermissionPolicy {
    PermissionMode read_only = PermissionMode::AlwaysAllow,
                   file_write = PermissionMode::Ask,
                   execute = PermissionMode::Ask,
                   out_of_workdir = PermissionMode::Ask;
    std::unordered_map<std::string, PermissionMode> per_tool_overrides;  // highest precedence
    PermissionMode mode_for(const std::string& tool_name, ToolCategory category) const;
};

struct McpPermissionPolicy {   // server-scoped, not per-tool
    std::unordered_map<std::string, PermissionMode> per_server;
    PermissionMode default_mode = PermissionMode::Ask;
    PermissionMode mode_for(const std::string& server_name) const;
};
```

**Composition order in `ToolExecutor::run()`** — out-of-workdir is a *second dimension*, not a category value, and folds in with `most_restrictive`:

1. **Is this tool allowed at all?** MCP tools (identified by the `mcp__` name prefix) resolve via `McpPermissionPolicy::mode_for(server_name)`; built-ins via `PermissionPolicy::mode_for(tool_name, category)`.
2. **Then**, only if permitted *and* the tool is `path_confinement_supported()` *and* its resolved path escapes `project_root`, fold in `out_of_workdir` via `most_restrictive`.
3. **Then** `apply_run_mode()`.

Step 2 must be a *fold*, not a substitution. If `out_of_workdir` replaced the tool's category, then `file_write: deny` + `out_of_workdir: allow` would **permit** writing `/etc/foo` while **denying** `./foo.cpp` — exactly inverted.

**Bash is excluded from out-of-workdir gating.** There is no reliable way to determine which paths an arbitrary shell command touches without parsing shell semantics. Bash's allowlist + operator-rejection model already permits reaching outside `project_root` via absolute paths or `..` in arguments (only `cd` targets are confined) — an accepted asymmetry, not an oversight.

**Permission run mode** — session-level, orthogonal to `PermissionPolicy`, wrapping its output:

```cpp
enum class PermissionRunMode { AllowOnly, Normal, Bypass };  // least to most permissive
PermissionMode apply_run_mode(PermissionMode base, PermissionRunMode run_mode);
// Bypass    -> always AlwaysAllow, ignoring `base`.
// AllowOnly -> base's AlwaysAllow/AlwaysDeny unchanged; base's Ask becomes AlwaysDeny.
// Normal    -> base unchanged (Ask still blocks on PermissionChecker).
```

**`AllowOnly` exists specifically to make `Ask` safe non-interactively** — without it a headless run hitting an `Ask`-gated tool blocks forever waiting for a human who was never attached; `AllowOnly` auto-denies instead of hanging. `Bypass` ("trust everything") and `AllowOnly` ("never block, never silently risk it either") are different risk postures, not gradations. Set via `permissions.json`'s `default_run_mode` and mutable per-session via `/permission-mode`, which requires confirmation.

## `/`-command system — `src/server/commands/`

Harness-level control operations dispatched from the client over the bidi stream and executed as code — they bypass the agent turn loop and the LLM entirely, except `LLMCommand`s which deliberately touch the provider.

```cpp
struct CommandArgs { std::vector<std::string> positional; std::unordered_map<std::string,std::string> flags; std::string raw; };
struct CommandResult { enum class Status { kOk, kError } status = Status::kOk; std::string message; };

class Command {
public:
    virtual std::string_view Name() const = 0;
    virtual std::string_view Summary() const = 0;
    virtual std::string_view Usage() const = 0;
    virtual bool RequiresConfirmation() const { return false; }
    virtual CommandResult Execute(CommandContext&, const CommandArgs&) = 0;
};

class HarnessCommand : public Command {};  // never touches LlmProvider; read-only ones may
                                            // run concurrently with an in-flight turn
class LLMCommand : public Command {};       // issues a real provider request; requires session idle
```

`CommandRegistry` holds both uniformly. `/help` lists live from `Name()`/`Summary()`/`Usage()` — no config-directory manifest, since a command's value is the code it runs and a data-only manifest would be a drift-prone shadow copy. `CommandDispatcher`: parse → busy-check → confirm if required → execute → emit result.

**Confirmation** reuses a transport-level primitive rather than `PermissionChecker` itself (tool permission requests carry LLM-originated tool input; command confirmations don't fit that shape):

```cpp
struct ConfirmationRequest { std::string kind, title, detail; std::vector<std::string> options; };
struct ConfirmationResponse { std::string chosen_option; };
class ConfirmationBroker { public: virtual ConfirmationResponse RequestConfirmation(const ConfirmationRequest&) = 0; };  // blocks
```

Both `PermissionChecker`'s implementation and `CommandDispatcher` compose the same `ConfirmationBroker` — the one place tool and command plumbing intentionally share code (the transport primitive, not the domain interface).

**`CommandContext`** borrows references the session already owns: active `LlmProvider` and its resolved `ProviderConfig`, `SessionStore`, `AgentPresetRegistry` (+ active preset, mutable for `/agent`), `Settings`, plus `EmitStatus`/`EmitError`. It deliberately does **not** expose bash session state: bash is a plugin and owns that itself (see below).

**Bundled commands** (`commands-core` plugin): `/new [name]`, `/attach <session>`, `/sessions`, `/agent <preset>`, `/model <name>`, `/permission-mode <normal|bypass|allow-only>` (confirm), `/compact` (`LLMCommand`), `/clear` (confirm), `/providers`, `/help [command]`.

**A plugin owns the commands that touch its own state.** `/restart-shell` lives in `tools-bash`, not `commands-core`, because bash's per-session cwd/env lives inside that plugin — one `.so` cannot reach into another's state. Registering a tool *and* its associated command from the same plugin is the general pattern, and the reason `minimal_plugin_register_tools` and `..._register_commands` are independent optional symbols rather than a single "kind" of plugin.

### Name collisions and aliases

**Collisions are fatal.** Two plugins registering the same tool name, command name, `ViewSpec::id`, or `ToolResultRenderer::tool_name` halt startup with a diagnostic naming both. Silent shadowing is the wrong failure mode — it makes behavior depend on plugin load order.

Resolution is disabling one plugin, rebuilding, or **aliasing** via an optional `plugins.json` field:

```json
"command_aliases": { "bash_over_ssh.bash": "ssh_bash", "commands-core.compact": "c" }
```

Alias keys are always fully qualified, so they are unambiguous even *before* collision resolution; aliases are applied at registration, **before** the collision check, so aliasing is itself a valid way to resolve one. This is not the command manifest rejected above: that would have *declared* commands as data (a shadow of code, free to drift). This *overrides metadata of code-registered entries*, and a key resolving to nothing is detectable at load.

**Three naming levels — this is what keeps a rename from silently breaking a permission rule:**

| Level | Value | Changes? |
|---|---|---|
| **Stable identity** | `plugin.original_name`, e.g. `bash_over_ssh.bash` | Never |
| **Surface name** | the alias if one exists, else the original name — what the model sees for a tool, what the user types for a command; must be unique | On alias |
| **Permission key** | the *non-alias*: bare `bash` when unambiguous, `plugin.original_name` when two plugins provide that name | Never |

```json
"per_tool_overrides": { "bash": "ask" }                        // no collision
"per_tool_overrides": { "bash_over_ssh.bash": "ask",            // two plugins provide `bash`
                        "tools-bash.bash": "deny" }
```

`per_tool_overrides`, `allowed_tools`, and `allowed_skills` **never reference an alias**, so renaming for typing convenience cannot detach a rule from what it was meant to govern.

**Replacing a bundled tool needs no new mechanism**: set `tools-bash` to `enabled: false` and ship a plugin registering `bash`. Permission config keyed `bash` continues to apply to the replacement. The fatal-collision rule is precisely what makes this an explicit act rather than an accident.

## Server plugins — `src/server/plugins/`

Native `.dylib`/`.so`, loaded into the server process. No sandboxing is possible via `dlopen` — plugins run as trusted native code with full process privileges; this is disclosed, not hidden.

**Source compatibility, not binary compatibility.** Plugins are compiled from the same tree with the same pinned toolchain as the binary, so the contract is source-level: an interface change is a compile error you fix on rebuild, not a silent break. The ABI tag is therefore a *"you forgot to rebuild"* guard, not a compatibility promise — which is what makes shipping core functionality as plugins safe even while `Tool` and `LlmProvider` are still evolving.

- Host and plugin bake an ABI tag (compiler id+version + stdlib, computed at build time). Plugin exports `extern "C" const char* minimal_plugin_abi_tag()`; mismatch → refuse to load, log, don't crash. With a matching tag, real C++ types cross the boundary directly — no C-only shim needed.
- *(distribution)* Ship a pinned reference toolchain (the CI build config doubles as the "build plugins against this" spec) so contributors aren't guessing which compiler/stdlib produces a loadable plugin.
- *(distribution)* No remote plugin installation or registry in v1; discovery is local-directory-only.
- `dlopen(path, RTLD_NOW | RTLD_LOCAL)`, held for process lifetime; no hot-unload in v1. `RTLD_LOCAL` governs whether the *plugin's* symbols join the global namespace — it does not stop the plugin resolving *host* symbols, which is what the next point relies on.
- **A plugin must not link the core static library.** It takes core's headers only and resolves host symbols at load time from the already-loaded server image. Linking `minimal_server_core` into both the host and each plugin would give every plugin its own copy of every global — separate registries, separate logger, separate everything — which fails at runtime in ways that look nothing like a linkage bug. This requires the host to export its symbols (`ENABLE_EXPORTS`, i.e. `-rdynamic` on ELF) and the plugin to tolerate undefined symbols at link time (`-undefined dynamic_lookup` on Mach-O; the ELF default already permits it).

**Discovery and enablement split by provenance:**

- **Bundled** — built in-tree, installed to `$PREFIX/lib/minimal/plugins/`, found relative to the executable → default **enabled**.
- **User** — dropped into the config dir → default **disabled**, opt-in via the TUI.

`plugins.json` records provenance per entry (`[{file, provenance, enabled}]`) so the rule is mechanical rather than a special case. "Came out of this build" vs. "someone dropped this in" is a real trust distinction, not an exemption.

**Startup ordering is load-bearing**: **config load → plugin load → sanity check → serve.** Providers and tools are plugin-registered, so "at least one usable provider" cannot be evaluated before plugins are in.

**Extension points** — each is one optional `extern "C"` symbol; the loader `dlsym`s for each known name and calls it only if present, so symbol presence *is* the capability declaration:

```cpp
// src/server/plugins/plugin_abi.h
extern "C" {
  const char* minimal_plugin_abi_tag();
  const char* minimal_plugin_name();
  const char* minimal_plugin_version();

  // --- Additive: many plugins may each contribute ---
  void minimal_plugin_register_tools(ToolRegistry&);
  void minimal_plugin_register_commands(CommandRegistry&);
  void minimal_plugin_register_provider_factories(ProviderFactoryRegistry&);
  void minimal_plugin_register_session_observer(SessionObserverRegistry&);

  // --- Exclusive: one global choice. First plugin (by plugins.json order) wins;
  //     a second registration logs a warning and is ignored. ---
  void minimal_plugin_register_mention_resolver(MentionResolverRegistry&);
  void minimal_plugin_register_auth_provider(AuthProviderRegistry&);
}
```

The ABI header has **two halves**, and both are frozen surface:

- **Extension points** plugins *implement*: `Tool`, `Command`, provider factory, `SessionObserver`, `MentionResolver`, `AuthProvider`.
- **Utilities** plugins *consume*: `path_confinement`, `file_access`, `atomic_write`, `HttpClient`, `Logger`, `SessionReader` (read-only history access), the frontmatter parser.

### `SessionObserver` — observe and react

Every other extension point is "provide an implementation." This is the one category for *reacting to harness events* — session naming, turn-completion notifications, git auto-commit, usage tracking, external audit sinks.

```cpp
struct SessionInfo {
    std::string session_id, parent_session_id, name, agent_preset;
    std::filesystem::path project_root;
};
struct TurnSummary {
    int64_t first_seq, last_seq;        // fetch content via SessionReader if needed
    StopReason stop_reason;
    Usage usage;
    std::chrono::milliseconds duration;
    std::vector<std::string> tool_names_invoked;
};
struct ToolInvocation {
    std::string tool_use_id, tool_name, summary;
    bool is_error;
    std::chrono::milliseconds duration;
};

class SessionObserver {
public:
    virtual ~SessionObserver() = default;
    virtual void OnSessionCreated(const SessionInfo&) {}
    virtual void OnTurnCompleted(const SessionInfo&, const TurnSummary&) {}
    virtual void OnToolExecuted(const SessionInfo&, const ToolInvocation&) {}
    virtual void OnSessionEnded(const SessionInfo&) {}
};
```

**`OnSessionEnded` is not optional garnish.** Any plugin holding per-session state leaks it forever without one — `tools-bash` holding cwd/env is the concrete case, since sessions terminate and nothing else would tell the plugin.

**Every payload is an owned value, never a reference.** Observers run on a detached task, so a `const std::vector<Message>&` would dangle. That is also why payloads are deliberately small: rather than copying turn content for every observer that doesn't want it, observers get **`SessionReader`** in the utilities half of the ABI (read-only, the same path `GetMessages` uses) and query by seq range when they need content. The session-titling observer reads seq 0 through it, instead of the payload special-casing "first user message".

**Hard constraint: observers must never wedge a turn.** They return `void` and cannot veto — vetoing is `PermissionChecker`'s job — and they are dispatched **fire-and-forget onto a thread pool**, never awaited. A hung observer leaks a pool thread; it cannot block the session worker. (Deliberately *not* "run with a timeout": a detached `std::thread` cannot be interrupted in C++, so a timeout there would be decorative. Leaking a thread is the honest failure mode, and it preserves the guarantee that matters.)

### `MentionResolver` and `AuthProvider`

```cpp
class MentionResolver {
public:
    virtual std::vector<std::string> Suggest(std::string_view prefix,
                                              const std::filesystem::path& project_root,
                                              int limit) const = 0;
};
```

Core default: `PrefixMentionResolver` — **case-sensitive prefix match, listing exactly one directory** (the deepest complete component in the prefix) and filtering its entries, precisely like bash path completion. It never walks recursively: a repo with `node_modules`/`.git` would make every debounced keystroke expensive, contradicting the low-CPU goal. Descent happens only when the user types `/`. A plugin can replace this wholesale — fuzzy matching is the obvious candidate, and is deliberately *not* built in.

```cpp
class AuthProvider {
public:
    // Called once per incoming connection, before any SessionService method runs.
    // false -> gRPC UNAUTHENTICATED.
    virtual bool Authenticate(const grpc::ServerContext&) const = 0;
};
```

Default is no authentication, matching the local-only topology. `src/server/rpc/auth_interceptor.{hpp,cpp}` delegates to a registered `AuthProvider` if any, else passes everything through. A shared-token implementation ships as an example plugin, keeping the auth-free default the actual default. This does **not** reopen shared/remote-server support.

### Core vs. plugin surface

| Subsystem | Placement | Why |
|---|---|---|
| Permission model, config + frontmatter loader, agent/subagent loops, tool-calling machinery, MCP client, session storage, gRPC transport, concurrency | **Core** | Mechanisms everything else calls into — the stable foundation, not customization points. |
| `path_confinement` / `file_access` / `atomic_write` / `HttpClient` / `Logger` | **Core, exposed to plugins as utilities** | A plugin tool must call these, never reimplement path safety. Inbound ABI surface. |
| `read`/`write`/`edit` (`tools-fs`), `bash` (`tools-bash`) | **Bundled plugins** | Not structurally privileged. Bash is split out: distinct security profile, and the one with a plausible alternate implementation (container/remote executor) someone would actually swap. |
| OpenAI provider (`provider-openai`) | **Bundled plugin** | Two-method interface; the obvious thing to fork. |
| Built-in `/`-commands (`commands-core`) | **Bundled plugin** | One plugin, not nine. |
| llama.cpp KV-cache commands (`llamacpp-kv`) | **Deferred to v1.x+** | Retained below as the worked example that drove the plugin design, not shipped in v1. |
| `spawn_subagent` tool | **Core** | Entry point to the core subagent loop; making it a plugin means exposing session *creation* across the ABI to relocate a thin wrapper. |
| `SkillTool` | **Core** | A thin accessor over the core md loader + skill registry. |
| `PrefixMentionResolver` | **Core default**, plugin-overridable | The minimum default needed to function. |
| Themes, skills, agent presets, prompts | **Not plugins — config** | Pure data; a plugin would add ABI/build complexity for something that is already just a file. |
| Syntax-highlighting grammars | **Fixed vendored set** | Runtime-loadable grammars are plausible future work. |

### `llamacpp-kv` — the worked example (deferred to v1.x+)

Not shipped in v1, but recorded because it is what drove the plugin design and it is the template for any provider-adjacent extension.

llama.cpp speaks the OpenAI API, so it needs no dedicated provider; only its non-standard `POST /slots/{id}?action=save|restore|erase` is special. Making this a plugin **deleted an abstraction** (`ProviderExtension`/`QueryExtension`) rather than fixing it, and dissolved a real bug along the way: a per-provider `default_slot_id` combined with a shared provider instance meant every session thrashed slot 0, and `/save-session` from one session could snapshot another's cache.

The plugin would read `base_url`/`api_key` from `CommandContext`'s resolved `ProviderConfig` and issue its own HTTP calls via the exposed `HttpClient`. **It needs no global config**: the save directory is a llama.cpp server-side flag (`--slot-save-path`), the filename is the command argument, the endpoint comes from the active provider. Slot pinning, if wanted, is `parameters: {"id_slot": N}` in a `providers.json` entry — the existing pass-through already carries exactly this class of knob.

Two residual notes, both now visible configuration rather than hidden bugs. Two sessions sharing one provider entry share its slot and will evict each other — use a second entry. And compaction rewrites the prefix a saved cache was built from, which makes a restored slot **less useful, not incorrect**: llama.cpp prefix-matches and recomputes the divergent tail. There is no correctness obligation for `/compact` or `/clear` to invalidate anything, which is why this plugin needs no cross-plugin notification hook.

## Client plugins — `src/client/plugins/`

The source-compatibility argument applies to the client exactly as to the server (same tree, same toolchain, pinned FTXUI), so passing `ftxui::Component` (a `shared_ptr<ComponentBase>`) across the boundary is no worse than the `unique_ptr<Command>` already passed server-side. Same ABI tag, same provenance-based enablement.

**A UI plugin is usually a *pair*** — a server-side data source plus a client-side view. Adding a bespoke core RPC per plugin would defeat the purpose, so there is exactly one generic channel:

```proto
message PluginCallRequest  { string plugin_name = 1; string method = 2; string json_payload = 3; }
message PluginCallResponse { string json_payload = 1; }

rpc PluginCall(PluginCallRequest) returns (PluginCallResponse);
rpc PluginSubscribe(PluginCallRequest) returns (stream PluginCallResponse);
```

Core defines neither payload nor semantics; plugin authors define their own JSON on both ends.

**Additive views: yes. Compositional: only through named slots.**

| Kind | Mechanism | Support |
|---|---|---|
| New self-contained view (panel or modal) | register a `View` — placement + default keybinding, remappable | **v1** |
| Fill a named slot | slot registry, small enumerated set | **v1 for renderers**, rest later |
| Wrap/replace core views, restructure the frame | — | **No** — write a different client |

Slots, in demand order:

1. **Tool-result renderers keyed by tool name — v1.** The strongest case in a plugin-first harness: a plugin shipping a `sql_query` tool wants a table, a `diff` tool wants colored hunks, not raw text. The plugin owns both the tool and its rendering. A renderer registered for a tool whose plugin is disabled falls back to default rendering rather than erroring.
2. **Status-bar segments — later.** The format string is already a slot mechanism; `{git_branch}` resolving to a plugin-registered provider is nearly free.
3. **Tools-panel / session-picker columns — later.**

```cpp
// src/client/plugins/view.hpp
enum class ViewPlacement { Panel, Modal };
struct ViewSpec {
    std::string id;                  // stable; how keybindings.json addresses it
    std::string title;
    ViewPlacement placement;
    std::string default_keybinding;  // user-remappable
};
class View {
public:
    virtual ~View() = default;
    virtual const ViewSpec& spec() const = 0;
    virtual ftxui::Component component() = 0;
};

// src/client/plugins/tool_result_renderer.hpp
class ToolResultRenderer {
public:
    virtual ~ToolResultRenderer() = default;
    virtual std::string_view tool_name() const = 0;
    // Takes the tool *input* as well as its result -- a diff renderer needs the
    // file path, not just the hunk text.
    virtual ftxui::Element Render(const nlohmann::json& tool_input,
                                   std::string_view result_text, bool is_error) const = 0;
};

// src/client/plugins/client_plugin_abi.h
extern "C" {
  const char* minimal_client_plugin_abi_tag();
  void minimal_client_plugin_register_views(ViewRegistry&);
  void minimal_client_plugin_register_renderers(ToolResultRendererRegistry&);
}
```

Both registries follow the server's collision rules: duplicate `ViewSpec::id` or `tool_name()` is fatal and alias-resolvable, and `keybindings.json` addresses views by their canonical id, never an alias. A renderer registered for a tool whose plugin is disabled falls back to default rendering rather than erroring.

**Consequence:** `keybindings.json` and the status-bar format string become **load-bearing plugin infrastructure**, not just user conveniences — they are how plugin-registered views and segments are addressed, so their schemas must accommodate names that don't exist at compile time.

**Caveat on the obvious first examples:** an "MCP server view" and an "agent context info view" are both views over **core** data (the MCP client and token accounting are core). Neither needs the plugin machinery — they need a core RPC plus a core view, which is far cheaper. The plugin system earns its keep when the *data* is third-party too.

## TUI rendering — `src/client/render/`

`MarkdownFTXUI` was considered and rejected — unmaintained. Instead **md4c** (`mity/md4c`, MIT, SAX-style C Markdown parser with GFM extensions, no transitive dependencies), with a hand-rolled adapter mapping its parse events onto `ftxui::Element`s — the same call made for `openai-cpp`: own the boundary rather than depend on someone's thin integration layer.

```cpp
// src/client/render/markdown_renderer.hpp
class MarkdownRenderer {
public:
    // md_parse() with MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS.
    // enter_block/leave_block/enter_span/leave_span/text callbacks build ftxui::Elements.
    ftxui::Element Render(std::string_view markdown_text) const;
};
```

**Streaming**: md4c has no incremental parse API, and more fundamentally *cannot* soundly parse partial input — `**bold` can't be interpreted before its closing `**` arrives, so chunk-parsing would produce wrong output, not just slower output. Resolved with a debounce:

```cpp
// src/client/render/streaming_markdown_view.hpp
class StreamingMarkdownView {
public:
    void AppendDelta(std::string_view text);  // O(1) amortized, per TextDeltaEvent
    void Finalize();                            // ContentBlockStop/MessageStop
    ftxui::Element Render() const;
private:
    std::string accumulated_text_;
    ftxui::Element cached_render_;
    std::chrono::steady_clock::time_point last_reparse_;
    bool finalized_ = false;
    static constexpr auto kReparseDebounce = std::chrono::milliseconds(120);
    // Re-parses only if !finalized_ && elapsed >= debounce; Finalize() always does one
    // authoritative reparse, replacing artifacts from mid-stream unclosed spans.
};
```

Scrollback (`GetMessages` results) never touches `StreamingMarkdownView` — those messages are complete, so each gets one `MarkdownRenderer::Render()`.

**Syntax highlighting**: **tree-sitter** core plus a curated grammar set (`cpp`, `python`, `javascript`/`typescript`, `bash`, `json`, `yaml`, `rust`, `go`). A hand-rolled regex tokenizer was considered and rejected: regex cannot soundly handle nested/context-free constructs (Rust nested block comments, C++ raw strings, JS template-literal interpolation), so it fails exactly where correctness matters, and every language becomes a bespoke ruleset to keep debugging — with visibly ugly failures, since this is user-facing output. tree-sitter's cost is front-loaded and mechanical: each grammar repo ships a generated `parser.c`, so vendoring means compiling a small C file, not running the tree-sitter CLI/Node toolchain. Per-language work is one node-type → category table.

```cpp
enum class HighlightCategory { Keyword, String, Comment, Number, Function, Type, Operator, Plain };

class SyntaxHighlighter {
public:
    // Returns plain text if `language` isn't in the curated set.
    ftxui::Elements Highlight(std::string_view code, std::string_view language) const;
};
```

The highlighter never hardcodes a color; the theme supplies one per `HighlightCategory`, keeping highlighting and theming separate.

**Themes**: `themes/*.json`, bundled `dark.json`/`light.json`, seeded like other config. Flat role/category → truecolor RGB:

```json
{
  "name": "dark",
  "colors": {
    "user_message": "#7dd3fc", "assistant_message": "#e5e7eb", "thinking": "#6b7280",
    "system_message": "#a1a1aa", "error": "#f87171", "attention": "#fbbf24",
    "border_focused": "#7dd3fc", "border_unfocused": "#3f3f46", "status_bar_bg": "#18181b",
    "syntax_keyword": "#c084fc", "syntax_string": "#86efac", "syntax_comment": "#71717a",
    "syntax_number": "#fbbf24", "syntax_function": "#7dd3fc", "syntax_type": "#fde047",
    "syntax_operator": "#e5e7eb"
  }
}
```

`src/client/render/color_downsample.{hpp,cpp}` maps truecolor to the nearest 256-/16-color entry, checked once at startup against `COLORTERM`/`TERM` — themes are authored once and degrade automatically. Bundled-theme contrast is validated against both light- and dark-background terminals: a color picked against one silently breaks on the other.

## TUI layout — `src/client/`

**FTXUI capability note** (verified against `component.hpp` / `component_options.hpp`): `Modal`, `Dropdown`, `Window` (titled + resizable), `ResizableSplit`, `Collapsible`, `Container::Tab`, `Menu`, `Toggle`, and `Hoverable` all exist, and `InputOption::multiline` exists and defaults to true. `Collapsible` implements the collapsed-by-default thinking/tool-activity design directly; `Window` gives the running-tools panel its title and resize; `ResizableSplit` gives layout proportions.

**A tab is a session. Nothing else.** Tabs are not the universal container: session tabs are dynamic, plural, and carry state/attention semantics, while utility views are singletons with no meaningful "thinking"/"waiting" state. Differentiate by usage pattern:

| Surface | Mechanism | Why |
|---|---|---|
| Session (chat + prompt) | **Tab** (`Container::Tab`) | Persistent, plural, stateful |
| Session picker | **Modal** | Transient — open, choose, dismiss |
| Permission / confirmation prompt | **Modal** | Transient, demands an answer |
| Running-tools view | **Panel** (`ResizableSplit`) | A *monitor* — wanted visible *while* working |
| `/help`, `/providers` output | inline system message in chat | already how `CommandResult` renders |

Frame: `tab bar / active session view / optional panel / status bar`, plus a modal layer. There is no separate "agent view" — that *is* the tab.

```
+----------------------------------------------------+
| main  explore  build  api-fix  test                 |
+----------------------------------------------------+
|              chat window (virtualized)              |
+----------------------------------------------------+
| > multiline prompt editor                            |
+----------------------------------------------------+
| running tools panel (optional, toggled)              |
+----------------------------------------------------+
| minimal v0.1.0 | gpt-4.1 | 12.3k/128k | build | ask |
+----------------------------------------------------+
```

**Client state model:**

- `SessionState` per tab: virtualized scrollback window (against `GetMessages`), `StreamingMarkdownView` for the in-flight message, derived tab state, pending prompt, per-session tool rows.
- `AppState`: ordered `SessionState` list, focused index, panel visibility, modal stack.
- `SessionClient`: one gRPC stream per session, demultiplexing `ServerEvent`s into the right `SessionState` by `session_id`.

FTXUI's `Component` is already the composition primitive — core UI does not need a parallel `View` abstraction. Plugin-contributed views are the one exception.

Layout: `src/client/app.cpp` assembles the frame; `src/client/view/{tab_bar,chat_view,prompt_editor,status_bar,session_picker,tools_panel}.*`; `src/client/render/*`; `src/client/session_client.*`.

### Tab states and attention

Every state is derived client-side from events already on the wire — no server work:

| State | Derived from | Indicator |
|---|---|---|
| Idle | `MessageStopEvent`, nothing pending | dim |
| Thinking | `MessageStartEvent` / `TextDeltaEvent` | spinner (focused tab only) |
| Working | `ToolExecutionEvent{started:true}` | spinner |
| **Waiting** | `PermissionRequest`/`ConfirmationRequest` pending | `!` in `theme.attention` |
| Done | subagent session stopped | ✓ (subagent tabs) |
| Error | `ErrorEvent` | `theme.error` |

**Ordering is logical, not drawn**: sort key `(root_session_open_ordinal, is_subagent, spawn_seq)`, so subagents sit immediately after their parent in spawn order. Subagents are flat (one level), so no recursive flattening is needed. Adjacency alone conveys grouping — no `↳`/`│`/nesting glyphs, which cost width and clutter the bar.

**Rendering is deliberately minimal**: session name (truncated) plus the active-tab highlight, with state carried by **color**, which costs zero width. The one exception to color-only encoding is `Waiting`, which gets a literal `!` — it is the single state demanding human action and must not depend on color alone (colorblind users, low-contrast terminals). Richer detail lives where there is room: the status bar for the focused session, the running-tools panel for cross-session detail. The tab bar's job is identity and attention, nothing more.

Blocking subagents auto-focus their tab (the parent is stuck waiting anyway); non-blocking ones open in the background. Navigation: `Ctrl+Tab`/`Ctrl+Shift+Tab` linear, `Alt+←/→` between root groups, plus **jump-to-next-attention** crossing everything. All remappable.

**Group lifecycle**: closing a tab terminates its session; closing a root tab terminates its whole group. Finished subagent tabs remain (✓) until closed manually so the transcript stays readable. Tab-bar overflow needs a scroll/condense rule — flagged, not designed.

### Running-tools panel

Cross-session (that is what distinguishes it from chat): command summary, agent/session, status, started-at, live stdout.

- `ToolExecutionEvent` carries `summary` and `started_at_unix_ms`.
- `ToolOutputDeltaEvent{tool_use_id, chunk}` carries live output, produced via `ToolContext::emit_output`.
- **Bounded at both ends**: the client keeps a ring buffer of `tools_view.stdout_buffer_lines` (default 100); the server coalesces on a ~100 ms timer and sends **only the tail that fits**, so a 50 MB `make` neither floods gRPC nor competes with turn traffic.
- The live view is **lossy by design** — a tail, answering "is this progressing?". The authoritative record is the capped, persisted `ToolResultBlock`. Two artifacts, two jobs.

With sequential tool dispatch there is at most one running tool *per session*, so this panel earns its keep with subagents and long bash commands, not parallel fan-out.

### Session picker

Since attached sessions are all visible as tabs, the picker is primarily a browser of **past (terminated)** sessions: filtered to the current `project_root` by default (`include_all_dirs` to widen), excluding `parent_session_id IS NOT NULL`, sorted by last activity, showing name / message count / last-activity. Deletion happens here, confirmed. Opened at startup when prior sessions exist for the cwd — otherwise a session is created directly — and via `/sessions`.

### Message-kind rendering

A client-side presentational categorization, broader than the wire `Role` enum (which stays `User`/`Assistant`):

| Kind | Source | Treatment |
|---|---|---|
| User | `Message{role: User}` text | Plain styled text — echoes typed input verbatim, not Markdown-rendered |
| Agent | `Message{role: Assistant}` text | `MarkdownRenderer` / `StreamingMarkdownView` |
| Thinking | `ThinkingBlock` | `Collapsible`, dimmed, collapsed by default |
| Tool activity | `ToolExecutionEvent`, `ToolUseBlock`/`ToolResultBlock` | `Collapsible` one-line status; a plugin-registered renderer for that tool name takes precedence |
| System/harness | `CommandResult`, `ErrorEvent`, subagent-completion injections | Muted, distinctly prefixed — separates "the harness told you something" from "the model said something" |

### Customization surface

Three tiers:

1. **Config** — no code: `themes/*.json`; **`keybindings.json`** (not optional: terminals disagree on what they can even *encode* — `Shift+Enter` is indistinguishable from `Enter` without the kitty keyboard protocol / CSI-u — so remapping must be user config; default newline-in-prompt is the more portable `Alt+Enter`); status bar as a format string (`"{version} | {model} | {tokens} | {agent} | {permission_mode} | {session}"`); layout ratios and collapse/timestamp toggles.
2. **Server plugins extend UI *content* without touching UI code** — a plugin command appears in `/help` via the existing system-message path; a plugin tool appears in chat and the tools panel automatically.
3. **Client plugins** for additive views and named slots; and for anything structural, **write a different frontend** against `SessionService` — language-agnostic, process-isolated. This is also why @-mention resolution lives server-side: every frontend inherits it.

## @-mention file references

Typing `@` triggers path autocomplete; selecting inserts `@<relative-path>`.

- **Suggestion** goes through the `SuggestPaths` RPC (debounced ~100–150 ms), resolved by the server's `MentionResolver` so the strategy is pluggable and every frontend inherits it.
- **Expansion is server-side**: on receiving a `UserChatTurn`, the server scans for `@<path>` tokens before wrapping it into a `Message`, resolving each via `path_confinement::confine` + `file_access::read_window` — reused from the Read tool, not reimplemented. A file mention inlines windowed content; a directory mention inlines only the file tree/names, keeping it bounded. **Not** `PermissionChecker`-gated: the human explicitly asked for that content, exactly as if they had pasted it.
- **Client-side**: a hand-rolled inline suggestion list in the prompt editor. `Dropdown` exists but is a click-to-open selector; type-ahead completion reads better inline.

## Non-interactive client (`minimal -p`)

A second, headless client against the same `SessionService` — not a TUI flag. It ships in v1 for three reasons: `PermissionRunMode::AllowOnly` was designed precisely for it (without a headless consumer that mode has no user and no test); it is the smallest real client, so it is what proves the proto is a genuine boundary rather than a TUI-shaped hole; and `--output-format json` is nearly free given the event stream is already structured.

Scope: one turn in; `text` or `jsonl` out; `AllowOnly` by default with an explicit `--dangerously-bypass`; exit code derived from `stop_reason`/`ErrorEvent`. No tabs, themes, or Markdown rendering.

**Attaching to child sessions is a client protocol obligation, not a TUI behavior.** Any conforming client must attach on `SubagentStartedEvent` — otherwise the initial-attach guard would kill a subagent spawned during a headless run. The headless client merges child output into its stream, or discards it under `--output-format text`.

## Agent loop — `src/server/agent/agent_loop.{hpp,cpp}`

Core, and the thing every other component in this document plugs into. Runs on the session worker thread (see Concurrency model), one turn at a time.

```
RunTurn(session, seed_message):
  append + persist seed_message
  loop:
    if cancelled: break
    if auto_compact && context_occupancy > trigger: compact()   # occupancy = last turn in+out
    request = BuildChatRequest(session)
    response = provider.StreamComplete(request, sink)    # sink emits StreamEvents; false => cancel
      on LlmError e:
        if e.is_retryable() && attempts < max: backoff; continue
        emit ErrorEvent; break
    append + persist assistant message
    if stop_reason != tool_use: break
    for each ToolUseBlock, sequentially:
      append + persist ToolExecutor::run(...) as ToolResultBlock
    flush queued subagent completions                    # the turn boundary
  observers.OnTurnCompleted(...)
```

`BuildChatRequest` is rebuilt **every turn**, because MCP server liveness and `/agent` can both change the set mid-session:

- **system** = `agents/subagent.md` (if the session has a parent) + the preset body + the allowed-skill registry listing.
- **messages** = the compacted view, never the raw persisted history.
- **tools** = registered tools ∩ `allowed_tools`, + live MCP tools for `allowed_mcp_servers`, + `SkillTool` if any skills are allowed, + `SpawnSubagentTool` **iff `parent_session_id` is null**.

Two requirements that are easy to miss and expensive to get wrong:

- **Tools must be emitted in deterministic (sorted) order.** `tools` sits at the very front of the serialized prefix providers cache on, so non-deterministic ordering would silently destroy prompt-cache hits on every single turn, with no visible symptom other than cost and latency.
- **`stop_reason == max_tokens` with no pending `tool_use` must be surfaced**, not treated as normal completion. Otherwise output is truncated mid-sentence and the turn looks successful.

**The subagent loop is not a second loop.** A child session's entire life is one `RunTurn` seeded with the task message, plus a capture-final-assistant-text step when it stops, bounded by `max_turns`. There is one loop implementation, not two.

## Context compaction — `src/server/agent/`

Triggered from the agent loop when `memory.auto_compact` is on and a session's **context occupancy** (see Token accounting) crosses `memory.context_compaction_trigger_tokens`. Setting `auto_compact: false` disables automatic compaction entirely, leaving `/compact` as the only trigger. Compaction only changes what goes into the *next* `ChatRequest.messages` — it never mutates persisted history, so scrollback always shows the full conversation.

- Keeps the most recent `memory.compaction_keep_recent_tokens` verbatim; everything older is replaced by one synthetic summary message.
- **The boundary must snap backwards to the nearest complete tool-call cycle.** A token-count boundary is oblivious to tool pairing, and cutting between an assistant `ToolUseBlock` and its `ToolResultBlock` orphans the call — a hard 400 on both OpenAI and Anthropic, not a soft degradation.
- Summarization goes through the active provider using `prompts/compression.md`, overridable per preset via the `compression_prompt` frontmatter field.

```cpp
struct CompactionResult { Message summary_message; int64_t original_token_count; };

class ContextCompactor {
public:
    // messages_to_compact must end on a complete tool-call cycle.
    CompactionResult Compact(const std::vector<Message>& messages_to_compact,
                              LlmProvider& provider, std::string_view compression_prompt);
};
```

Also exposed as `/compact` (an `LLMCommand`) for proactive use. No confirmation — it touches no persisted data.

## Config

**Path resolution** (`src/server/config/config_paths.{hpp,cpp}`): `MINIMAL_CONFIG_DIR` env override → else platform default. Linux/macOS: `$XDG_CONFIG_HOME/minimal` or `~/.config/minimal` (terminal-tool convention, matching `git`/`gh`/`rg`, not macOS's GUI `~/Library/Application Support`). `%APPDATA%\minimal` is reserved for when Windows support lands (2.x) — see Deferred; it is not a supported target today.

**Config is read once at startup; there is no hot-reload.** This is a decision, not an omission. Reloading raises questions with no clean answers — does a tightened permission apply to an in-flight tool call? does an edited system prompt apply retroactively, invalidating the cached prefix? what happens to a session whose provider was swapped underneath it? — in exchange for almost nothing, because sessions already terminate with their client and the server already exits with the last client. Restarting to pick up config costs nothing that quitting did not already cost.

**File format**: agents, skills, and prompts are Markdown with YAML frontmatter — the same convention Claude Code and opencode use: frontmatter for structured metadata, Markdown body for prompt text, loaded verbatim. `src/server/config/frontmatter.{hpp,cpp}` provides `parse_frontmatter_markdown(contents) -> {YAML::Node frontmatter, std::string body}` (split on `---` delimiters, frontmatter to **yaml-cpp**, remainder as body). The `.json` files stay plain JSON — harness config, not prompt content, with no body to split.

- **`agents/*.md`** — presets: frontmatter `name, description, provider, model, allowed_tools, allowed_skills, allowed_mcp_servers, permission_mode: {read_only, file_write, execute, out_of_workdir, overrides}, mcp_permissions: {<server>: allow|ask|deny}, compression_prompt?`; **body is the system prompt directly**. Bundled `plan.md`, `build.md`.
- **`agents/subagent.md`** — plain Markdown, no frontmatter. Prepended to the resolved preset's body whenever `parent_session_id` is non-null.
- **`prompts/compression.md`** — default compaction prompt; a preset's `compression_prompt` overrides it, resolved relative to this directory.
- **`skills/<name>/SKILL.md`** (+ supporting files) — lazy-loaded: frontmatter (`name`, `description`) resident in the registry; body loads only on invocation.
- **`themes/*.json`** — role/category → truecolor RGB.
- **`keybindings.json`** — action → key. Must accommodate plugin-registered action names unknown at compile time.
- **`providers.json`** — `[{name, provider: "<kind string>", base_url, api_key | api_key_env, model_name, parameters: {}, context_window_tokens?}]`. `provider` is looked up in `ProviderFactoryRegistry`, so any plugin-registered kind is valid — there is no closed enum. `context_window_tokens` is informational, feeding the status bar's `used/max`.
- **`mcps.json`** — `[{name, transport: "stdio"|"http", command, args[], env{} | url, headers{}, enabled}]`. `env`/`headers` support `${ENV_VAR}` interpolation, resolved once at load and never logged. No per-server risk category — MCP permission is server-scoped, resolved separately.
- **`permissions.json`** — security config, split from `settings.json` so it can be reviewed and locked down independently: `{default_run_mode: "normal"|"bypass"|"allow-only" (default "normal"), fallback: {read_only, file_write, execute, out_of_workdir}, per_tool_overrides: {}, mcp_default_mode: "allow"|"ask"|"deny" (default "ask")}`. A global floor a preset can still override — no clamping; guardrails presets cannot loosen are future scope.
- **`settings.json`** — `default_agent_preset, default_provider, default_theme, max_concurrent_subagents (default 4), memory.{auto_compact (default true), context_compaction_trigger_tokens, compaction_keep_recent_tokens, scrollback_page_size}, tools.{read,edit,bash}.*, tools_view.stdout_buffer_lines (default 100), session_db_path, logging.{level, path, max_size_mb, max_files}`. `logging` backs a **spdlog** rotating file sink (no custom rotation code), with `path` alongside `session_db_path` under the platform *data* dir, not the config dir.
- **`plugins.json`** — `[{file, provenance: "bundled"|"user", enabled}]`.
- **No `commands/` config dir** — a command's value is its code; a data-only manifest would be a drift-prone shadow copy.

**First-run seeding**: bundled files are compiled into the binary at build time (CMake-generated manifest of `{relative_path, sha256, content}`). A tracked `.bundled_manifest.json` distinguishes "destination missing → install", "on-disk matches last-installed hash and bundled content changed → safe to update", "on-disk hash diverged → user-edited, leave alone and drop `.new` alongside".

**Permission-mode precedence** (highest → lowest): preset `permission_mode.overrides[tool]` → preset `permission_mode.<category>` → `permissions.json` `per_tool_overrides[tool]` → `permissions.json` `fallback.<category>` → hard-coded default (`read_only=Allow, file_write=Ask, execute=Ask, out_of_workdir=Ask`). Note `out_of_workdir` is a *dimension* folded in via `most_restrictive`, not a `ToolCategory` value — see Permission model.

**MCP permission precedence** (separate chain, server-scoped): preset `mcp_permissions[server]` → `permissions.json` `mcp_default_mode` → `Ask`.

**Provider/model resolution**: preset `model` → resolved provider's `model_name`; provider from preset `provider`, or `settings.json` `default_provider` if the preset says `"default"`.

Both permission chains are then subject to `apply_run_mode()`.

**`AgentPreset`** — the resolved struct these chains populate, computed once at load/switch time, never re-resolved per tool call:

```cpp
struct AgentPreset {
    std::string name, description, system_prompt;  // subagent.md (if any) + body
    ProviderConfig provider;
    std::vector<std::string> allowed_tools, allowed_skills, allowed_mcp_servers;
    PermissionPolicy permission_mode;
    McpPermissionPolicy mcp_permissions;
    std::string compression_prompt;
};

class AgentPresetRegistry {
public:
    const AgentPreset& Resolve(const std::string& preset_name) const;
};
```

`ToolExecutor`/`CommandDispatcher` consume the merged `AgentPreset`, never `permissions.json`/`providers.json` directly.

## Skills

`allowed_skills` **defaults to empty** — skills are off unless a preset opts in. The resolved preset renders its allowed skills' frontmatter (`name` + `description` only) into the system prompt as a registry listing; that listing is the entire resident cost.

Invocation is a core `SkillTool` (category `ReadOnly`): takes `{name}`, validates it against `allowed_skills`, and returns the `SKILL.md` body as its `ToolResult`. That call is the lazy-load trigger — bodies are never read until invoked.

```cpp
// src/server/config/skill_registry.hpp
struct SkillMeta { std::string name, description; std::filesystem::path body_path; };

class SkillRegistry {
public:
    const std::vector<SkillMeta>& All() const;                        // frontmatter only, resident
    std::optional<std::string> LoadBody(std::string_view name) const; // the lazy read
};
```

`All()` is what the preset filters to build the system-prompt listing; `LoadBody()` is called only from `SkillTool::execute`.

## MCP client — `src/server/mcp/`

Core. Server subprocesses are **shared across sessions** (one per `mcps.json` entry, not one per session), matching the low-footprint goal.

- **Liveness is checked when a session starts** with a preset listing the server in `allowed_mcp_servers` — i.e. on agent selection, not at server boot.
- **Tool discovery happens once per MCP server process** and is cached there. The discovered tool list is a property of the shared subprocess, not of a session, so session start and `/agent` switches only **re-filter** that cache against `allowed_mcp_servers` — they never re-discover, and `BuildChatRequest` never queries a server per turn. Tool *invocations* remain lazy, dispatched on demand. Re-discovery happens only if a server process restarts.
- A server failing the check is **disabled for that session** with a surfaced error; the session proceeds without it.
- A subagent whose preset requires a dead server **does not start** — `SpawnSubagentTool` returns an MCP error as its `ToolResultBlock`.
- `/agent` mid-session stops exposing a de-scoped server's tools but does **not** kill a subprocess other sessions may be using.

`ToolRegistry` namespaces MCP-sourced tools as `mcp__<server_name>__<tool_name>` at registration, avoiding collisions with built-ins and across servers. MCP tools flow through the identical registry/executor/permission machinery as every other tool.

## Session storage — SQLite

- **`sessions`**: `id, parent_session_id (nullable, self-FK), name, project_root, agent_preset, created_at, updated_at`.
- **`messages`**: `id, session_id, seq, role, content_json, token_count, created_at`, indexed `(session_id, seq)`.
- Keyset pagination for scrollback (`WHERE session_id=? AND seq < ? ORDER BY seq DESC LIMIT N`) — no `OFFSET` scans over a 1M-token history. WAL mode for concurrent read-while-write.

**`project_root` is per-session**, not global — it is the boundary for path confinement, @-mentions, `SuggestPaths`, and bash's initial cwd, and the session picker filters on it. The client supplies its cwd in `CreateSessionRequest`.

**Session naming, layered:**

1. **Core default, free**: name = truncated first user message. No model, no latency, no failure mode.
2. **Optional observer plugin**: async LLM titling after turn 1 through the already-configured provider (~100 tokens), overwriting the default.

A local summarization model (BART/ONNX) was considered and rejected: domain mismatch (`bart-large-cnn` is trained on news prose, emits multi-sentence extractive summaries rather than short titles, and has never seen a code-bearing chat transcript); dependency weight (~400M params — roughly 1.6 GB fp32, ~400 MB int8 — plus ONNX Runtime or libtorch, a *second* inference stack, in a project whose first goal is low footprint and which rejected far lighter dependencies); and redundancy (the configured provider does it better in-domain with zero new dependencies, and is already fully local when pointed at llama.cpp).

Names are therefore mutable after creation; `SessionMetadataEvent` notifies attached clients so tab labels and the picker refresh.

**Schema versioning**: `PRAGMA user_version`, with an ordered migration list applied in a transaction at startup. A database *newer* than the binary understands is a fatal startup error, not a best-effort open.

## Subagents

A subagent is **the same `Session` entity as a top-level conversation** — same message model, same `OpenSession` stream, same tool/permission/command plumbing, its own tab. What distinguishes it is one field (`parent_session_id`) plus restrictions derived from it, not a different type.

**"Stopped" is uniform**: a session is stopped when its agent loop produces an assistant turn with `stop_reason == end_turn` and no pending `tool_use` — the same condition that means "waiting for the next user message" at top level. For a session with a parent, that moment is also completion, and the final assistant message's text is the result.

- **Spawning**: core `SpawnSubagentTool` (category `Execute`, permission-gated) takes `{task, agent_preset?, blocking? (default true), max_turns?}`. It creates a child session with `parent_session_id` set, seeds `messages[0] = Message{role: User, content: [TextBlock{task}]}` (the parent model's own task formulation), emits `SubagentStartedEvent{child_session_id, task, blocking}` on the parent's queue, and starts the loop.
- **System prompt stacking**: for any session with a parent, the system prompt is `agents/subagent.md` (the harness convention: "you are in subagent mode; you'll be given a task; when done or the task is impossible, summarize for the parent") followed by the resolved preset's body. Concatenation, not a new mechanism.
- **No recursion is a hard rule, not preset config**: if `parent_session_id` is non-null, `SpawnSubagentTool` is unconditionally excluded from that session's tool set regardless of `allowed_tools`. A misconfigured preset cannot reopen recursion. Flat, one level.
- **Permission inheritance — a child can never be more permissive than its parent**: effective `PermissionPolicy` is `most_restrictive(parent, child_preset)` per category; `allowed_tools`/`allowed_skills`/`allowed_mcp_servers` are **intersected** with the parent's; `PermissionRunMode` is clamped (`AllowOnly < Normal < Bypass`). Without this, a parent at `file_write: ask` could spawn a child preset at `file_write: allow`.
- **Concurrency limit**: `max_concurrent_subagents` per parent session, default 4. Over-limit spawns return an **error `ToolResultBlock`** ("subagent limit reached") rather than blocking, so the model adapts instead of hanging. The sanity check *warns* above 8 — past that the task has outgrown the harness — but does not block, matching the build-it-yourself philosophy.
- **Blocking mode**: `execute()` drives the child's loop to completion on the parent's worker thread and returns the child's final text as the `ToolResultBlock`. The child's tab auto-focuses, since the parent is stuck waiting anyway.
- **Non-blocking mode**: `execute()` returns an acknowledgment immediately and the parent's turn continues; the child runs on its own worker. On completion the result is **queued** for the parent and flushed by the parent's worker **at a turn boundary** — after all tool results for the current assistant turn are appended, before the next provider call. Injecting it truly out-of-band could land a `User` message between a `tool_use` and its `tool_result`, which is a hard 400.

## Startup sanity check — `src/server/config/sanity_check.{hpp,cpp}`

Runs after config and plugin load, before serving. Behaves like a linter, not a fail-fast validator — **it surfaces every problem found, not just the first** — so a misconfigured setup yields one complete diagnostic instead of a fix-one-rerun-find-the-next loop. It also answers "what happens on a cold start with nothing configured": that is one more failing check with a clear reason, not a bespoke onboarding wizard.

```cpp
enum class CheckSeverity { Fatal, Warning };
struct CheckResult { CheckSeverity severity; std::string check_name, message; };

class StartupSanityCheck {
public:
    std::vector<CheckResult> RunAll() const;   // runs every check regardless of earlier failures
};
```

Checks: every config file parses; at least one usable `providers.json` entry exists with a resolvable key *and* a registered provider kind; each `mcps.json` entry's `command` resolves on `PATH` (stdio) or `url` is well-formed (http); `bash.allowlist` is non-empty; `project_root` exists and is writable; the SQLite database is accessible and not newer than the binary; each discovered plugin's ABI tag matches; `max_concurrent_subagents` is sane; **no registration-name collisions** (fatal — see Name collisions and aliases); every `command_aliases` key resolves to something registered.

Two checks specifically for the naming model: warn when a `per_tool_overrides` / `allowed_tools` / `allowed_skills` entry resolves to **nothing** registered (catches typos and entries stranded by a disabled plugin), and warn when a bare permission key is **ambiguous** because two plugins provide that original name — the intended rule is then undefined and must be written in qualified `plugin.name` form.

**Severity splits by provenance**: a failed **bundled** plugin is **Fatal** — losing `tools-fs` or `provider-openai` is a useless harness, not a degraded one — while a failed **user** plugin, or one bad `mcps.json` entry, is a **Warning** that disables just that piece.

## Deferred / open items

- Anthropic Messages-shape provider (the unified model needs only additive changes: `cache_control` fields and a widened `system`).
- **Windows support generally — out of scope until 2.x**; WSL is the interim answer. This is broader than plugin loading (`LoadLibrary` vs `dlopen`): the bash tool is `posix_spawn` + `setrlimit` + `killpg`, POSIX by construction rather than by porting accident, so a Windows target needs a genuinely different execution backend, not a shim.
- `llamacpp-kv` plugin (`/save-session`, `/restore-session`, `/erase-session`) — designed, deliberately not shipped in v1.
- Parallel tool dispatch (sequential in v1; `request_id` correlation already supports it).
- Session detach (`EndSession{DETACH}`), deliberately deferred — see Session lifetime.
- Status-bar segment and panel/picker column slots for client plugins (renderer slot ships in v1).
- Tab-bar overflow behavior with many sessions × subagents.
- Backend-reported `usage` of `0` from some OpenAI-compatible proxies — no fallback estimate designed.
- Bash's no-`$VAR`-expansion restriction (`export PATH=$PATH:/usr/local/bin` does not work under the current rule) — a known rough edge worth a dedicated revisit.

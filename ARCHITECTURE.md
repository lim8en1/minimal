# minimal — Architecture

Local-first, memory/CPU-efficient coding-agent harness. C++ server/TUI-client split over gRPC. Distributed to a team and public on GitHub — not a single-user personal tool, which shapes the decisions marked *(distribution)* below.

**Product philosophy: build-it-yourself, customize-as-much-as-you-want.** Core owns *mechanisms* and the minimum default needed to function; nearly everything interesting ships as a **plugin using the same shape a third-party plugin would**. Read/write/edit/bash, the OpenAI provider, and the built-in commands are all bundled plugins, not privileged code paths.

## Goals

- Low memory/CPU footprint; must handle 512k–1M token conversation histories without holding full history in memory or re-rendering it.
- Read/write/edit/bash tools, MCP client, skills, agent presets ("declarations" — config bundles, not subagents), an extensible `/`-command system, and a native plugin mechanism on both server and client.

## Distribution

- **License: MIT.**
- **Server topology: local-only, one server per user.** Each team member installs and runs the binary on their own machine — no shared/remote server in v1. This is a *security* assumption, not just a deployment default: the bash tool, path confinement, and permission checker all assume the server acts on behalf of one trusted local user on their own filesystem. A shared/remote server (multiple people connecting to one instance) would need per-connection auth and a much stricter tool-sandboxing model, and is explicitly out of scope until revisited. A single local server does serve multiple concurrent clients.
- **Dependency management**: vcpkg manifest mode (`vcpkg.json`) so `cmake --build .` works for any contributor without bespoke local setup. All of grpc, protobuf, curl, sqlite3, nlohmann-json, ftxui, yaml-cpp, md4c, tree-sitter (core), re2, spdlog, and catch2 are present in the registry — verified, not assumed. Only `hkr04/cpp-mcp` is genuinely absent (niche library) and is vendored via `FetchContent`. The curated `tree-sitter-<lang>` **grammars** are vendored the same way, since vcpkg packages the core library but not the per-language parsers — `tree-sitter-bash` is the first in the tree (the bash tool's command scanner), and the rest arrive with client-side highlighting.
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
  plugins/                    # bundled plugin IMPLEMENTATIONS -- what the harness loads
    server/                       # tools-fs, tools-search, tools-bash, provider-openai, commands-core
    client/                       # (none bundled yet)
  bundled/                    # non-code assets that ship WITH the binary
    prompts/compression.md        # all of it copied into the config dir by
    agents/{plan,build,subagent}.md   #   `cmake --install` -- never read from here
    settings.json  permissions.json   #   at runtime, never compiled into the binary
    providers.json  providers.example.json
  cmake/SeedConfig.cmake      # the copy: never overwrites an existing file
  src/                        # the harness ITSELF
    common/                   # shared by server AND client
      config_paths.{hpp,cpp}      # the client reads themes/ + keybindings.json too
      abi_tag.{hpp,cpp}           # both plugin loaders must compute an identical tag
      bundled_assets.{hpp,cpp}    # executable-relative lookup -- PLUGINS ONLY
      model_ref.{hpp,cpp}         # <provider>/<model>; the client validates one too
      version.hpp
    server/
      agent/    # agent_loop, session, session_registry, context_compactor,
                #   compaction_boundary, compaction_trigger, presets,
                #   session_observer(_registry),
                #   subagent_manager + spawn_subagent_tool (core, not a plugin)
      llm/      # unified model, provider iface, http_client, sse_parser
      tools/    # Tool iface, registry, executor, permission model, path_confinement
      commands/ # Command iface, registry, dispatcher, ConfirmationBroker,
                #   CommandContext + the SessionControl interface it borrows
      plugins/  # the plugin HOST: loader, plugin_abi.h, name/collision registry,
                #   observer/mention/auth registries. Harness code -- distinct from
                #   the top-level plugins/, which is what gets loaded.
      rpc/      # SessionService impl, server, pending_requests, event_queue,
                #   wire_permission_checker, proto_convert, auth_interceptor
      config/   # loaders, frontmatter, skill_registry, sanity_check
      mcp/      # cpp-mcp client wrapper, shared server processes
      session/  # SQLite store, migrations, SessionReader, message_codec, ulid
    client/
      view/     # tab_bar, chat_view, prompt_editor, status_bar, session_picker, tools_panel
      render/   # markdown_renderer, streaming_markdown_view, syntax_highlighter, theme
      plugins/  # client loader, view/renderer registries
      session_client.{hpp,cpp}  app.cpp
  tests/
    support/                  # FakeLlmProvider etc. -- exported for plugin authors
```

`common/` is narrowly justified rather than a catch-all: the client resolves the *same* config directory as the server (it reads `themes/*.json` and `keybindings.json`), and both plugin loaders must compute an identical ABI tag. Everything else stays server- or client-private.

**`plugins/` is a top-level peer of `src/`, not a subdirectory of it.** Bundled plugins are not part of the server or client binary — they are separate `.so` artifacts, categorically identical to a third-party plugin and differing only in who publishes them. Nesting them under `src/server/` would assert a containment the linkage specifically avoids (plugins take core's headers but never link its static library). The plugin *host* — loader, ABI header, registries — is genuinely harness code and lives at `src/server/plugins/` and `src/client/plugins/`. Two directories, two meanings: what loads, and what is loaded.

## Testing

Catch2, deliberately without a mocking framework. **The design already makes fakes cheap** — `LlmProvider` is two methods, `PermissionChecker` / `ConfirmationBroker` / `MentionResolver` are one each — so hand-written fakes suffice, and that property is worth protecting: an interface that needs GMock to test is an interface that grew too big.

Four tiers, in value order:

1. **Pure logic**, no I/O or network: `path_confinement` (security-critical — `..`, symlink escape, encoded traversal, embedded NUL), the `most_restrictive` lattice and both permission precedence chains, `SseParser` frame-splitting across write-callback boundaries, frontmatter parsing, alias/collision resolution, compaction boundary snapping to tool-call pairs, occupancy-vs-cumulative token accounting.
2. **Agent loop against a scripted `FakeLlmProvider`**: the full tool-call cycle, retry-on-`is_retryable`, cancellation, compaction trigger, subagent blocking and non-blocking paths, `max_tokens` surfacing. Deterministic, no network.
3. **Tools against a temp `project_root`**: read/write/edit on a tmpdir; bash pipelines and redirection, the cwd/env state round-trip through the shell's side channel, command disclosure and grant conjunction, timeout/killpg, bounded reaping, and cancellation behavior.
4. **End-to-end**: server plus the headless `-p` client over a real gRPC channel on a unix socket. The `-p` client is the natural E2E harness — a second reason to build it before the TUI. Covers the turn round-trip, the permission round-trip in both directions, a session grant silencing the second prompt, `AllowOnly` auto-deny, stream-close termination, and refcounted server exit.

**Tests whose failure mode is a hang need a deadline, and `std::async` cannot provide one** — the future it returns blocks in its destructor until the task completes, so a wedged worker takes the whole suite down and reports nothing. `tests/support/deadline.hpp` runs the work on a detached thread with refcounted state instead, so an abandoned thread is leaked rather than joined. The caller's obligation is that the callable owns everything it touches.

The same trap exists on the client side of a bidi stream: gRPC's sync API blocks in `Write()` when the peer is not reading, so a client that calls `Finish()` with events still queued deadlocks both sides. Read to end of stream first.

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

**Attach happens on the first `ClientEvent`, not at connection setup.** `OpenSession` carries no request message, and `session_id` rides on every `ClientEvent` rather than on stream metadata — so the stream is *named* by its first inbound message. The initial-attach guard therefore covers the window from `CreateSession` until that first event arrives, which makes it a function of client latency rather than of connect time.

**`TurnCompletedEvent` is the end-of-turn signal**, and the stream stays open across it. Nothing else can serve: a turn makes one provider call per tool round, so `stop_reason` arrives several times before the turn is actually finished. Without an explicit terminator a single-turn client cannot know when to half-close, and both sides block — the client waiting for output that will never come, the server waiting for a turn that will never be sent. An interactive client never notices, because a human decides when to stop; `minimal -p` has no human, which is why building it first is what found this. It is emitted on **every** exit path including failures, since a client that learns a turn ended only by seeing it succeed hangs on every error.

**Session lifetime**: stream close **terminates** the session — see "Session lifetime" below. There is no detached state in v1, which is why `PendingRequests` has no grace-timer machinery.

**Both session queues are bounded, by different rules.** The outbound `EventQueue` caps at 65,536 events and blocks the producer (`Push` is worker-only; the reader uses `TryPush` and answers refusals). The session's *work* queue (turns + mutating commands) caps at 32 items and REFUSES beyond that, answering each refusal on the wire — one turn can run for minutes, so an unbounded work queue let a client enqueue transcript-sized turns faster than the worker consumed them, growing memory without limit. Termination closes the outbound queue BEFORE joining the worker: `Push` blocks at the cap and is woken only by `Close` or `Pop`, so a worker parked on a full queue behind a stalled writer deadlocks a join that waits for a close scheduled after the join. Closing first wakes the producer; `Pop` still drains before honoring the close, so a live writer receives the backlog.

**Termination runs in a fixed order, and the order is the invariant.** `Terminate` performs exactly three steps — `PendingRequests::Shutdown()` → `EventQueue::Close()` → join the worker — and each step exists to wake a worker parked at one specific spot before the next step can strand it there:

- **`Shutdown()` before `Close()`** — a worker parked in `Registration::AwaitResponse()` (an unasked permission or confirmation question) is woken only by `Shutdown`; a worker parked in a blocking `Push` (full outbound queue) is woken only by `Close`. Both parks are reachable during teardown, and the two wake-ups come from different files: the blocking `Push` is `WirePermissionChecker::check` (`wire_permission_checker.cpp`), the ordering is `Session::Terminate` (`agent/session.cpp`). No comment inside either file alone can keep them correct, which is why the order is recorded here as well as at both code sites.
- **`Close()` before the join** — as above: a full-queue `Push` parked behind a stalled writer would otherwise deadlock the join forever.
- Swapping either pair deadlocks the worker on an unasked question or a full queue; both failure modes leave the join hanging with the test suite still green, since only an E2E termination with a mid-turn `Ask` exercises the window.

**Inbound dispatch** (reader thread — every branch is non-blocking):

| `ClientEvent` | Action |
|---|---|
| `UserChatTurn` | Push onto the session's turn queue; wake the worker. |
| `CommandInvocation` | Read-only `HarnessCommand` → post to the bounded command pool (never a detached per-command thread: unbounded thread pileup under flood, and a shutdown window where a task outlives the registries it borrows); anything else → queue for the worker at a turn boundary. Refusals — pool saturated, work queue full — are ANSWERED on the wire, never dropped: silence on a `request_id` is a client hang. |
| `PermissionResponse` / `ConfirmationResponse` | `PendingRequests::Resolve(request_id, typed answer)` — signals the blocked worker. |
| `CancelTurn` | Set the session's atomic cancel flag. |

```cpp
// src/server/rpc/pending_requests.hpp

// WHAT AN ANSWER IS, made a type. Two brokers share one PendingRequests (the
// permission checker and the confirmation broker); a bare string payload kept
// them apart only by convention. With the variant, a cross-delivered answer is
// structurally unreadable at the waiter instead of merely failing to parse
// there. The "perm-"/"confirm-" id prefixes remain as diagnostics; they are no
// longer load-bearing.
struct PermissionAnswer    { std::string serialized_response; };  // parsed on the worker
struct ConfirmationAnswer  { std::string chosen_option; };
using PendingAnswer = std::variant<PermissionAnswer, ConfirmationAnswer>;

class PendingRequests {
public:
    // Registration is SEPARATE from waiting, and returns the handle you need in
    // order to wait. That makes the correct order the only expressible one:
    //
    //     auto registration = pending.Register(id);
    //     queue.Push(request_event);              // safe: the slot exists
    //     auto answer = registration.AwaitResponse();
    //
    // A single AwaitResponse(id) call cannot work. Waiting before publishing
    // blocks before the request goes out; publishing before waiting loses a fast
    // client's answer, which lands in the gap. RAII on the slot matters too: the
    // waiter is the session worker running tool dispatch, which can throw.
    class Registration {
    public:
        // nullopt means CANCELLED -- the session is terminating. Callers map that
        // onto a decision themselves; encoding "deny" here would put a policy
        // choice inside a synchronization primitive.
        std::optional<PendingAnswer> AwaitResponse();
    };
    [[nodiscard]] Registration Register(std::string request_id);

    // Reader thread. False if no slot is registered, which is not an error: a
    // client may answer a request whose session already terminated. Unknown ids
    // are never buffered for a future waiter -- that would let a client grow this
    // map without bound by answering things nobody asked. The reader constructs
    // the variant from the ClientEvent's own payload case -- it knows the type,
    // parses nothing, and the type travels with the value.
    bool Resolve(const std::string& request_id, PendingAnswer answer);

    // Wakes every waiter with nullopt. Permanent, so a worker reaching a gate
    // DURING teardown returns instead of parking on a stopped reader.
    void CancelAll();
};
```

Waits indefinitely by design -- no timeout, no fail-closed. A client is present by
construction (no detached sessions), so a human may simply be away, and
auto-denying their work is worse than waiting.

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

**Termination reuses the cancellation path** (atomic cancel flag → `bool` stream sink aborts the provider request → tools observe `is_cancelled`), then joins and reaps the worker. No new mechanism. Cancel is a GENERATION, not a flag: the worker records the generation when it pops a turn, and a turn is cancelled iff the generation has moved since — a flag cleared before the pop loses a cancel that landed while the worker was between turns, one cleared after the pop loses one that landed in the pop-to-clear window. A cancel landing inside a retry backoff reports `StopReason::Cancelled`, not `Unknown` — the headless exit code derives from this value.

**Server shutdown must be *requested*, not performed, from an RPC handler.** The last-client-detached callback fires at the tail of `OpenSession`, on the handler thread, and `grpc::Server::Shutdown()` blocks until every in-flight RPC completes — including the one calling it. So `Server::RequestShutdown()` only sets a flag, and a thread inside `Server::Wait()` does the blocking part. Getting this wrong hangs the binary on every run while every unit test still passes, because a test that merely records the callback never exercises it.

- **Confirm when work is in flight.** Closing a tab mid-turn destroys running work, so it routes through the existing `ConfirmationBroker`; closing an idle tab needs no prompt.
- **Terminating a blocking subagent must unblock its parent** — the parent's `SpawnSubagentTool` is synchronously waiting and receives an error `ToolResultBlock` (the child's completion signal fires on every exit path, teardown included) rather than hanging. Cancelling the parent's turn propagates the same way: the wait observes the parent's cancellation and terminates the child. "Every exit path" includes a terminate that lands BEFORE the seeded turn ever ran (the initial-attach reaper, or a terminate racing `SubmitTurn`): the completion hook fires from `Terminate` with a synthesized `Cancelled` result, because the only alternative is a blocking parent waiting on a promise nothing will ever set.
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
    // Returns the assembled response, not void: the caller needs the final
    // message to append to history, and the provider already maintains exactly
    // that state to reassemble streamed tool-call fragments. Returning it stops
    // every caller re-deriving it from the event stream.
    virtual ChatResponse StreamComplete(const ChatRequest&, const StreamEventSink&) = 0;
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

`ProviderCacheKey` is the `providers.json` entry **name** plus a config-generation counter — entries are immutable at runtime, so this is sufficient, and it avoids hashing an API key into a map key. `parameters` is not part of the key: it lives on `ChatRequest`, populated fresh per call. This cache is real, not aspirational: `SessionRegistry::CreateResolved` and the `/agent`//`model` rebuild path both route through it, sessions hold a `shared_ptr` rather than ownership, and switching away from and back to an entry reuses the instance.

**Model discovery — a third registry, keyed by the same kind string.** A providers.json entry may set `"discover": true`, in which case the models it serves come from the backend's own listing (`GET <url>/models`) instead of a hand-written `models` object — the point being `context_window_tokens`, the one field whose staleness silently disables compaction, and the practical unconfigurability of a provider serving fifty models. `ModelCatalogRegistry` maps the same `backend` kind string a factory registers to a `ModelCatalog` whose `List()` reports `{id, context_window_tokens, metadata}`; one OpenAI-shape implementation beside `provider-openai` covers llama.cpp (`meta.n_ctx_train`), vLLM (`max_model_len`), and OpenRouter (`context_length`) alike. Discovery is fetched **lazily at first resolve, never at startup** (the sanity check inspects it structurally — which kinds registered — and never probes), the runtime cache negative-caches failures and is never written back to `providers.json`, a failed discovery degrades to the entry's static models, and `/providers refresh` evicts and refetches on demand. The config layer never links plugins: the fetch reaches it as a hook injected from `main.cpp` after plugin load. Details and decisions in `docs/EXTENSIBILITY.md` §2.6.

**The request body is serialized, not materialized.** The OpenAI serializer builds the wire body incrementally into a string — message text is escaped straight into the buffer; only O(1) pieces (tool definitions, tool_choice, `parameters` values) pass through nlohmann — and the retry loop builds the `ChatRequest` once per tool round rather than once per attempt. Against the 512k–1M token goal, a DOM of the whole history per call (let alone per retry) was the remaining RAM spike. The merge-order guarantee survives exactly: parameter keys that name structural fields are DROPPED rather than emitted after them, which preserves "structural always wins" without relying on any parser's duplicate-key resolution.

`StopReason` carries a **`Cancelled`** value alongside `EndTurn`/`ToolUse`/`MaxTokens`/`StopSequence`/`ContentFilter`/`Refusal`/`Unknown`. Cancellation is expected, not exceptional, so it must not surface as an `LlmError`: the provider catches its own aborted transfer and returns a partial `ChatResponse` carrying this stop reason, and the loop treats it as a clean exit rather than a failure.

**Token accounting — two distinct quantities, never conflated:**

- **Context occupancy** (status-bar `used/max`, compaction trigger) = `last_turn.input_tokens + last_turn.output_tokens`. Each turn's `input_tokens` already contains the whole history, so *summing* across turns double-counts quadratically and would fire compaction far too early.
- **Cumulative totals** (genuinely additive) = running sum of `output_tokens`, plus summed `input_tokens` if cost display is added later.

Counts come from the backend's own reported usage; there is no separate tokenizer. Some OpenAI-compatible proxies report `0` — no fallback estimate is designed for that, flagged as a known gap.

**Transport**: no `openai-cpp` dependency (researched and rejected — blocking-only, no streaming, no typed message/tool modeling beyond raw JSON passthrough, unmaintained since April 2024, TLS verification appears off by default). Hand-rolled instead: `HttpClient` (blocking POST, streaming POST, and GET over libcurl — the GET is the model-catalog listing path, its response body buffered under an 8 MiB cap) and `SseParser` (generic incremental SSE frame parser handling frames split across libcurl write-callback invocations). Both are exposed to plugins as consumable utilities. TLS verification left at libcurl defaults (on).

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
- **The classification is not a lock.** `confine` canonicalizes a path and returns it; the file at that path can be swapped between classification and the tool's later `open()`. Accepted asymmetry, same as bash's: the threat is model error, not a hostile local process, and against the latter only a container is a boundary. Hardening (`O_NOFOLLOW` reopen) is revisited only if the topology leaves the local single-user assumption.
- `file_access::read_window(path, offset, limit, max_line_length)` — streams line-by-line, memory proportional to `limit` not file size. Shared by Read and Edit's `view`.
- `atomic_write::write_file(path, content)` — temp file + fsync + rename. Shared by Write and Edit's mutating commands.

**Read**: `{path, offset, limit}` → line-numbered windowed text + truncation footer (textual pagination hint, since the model only sees text).

**Write**: `{path, content}` → full create/overwrite; requires parent dir to exist (no silent mkdir).

**Edit**: matches Anthropic's built-in `text_editor_20250728` vocabulary — `view` / `create` / `str_replace` (exact match, error unless exactly one occurrence) / `insert` — so an Anthropic provider can later be handed the native schema-less tool without changing file-mutation semantics.

**Bash**: **commands run through `/bin/sh -c`.** Pipes, redirection, `&&`, `||`, globs and `$VAR` all work, and that is load-bearing rather than a convenience: without a pipe, `convert_to_json.py big.csv | jq '.rows[0]'` has to round-trip the entire CSV through the context window, so forbidding pipes makes the harness *less* token-efficient — the one thing it exists to be.

**There is no allowlist, and no attempt at one.** A binary list is unmanageable security theatre at every granularity. Keyed on name it dies to `cp /bin/bash /tmp/git`; keyed on resolved path or hash it dies to the interpreters you must allow anyway — `git config alias.x '!sh'`, `make` running a Makefile, `find -exec`, `xargs`, `env`, `npm run`. There is no set of allowed binaries for a coding agent that is not also a set of ways to run arbitrary code. An earlier draft forbade shell metacharacters to protect such a list; when the list went, the restriction was left behind with a new rationale ("approval fidelity") attached to it. It has now gone too, since it bought no boundary — the same paragraph had to concede that `bash -c "rm -rf /"` contains no metacharacters — while costing pipes.

**The permission gate is the boundary.** Bash is category `Execute` (`Ask` by default) and the operator approves the command. This is coherent with the local, single-user server assumption above: the real threat is **model error, not model malice**. Against a mistaken model a prompt works; against an adversarial one holding a shell, only a container does — which is why the swap-out path (a plugin registering `bash` against a container or remote executor) matters more than any character-level rule ever could.

**Consent scoping** makes `Ask` livable without weakening it — see *Consent scoping* below.

**Persistent session state (cwd + env only), stateless process-per-call.** `cd` and `export` inside `sh -c` would die with the subshell, so the wrapper appends a trailer that reports the shell's final `pwd -P` and `env -0` on fd 9, and the session adopts them wholesale (a merge would resurrect variables the command deliberately `unset`). This keeps `cd`/`export` chaining across calls with no long-lived interpreter to hang, corrupt, or leak. The payload is validated, never trusted — a command can write to fd 9 itself — and anything unexpected leaves the previous state untouched, which is also what a command that `exit`s before the trailer runs produces. `{"restart": true}` resets cwd/env, and `/restart-shell` — registered by the *same* plugin — does the same from the command side.

**Limits.** `timeout_ms` is per-invocation, model-supplied and clamped to a configured ceiling; "until it finishes" is deliberately not offered, since the deadline is the only hard bound on a session worker thread. Resource limits via `setrlimit` (child-side, before exec — hence fork + execve rather than posix_spawn, whose pre-exec functions may only call async-signal-safe functions). On timeout or cancellation the child's process group is killed *and* the child itself: the child sets its own pgid via `setsid()`, so between fork and that call `killpg` would find no such group and fail. Reaping is deadline-bounded rather than a blocking `waitpid`, because EOF on the pipes does not mean the process is gone — it can close its stdio and keep running. stdin is `/dev/null`: inherited, a command reading fd 0 blocks until the deadline and competes for the operator's terminal. **Streams stdout incrementally through `ToolContext::emit_output`** rather than buffering to `max_output_bytes` — the cap (head ¼ + tail ¾, marker in the middle) still bounds the persisted result, and with pipes available the model can filter at the source (`| tail -50`) rather than being handed schema knobs to do it. Every invocation logged (command, exit code, duration).

That cwd/env map lives **inside the `tools-bash` plugin**, keyed by session id — core holds no bash state. The plugin therefore also registers a `SessionObserver` purely to implement `OnSessionEnded` and free the entry; without it the map would grow for the process lifetime. This is the canonical example of why plugin-owned per-session state needs a lifecycle signal.

## Search tools — the `tools-search` plugin

**`grep`** and **`glob`** are bundled plugins, not core — same categorization as `tools-fs`/`tools-bash`. Both are `ReadOnly` (⇒ `AlwaysAllow` by default, with the out-of-workdir `Ask` fold applying when the search root escapes `project_root`), both declare `path_confinement_supported()`/`ExtractPath()`, and neither holds per-session state, so neither needs a `SessionObserver`.

**The regex engine is RE2, and the reason is the session worker.** Grep patterns are model-authored, and a backtracking engine (std::regex, PCRE2) hands the model a way to wedge the worker thread with a pathological pattern — the exact hang bash's `timeout_ms` exists to bound. RE2's linear-time guarantee makes that failure mode *unreachable* rather than bounded, which is why a dependency was added for something `std::regex` technically covers. RE2 also means no backreferences/lookarounds; the tool description says so, because the model is the one writing the patterns.

**Real `.gitignore` support, with one deliberate deviation.** Per-directory `.gitignore` files stack with git's precedence (deeper file overrides shallower, last matching rule in a file wins — which is what makes `!` re-inclusion work), covering anchoring (`/` prefix or interior slash), dir-only (trailing `/`), `**` in its three special placements, character classes, and `\#`/`\!` escapes. Braces are literal characters, as in git — the *glob* tool's `{a,b}` alternation is a different caller with different semantics, and the translator takes a flag rather than letting the two silently disagree. The deviation: the git **index** is never consulted (no git dependency, the same call `SuggestPaths` made), so untracked-but-unignored files are searched — exactly ripgrep's default behaviour. A line whose pattern cannot be translated (unterminated `[`) becomes a rule that never matches rather than voiding the file's other rules: one malformed line cannot silently widen what a search returns.

**Symlinks are never followed** — directories because a symlink loop is the classic traversal wedge, files because a symlink can point outside the search root and silently search beyond where the model aimed. `.git`/`.hg` are skipped unconditionally, ignored-directory pruning is git-faithful (nothing under an ignored directory is considered, which is also why re-inclusion under one is impossible here as in git).

**Bounded the same way as everything else that runs on the worker.** Traversal is a per-directory name-sorted DFS (deterministic output — a model diffing two calls should see signal), reads each candidate file whole under `grep.max_file_bytes` (the walker skips larger files and *discloses the skip* — a bare "no matches" over a partial walk would be a false negative), detects binary files with the same NUL-in-first-8-KiB heuristic as `read_window`, polls `is_cancelled` per entry, and carries a wall-clock `timeout_ms` per invocation. RE2 already removed the regex-shaped hang; the timeout bounds the *walk*, which is O(tree). Results are capped at `head_limit` with a truncation footer in the text — the model has no structured channel, the same rule as `read`'s pagination footer — and a budget-stopped total is reported as "at least N" rather than a falsely exact count. Content mode streams through `ToolContext::emit_output` like bash does.

**Both are tools, not bash.** Pipes exist and `rg` remains reachable through bash for anything these can't express; the dedicated tools earn their keep on structure (line numbers, context blocks, counts) and on respect for ignore rules without the model having to construct a filter. `bundled/agents/plan.md` points the model at them first.

## Permission model — `src/server/tools/`

Core, and never plugin-replaceable in its **substrate**: the mode lattice, precedence chains, run modes, subagent ceilings, and grant ownership are what everything calls into, not a strategy with alternatives. The one pluggable seam is the **decision source for `Ask`** — who answers when the substrate has resolved a mode to `Ask` and no grant satisfies it. A plugin may register a `PermissionAdvisor` that answers automatically and abstains to the human; the substrate itself is not exposed. The line and the design are specified in `docs/EXTENSIBILITY.md` §3.

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

**Bash is excluded from out-of-workdir gating.** There is no reliable way to determine which paths an arbitrary command touches without parsing shell semantics. Bash reaches outside `project_root` freely — absolute paths, `..`, and `cd` alike — and the operator-approval model is what governs it instead. An accepted asymmetry, not an oversight.

**Permission run mode** — session-level, orthogonal to `PermissionPolicy`, wrapping its output:

```cpp
enum class PermissionRunMode { AllowOnly, Normal, Bypass };  // least to most permissive
PermissionMode apply_run_mode(PermissionMode base, PermissionRunMode run_mode);
// Bypass    -> always AlwaysAllow, ignoring `base`.
// AllowOnly -> base's AlwaysAllow/AlwaysDeny unchanged; base's Ask becomes AlwaysDeny.
// Normal    -> base unchanged (Ask still blocks on PermissionChecker).
```

**`AllowOnly` exists specifically to make `Ask` safe non-interactively** — without it a headless run hitting an `Ask`-gated tool blocks forever waiting for a human who was never attached; `AllowOnly` auto-denies instead of hanging. `Bypass` ("trust everything") and `AllowOnly` ("never block, never silently risk it either") are different risk postures, not gradations. Set via `permissions.json`'s `default_run_mode` and mutable per-session via `/permission-mode`, which requires confirmation.

### Consent scoping

A bare `Ask` prompts for every invocation, which trains the operator to click through. So a tool may **disclose** what an invocation reaches, and the prompt offers session grants for it: `make 2>&1 | tail -50` yields *allow once* · *deny* · *allow `/usr/bin/make *` this session* · *allow `/usr/bin/tail *` this session*.

```cpp
struct GrantOffer { std::string key, label; };  // "bash:/usr/bin/make", "/usr/bin/make *"
struct PermissionDisclosure { std::string summary; std::vector<GrantOffer> grants; bool complete = true; };
virtual PermissionDisclosure Tool::Disclose(const nlohmann::json&, const ToolContext&) const;
```

`ToolExecutor` owns the grant set (it already holds a per-session `PermissionPolicy` by value). The mechanism is not bash-specific — `edit` can disclose `edit:/abs/path.cpp` with no further core change.

**This is a prompt-fatigue mechanism, not a security boundary**, and five rules keep it from decaying into the allowlist deliberately rejected above:

1. **Conjunction.** An invocation runs unprompted only if **every** disclosed key is already granted. `git status; rm -rf /` discloses `{git, rm}`, so a grant on git cannot carry it.
2. **No vacuous satisfaction.** An empty or incomplete disclosure never satisfies the conjunction. This is logic, not policy: "nothing to check" must not evaluate to "check passed".
3. **Resolved paths, not names.** Keys use the path resolved against the *session* `PATH`, so `cp /bin/bash /tmp/git` discloses `/tmp/git`. That prevents nothing — nothing at this layer does — but it makes the substitution visible at the moment consent is given, which is the only place it can be.
4. **Never persisted.** Session-scoped and in-memory; not in `permissions.json`, not in the session store, and never seeded from config. A grant's whole value is a human judging a specific disclosed invocation *at that moment*; serialising it keeps the shape and discards the substance. Remembering grants across sessions is exactly how this would become an allowlist again.
5. **Blanket by binary, and worded that way.** Argument patterns (`git *` vs `git push --force *`) are a pattern language that fails as an allowlist does. A grant means "this binary, any arguments".

**No interpreter special-casing.** A grant on `/bin/bash` covers `bash -c '…'`; a grant on `/usr/bin/make` covers whatever the Makefile runs. Honouring a grant on git while refusing one on bash would be an interpreter denylist — the same theatre, defeated the same way. The dial for "stop asking entirely" already exists and is honest about what it means: `per_tool_overrides: {"bash": "allow"}`.

**Disclosure is a lower bound**, and the prompt must say so. `complete = false` covers only *syntactic* blindness, where the program is not a literal in the string (`$X args`, `eval "$CMD"`). It cannot cover semantic indirection: `find . | xargs rm` names `find` and `xargs`, never `rm`. No parser fixes that.

That lower bound has a floor the scanner owes rule 1, and it is the line between the two cases: **a program that appears as a literal in a command position is either named or `complete` is false — never both missing.** The difference from `xargs rm` is what the operator was shown. Granting xargs is a decision about xargs, taken knowingly with the lower bound stated; nobody decides anything about a program that was never disclosed. Shell reserved words broke this floor — `then` and `if` were recorded *as* the program of their list, which demoted the real one to an argument, so `ls; if true; then rm -rf /; fi` disclosed `{ls}`, called itself complete, and a held grant on `ls` satisfied the conjunction (§12.1).

**The scanner is `tree-sitter-bash`** (vendored via `FetchContent`; see *Dependency management*). The grammar yields a `command_name` node at every command position — inside `if`/`while`/`for`, function bodies, subshells, command substitutions — which is the whole class of compound forms, and heredoc bodies come back as data rather than commands. But the structural win is the error signal, not the coverage: a hand-rolled walker cannot tell "I parsed this" from "I mis-parsed this", which is exactly how the reserved-word hole produced `complete = true` on a string it had misread. tree-sitter is error-recovering — it produces a tree for input it does not understand rather than refusing — so the walker checks the root for ERROR/MISSING nodes and maps their presence onto `complete = false`. A parse the grammar did not confidently understand never claims to be complete. The two keywords the grammar leaves unstructured (`time`, `coproc`) are handled by name so their real program is still disclosed.

**Non-interactive mode is unaffected.** `AllowOnly` still means Ask → auto-deny; grants start empty in a headless session and stay empty, because nothing can prompt to create one. Running bash headless is `per_tool_overrides: {"bash": "allow"}` — containerisation is then the operator's responsibility, which is the contract either way.


**On the wire.** `PermissionRequest` carries `repeated GrantOffer grants` and a `bool complete`; `PermissionResponse` carries `repeated string granted_keys`. Two rules hold at the boundary, and both are tested across a real socket rather than only in-process:

- **`granted_keys` is filtered against what the request offered.** A grant is only meaningful as the answer to something a human was shown, so accepting arbitrary keys off the wire would let a client mint permission for a binary no prompt ever displayed — precisely what disclosure exists to prevent.
- **`complete = false` survives serialization.** If it did not, the wire path would be silently more permissive than the in-process one: an unnameable command would satisfy the conjunction vacuously.

`ToolExecutor` remains the sole owner of the grant set; the checker only reports what the operator chose.

## `/`-command system — `src/server/commands/`

Harness-level control operations dispatched from the client over the bidi stream and executed as code — they bypass the agent turn loop and the LLM entirely, except `LLMCommand`s which deliberately touch the provider.

```cpp
struct CommandArgs { std::vector<std::string> positional; std::unordered_map<std::string,std::string> flags; std::string raw; };
enum class CommandDisplay { Status, View, Silent };  // where a result surfaces -- never the chat
struct CommandResult {
    enum class Status { kOk, kError } status = Status::kOk;
    std::string message;
    CommandDisplay display = CommandDisplay::Status;  // Status: ephemeral line (errors too, styled
                                                      //   as errors -- never persisted).
                                                      // View: open/refresh the view named by
                                                      //   view_hint. Silent: status bar only.
    std::string view_hint;  // a client view id; a pair plugin registers both sides
};

class Command {
public:
    virtual std::string_view Name() const = 0;
    virtual std::string_view Summary() const = 0;
    virtual std::string_view Usage() const = 0;
    virtual bool RequiresConfirmation() const { return false; }
    // Touches no session state, so it may run beside a live turn -- on a
    // short-lived task, never inline on the reader. Defaults to the safe answer.
    virtual bool ReadOnly() const { return false; }
    virtual CommandResult Execute(CommandContext&, const CommandArgs&) = 0;
};

class HarnessCommand : public Command {};  // never touches the conversation path; read-only ones
                                            // may run concurrently with an in-flight turn
class LLMCommand : public Command {};       // may issue provider requests or submit turns
class PromptShorthandCommand : public LLMCommand {};  // submits an interpolated prompt as a
                                                      // user turn; the only kind that enters the chat
```

**Command results never enter the chat, and never reach the model.** A result is a wire event only — by construction it appends to no history and is sent to no provider; the ruling is about display. `CommandDisplay` decides where it surfaces: an ephemeral **status line** (errors included, styled as errors — never persisted), a **view** (`view_hint` names the client view a pair plugin registered), or **silent**. The one exception is `PromptShorthandCommand`: it submits an interpolated prompt as a user turn, so its content reaches the model *as the turn itself* and enters the chat exactly as a normal user turn would — never as a result message. The Harness/LLM boundary is the **conversation path** — the turn queue and chat-completions requests — not "network vs. no network": `/model-info` is a read-only `HarnessCommand` that fetches model metadata over HTTP and renders a view, safely beside a live turn. Subagent completion injections are *not* command results; they stay real messages the model must see. Finer inheritance under `Command` stays open; `docs/EXTENSIBILITY.md` §4.4 is the spec.

`CommandRegistry` holds both uniformly. `/help` lists live from `Name()`/`Summary()`/`Usage()` — no config-directory manifest, since a command's value is the code it runs and a data-only manifest would be a drift-prone shadow copy. `CommandDispatcher`: parse → busy-check → confirm if required → execute → emit result.

**Confirmation** reuses a transport-level primitive rather than `PermissionChecker` itself (tool permission requests carry LLM-originated tool input; command confirmations don't fit that shape):

```cpp
struct ConfirmationRequest { std::string kind, title, detail; std::vector<std::string> options; };
struct ConfirmationResponse { std::string chosen_option; };
class ConfirmationBroker { public: virtual ConfirmationResponse RequestConfirmation(const ConfirmationRequest&) = 0; };  // blocks
```

Both `PermissionChecker`'s implementation and `CommandDispatcher` compose the same `ConfirmationBroker` — the one place tool and command plumbing intentionally share code (the transport primitive, not the domain interface).

**`CommandContext`** borrows references the session already owns: active `LlmProvider` and its resolved `ProviderConfig`, `SessionStore`, `AgentPresetRegistry` (+ active preset, mutable for `/agent`), `Settings`, plus `EmitStatus`/`EmitError`. It deliberately does **not** expose bash session state: bash is a plugin and owns that itself (see below).

**Bundled commands** (`commands-core` plugin): `/help [command]`, `/providers`, `/compact` (`LLMCommand`), `/clear` (confirm), `/agent <preset>`, `/model <provider>/<model>`, `/permission-mode <normal|bypass|allow-only>` (confirm). `/help` and `/providers` are `ReadOnly()` and render as views; the config-changing commands surface as status lines. None enters the chat.

`/new`, `/attach` and `/sessions` are **deferred to the TUI** (track E): they drive a tab model, and a command creating a session no client can attach to would be wire surface built ahead of its consumer.

**`/agent` and `/model` are two knobs.** A preset supplies a *default* model; a model the user named outranks it and **survives a later `/agent`**, which re-resolves the preset from disk and must not silently undo the choice. The override lives on the session, not in the preset, which is what makes that possible. Neither takes the searchable picker ARCHITECTURE's UX implies — that arrives with the TUI; until then `/agent <name>` and `/model <provider>/<model>` take exact names, and `--agent`/`--model` cover the headless path.

**The dispatch split, and why it is structural.** Turns and mutating commands share ONE queue on the session, and the worker pops from it only between `RunTurn` calls — so "a command never lands between a `tool_use` and its `tool_result`" is true by construction rather than by a check someone could forget. It also makes a preset or model switch safe to swap the session's `LlmProvider` under `AgentLoop`'s reference: the worker is provably not inside a provider call at that moment.

**A plugin owns the commands that touch its own state.** `/restart-shell` lives in `tools-bash`, not `commands-core`, because bash's per-session cwd/env lives inside that plugin and a command belongs to the plugin that owns the state it touches. Registering a tool *and* its associated command from the same plugin is the general pattern, and the reason `minimal_plugin_register_tools` and `..._register_commands` are independent optional symbols rather than a single "kind" of plugin. What other plugins may now do is *ask*: the service registry (§ Cross-plugin services) is the sanctioned pull channel, and `/restart-shell` itself resolves the store through it.

### Name collisions and aliases

**Collisions are fatal**, and they are now actually *detected*: `NameRegistry` existed and was unit-tested from the beginning but nothing outside its own tests ever constructed one, so a duplicate registration silently shadowed via `insert_or_assign` and which plugin won depended on directory iteration order. `plugins/name_resolution.{hpp,cpp}` (`ResolveRegistrationNames`) feeds it from the tool, command, and service attributions, applies `aliases` routed to whichever kind claims each key, and the startup sanity check reports the result. `ToolRegistry::Register` now *shadows* a duplicate rather than destroying it — destroying the first registrant made a collision unfixable by aliasing, since the object an alias would have renamed was already gone. Two plugins registering the same tool name, command name, service name, `ViewSpec::id`, or `ToolResultRenderer::tool_name` halt startup with a diagnostic naming both. Silent shadowing is the wrong failure mode — it makes behavior depend on plugin load order.

Resolution is disabling one plugin, rebuilding, or **aliasing** via an optional `plugins.json` field. The key is `aliases` — it applies to TOOLS as well as commands (one flat `plugin.name`-keyed namespace; a plugin shipping both a tool and a command under one original name aliases them together or not at all). It was renamed from `command_aliases`, which claimed a scope it did not have; the old key is ignored with a startup warning rather than silently applying:

```json
"aliases": { "bash_over_ssh.bash": "ssh_bash", "commands-core.compact": "c" }
```

Alias keys are always fully qualified, so they are unambiguous even *before* collision resolution; aliases are applied at registration, **before** the collision check, so aliasing is itself a valid way to resolve one. In practice that means `ToolRegistry::ApplyAliases()` — run once after every plugin and core built-in has registered — re-keys each aliased tool under its surface name, wrapping it in `AliasedTool` so `spec()` advertises the surface name the model must call it by. This is not the command manifest rejected above: that would have *declared* commands as data (a shadow of code, free to drift). This *overrides metadata of code-registered entries*, and a key resolving to nothing is detectable at load.

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

`per_tool_overrides`, `allowed_tools`, and `allowed_skills` **never reference an alias**, so renaming for typing convenience cannot detach a rule from what it was meant to govern. This is enforced at EVERY policy lookup, not just the first: the subagent ceiling — the parent's policy handed down as the child's upper bound — also resolves through the permission key, because an aliased tool missing its parent's per-tool `deny` would let the child run exactly the tool the parent's operator had denied, under its alias. Two lookups, one rule, and a test fails if either regresses to the surface name.

Two more startup guarantees close holes that registration ORDER had reopened. Core tools that register at serve stage — `spawn_subagent` is the one — are RESERVED by name before the fatal-collision check runs, so a plugin claiming the name is a diagnosed collision rather than a silent `insert_or_assign` shadow. And both registries keep the FIRST registration of a duplicate name, identically, before and after alias application: the loser is parked where diagnostics can see it, never swapped in silently.

**Replacing a bundled tool needs no new mechanism**: set `tools-bash` to `enabled: false` and ship a plugin registering `bash`. Permission config keyed `bash` continues to apply to the replacement. The fatal-collision rule is precisely what makes this an explicit act rather than an accident.

## Server plugins — `src/server/plugins/`

Native `.dylib`/`.so`, loaded into the server process. No sandboxing is possible via `dlopen` — plugins run as trusted native code with full process privileges; this is disclosed, not hidden.

**Source compatibility, not binary compatibility.** Plugins are compiled from the same tree with the same pinned toolchain as the binary, so the contract is source-level: an interface change is a compile error you fix on rebuild, not a silent break. The ABI tag is therefore a *"you forgot to rebuild"* guard, not a compatibility promise — which is what makes shipping core functionality as plugins safe even while `Tool` and `LlmProvider` are still evolving.

- Host and plugin bake an ABI tag (compiler id+version + stdlib, computed at build time). Plugin exports `extern "C" const char* minimal_plugin_abi_tag()`; mismatch → refuse to load, log, don't crash. With a matching tag, real C++ types cross the boundary directly — no C-only shim needed.
- *(distribution)* Ship a pinned reference toolchain (the CI build config doubles as the "build plugins against this" spec) so contributors aren't guessing which compiler/stdlib produces a loadable plugin.
- *(distribution)* No remote plugin installation or registry in v1; discovery is local-directory-only.
- `dlopen(path, RTLD_NOW | RTLD_LOCAL)`. `RTLD_NOW` so an unresolved symbol surfaces at load with a name attached rather than as a crash the first time that path runs. `RTLD_LOCAL` governs whether the *plugin's* symbols join the global namespace; it does not stop the plugin resolving *host* symbols.
- **Handles are held for process lifetime and never `dlclose`d.** A registered `Tool`'s vtable lives inside its `.so`, so unloading a plugin while the registry still holds its objects is a use-after-free that surfaces as an unrelated crash much later. This is the kind of thing someone tidies up in good faith, so it is called out in `plugin_loader.hpp` as well.
- **`minimal_server_core` is a SHARED library, and plugins link it normally.**

  This reverses an earlier rule ("a plugin must not link core") whose reasoning is worth preserving: while core was *static*, linking it into both the host and each plugin would have given every plugin its own copy of every global — separate registries, separate logger — failing at runtime in ways that look nothing like a linkage bug. That was correct about static libraries.

  But it left a latent failure that only appears once something actually calls `dlopen`: an archive member is pulled into an executable **only if referenced**, so a plugin calling a core function the server itself never calls would find that symbol absent from the host image and fail at `dlopen(RTLD_NOW)`. The arrangement worked only because the stub server happened to reference the little it needed. Making core shared dissolves both problems at once — one copy in memory, one set of globals, ordinary symbol resolution — instead of working around either with `-rdynamic`/`-undefined dynamic_lookup` and forced whole-archive linking.

  Verified rather than assumed: a bundled plugin reports `@rpath/libminimal_server_core.<soversion>.dylib` under `otool -L`, with core symbols undefined in the plugin itself.

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
  void minimal_plugin_register_tools(ToolRegistry&, const PluginRegistrationContext&);
  void minimal_plugin_register_commands(CommandRegistry&, const PluginRegistrationContext&);
  void minimal_plugin_register_provider_factories(ProviderFactoryRegistry&,
                                                  const PluginRegistrationContext&);
  void minimal_plugin_register_session_observer(SessionObserverRegistry&,
                                                const PluginRegistrationContext&);
  void minimal_plugin_register_services(ServiceRegistry&, const PluginRegistrationContext&);
  void minimal_plugin_register_permission_advisor(PermissionAdvisorRegistry&,
                                                  const PluginRegistrationContext&);

  // --- Exclusive: one global choice. First plugin (by plugins.json order) wins;
  //     a second registration logs a warning and is ignored. ---
  void minimal_plugin_register_mention_resolver(MentionResolverRegistry&);
  void minimal_plugin_register_auth_provider(AuthProviderRegistry&);
}
```

The ABI header has **two halves**, and both are frozen surface:

- **Extension points** plugins *implement*: `Tool`, `Command`, provider factory, `SessionObserver`, service provider, `PermissionAdvisor`, `MentionResolver`, `AuthProvider`.
- **Utilities** plugins *consume*: `path_confinement`, `file_access`, `atomic_write`, `HttpClient`, `Logger`, `SessionReader` (read-only history access), the frontmatter parser — and the `ServiceRegistry` pointer carried on `PluginRegistrationContext`, which is the consumer side of the service channel (§ Cross-plugin services).

The surface above is verified against the tree. The remaining designed points — MCP lifecycle observers, model auto-discovery — are catalogued with suggested architectures in `docs/EXTENSIBILITY.md`.

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

**Hard constraint: observers must never wedge a turn.** They return `void` and cannot veto — vetoing is `PermissionChecker`'s job — and they are dispatched **fire-and-forget**, never awaited. (Deliberately *not* "run with a timeout": a detached task cannot be interrupted in C++, so a timeout there would be decorative.)

**Dispatch is one ordered thread, not a pool.** A pool guarantees no ordering between tasks, and these notifications are inherently ordered — an observer that keys off `OnSessionCreated` and releases on `OnSessionEnded` is broken if those can arrive in either order or concurrently. A pool would also make every observer's own state a shared mutable, so writing a correct observer would demand that plugin authors be concurrency experts. Serializing costs nothing real (observer work is meant to be short) and buys both ordering and the guarantee that an observer never runs concurrently with itself. Verified: a second dispatch thread makes a counting observer drop notifications intermittently.

The failure mode shifts accordingly. A hung observer stalls *further* notifications rather than leaking one pool thread — contained (sessions keep running; only notifications stop), and the right trade against silently racing plugin state.

A throwing observer is expected rather than exceptional, since observers are plugin code: each call is wrapped, and one observer's failure never stops the others.

### Cross-plugin services — ask, don't grab

The sanctioned **pull** channel (`SessionObserver` is the push channel): one plugin can reach another's services without either exposing internals. The providing plugin exports `minimal_plugin_register_services(ServiceRegistry&, const PluginRegistrationContext&)` and hands the registry a non-owning pointer to an **abstract** interface under a name; the same registry pointer rides `PluginRegistrationContext::services` to every registration hook, so a consumer captures it at registration and resolves with `Find` lazily at use time — plugin load order is file order, so the consumer may load before its provider. `Find` returning null is a runtime condition (the provider is disabled), not an error: degrade with a clear message.

Three rules keep it from becoming an event bus or a hidden dependency:

- **Typed at the edges, named in the middle.** The provider ships a header-only, pure-virtual interface (its symbols live inside the providing `.so`, `RTLD_LOCAL`, so nothing else can call them by name — virtual dispatch needs no symbols); a consumer `static_cast`s `Find`'s result to it. The name is the only runtime coupling; the header is the contract, under the same source-compatibility model as everything else.
- **Collisions are fatal, not shadowed.** Service names feed `NameRegistry` as their own `NameKind` — a separate namespace, so a service may share a name with a tool or command — and the startup check names both claimants. Services are not user-aliasable: the name is addressed by code, never by the model or the user.
- **No bus, no subscribe.** Push already exists; this is pull, deliberately.

First consumer: `tools-bash` provides its per-session shell state as `BashSessionStateService` (shipped header `bash_state_service.hpp`, name `tools-bash.session_state`), and `/restart-shell` resolves the store through `Find` — the same lookup a third-party consumer would use. The full §2.2 spec is `docs/EXTENSIBILITY.md`.

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
| `read`/`write`/`edit` (`tools-fs`), `grep`/`glob` (`tools-search`), `bash` (`tools-bash`) | **Bundled plugins** | Not structurally privileged. Bash is split out: distinct security profile, and the one with a plausible alternate implementation (container/remote executor) someone would actually swap. |
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

**Additive views: yes. Compositional: only through named slots.** The frame, the tab model, and the transport stay core; presentation is plugin surface — and most of the TUI is presentation. `docs/EXTENSIBILITY.md` §4 is the full core-vs-pluggable split and wins where the two disagree.

| Kind | Mechanism | Support |
|---|---|---|
| New self-contained view (panel or modal) | register a `View` — placement + default keybinding, remappable | **v1** |
| Fill a named slot | slot registry | **v1**: tool-result renderers, message-kind presentation, status-bar segments; follow-on: markdown engine, syntax grammars, chat surface |
| Wrap/replace core views, restructure the frame | — | **No** — write a different client. One exception: the chat surface below is replaceable wholesale, exclusively. |

Slots:

1. **Tool-result renderers keyed by tool name — v1.** The strongest case in a plugin-first harness: a plugin shipping a `sql_query` tool wants a table, a `diff` tool wants colored hunks, not raw text. The plugin owns both the tool and its rendering. A renderer registered for a tool whose plugin is disabled falls back to default rendering rather than erroring. A renderer for a *core* tool (a diff view for `edit`) is equally legitimate — bundled tools are themselves plugins, replaceable via `plugins.json`, and rendering follows the same rule.
2. **Message-kind presentation — v1.** Additive renderers keyed by message kind (agent text, thinking, tool activity, system); core supplies the defaults § *Message-kind rendering* specifies. The data plane — event demux, scrollback virtualization, streaming buffer — stays core regardless of who renders a row.
3. **Chat surface — exclusive.** One plugin may replace the whole chat view. The host pushes the session's events; scrollback pages come through the shared client API; second registration warns and is ignored (the `MentionResolver`/`AuthProvider` pattern). The interface is deliberately big and deliberately the only one of its kind — message-kind slots cover the common case, a whole new chat UI is a whole different component.
4. **Status-bar segments — v1.** Plugins register segment providers (`{git_branch}`); the format string orders and selects them by id; an unknown id renders nothing with a startup warning. Config keeps layout control; plugins add content.
5. **Markdown engine and syntax grammars — follow-on.** The md4c adapter becomes an exclusive-slot default; a replacement engine must carry the streaming contract itself (§ *TUI rendering*). Grammar registration is additive and language-keyed, the vendored set registering as the defaults.
6. **Tools-panel / session-picker columns — later.**

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
  void minimal_client_plugin_register_message_renderers(MessageRendererRegistry&);
  void minimal_client_plugin_register_status_segments(StatusSegmentRegistry&);
  void minimal_client_plugin_register_chat_surface(ChatSurfaceRegistry&);        // exclusive
  void minimal_client_plugin_register_markdown_engine(MarkdownEngineRegistry&);  // exclusive
  void minimal_client_plugin_register_syntax_grammars(SyntaxGrammarRegistry&);
}
```

Both registries follow the server's collision rules: duplicate `ViewSpec::id` or `tool_name()` is fatal and alias-resolvable, and `keybindings.json` addresses views by their canonical id, never an alias. A renderer registered for a tool whose plugin is disabled falls back to default rendering rather than erroring.

Three policies complete the surface:

- **Plugin modals never preempt the permission modal.** Permission/confirmation prompts are always topmost in the modal stack; a plugin modal cannot mask one or steal its input. Invariant, tested.
- **Client plugins own their data path.** A plugin view wanting live data subscribes itself through the generic `PluginCall`/`PluginSubscribe` channel; core transports and never interprets plugin payloads. A plugin *pair* binding a server command to a client view works the same way: the client side observes its own `CommandResult` — whose `view_hint` names the view — and opens or refreshes it.
- **Attention is raiseable.** Plugins receive an `AttentionSink` at registration and may raise or clear tab/global attention; core event derivation (§ *Tab states and attention*) remains the default source, and the `!` glyph stays the only color-independent signal.

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

**Syntax highlighting**: **tree-sitter** core plus a curated grammar set (`cpp`, `python`, `javascript`/`typescript`, `bash`, `json`, `yaml`, `rust`, `go`). A hand-rolled regex tokenizer was considered and rejected: regex cannot soundly handle nested/context-free constructs (Rust nested block comments, C++ raw strings, JS template-literal interpolation), so it fails exactly where correctness matters, and every language becomes a bespoke ruleset to keep debugging — with visibly ugly failures, since this is user-facing output. tree-sitter's cost is front-loaded and mechanical: each grammar repo ships a generated `parser.c`, so vendoring means compiling a small C file, not running the tree-sitter CLI/Node toolchain. Per-language work is one node-type → category table. The `FetchContent` vendoring path this needs already exists — built for the bash tool's command scanner, one track earlier — so a grammar here is a target declaration, not new plumbing.

```cpp
enum class HighlightCategory { Keyword, String, Comment, Number, Function, Type, Operator, Plain };

class SyntaxHighlighter {
public:
    // Returns plain text if `language` isn't in the curated set.
    ftxui::Elements Highlight(std::string_view code, std::string_view language) const;
};
```

The highlighter never hardcodes a color; the theme supplies one per `HighlightCategory`, keeping highlighting and theming separate.

**Both engines are slots, not privileged code paths.** The md4c adapter is the default registrant of an exclusive markdown-engine slot — a replacement engine must carry the streaming contract itself (partial input cannot soundly parse; the debounce and the authoritative `Finalize()` reparse are the implementer's obligations, not the host's). The syntax highlighter takes additive, language-keyed grammar registrations with the vendored set as the defaults. `docs/EXTENSIBILITY.md` §4.2 specifies both.

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
| Command results (`/help`, `/providers`, config changes) | view, status line, or transient error — **never a chat row** (§ `/-command system`) | `CommandResult.display` decides |

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
| System/harness | `ErrorEvent`, subagent-completion injections | Muted, distinctly prefixed — separates "the harness told you something" from "the model said something". Command results are **not** in this row: they surface as views/status lines and never as chat messages (§ `/-command system`) |

### Customization surface

Three tiers:

1. **Config** — no code: `themes/*.json` (now with namespaced plugin sections — `git_plugin.branch` — merged after core roles so a plugin can never shadow one); **`keybindings.json`** (not optional: terminals disagree on what they can even *encode* — `Shift+Enter` is indistinguishable from `Enter` without the kitty keyboard protocol / CSI-u — so remapping must be user config; default newline-in-prompt is the more portable `Alt+Enter`); status bar as a format string (`"{version} | {model} | {tokens} | {agent} | {permission_mode} | {session}"`) that orders and selects registered segments — core's and plugins' alike; layout ratios and collapse/timestamp toggles.
2. **Server plugins extend UI *content* without touching UI code** — a plugin command appears in `/`-autocomplete via `ListCommands`, and its results surface as a view or status line, never as a chat message (§ `/-command system`); a plugin tool appears in chat and the tools panel automatically.
3. **Client plugins** for additive views, message-kind and tool-result renderers, status segments, and — exclusively — the chat surface and markdown engine; for anything beyond (frame, tab model, transport), **write a different frontend** against `SessionService` — language-agnostic, process-isolated. This is also why @-mention resolution lives server-side: every frontend inherits it.

## @-mention file references

Typing `@` triggers path autocomplete; selecting inserts `@<relative-path>`.

- **Suggestion** goes through the `SuggestPaths` RPC (debounced ~100–150 ms), resolved by the server's `MentionResolver` so the strategy is pluggable and every frontend inherits it.
- **Expansion is server-side**: on receiving a `UserChatTurn`, the server scans for `@<path>` tokens before wrapping it into a `Message`, resolving each via `path_confinement::confine` + `file_access::read_window` — reused from the Read tool, not reimplemented. A file mention inlines windowed content; a directory mention inlines only the file tree/names, keeping it bounded. **Not** `PermissionChecker`-gated: the human explicitly asked for that content, exactly as if they had pasted it.
- **Client-side**: a hand-rolled inline suggestion list in the prompt editor. `Dropdown` exists but is a click-to-open selector; type-ahead completion reads better inline.

## Non-interactive client (`minimal -p`)

A second, headless client against the same `SessionService` — not a TUI flag. It ships in v1 for three reasons: `PermissionRunMode::AllowOnly` was designed precisely for it (without a headless consumer that mode has no user and no test); it is the smallest real client, so it is what proves the proto is a genuine boundary rather than a TUI-shaped hole; and `--output-format json` is nearly free given the event stream is already structured.

Scope: one turn in; `text` or `jsonl` out; `AllowOnly` by default with an explicit `--dangerously-bypass`; exit code derived from `stop_reason`/`ErrorEvent`. No tabs, themes, or Markdown rendering.

```
minimal -p PROMPT [--server TARGET] [--agent PRESET] [--model PROVIDER/MODEL]
                  [--resume SESSION_ID]
                  [--output-format text|jsonl] [--dangerously-bypass]
```

`--server` defaults to `unix://<data dir>/server.sock` and accepts **either a bare socket path or a full gRPC target** — `minimal-server --listen` takes a path, and two halves of the same pairing that disagree is a trap rather than a preference (`--server /tmp/x.sock` produced a gRPC resolver error naming neither flag). A value containing `://`, or shaped like `host:port`, is passed through untouched. **The client does not yet auto-spawn a server**; it reports how to start one, naming the `--listen` argument that matches what was dialled. The refcounted-exit half is implemented, which is what auto-spawn will rely on.

`--resume SESSION_ID` continues a stored session instead of creating one — the headless expression of `CreateSessionRequest.session_id` (§ *Session storage*, "Resume"). The resumed session keeps its stored preset and `project_root`, so the flag is mutually exclusive with `--agent`, and the run-mode default (`AllowOnly`) applies to the resumed turn exactly as to a fresh one. It requires `-p`: resuming runs one headless turn, and the interactive surface that would own a non-headless resume is the TUI's session picker, not a flag.

**Argument parsing rules, because the failure mode was silence** (`client/cli_args.{hpp,cpp}`): a value-taking flag never accepts a flag-shaped value — write `--flag=value` when the value legitimately starts with `-` — and an unrecognised argument is an error rather than being dropped. Both matter together: `minimal -p --server unix:///x "do the thing"` used to send the model the prompt `"--server"`, discard the target and the real prompt, and then report that it could not reach the *default* socket.

Exit codes are interface, because a script piping `-p` has to tell "the model finished" from "it was cut off" from "the call failed": `0` ok, `2` truncated (`max_tokens`/`stop_sequence`), `3` model error or `ErrorEvent`, `4` cancelled, `64` usage, `69` no reachable server.

**SIGINT/SIGTERM during a turn cancel through the protocol, not by dying.** The handler does only async-signal-safe work (set a flag, write one byte to a self-pipe); a watcher thread turns that into a `CancelTurn` ClientEvent on the live stream — the same path § *Cancellation* describes — and the process exits `4` once the server reports `turn_completed` with `StopReason::Cancelled`. The signal handlers are installed only while a turn is in flight: before the turn exists there is nothing to abandon, and after it ends a late signal should kill, not defer. A second signal after the cancel was issued restores the default disposition rather than waiting out a wedged server.

**Attaching to child sessions is a client protocol obligation, not a TUI behavior.** Any conforming client must attach on `SubagentStartedEvent` — otherwise the initial-attach guard would kill a subagent spawned during a headless run. Attaching is expressible without submitting work through a dedicated no-op `Attach` `ClientEvent` payload (the stream is named by its first event, and before `Attach` existed every payload did something). The headless client merges child output into its stream, or discards it under `--output-format text`.

## Agent loop — `src/server/agent/agent_loop.{hpp,cpp}`

Core, and the thing every other component in this document plugs into. Runs on the session worker thread (see Concurrency model), one turn at a time.

```
RunTurn(session, seed_message):
  append + persist seed_message
  loop:
    if cancelled: break
    if auto_compact && context_occupancy > trigger: compact()   # occupancy = last turn in+out;
                                        # trigger resolved per session — §Context compaction
    request = BuildChatRequest(session)
    response = provider.StreamComplete(request, sink)    # sink emits StreamEvents; false => cancel
      on LlmError e:
        if e.is_retryable() && attempts < max: backoff; continue  # exponential,
                                        # equal jitter, polls the cancel flag
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

**Tool lifecycle events.** The loop brackets each `ToolExecutor::run` with a `ToolLifecycleEvent` (started, then finished-with-`is_error`), surfaced as `ToolExecutionEvent`. The finish is emitted even when the tool failed: a client that showed a tool as started must always see it finish, or its running-tools panel accumulates entries that never clear. It deliberately carries **no summary line** — producing one means calling `Tool::Disclose()`, which for bash resolves `argv[0]` against the session `PATH`. That is worth paying when a human is about to be asked a question, not on every call of an already-allowed tool; rendering from `tool_name` + `input` is the client's job.

- **Tools must be emitted in deterministic (sorted) order.** `tools` sits at the very front of the serialized prefix providers cache on, so non-deterministic ordering would silently destroy prompt-cache hits on every single turn, with no visible symptom other than cost and latency.
- **`stop_reason == max_tokens` with no pending `tool_use` must be surfaced**, not treated as normal completion. Otherwise output is truncated mid-sentence and the turn looks successful.

**The subagent loop is not a second loop.** A child session's entire life is one `RunTurn` seeded with the task message, plus a capture-final-assistant-text step when it stops, bounded by `max_turns`. There is one loop implementation, not two.

## Context compaction — `src/server/agent/`

Triggered from the agent loop when `memory.auto_compact` is on and a session's **context occupancy** (see Token accounting) crosses the threshold resolved from `memory.context_compaction_trigger` — a **one-of strategy choice**, not a bag of knobs:

```json
"memory": { "context_compaction_trigger": { "percent": 80 } }
```

| Key | Trigger fires when occupancy crosses |
|---|---|
| `percent` (1–100) | that percentage of the active provider entry's `context_window_tokens` |
| `buffer_tokens` (> 0) | `context_window_tokens` minus that many tokens — i.e. the context comes within that distance of the window's edge |
| `tokens` (> 0) | that absolute count — the original behaviour; also what the pre-strategy flat key `context_compaction_trigger_tokens` still parses as |

Absent means never trigger (only `/compact` then). The loader rejects anything but exactly one key — a typo like `percnet` must not silently disable compaction, which is precisely the failure mode a bare unknown-key policy creates.

The window-relative strategies resolve **per session, against the window of the provider entry that session actually uses** — the window rides on the resolved `AgentPreset`, not on the loop config, because `/agent` can switch provider entries mid-session and the trigger must track the window in use. A model with no `context_window_tokens` anywhere — its own, its provider's default, or model discovery (track N fills the gap from the backend's own listing when it can) — leaves a window-relative strategy with nothing to be relative to: the trigger then never fires, and the loop says so once per session rather than silently. That is a live state, not a corner case — the seeded `providers.json` is empty and the field is optional; a buffer at least as large as the window degenerates the same way.

Setting `auto_compact: false` disables automatic compaction entirely, leaving `/compact` as the only trigger. Compaction only changes what goes into the *next* `ChatRequest.messages` — it never mutates persisted history, so scrollback always shows the full conversation.

**The view, however, is checkpointed — `compactions` table, one row per session.** Each successful compaction upserts `{boundary_seq, summary_message}`: a cache of the *derived view*, never a message row, so `messages` stays the exact record of what was said and the sentence above stays literally true. The reason it exists is the resume path. Without a checkpoint, a resumed session loads the full uncompacted transcript and re-summarizes from scratch — real provider calls, real money, on *every* resume — and worse, the halving strategy silently assumes the half being summarized fits the model window. That assumption holds for a live session (occupancy trips the trigger near 0.8×window), and breaks for a transcript well beyond it: resuming a 3M-token transcript against a 200k window could not even fit its own summarization request, and the first turn failed with a 400 before any compaction could converge. With a checkpoint, resume reconstructs `[summary + tail]` — the working set the live session actually had, bounded by the kept context, zero summarization calls. Correctness relies on one invariant: the persisted messages in the loop's working history are always a **contiguous suffix** of the stored transcript (appends continue at `MAX(seq)+1`; compaction only folds from the head), which is what makes `boundary_seq` derivable from where the persisted portion starts. Failure semantics degrade to the old behavior, never to a lost transcript: a failed checkpoint write logs and continues; `/clear` deletes the checkpoint, because it drops the *view* and the checkpoint is a view; deleting the session cascades at the SQL layer. Sessions that never compacted — and ones that compacted before checkpoints existed — take the full-transcript load path.

- Everything older than the cut is replaced by one synthetic summary message; the cut is currently half the working history by *message count* (snapped, below). Selecting it by `memory.compaction_keep_recent_tokens` needs per-message token counts, which are not tracked in memory — flagged in the development plan's known gaps.
- **The boundary must snap backwards to the nearest complete tool-call cycle.** A count-based boundary is oblivious to tool pairing, and cutting between an assistant `ToolUseBlock` and its `ToolResultBlock` orphans the call — a hard 400 on both OpenAI and Anthropic, not a soft degradation. The snap lives in the agent loop (`snap_compaction_boundary`), so the compactor always receives a range that ends on a complete cycle.
- Summarization goes through the active provider using `prompts/compression.md`, overridable per preset via the `compression_prompt` frontmatter field. **A missing prompt disables compaction outright** — `compact_messages` refuses to send a summarization request with no instructions, which would burn tokens to produce garbage — so delivering the default is what makes the feature exist at all rather than a packaging detail. The request passes the messages **verbatim, tool blocks included** — both wire formats accept them in history without a `tools` array — with the prompt as `system`. It runs through `StreamComplete` with a sink that polls the session's cancel flag, so `CancelTurn` aborts an in-flight summarization instead of stalling until `request_timeout`.

```cpp
struct CompactionResult { Message summary_message; int64_t context_tokens_before; };

// Production compactor. nullopt = "no compaction this cycle" -- empty prompt,
// provider error, cancellation, or a response with no usable text. Failure is a
// value, not an exception: the caller leaves history untouched, so a broken
// summarizer degrades to no compaction, never to a lost transcript. The prompt
// and model are per-call rather than bound at construction because /agent can
// switch presets mid-session.
// Everything that varies per call, including the preset's sampling parameters and
// max_tokens -- the summarization request is built from the same settings as the
// conversation it summarizes.
struct CompactionRequest {
    std::string_view model_name, compression_prompt;
    nlohmann::json parameters;
    std::optional<int> max_tokens;
    int64_t last_reported_input_tokens;
    const std::vector<Message>* full_history;   // fallback estimate only
    const common::Logger* logger;               // null = DefaultLogger
    std::string_view session_label;             // "session <id>: " on log lines
    std::function<bool()> is_cancelled;
};

using Compactor = std::function<std::optional<CompactionResult>(
    std::span<const Message> messages_to_compact, LlmProvider& provider,
    const CompactionRequest& request)>;

std::optional<CompactionResult> compact_messages(/* same signature */);
```

The summary is a `User` message carrying a fixed framing line plus the model's text — the shape both wire formats accept at the head of a history. **A summary that stopped on `max_tokens` is refused**, for the same reason the loop refuses to treat `MaxTokens` as normal completion, and with a worse consequence: it is cut off mid-sentence and would permanently replace half the conversation with a fragment whose only other copy is in SQLite, where the loop cannot reach it.

`context_tokens_before` is the size of the **whole context** going into the request that tripped the trigger — not the size of the range replaced. Both branches measure that same span: the backend's own count when it reported one (ground truth), else a bytes/4 estimate over the same history. There is no tokenizer in this codebase. The two used to measure different things, so the logged number overstated what compaction had replaced by roughly the size of everything it kept. A failed attempt sets a per-turn cooldown so the loop does not re-invoke a failing summarizer on every tool cycle of the same turn — each retry would resend half the context. The trigger stays armed for the next `RunTurn`, where the flag resets.

Also exposed as `/compact` (an `LLMCommand`) for proactive use. No confirmation — it touches no persisted data.

## Bundled assets

`bundled/` is the tree that ships with the source and is copied into the user's
config dir by **`cmake --install`** (`cmake/SeedConfig.cmake`). Prompts, agent
presets, `settings.json` and `permissions.json` all arrive that way.

**Nothing is compiled into the binary, and nothing is read from the install prefix
at runtime.** That is the rule, and it follows from the product philosophy rather
than from convenience: a file the user cannot see is a file they cannot change, so
every default has to land somewhere they can open it. One copy, in the config dir,
and that copy is the one that runs — no runtime lookup, no build-tree/installed
precedence, no shadowing question.

**NEVER OVERWRITES.** An existing file is the user's, edited or not. Silently
reverting someone's compaction prompt or preset on an upgrade would be a far worse
failure than shipping without a default — so a bundled file that gains a better
default in a later release does *not* propagate to anyone who already has one.
That is the deliberate trade. Seeding is per file, so a config dir missing only
`agents/` still gets the presets.

Seeding is best-effort and skips entirely under `DESTDIR` (a packaging root has no
user whose config dir it is). A missing bundle warns rather than failing the
install.

**`providers.json` seeds with no providers**, with `providers.example.json` beside it. It is
the one thing seeding cannot do for you: a `base_url` that is right on the machine
that built the binary is wrong on the next one, and a plausible-looking default
pointing at nothing is worse than an empty list that obviously needs filling in.
The install prints where to edit. The server still *starts* with no providers, so
the startup sanity check can report every problem at once rather than dying on the
first one.

**Plugins are the exception, and the only one.** They are code the user does not
edit, so they install under the prefix and are found relative to the running
executable (`common/bundled_assets.{hpp,cpp}`), which has to handle two layouts —
an installed tree (`<prefix>/bin/minimal-server`, libraries under `<prefix>/lib`)
and a build tree (`build/src/server/minimal-server`, libraries under
`build/plugins`). The install destination is compiled in from the same CMake cache
variable the install rule uses, so overriding one moves both.

## Config

**Path resolution** (`src/server/config/config_paths.{hpp,cpp}`): `MINIMAL_CONFIG_DIR` env override → else platform default. Linux/macOS: `$XDG_CONFIG_HOME/minimal` or `~/.config/minimal` (terminal-tool convention, matching `git`/`gh`/`rg`, not macOS's GUI `~/Library/Application Support`). `%APPDATA%\minimal` is reserved for when Windows support lands (2.x) — see Deferred; it is not a supported target today.

**Config is read once at startup; there is no hot-reload.** This is a decision, not an omission. Reloading raises questions with no clean answers — does a tightened permission apply to an in-flight tool call? does an edited system prompt apply retroactively, invalidating the cached prefix? what happens to a session whose provider was swapped underneath it? — in exchange for almost nothing, because sessions already terminate with their client and the server already exits with the last client. Restarting to pick up config costs nothing that quitting did not already cost.

**File format**: agents, skills, and prompts are Markdown with YAML frontmatter — the same convention Claude Code and opencode use: frontmatter for structured metadata, Markdown body for prompt text, loaded verbatim. `src/server/config/frontmatter.{hpp,cpp}` provides `parse_frontmatter_markdown(contents) -> {YAML::Node frontmatter, std::string body}` (split on `---` delimiters, frontmatter to **yaml-cpp**, remainder as body). The `.json` files stay plain JSON — harness config, not prompt content, with no body to split.

- **`agents/*.md`** — presets: frontmatter `name, description, model, allowed_tools, allowed_skills, allowed_mcp_servers, permission_mode: {read_only, file_write, execute, out_of_workdir, overrides}, mcp_permissions: {<server>: allow|ask|deny}, compression_prompt?`; **body is the system prompt directly**. `model` is a `<provider>/<model>` reference, or absent to take `settings.default_model`. `plan.md` and `build.md` ship in `bundled/agents/` and are seeded like everything else there.
- **`agents/subagent.md`** — plain Markdown, no frontmatter. Prepended to the resolved preset's body whenever `parent_session_id` is non-null. Also seeded.
- **`prompts/compression.md`** — default compaction prompt; a preset's `compression_prompt` overrides it, resolved relative to this directory. **Seeded from `bundled/prompts/` by `cmake --install` and never overwritten after** — see *Bundled assets* below.
- **`skills/<name>/SKILL.md`** (+ supporting files) — lazy-loaded: frontmatter (`name`, `description`) resident in the registry; body loads only on invocation.
- **`themes/*.json`** — role/category → truecolor RGB.
- **`keybindings.json`** — action → key. Must accommodate plugin-registered action names unknown at compile time.
- **`providers.json`** — **a provider is a CONNECTION, not a model.** An object keyed by provider name; each carries `backend` (a `ProviderFactoryRegistry` kind, so any plugin-registered kind is valid — no closed enum), `url`, `api_key | api_key_env`, and a nested `models` object keyed by model id — optional when the entry sets `"discover": true` (track N: the backend lists its own models at runtime, fetched lazily at first resolve and never written back; static entries beside it still resolve, and a discovered model's `context_window_tokens` overrides the provider-level default the same way a static model's own value would). `parameters` and `max_tokens` may sit on the provider as defaults and be overridden per model; `context_window_tokens` is per model, and is both informational (the status bar's `used/max`) **and the basis the window-relative compaction triggers resolve against — a model that declares none never fires them** (see *Context compaction*).

  ```json
  { "providers": { "local": {
        "backend": "openai", "url": "http://localhost:8000/v1",
        "max_tokens": 4096, "parameters": {"temperature": 0.3},
        "models": { "qwen4": {"context_window_tokens": 131072},
                    "small": {"context_window_tokens": 32768, "max_tokens": 1024} } } } }
  ```

  A map rather than an array because the key *is* the name, so it cannot disagree with a `name` field beside it. The earlier shape made an entry *be* a model, which meant two models on one endpoint duplicated the connection and — since `parameters`/`max_tokens`/`context_window_tokens` lived on the entry — one endpoint serving a 128k model and a 32k model was **inexpressible**. A top-level array is refused by name, not silently misparsed. A model's `parameters` *replaces* its provider's rather than merging key-by-key, so what a model declares is exactly what it sends.

  `max_tokens` is typed rather than left to `parameters` because the serializer treats it as a structural field and erases whatever `parameters` put there — so it was the only route to an output cap, and it did not exist. It applies to every request the session makes, summarization included: a cap the conversation honours and the compactor ignores is how a summary gets truncated.
- **`mcps.json`** — `[{name, transport: "stdio"|"http", command, args[], env{} | url, headers{}, enabled}]`. `env`/`headers` support `${ENV_VAR}` interpolation, resolved once at load and never logged. No per-server risk category — MCP permission is server-scoped, resolved separately.
- **`permissions.json`** — security config, split from `settings.json` so it can be reviewed and locked down independently: `{default_run_mode: "normal"|"bypass"|"allow-only" (default "normal"), fallback: {read_only, file_write, execute, out_of_workdir}, per_tool_overrides: {}, mcp_default_mode: "allow"|"ask"|"deny" (default "ask")}`. A global floor a preset can still override — no clamping; guardrails presets cannot loosen are future scope.
- **`settings.json`** — `default_agent_preset, default_model (a `<provider>/<model>` reference), default_theme, max_concurrent_subagents (default 4), memory.{auto_compact (default true), context_compaction_trigger (a one-of object: `{"percent": 1..100}` | `{"tokens": >0}` | `{"buffer_tokens": >0}`; absent = never; exactly one key or the loader rejects the file; the window-relative strategies resolve against the active MODEL's context_window_tokens and never fire without one — see Context compaction; the pre-strategy flat key context_compaction_trigger_tokens still parses as the fixed strategy, and the new object wins when both are present; the seeded settings.json sets `{"percent": 80}`), compaction_keep_recent_tokens (NOT YET HONORED — see Context compaction), scrollback_page_size (default 100 when unset, applied by the RPC layer to both GetMessages and ListSessions)}, tools.{read,edit,bash}.*, tools_view.stdout_buffer_lines (default 100), session_db_path, logging.{level, path, max_size_mb, max_files}`. `logging` backs a **spdlog** rotating file sink (no custom rotation code), with `path` alongside `session_db_path` under the platform *data* dir, not the config dir. Applied by `common::ConfigureLogging` at the very top of `main`, before anything logs -- `DefaultLogger()` caches spdlog's default on first use for the process lifetime, so the ordering is load-bearing. The console sink stays alongside the file one; a log file that cannot be opened degrades to console-only rather than refusing to start.
- **`plugins.json`** — an object, not a bare array (an earlier draft said array while also placing the aliases at the top level; both cannot hold):

  ```json
  {
    "plugins": [ {"file": "tools-fs.so", "provenance": "bundled", "enabled": true} ],
    "aliases": { "bash_over_ssh.bash": "ssh_bash" }
  }
  ```

  Newly discovered plugins are appended on startup with enablement defaulted by provenance, so the file is self-maintaining and the user only edits it to change a decision. Malformed content degrades to empty rather than throwing — a broken `plugins.json` should not prevent the server starting.
- **No `commands/` config dir** — a command's value is its code; a data-only manifest would be a drift-prone shadow copy.

**Seeding**: `cmake --install` copies `bundled/` into the config dir and never overwrites — see *Bundled assets*. Nothing is compiled into the binary and nothing is read from the install prefix at runtime, so every file above is one the user can see and edit, and the copy they edit is the copy that runs.

**Permission-mode precedence** (highest → lowest): preset `permission_mode.overrides[tool]` → preset `permission_mode.<category>` → `permissions.json` `per_tool_overrides[tool]` → `permissions.json` `fallback.<category>` → hard-coded default (`read_only=Allow, file_write=Ask, execute=Ask, out_of_workdir=Ask`). Note `out_of_workdir` is a *dimension* folded in via `most_restrictive`, not a `ToolCategory` value — see Permission model.

**MCP permission precedence** (separate chain, server-scoped): preset `mcp_permissions[server]` → `permissions.json` `mcp_default_mode` → `Ask`.

**Model addressing — one syntax, four positions.** A model is named `<provider>/<model>` in preset frontmatter (`model:`), in `settings.json` (`default_model`), and by `/model` and `--model`. One parser (`common/model_ref.{hpp,cpp}`, in `common/` because the client validates a `--model` value too and must not accept what the server would reject).

**Precedence: preset supplies a default, the user decides.** A preset's `model` is where a session starts; a model the user named with `--model` (or later `/model`) outranks it, and keeps outranking it across a later `/agent` — `/agent` and `/model` are two knobs, not one.

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
    AgentPreset Resolve(const std::string& preset_name) const;  // by value; internally cached+locked
};
```

`ToolExecutor`/`CommandDispatcher` consume the merged `AgentPreset`, never `permissions.json`/`providers.json` directly.

## Skills

`allowed_skills` **defaults to empty** — skills are off unless a preset opts in. The resolved preset renders its allowed skills' frontmatter (`name` + `description` only) into the system prompt as a registry listing (sorted by name, so the cached prefix never reorders); that listing is the entire resident cost.

Invocation is a core `SkillTool` (category `ReadOnly`): takes `{name}`, validates it against `allowed_skills` (delivered per-invocation via `ToolContext::allowed_skills`, which the agent loop populates from the active preset), and returns the `SKILL.md` body as its `ToolResult`. That call is the lazy-load trigger — bodies are never read until invoked. Its error ladder distinguishes *unknown* (no such candidate), *not loaded* (candidate exists but failed, carrying the reason), and *not allowed in this session* (listing what is).

**The directory name is the identity** — what `allowed_skills` references, what the listing shows, and what `Find()`/`LoadBody()` key on. A frontmatter `name` is optional but must agree with it when present; a disagreement is itself a load failure. This holds even for failed skills, so every entry has one stable name to point at.

```cpp
// src/server/config/skill_registry.hpp
struct SkillMeta {
    std::string name;               // directory name under skills/ -- the identity
    std::string description;        // frontmatter; empty when not loaded
    std::filesystem::path body_path;  // the SKILL.md candidate path
    bool loaded = false;
    std::string load_error;         // reason when !loaded
};

class SkillRegistry {
public:
    static SkillRegistry Load(std::filesystem::path config_dir);      // frontmatter-only scan
    const std::vector<SkillMeta>& All() const;                        // EVERY candidate, sorted by name
    const SkillMeta* Find(std::string_view name) const;               // null = no candidate at all
    std::optional<std::string> LoadBody(std::string_view name) const; // the lazy read; nullopt unless loaded
};
```

**Every entry directly under `skills/` becomes an entry in `All()` — including ones that failed to load**, with `loaded=false` and a `load_error` naming the problem (no SKILL.md, stray file, unterminated/malformed frontmatter, missing `description`, name mismatch). Consumers filter on `loaded`: the preset's listing renders only allowed ∩ loaded, and `SkillTool` reports a not-loaded skill's reason at invocation. `Load()` never throws for individual candidate problems — a broken skill degrades to an entry with a reason, not a startup failure (the sanity check surfaces these). `LoadBody()` is called only from `SkillTool::execute` and holds no mutable state, so concurrent calls from multiple session workers are safe.

## MCP client — `src/server/mcp/`

Core. Server subprocesses are **shared across sessions** (one per `mcps.json` entry, not one per session), matching the low-footprint goal. The `McpClientManager` owns them, constructed at serve stage with the parsed config.

**Transports.** cpp-mcp (`stdio_client`) serves the stdio transport, but through a **shim island** (`mcp_stdio_shim.{hpp,cpp}`): cpp-mcp vendors its own older nlohmann/json whose ABI namespace differs from vcpkg's, so the two cannot share a translation unit — the shim's one TU sees only cpp-mcp's headers and crosses back as strings, bools and ints. cpp-mcp's bundled `sse_client` is **not used for http**: it speaks the legacy SSE protocol and cannot talk to a Streamable-HTTP server (verified against the official python SDK). The http transport is a minimal hand-rolled client over `llm::SseParser` and **`llm::HttpSession`** — one CURL handle kept alive across the connection's whole life (all calls run on the single worker thread, which is exactly HttpSession's contract), so consecutive requests to the same host ride one TCP+TLS connection instead of re-handshaking per tool call; HttpClient's stateless per-call handles remain the right shape for providers and are untouched. The client POSTs the JSON-RPC message with `Accept: application/json, text/event-stream`, echoes the `mcp-session-id` from the initialize response, and takes the answer from whichever shape the server picks — a JSON body, or an SSE stream aborted the moment the matching response frame arrives.

**All protocol I/O runs on one dedicated thread per connection.** cpp-mcp's client blocks internally (up to a fixed 60 s) with no deadline parameter, so a caller-visible timeout or a `CancelTurn` cannot be honoured on the caller's thread. Callers wait on their turn's `tools::CancelToken` condition variable — completion and cancellation both notify it, so a waiter wakes on exactly the events that can change its outcome; no polling, no 25 ms slices (the polling `is_cancelled` function remains in `ToolContext` for tools that merely check it occasionally). A deadline or cancellation returns promptly while the underlying request may still complete in the background — the same accepted asymmetry as bash's killpg window — and any exception escaping a worker job is contained by the loop instead of reaching `std::terminate`. A worker wedged inside a blocking call is **detached, never joined** (it holds its state by `shared_ptr`), so destroying a connection cannot hang on a dead server — and `Connection::Open` bounds the initialize handshake itself, which cpp-mcp cannot do (verified: it blocks indefinitely if the subprocess dies during startup). Teardown also **kills the stdio child outright** (`Transport::Abort`): cpp-mcp's 60 s request cap would otherwise keep the child alive (and the wedged worker with it) for up to a minute past teardown, or orphan it forever if the server process exits first. The kill is safe against pid reuse — `waitpid(WNOHANG)` proves the target is alive and still our child — and SIGTERM escalates to SIGKILL from a detached watcher. To make that possible, the vendored cpp-mcp carries a tiny header patch (`patches/cpp-mcp-child-pid.patch`, applied idempotently by `cmake/ApplyCppMcpPatches.cmake`): an atomic `process_id_` plus a `child_pid()` accessor, readable from the teardown thread while a worker is still inside `initialize()`. Writing to a dead child's stdin raises SIGPIPE, whose default disposition kills the whole server process (observed as exit 141); the shim ignores SIGPIPE process-wide before spawning, so the write surfaces as EPIPE and degrades to a tool error. Children spawned afterwards inherit SIG_IGN — the same trade hosted runtimes (Python, Go) make.

**Timeouts and limits are config**: `settings.json` `tools.mcp.{connect_timeout_ms (default 10000), request_timeout_ms (default 60000), max_list_pages (default 100)}`. The request deadline is **total, not idle**: the http transport caps the whole transfer (including the SSE stream it may answer through), so a server that accepts the connection and then goes silent degrades to a bounded error rather than wedging the connection's worker. `tools/list` pagination (http transport) is capped at `max_list_pages` pages — a server that never stops returning `nextCursor` pages would otherwise occupy the worker thread forever, since each page is individually within the request deadline.

- **Liveness is checked when a session starts** with a preset listing the server in `allowed_mcp_servers` — i.e. on agent selection, not at server boot. The check's I/O runs under a **per-server mutex**: the manager-wide lock covers only the map lookup, so a slow or wedged server stalls its own callers, never other sessions' checks or the cheap status lookups. A successful check is a **verdict valid for 2 s** — near-simultaneous callers (a subagent spawn right after the parent's check, a child session starting while another caller's restart is still in flight) reuse it instead of paying another ping round trip (a full TCP+TLS handshake over http). Deliberately short: it is a dedup for near-simultaneous callers, not a liveness cache — a server dead 3 s is still caught by the next session's check, and one dead inside the window is surfaced immediately by its next tool call.
- **Tool discovery happens once per MCP server process** and is cached there. The discovered tool list is a property of the shared subprocess, not of a session, so session start and `/agent` switches only **re-filter** that cache against `allowed_mcp_servers` — they never re-discover, and `BuildChatRequest` never queries a server per turn. Tool *invocations* remain lazy, dispatched on demand. Re-discovery happens only if a server process restarts: the manager respawns on the next liveness check and re-registers the tool set atomically.
- A server failing the check is **disabled for that session** with a surfaced error (an `ErrorEvent` on the session's queue); the session proceeds without it, and later `/agent` switches do not re-test it.
- A subagent whose preset requires a dead server **does not start** — `SpawnSubagentTool` returns an MCP error as its `ToolResultBlock`, checked against the parent-intersected server list before any child row exists.
- `/agent` mid-session stops exposing a de-scoped server's tools but does **not** kill a subprocess other sessions may be using.

`ToolRegistry` namespaces MCP-sourced tools as `mcp__<server_name>__<tool_name>` at registration, avoiding collisions with built-ins and across servers. MCP tools flow through the identical registry/executor/permission machinery as every other tool — with three qualifications:

1. **They are registered and replaced through `ReplaceMcpTools(server, tools)`**, which bypasses attribution, aliasing and the shadowed list (the namespacing cannot collide). Registry-wide ownership is `shared_ptr` and `Find()` hands out a copy, so an executor that is inside a blocking permission Ask or `execute()` keeps its tool object alive no matter what a concurrent replacement does — the last reference destroys it, and there is no retired list to grow or reason about. Registry reads (`Find`/`DefinitionsFor`) run under a `shared_mutex` — MCP replacement is the first runtime mutation of a registry that was previously read-only after startup, and it overlaps session workers.
2. **`allowed_tools` never governs them** — `DefinitionsFor` excludes `mcp__*` names so an `allowed_tools: ["*"]` cannot advertise a dead or de-scoped server's tools. The model sees exactly `DefinitionsForMcpServers(live_servers)`: the preset's allowed set minus per-session disabled, resolved with the manager.
3. **MCP tools declare `ToolCategory::ReadOnly`**, which is display-only: the only gate consulted is the server-scoped `McpPermissionPolicy` (`mcp__<server>__<tool>` → `McpPermissionPolicy::mode_for(server)`), so `out_of_workdir` never folds in and `Disclose()` offers no session grants — the consent unit is the server. A cancelled or timed-out call returns an error `ToolResultBlock`, never a hung worker.

**Known limitation**: cpp-mcp splits the stdio command line on whitespace, so a `mcps.json` `args[]` entry containing a space cannot be passed to the child.

## Session storage — SQLite

Built, in `src/server/session/`: `SessionDatabase` (connection, pragmas, migrations, lifecycle ops), `SqliteHistoryStore` (the `HistoryStore` the agent loop writes through), `SessionReader` (the read side `GetMessages` answers from), `message_codec` (strict `llm::Message` ↔ JSON) and `ulid`. The in-memory placeholder is **gone**, not retained as an option: keeping it would mean scrollback answered from memory for an open session and from SQLite for a closed one, so what a client saw would depend on whether a tab happened to be open.

**One connection, one mutex, every method locking.** An append is a sub-millisecond transaction against second-scale LLM turns, so serialising concurrent session workers costs nothing measurable and removes every intra-process locking question SQLite would otherwise raise. WAL plus `busy_timeout` still cover out-of-process readers.

- **`sessions`**: `id, parent_session_id (nullable, self-FK), name, project_root, agent_preset, created_at, updated_at`.
- **`messages`**: `id, session_id, seq, role, content_json, token_count, created_at`, with `UNIQUE (session_id, seq)` — which is the `(session_id, seq)` index keyset pagination queries against.
- Keyset pagination for scrollback (`WHERE session_id=? AND seq < ? ORDER BY seq DESC LIMIT N`) — no `OFFSET` scans over a 1M-token history. WAL mode for concurrent read-while-write.
- **`token_count` holds the backend-reported output tokens for assistant messages and 0 otherwise.** There is no per-message input count to store: each turn's `input_tokens` already covers the whole history, so attributing it to one row would be fiction (see Token accounting).
- **Timestamps are strictly increasing within a process**, even when operations land in the same millisecond: the store issues `max(wall_ms, last_issued + 1)`. `updated_at` is the picker's sort key, and a tie would fall back to id order — random within a millisecond, since ids break same-ms ties with their entropy suffix. Drift from wall clock is at most one ms per operation.

**`project_root` is per-session**, not global — it is the boundary for path confinement, @-mentions, `SuggestPaths`, and bash's initial cwd, and the session picker filters on it. The client supplies its cwd in `CreateSessionRequest`.

**Session naming, layered:**

1. **Core default, free**: name = the first user message *with text*, whitespace-collapsed and truncated to 60 code points (UTF-8-safe). Set in the same transaction as that append, only while the name is still empty — so an explicit creation name or a later observer-set name is never clobbered. A tool-result-only user message (e.g. an injected subagent completion) is not a name; the search continues with later messages. No model, no latency, no failure mode.
2. **Optional observer plugin**: async LLM titling after turn 1 through the already-configured provider (~100 tokens), overwriting the default.

A local summarization model (BART/ONNX) was considered and rejected: domain mismatch (`bart-large-cnn` is trained on news prose, emits multi-sentence extractive summaries rather than short titles, and has never seen a code-bearing chat transcript); dependency weight (~400M params — roughly 1.6 GB fp32, ~400 MB int8 — plus ONNX Runtime or libtorch, a *second* inference stack, in a project whose first goal is low footprint and which rejected far lighter dependencies); and redundancy (the configured provider does it better in-domain with zero new dependencies, and is already fully local when pointed at llama.cpp).

Names are therefore mutable after creation; `SessionMetadataEvent` notifies attached clients so tab labels and the picker refresh.

**Schema versioning**: `PRAGMA user_version`, with an ordered migration list applied in a transaction at startup. A database *newer* than the binary understands is a fatal startup error, not a best-effort open. All pending migrations share one transaction, so a failure part-way leaves the schema exactly as it was opened rather than half-applied.

**Atomicity**: each `AppendMessage` is one transaction covering the insert, the default-name set, and the `updated_at` touch. A crash therefore leaves an exact prefix of the transcript, never a torn row. `synchronous=NORMAL` under WAL makes that guarantee against *process* death, not power loss — the right trade for a local single-user tool.

**Who mints a session id.** The database, not the RPC layer: it is the only party that can guarantee uniqueness across restarts. So `CreateSession` inserts the row first and takes `session_id`, `created_at` and `updated_at` from it. That ordering has a consequence worth stating — if constructing the `Session` then fails (unknown preset, unregistered provider kind), the row is **deleted** before the error is returned, or every preset typo would leave a message-less session in the picker forever.

**Resume.** `CreateSessionRequest` carries an optional `session_id`; set, it means *resume that stored session* rather than create a new one. The contract: the stored row's `name`, `agent_preset`, and `project_root` govern and the request must not send its own (refused, not ignored — the client already has all three from `ListSessions`); `run_mode` still applies, being a property of the client. Subagent sessions are not resumable, and a session that is already open is refused. A failed resume never deletes the row — the asymmetry with fresh creation, whose just-minted row is rolled back.

The load path depends on whether a compaction checkpoint exists. **With one** (the session compacted while alive), resume loads `[checkpoint summary] + SessionReader::LoadAfter(boundary_seq)` — the working set the live session had, with no summarization call and a load proportional to the kept context rather than the transcript (see *Context compaction* for why: re-summarizing a transcript beyond the model window was not merely costly, it could not converge). **Without one**, the load path is `SessionReader::LoadAll` — the one read allowed to materialize a whole transcript, ascending by seq, one-shot, because the loop's working history is what gets sent every turn and a resumed session must hold it exactly as a live one does. Either way `AgentLoop` takes it as a constructor-seeded history, so "ran a turn before seeding" is inexpressible, and the store never sees it again: appends continue at `MAX(seq) + 1`, which is also what the store's `last_appended_seq()` initializes from — a store that assumed "started empty" would restart the seq accounting at -1 while SQL kept assigning the real continuation. Seeded history seeds the compaction trigger too (`TokenAccumulator::SeedOccupancy` with the same bytes/4 roughness the compactor's fallback uses, and like it, estimate-only: cumulative totals are never touched), so a resumed session over the trigger compacts *before* its first provider call instead of failing it.

One stored shape is not provider-valid: a transcript torn mid-tool-sequence — a cancel leaves denial results, but a process killed between the assistant append and the results append leaves an unanswered `tool_use`, a hard 400 on both wire formats. Resume persists one synthesized user message of error `ToolResultBlock`s for the unmatched calls (never a rewrite of the stored tail) before the session starts, so the transcript stays an exact, valid prefix.

**Ids are ULIDs** — 48-bit millisecond timestamp plus 80 bits of randomness, 26 Crockford base32 characters, so they sort by creation under plain string comparison and read chronologically in logs. The random half comes straight from `std::random_device`: seeding a PRNG from one draw would cap the real entropy of all 80 bits at the seed's width, and a same-millisecond collision is a `PRIMARY KEY` failure thrown out of `CreateSession`, not a cosmetic problem.

## Subagents

A subagent is **the same `Session` entity as a top-level conversation** — same message model, same `OpenSession` stream, same tool/permission/command plumbing, its own tab. What distinguishes it is one field (`parent_session_id`) plus restrictions derived from it, not a different type.

**"Stopped" is uniform**: a session is stopped when its agent loop produces an assistant turn with `stop_reason == end_turn` and no pending `tool_use` — the same condition that means "waiting for the next user message" at top level. For a session with a parent, that moment is also completion, and the final assistant message's text is the result.

- **Spawning**: core `SpawnSubagentTool` (category `Execute`, permission-gated) takes `{task, agent_preset?, blocking? (default true), max_turns?}`. It creates a child session with `parent_session_id` set, seeds `messages[0] = Message{role: User, content: [TextBlock{task}]}` (the parent model's own task formulation), emits `SubagentStartedEvent{child_session_id, task, blocking}` on the parent's queue, and starts the loop.
- **System prompt stacking**: for any session with a parent, the system prompt is `agents/subagent.md` (the harness convention: "you are in subagent mode; you'll be given a task; when done or the task is impossible, summarize for the parent") followed by the resolved preset's body. Concatenation, not a new mechanism.
- **No recursion is a hard rule, not preset config**: if `parent_session_id` is non-null, `SpawnSubagentTool` is unconditionally excluded from that session's tool set regardless of `allowed_tools`. A misconfigured preset cannot reopen recursion. Flat, one level.
- **Permission inheritance — a child can never be more permissive than its parent**: effective `PermissionPolicy` is `most_restrictive(parent, child_preset)` per category; `allowed_tools`/`allowed_skills`/`allowed_mcp_servers` are **intersected** with the parent's; `PermissionRunMode` is clamped (`AllowOnly < Normal < Bypass`). Without this, a parent at `file_write: ask` could spawn a child preset at `file_write: allow`.
- **Concurrency limit**: `max_concurrent_subagents` per parent session, default 4. Over-limit spawns return an **error `ToolResultBlock`** ("subagent limit reached") rather than blocking, so the model adapts instead of hanging. The sanity check *warns* above 8 — past that the task has outgrown the harness — but does not block, matching the build-it-yourself philosophy.
- **Blocking mode**: the child's own worker runs its single turn — one drive path for both modes — while `execute()` blocks the parent's worker on the completion signal, which fires on every exit path of the child's turn, so a failure unblocks the parent exactly like a success. The child's tab auto-focuses, since the parent is stuck waiting anyway.
- **Non-blocking mode**: `execute()` returns an acknowledgment immediately and the parent's turn continues; the child runs on its own worker. On completion the result is **queued** for the parent and flushed by the parent's worker **at a turn boundary** — after all tool results for the current assistant turn are appended, before the next provider call. Injecting it truly out-of-band could land a `User` message between a `tool_use` and its `tool_result`, which is a hard 400.

## Startup sanity check — `src/server/config/sanity_check.{hpp,cpp}`

Runs after config and plugin load, before serving. Behaves like a linter, not a fail-fast validator — **it surfaces every problem found, not just the first** — so a misconfigured setup yields one complete diagnostic instead of a fix-one-rerun-find-the-next loop. It also answers "what happens on a cold start with nothing configured": that is one more failing check with a clear reason, not a bespoke onboarding wizard.

```cpp
enum class CheckSeverity { Fatal, Warning };
struct CheckResult { CheckSeverity severity; std::string check_name, message; };

class StartupSanityCheck {
public:
    std::vector<CheckResult> RunAll();   // runs every check regardless of earlier failures
};
```

Checks: every config file parses; at least one usable `providers.json` entry exists with a resolvable key *and* a registered provider kind; at least one agent preset parses and `settings.json`'s `default_agent_preset` resolves end to end; referenced compression prompts exist; each `mcps.json` entry's `command` resolves on `PATH` (stdio) or `url` is well-formed (http); the config and data directories exist and are writable; the SQLite database is accessible and not newer than the binary; each discovered plugin's load status is reported with severity split by provenance; `max_concurrent_subagents` is sane; **no registration-name collisions** (fatal — see Name collisions and aliases); every `aliases` key resolves to something registered.

`project_root` is per-session, supplied by the client in `CreateSessionRequest`, so it is **not** a startup check. It is validated in `CreateSession` — it must exist, be a directory, and be writable — with a `FAILED_PRECONDITION` reason rather than failing later inside tool execution.

Two checks specifically for the naming model: warn when a `per_tool_overrides` / `allowed_tools` / `allowed_skills` entry resolves to **nothing** registered (catches typos and entries stranded by a disabled plugin) — for `allowed_skills`, an entry that resolves to a *not-loaded* skill warns too, carrying its `load_error` — and warn when a bare permission key is **ambiguous** because two plugins provide that original name; the intended rule is then undefined and must be written in qualified `plugin.name` form. Every not-loaded skill entry is itself a warning.

**Severity splits by provenance**: a failed **bundled** plugin is **Fatal** — losing `tools-fs` or `provider-openai` is a useless harness, not a degraded one — while a failed **user** plugin, or one bad `mcps.json` entry, is a **Warning** that disables just that piece.

## Deferred / open items

- Anthropic Messages-shape provider (the unified model needs only additive changes: `cache_control` fields and a widened `system`).
- **Windows support generally — out of scope until 2.x**; WSL is the interim answer. This is broader than plugin loading (`LoadLibrary` vs `dlopen`): the bash tool is fork + `execve` + `setrlimit` + `killpg`, POSIX by construction rather than by porting accident, so a Windows target needs a genuinely different execution backend, not a shim.
- `llamacpp-kv` plugin (`/save-session`, `/restore-session`, `/erase-session`) — designed, deliberately not shipped in v1.
- Parallel tool dispatch (sequential in v1; `request_id` correlation already supports it).
- Session detach (`EndSession{DETACH}`), deliberately deferred — see Session lifetime.
- Status-bar segment and panel/picker column slots for client plugins (renderer slot ships in v1).
- Tab-bar overflow behavior with many sessions × subagents.
- Backend-reported `usage` of `0` from some OpenAI-compatible proxies — no fallback estimate designed.
- **Sessions that last compacted before compaction checkpoints existed (schema v4) have no checkpoint row, so they still resume from the full uncompacted transcript** — correct, and cost-free for transcripts that fit the window, but a transcript well beyond the model window cannot converge its first compaction, checkpoint or no checkpoint: the halving strategy's summarization input must fit the window, and re-summarizing only ever writes a checkpoint once it succeeds (see *Context compaction*). For such a session the honest remedies are a larger-window model for one manual `/compact`, or starting fresh.

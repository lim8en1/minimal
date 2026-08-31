# minimal — Development Plan

Companion to `ARCHITECTURE.md`, which remains the single source of truth for *design*. This document covers only *sequencing*: what is built, what blocks what, and what "done" means for each remaining piece. Every milestone cites the `ARCHITECTURE.md` section that specifies it; where this plan and that document disagree, that document wins and this one is stale.

**Status: Milestones A, B, C, D, F, G and H are complete, and a post-merge review of A+B+H has been actioned (§8). Only E (the TUI) remains. Clean build, zero warnings.** Track **J (permission advisor) is complete** — including the SessionChannels extraction, the E2E over a real socket through the real plugin loader, and the three §6 invariant proofs. Tracks **K (per-plugin config)**, **L (cross-plugin services)** and **M (tool history access)** are also complete. Track **N (model auto-discovery) is complete** — lazy discovery through a `ModelCatalogRegistry` keyed by the provider kind string, `/providers refresh`, network-free structural sanity checks. A post-N review hardened the transport (the loopback + live-backend tests), the concurrency (per-provider fetch gates — no config lock is ever held across the network call), and the resolve contract (`Resolve` returns a snapshot; refresh replaces entries without pulling them out from under a holder). Track **O (search tools) is complete** — the bundled `tools-search` plugin registers `grep` and `glob`: RE2-backed, `.gitignore`-aware, bounded on every axis the session worker cares about (§4 O).

`minimal -p` holds real conversations against a local model using `read`, `bash`, and `skill`; a permission prompt round-trips over the wire in both directions; a session grant silences the second prompt; the server exits when the last client detaches. Transcripts persist to SQLite, survive the server process, and are **resumable**: `CreateSession` with a `session_id` reopens a stored session, loads its transcript as the loop's working history without re-appending it, and the next turn continues the conversation (`minimal -p --resume <id> PROMPT`). **Subagents work** (§5 D): a model can spawn blocking and non-blocking children over the `spawn_subagent` tool, permission inheritance is enforced as ceilings, and the headless client honours its attach obligation over a real socket. `cmake --install` produces a config dir the server actually starts against (§8), and the startup sanity check reports every problem with it at once (§4 F).

---

## 1. Where the project actually is

**Milestone A closed the gap this section described.** The server now serves; the headless client is a real client; every layer below has been executed rather than only unit-tested. What follows is updated for that.

### Built and tested

| Subsystem | Location | Tests |
|---|---|---|
| Config: settings/providers/permissions/plugins/mcps, frontmatter, presets, skills, sanity check, XDG paths | `src/server/config/` | 100 |
| Permission model: lattice, run modes, precedence chains, out-of-workdir fold, MCP policy, consent scoping | `src/server/tools/` | 40 |
| Tools: `Tool`/registry/executor, path confinement, `file_access`, `atomic_write`, `SkillTool`, `AliasedTool` | `src/server/tools/` | 51 |
| Plugins: discovery, `dlopen` loader + ABI guard, collision/alias registry + name resolution | `src/server/plugins/` | 22 |
| LLM: provider interface, chat/stream types, SSE parser, HTTP client, token accounting | `src/server/llm/` | 30 |
| Agent loop: tool cycle, retries, cancellation, compaction trigger + boundary snapping, production compactor | `src/server/agent/` | 29 |
| `tools-fs`, `tools-search`, `tools-bash`, `provider-openai` | `plugins/server/` | 105 |

### Built in Milestone A

| Piece | Location | Notes |
|---|---|---|
| Proto ↔ domain conversion | `src/server/rpc/proto_convert.*` | Was unowned by any milestone; found during planning. |
| `PendingRequests`, `EventQueue` | `src/server/rpc/` | The rendezvous and the outbound queue. |
| `WirePermissionChecker` | `src/server/rpc/` | First real `PermissionChecker`; no gRPC dependency, so testable in-process. |
| `Session`, `SessionRegistry` | `src/server/agent/` | Three-thread topology, initial-attach guard. |
| `SessionObserver` + registry | `src/server/agent/` | Track I; closes the `tools-bash` state leak. |
| `SessionServiceImpl`, `Server` | `src/server/rpc/` | `CreateSession` + `OpenSession`; refcounted exit. |
| `SessionClient`, `minimal -p` | `src/client/` | text and jsonl output, exit codes, `AllowOnly` default. |

### Built in Milestone B

| Piece | Location | Notes |
|---|---|---|
| `SessionDatabase` | `src/server/session/` | One connection, one mutex, WAL, transactional migrations. |
| `SqliteHistoryStore` | `src/server/session/` | The durable `HistoryStore`; replaced `InMemoryHistoryStore`. |
| `SessionReader` | `src/server/session/` | Keyset paging — what `GetMessages` answers from. |
| `message_codec`, `ulid` | `src/server/session/` | Strict `llm::Message` ↔ JSON; time-sortable session ids. |

### Built in Milestone H

| Piece | Location | Notes |
|---|---|---|
| `compact_messages` | `src/server/agent/context_compactor.*` | Provider-backed summarization; failure is a value, not an exception. |
| `bundled/prompts/compression.md` | `bundled/` | The default compaction prompt, seeded into the config dir on first run. |
| `bundled_assets` | `src/common/` | Executable-relative asset lookup + prompt seeding; absorbed plugin discovery's copy. |

### Built in Milestone F

| Piece | Location | Notes |
|---|---|---|
| `StartupSanityCheck` | `src/server/config/sanity_check.*` | Linter semantics; hands loaded config + opened DB to the serve stage. 26 cases. |
| `mcps_config` | `src/server/config/mcps_config.*` | Parse-only loader (interpolation is G's); bad entries degrade individually. 11 cases. |
| Alias application | `src/server/tools/`, `src/server/plugins/name_resolution.*` | `ApplyAliases` + `AliasedTool` wrapper; shadowed duplicates; surface→permission-key map consumed by `ToolExecutor`/`DefinitionsFor`. 5 loader cases + 4 wrapper cases + 1 `NameRegistry` regression. |
| `project_root` validation | `src/server/rpc/session_service.cpp` | Per-session — validated in `CreateSession`, not at startup (exists, directory, writable). 2 wire cases. |

### Still not built

The TUI (track E).

### The risk this creates

Test depth was inverted relative to integration risk: every well-tested layer was exercised through fakes, and **every layer that had never executed was one that connected two tested ones**. Milestone A forced that moment early and cheaply, and §3's closing section records what it found. The B merge is a second instance of the same effect at a different seam — two branches off one base, each internally green, whose mismatches only existed *between* them.

---

## 2. Dependency structure

```
                        ┌──────────────────────────────┐
                        │  A. Session + RPC + headless │  ← keystone
                        │     (§Concurrency model)     │
                        └──────────────┬───────────────┘
                                       │ unblocks
              ┌──────────────┬─────────┼──────────┬──────────────┐
              ▼              ▼         ▼          ▼              ▼
        C. /-commands   D. Subagents  E. TUI   F. Sanity     (B is not here:
        (§/-command)    (§Subagents) (§TUI)    check          it is independent)
              │              │                 (§Startup…)
              └──────┬───────┘
                     ▼
             G. MCP client (§MCP client)
```

**Independent — may start now, in any order, by anyone:**

- **B. Session storage (SQLite)** — **Done, and wired.** `src/server/session/`: `SessionDatabase` (one connection, one mutex, WAL, transactional migrations), `SqliteHistoryStore` (the `HistoryStore` the agent loop writes through), `SessionReader` (keyset paging + session metadata). `GetMessages` and `ListSessions` now answer for real; `agent::InMemoryHistoryStore` is deleted.
- ~~**F. Startup sanity check**~~ — **done.** Linter-style `RunAll()` in `src/server/config/sanity_check.{hpp,cpp}`, wired as stage 3 in `main.cpp`; `mcps_config` parse-only loader landed; `NameRegistry` wired through `ResolveToolNames` + `ToolRegistry::ApplyAliases`; `CreateSession` validates the per-session `project_root` (§4 F).
- ~~**H. Production compactor**~~ — **done.** `compact_messages` replaces `Session`'s no-op stub, and the bundled prompt is delivered (§4 H).
- ~~**I. `SessionObserver` registry**~~ — **done**, landed with A2. `minimal_plugin_register_session_observer` is live in the plugin ABI and `tools-bash` implements it.
- **J. Permission advisor** — independent of E and G; designed in `docs/EXTENSIBILITY.md` §3, specified below (§4 J).
- ~~**K. Per-plugin config**~~ — **done** (§4 K). `PluginRegistrationContext` carries the per-plugin `config` on every registration seam.
- ~~**L. Cross-plugin services (ServiceRegistry)**~~ — **done** (§4 L). `minimal_plugin_register_services` is live, `NameKind::Service` collisions are fatal, and `tools-bash` provides `BashSessionStateService` under `tools-bash.session_state`; `/restart-shell` resolves through the service. One deviation from the spec below: the command stayed in `tools-bash` (a command belongs to the plugin that owns the state it touches) — the service path and the absent-provider degrade are covered by tests against the extracted `RestartShellCommand`.
- ~~**M. Tool history access**~~ — **done** (§4 M). `ToolContext::history` is a `session::HistoryWindow` — a bound, windowed slice of `SessionReader` exposing only keyset-paged reads, so the windowed-only rule is a property of the type. `SessionReader` and `history_window.hpp` joined the frozen include set in `plugin_abi.h`.
- ~~**N. Model auto-discovery**~~ — **done** (§4 N). `minimal_plugin_register_model_catalogs` is live, `providers.json` accepts `"discover": true` (with or without a static `models` object), discovery fetches lazily at first resolve with negative caching, `/providers refresh` evicts and refetches, and the discovered `context_window_tokens` reaches the compaction trigger. Two spec deviations, both decisions recorded in §4 N below: the sanity check stays **network-free** (structural warnings only), and persistence/TTL caching was **rejected** — the server is short-lived, so a runtime cache needs no TTL, and a month-old cached window is the exact staleness the track exists to eliminate.
- ~~**O. Search tools (grep/glob)**~~ — **done** (§4 O). A new bundled plugin `tools-search` registers `grep` and `glob` — read-only, `.gitignore`-aware, RE2-backed, bounded on every axis the session worker cares about. Independent of E and G.

**Strictly dependent on A:** C, D, E, G, and every RPC beyond `CreateSession`/`OpenSession`. **A is now done, so all of them were unblocked; D is now done too.**

**Deferred out of A, deliberately:** client auto-spawn of the server (connect-or-spawn with a lock file). The refcounted-exit half it depends on is built and tested.

**Rationale for A first** is `ARCHITECTURE.md` § *Non-interactive client*: the headless client is "the natural E2E harness — a second reason to build it before the TUI." It converts the entire existing suite from unit-tested to actually-exercised at the lowest possible cost.

---

## 3. Milestone A — Session, RPC, and the headless client

**The keystone. Large enough to want its own plan before implementation begins.** Split into four sub-milestones that land in order; each is independently testable.

> Specified by: § *Concurrency model — `src/server/rpc/`*, § *Session lifetime*, § *Non-interactive client (`minimal -p`)*.

### A1 — `PendingRequests` and the outbound queue

The two primitives everything else in A rests on. Build and test them *first, in isolation*, because they are where the deadlock lives.

**What.** `src/server/rpc/pending_requests.{hpp,cpp}` exactly as specified in § *Concurrency model*: `AwaitResponse(request_id)` blocks the worker; `Resolve(request_id, choice)` is called from the reader. Plus `src/server/rpc/event_queue.{hpp,cpp}` — the session-owned outbound queue with a single draining writer.

**How.** `AwaitResponse` waits indefinitely by design — no timeout, no fail-closed. A client is present by construction (no detached sessions), and a human being away is not a reason to deny their work. The queue belongs to the `Session`, not the stream, so events produced before a client attaches buffer rather than drop.

**Acceptance criteria**
- `AwaitResponse` returns the value passed to `Resolve` for the matching `request_id`, and ignores non-matching ids.
- `Resolve` for an unknown id is a no-op, not a crash — a client may answer a request whose session already terminated.
- Events pushed before any writer attaches are delivered, in order, on attach.
- Terminating a session unblocks every waiter with a terminal result rather than hanging.

**Test cases** (`tests/pending_requests_test.cpp`, `tests/event_queue_test.cpp`)
- [x] Resolve-before-await and await-before-resolve both work (the ordering race).
- [x] Two concurrent waiters on different ids are resolved independently.
- [x] Resolve with an unknown id: no-op, no crash.
- [x] Termination wakes a blocked waiter; assert it returns rather than joining a hung thread.
- [x] Queue: backlog buffered before attach, flushed in order on attach.
- [x] Queue: single-writer invariant — concurrent producers never interleave a partial event.
- [x] **Invariant test:** a deliberately-reintroduced "resolve only if a waiter is already registered" bug must fail the ordering-race case.

### A2 — `Session`

**What.** `src/server/agent/session.{hpp,cpp}`: owns `AgentLoop`, `ToolExecutor`, provider instance, resolved `AgentPreset`, `HistoryStore`, the outbound queue, the atomic cancel flag, and the worker thread. Plus `SessionRegistry` keyed by session id.

**How.** Three threads per session (§ *Concurrency model* table). The worker blocks freely; the reader never blocks on anything but `Read()`; one writer per attached stream is the sole `Write()` caller. Termination reuses the cancellation path — atomic flag → `bool` stream sink aborts the provider → tools observe `is_cancelled` — then joins and reaps. **No new termination mechanism.**

**Acceptance criteria**
- A session runs a full turn against `FakeLlmProvider` with a real `ToolExecutor` and real `tools-fs`/`tools-bash` tools — no fakes below the provider.
- Cancelling mid-turn aborts the provider stream and stops the worker within one tool boundary.
- Stream close after attach terminates the session; a never-attached session terminates on the ~10 s initial-attach guard (§ *Session lifetime*, "one carve-out").
- `project_root` comes from `CreateSessionRequest` and reaches `ToolContext::project_root` and bash's initial cwd.

**Test cases** (`tests/session_test.cpp`)
- [x] Full turn: user text → tool call → tool result → assistant text, through the real executor.
- [x] Cancel mid-turn: worker exits, no zombie thread, partial transcript retained.
- [x] Terminate with a turn in flight: joins cleanly, no leaked thread, no use-after-free of the queue.
- [x] Never-attached session terminates on the initial-attach guard.  *(`session_registry_test.cpp`)*
- [x] Two sessions with different `project_root`s do not see each other's bash cwd/env (integration with `BashSessionStateStore`).
- [x] **Invariant test:** running the agent loop on the reader thread instead of the worker must deadlock the permission path — assert the worker/reader split is load-bearing rather than incidental.  *(`wire_permission_checker_test.cpp`)*

### A3 — `SessionService`

**What.** `src/server/rpc/session_service.{hpp,cpp}` implementing `CreateSession` and `OpenSession` only. The other five RPCs are explicitly deferred to B and E.

**How.** Inbound dispatch is the table in § *Concurrency model* — every branch non-blocking. `OpenSession` attaches a writer to the named session's queue and flushes the backlog. Server refcounted by attached clients; exits when the last leaves (§ *Session lifetime* — no daemon).

Also lands the **wire-backed `PermissionChecker`**: emits `PermissionRequest` onto the queue, calls `PendingRequests::AwaitResponse`, maps the answer back. This is the first real implementation of that interface — today only test fakes exist — and it is where consent scoping's `PermissionDisclosure` finally reaches a human.

**Acceptance criteria**
- `CreateSession` → `OpenSession` → one `UserChatTurn` → `StreamEvent`s → terminal stop reason, over a real gRPC channel on a unix socket.
- A permission prompt round-trips: server blocks, client answers, worker proceeds. **Deny** produces an error `ToolResultBlock`, not a hang.
- `CancelTurn` interrupts an in-flight turn.
- Closing the stream terminates the session and the transcript survives.
- Server exits when the last client detaches.

**Test cases** (`tests/session_service_test.cpp` — real channel, unix socket)
- [x] Round-trip a turn end to end; assert `ServerEvent` payload ordering.  *(ordering added in §9: `TurnCompletedEvent` last and exactly once, tool start before finish, both between the two `message_stop`s)*
- [x] Permission allow and deny, both round-tripped over the wire.
- [x] A disclosure with grants: answering with `granted_keys` silences the second prompt (proves consent scoping survives serialisation — currently only proven in-process).
- [x] `CancelTurn` mid-stream.  *(added in §9, using a permission prompt as the rendezvous so "mid-turn" is deterministic; also asserts the session still prompts and runs a tool on the NEXT turn, which is the deny-lock regression over the wire)*
- [x] Client disconnects mid-turn: session terminates, no leaked thread.
- [x] Two concurrent sessions on one channel do not cross-talk (`session_id` routing).
- [~] **Invariant test:** removing the single-writer discipline must produce interleaved/corrupt writes under concurrent producers. **Covered at the queue, not at the stream** — `event_queue_test.cpp` proves concurrent producers lose no events and keep per-producer order, which is the mechanism; nothing proves a second concurrent `Write()` on the `ServerReaderWriter` corrupts the stream. A second writer is now refused outright (§8), so the reachable half is guarded.

### A4 — Headless client (`minimal -p`)

**What.** `src/client/app.cpp` `RunHeadless` (`app.cpp:18`) and a real `SessionClient` (currently a constructor and destructor).

**How.** Connect, `CreateSession(project_root = cwd)`, send one `UserChatTurn`, stream `ServerEvent`s to stdout as text or JSONL, exit on stop reason. Defaults to `PermissionRunMode::AllowOnly` so an `Ask`-gated tool auto-denies instead of blocking on a human who was never attached (§ *Non-interactive client*).

**Acceptance criteria**
- `minimal -p "list the files here"` against a local llama server produces a real answer using real tools.
- Exit code reflects the terminal stop reason; JSONL mode is machine-parseable.
- An `Ask`-gated tool auto-denies and the run completes rather than hanging.

**Test cases**
- [x] Scripted end-to-end against a fake provider over a real socket (CI-safe, no network).
- [x] `AllowOnly` auto-deny path completes rather than blocking.
- [x] Attaching to a child session is a client obligation (§ *Session lifetime*) — the headless client does it, since the initial-attach guard is a safety net and not a designed flow. Closed with D: `SubagentStartedEvent` now exists to attach to, an `Attach` client event exists to attach *with* (§5 D), and `subagent_e2e_test.cpp` proves both over a real socket.

### Milestone A test-list status

Checked against the tree, not from memory. `[~]` is partial, and the reason is on the line.
Two of the three gaps this audit found are closed in §9; the third (child-attach) was
unenforceable until D emitted a `SubagentStartedEvent`, and is closed with D (§5 D).

### Milestone A definition of done — met

- [x] `minimal -p` holds a real conversation against a local model, using `read`, `bash`, and `skill`.
- [x] A permission prompt is answered over the wire in both directions; a session grant silences the second prompt.
- [x] Full suite green; clean build, zero warnings.
- [x] `ARCHITECTURE.md` updated where the implementation diverged.

### What first contact actually found

Divergence was the expected outcome, and this is the record of it. Each item is now reflected in `ARCHITECTURE.md`.

**Protocol gaps — the wire could not express things the design assumed.**

1. **No end-of-turn signal.** A turn makes one provider call per tool round, so `stop_reason` arrives repeatedly before the turn is done. A single-turn client had no way to know when to half-close, and hung. Added `TurnCompletedEvent`, emitted on every exit path including failures. An interactive client would never have surfaced this — which is the argument for building `-p` first, made concrete.
2. **Consent scoping could not cross the wire at all.** `PermissionRequest` carried only a summary string. Added `GrantOffer`/`complete` and `granted_keys`, with the offered-keys filter enforced at the boundary.
3. **No wire representation for run mode.** `AllowOnly` is a property of the *client*, not of config. Added `PermissionRunMode` to `CreateSessionRequest`.
4. **`StopReason::Cancelled` had no counterpart**, so a cancelled run reported `UNSPECIFIED` — and the headless exit code derives from `stop_reason`.
5. **`AgentPreset` dropped the provider kind**, so a resolved preset could not construct the provider it described.

**Design corrections — the doc specified something that does not work.**

6. **`PendingRequests` needed a two-phase API.** The sketched `AwaitResponse(id)` cannot be called correctly: waiting before publishing blocks before the request goes out, publishing before waiting loses a fast client's answer. Registration now returns an RAII handle, making the correct order the only expressible one.
7. **Observer dispatch had to be serialized, not pooled.** A pool gives no ordering between `OnSessionCreated` and `OnSessionEnded` and makes every observer's state a shared mutable. Caught by a test that aborted on a data race.
8. **Shutdown cannot be called from an RPC handler.** `grpc::Server::Shutdown()` waits for in-flight RPCs including the caller, so the last-client-detached callback deadlocked the binary on every run — while every test passed, because the test only recorded the callback. Split into `RequestShutdown()` and `Shutdown()`.

**Testing infrastructure.**

9. **`std::async` cannot bound a test whose failure mode is a hang** — its future blocks in the destructor. Added `tests/support/deadline.hpp`.
10. **A gRPC client must drain before `Finish()`**, or it deadlocks against a server still writing. Cost several test hangs before being isolated.

Items 1, 6, 7, 8 and 9 were each discovered by a hang or a crash rather than by review.

---

## 4. Independent tracks

### B. Session storage — SQLite

> § *Session storage — SQLite*. Built on a branch off `cab3a91`, in parallel with and unaware of Milestone A, then merged. **Done** — 31 cases in `tests/session_store_test.cpp` plus 5 integration cases in `tests/session_service_test.cpp`. **Resume — the gap ARCHITECTURE.md recorded as "what is not built" — is closed too; see "What resume needed" below.**

**What.** A `HistoryStore` implementation over SQLite3 (already a `find_package` dependency, currently unused), plus session metadata for `ListSessions`. Landed as `src/server/session/`: `message_codec` (strict `llm::Message` ↔ JSON), `ulid` (time-sortable session ids), `session_database` (connection, pragmas, transactional migrations, lifecycle ops), `sqlite_history_store` (the drop-in adapter), `session_reader` (keyset paging).

**Acceptance criteria**
- Messages persist and reload byte-identically, including tool-call blocks. *(Structural equality of the decoded `Message`; the codec is strict on decode.)*
- A crash mid-turn leaves a readable transcript (no partial-write corruption). *(Proven with a real fork + SIGKILL mid-append; every append is one transaction.)*
- Schema version recorded; a database newer than the binary is refused (§ *Startup sanity check*).

**Test cases** (`tests/session_store_test.cpp`)
- [x] Round-trip every `Message` content-block variant, including `ToolUseBlock`/`ToolResultBlock` pairs.
- [x] Reopen after an abrupt close; assert no partial message is returned.
- [x] Schema-version mismatch is refused with a clear error.
- [x] Paging: `GetMessages` by `seq` range over a large transcript stays bounded in memory.

Plus, beyond the original list: migration rollback proven load-bearing (fault-injected failing migration via an `OpenOptions` test seam — delete the transaction, watch it fail), default session naming (set once, UTF-8-safe 60-code-point truncation, tool-result-only messages skipped), `ListSessions` filters/sort/`message_count`, cascade delete, and concurrent appends across sessions.

#### What the merge with A found

Two branches off the same base that had never met. Only three files conflicted textually — both `CMakeLists.txt` and this document — and none of the following showed up as a conflict:

1. **`SessionServiceImpl::CreateSession` stamped every unnamed session with the literal `"session"`.** The database's default-naming rule is guarded by `WHERE ... AND name = ''` so it can never clobber a chosen name; a placeholder satisfies that guard and suppresses default naming permanently. Nothing below the RPC layer could catch it — the storage tests call `SessionDatabase::CreateSession("")` directly. Now `""`, with a regression test that drives a real turn over the wire and reads the name back through `ListSessions`.
2. **Two `StoredMessage` types**, `agent::` and `session::`, describing the same row. Collapsed onto `session::`, which already mirrors the proto field for field.
3. **Who mints a session id.** A minted `s-<ms>-<n>` from a counter; B's database mints a ULID. The database wins, which means the row now exists before the `Session` does — and a `Session` that fails to construct has to delete it, or every preset typo leaves a message-less row in the picker.
4. **`HistoryStore::Append` had a default argument on a pure virtual.** Defaults resolve from the static type, so base and override can silently disagree.
5. **`GenerateUlid` had 32 bits of real entropy, not 80** — a `mt19937_64` seeded from a single `random_device()` draw. A same-ms collision is a `PRIMARY KEY` failure out of `CreateSession`, so the birthday bound mattered.
6. **`TurnSummary`'s seq bounds were computed from `InMemoryHistoryStore::size()`.** With the store behind an interface there is no size, and counting locally would be right only for a session that started empty. `HistoryStore` now reports `last_appended_seq()`, cached from what SQL actually assigned.

### B (continued). Session resume — done

> § *Session storage — SQLite*, "What is not built: resume". The gap both documents recorded.

#### What resume needed

The B merge left the database holding conversations that could be *read* but not *resumed*: `AgentLoop` had no seeded-history parameter, so reopening a stored session would have replayed the whole transcript back as duplicate rows. Closing that was two named things — a load-without-appending path and a `session_id` on `CreateSessionRequest` — plus everything they imply:

1. **The wire contract.** `CreateSessionRequest.session_id` set means resume; the stored row's `name`/`agent_preset`/`project_root` govern and the request must not carry its own (refused, not ignored); `run_mode` still applies, being a property of the client. Unknown id → `NOT_FOUND`; already open → `ALREADY_EXISTS`; subagent row → `FAILED_PRECONDITION`; a failed resume never deletes the row (the asymmetry with fresh creation's rollback).
2. **The load path.** `SessionReader::LoadAll` — the one read allowed to materialize a whole transcript, one-shot and ascending, because the working history is what gets sent every turn. `AgentLoop` takes it constructor-seeded, so a turn can never run against a half-initialized history; the store never sees it again, and appends continue at `MAX(seq) + 1`.
3. **Seq accounting had to stop assuming "started empty".** `SqliteHistoryStore` initializes `last_appended_seq()` from the database's own `MAX(seq)` — its header already promised exactly this; resume is what makes it true. Reverting the init fails the continuation test.
4. **A torn tail is not resumable.** A process killed between the assistant append and the results append leaves an unanswered `tool_use` — a hard 400 on both wire formats. (A live cancel does *not* produce this shape: it appends denial results before breaking, which the first draft of the E2E test assumed otherwise and had to be rewritten for.) Resume persists one synthesized user message of error `ToolResultBlock`s for the unmatched calls — a real new row, never a rewrite of the stored tail — before the session starts.
5. **Compaction had to arm before the first call.** A resumed session has no backend usage yet, so the trigger would stay cold while a 500k-token context 400'd. `TokenAccumulator::SeedOccupancy` records the bytes/4 estimate — last-turn occupancy only, never the cumulative totals, which are genuinely additive — and the first real response replaces it.
6. **The client.** `minimal -p --resume SESSION_ID PROMPT`, with the parse-time rules the flag needs (requires `-p`, refuses `--agent` alongside, same value-taking rules as every other flag) and E2E coverage through the real `SessionClient`.

All four invariant proofs were run per §6: suppressing the seeded load, the repair, or the seq init each fails its test, independently.

### F. Startup sanity check — done


> § *Startup sanity check — `src/server/config/sanity_check.{hpp,cpp}`*. **Done** — 26 cases in `tests/sanity_check_test.cpp`, plus 11 `mcps_config` cases, 5 alias/collision cases in `plugin_loader_test.cpp`, 4 `AliasedTool` cases, 1 `NameRegistry` regression case, and 2 session-creation `project_root` cases.

**How.** `StartupSanityCheck::RunAll()` returns `vector<CheckResult>` and **runs every check regardless of earlier failures** — it is a linter, not a fail-fast validator, so a misconfigured setup yields one complete diagnostic instead of a fix-one-rerun-find-the-next loop. A cold start with nothing configured is one more failing check with a clear reason, not a bespoke onboarding wizard.

**Acceptance criteria** — all met:
- [x] Every config file parses; core files (settings/permissions/providers) are Fatal when missing or malformed, `plugins.json`/`mcps.json` degrade with a Warning by design.
- [x] A usable `providers.json` entry exists with a registered provider kind (Fatal when none); bad kinds, a dangling `default_provider`, and duplicate kinds across plugins each warn.
- [x] Presets parse eagerly (`agents/*.md` frontmatter); zero presets is Fatal (the cold-start case); `default_agent_preset` must resolve end to end or it is Fatal; non-default breakage and missing compression prompts warn.
- [x] Each `mcps.json` entry's `command` resolves on `PATH` (stdio) or `url` is well-formed (http); disabled entries are skipped. A parse-only `mcps_config` loader landed for this — G consumes it.
- [x] `project_root` — implemented where it actually lives: it is per-session, so the startup check covers the server's own footprint (config dir + data dir exist and are writable), and `CreateSession` now validates the client-supplied root (exists, is a directory, is writable) with `FAILED_PRECONDITION`.
- [x] The SQLite database opens (migrating) and is not newer than the binary; the opened database is handed to the serve stage rather than reopened.
- [x] Each discovered plugin's load outcome is reported; bundled failures are Fatal, user failures Warning, deliberate disables silent.
- [x] `max_concurrent_subagents` warns at ≤ 0 and > 8.
- [x] **No registration-name collisions (fatal)** — wired through `NameRegistry` at last; the diagnostic names both plugins.
- [x] Every `aliases` key resolves to something registered; dangling aliases warn.

**Test cases** (`tests/sanity_check_test.cpp`)
- [x] Each check fires on its own broken fixture and passes on a good one.
- [x] **Two independent failures both appear in one `RunAll()` result** — the linter property is the whole point and is the easy one to regress into fail-fast; the test asserts results from checks that run *after* the first Fatal fires.
- [x] Bundled-vs-user severity split is respected.
- [x] A name collision is fatal — this is the rule that makes tool replacement deliberate (§ *Name collisions and aliases*).
- [x] Unknown `allowed_skills` entries are reported (previously silently dropped by design).

#### What first contact found

1. **`ToolRegistry::Register` destroyed colliding tools before aliasing could save them.** `insert_or_assign` on the second registration deleted the first object, so the doc's flagship alias flow (two plugins providing `bash`, one aliased to `ssh_bash`) passed collision detection while serving only one tool — silent shadowing, the exact failure mode the fatal-collision rule exists to prevent. `Register` now keeps the first in the map and holds duplicates in a shadowed list; `ApplyAliases()` (once, after all registration) re-keys aliased tools under their surface names via a new `AliasedTool` wrapper, and `ResolveToolNames()` feeds `NameRegistry` for collisions, dangling aliases, and permission keys.
2. **`NameRegistry` was built and tested but wired to nothing.** No production path fed it, so collisions were never detected and aliases were parsed but never applied. `main.cpp` now applies aliases, resolves names, and installs the surface→permission-key map before the sanity check runs.
3. **Permission keys were never consulted.** `ToolExecutor::ResolveMode` and `ToolRegistry::DefinitionsFor` matched on the registered name, which is the surface name once aliasing exists — so `per_tool_overrides`/`allowed_tools` (which must never reference an alias) would have silently detached from aliased tools. Both now go through `PermissionKeyFor()`, which falls back to the name itself for unaliased and MCP tools: zero behavior change for every existing configuration.
4. **`NameRegistry::Resolve` missed dangling aliases for kinds with no registrations.** The alias loop ran inside the per-kind entry loop, so an alias naming a disabled or absent plugin was silently kept — the exact typo the check exists to catch. Now kinds with zero entries are walked separately.
5. **The spec's "`project_root` exists and is writable" check is unimplementable as written** — `project_root` is per-session (§ *Session storage*), supplied by the client in `CreateSessionRequest`. Split per the resolution above; `ARCHITECTURE.md` updated to match.
6. **Presets loaded lazily, so broken ones stayed silent until the first session.** The sanity check parses `agents/*.md` eagerly — `agent_preset_registry.cpp`'s "a future sanity_check makes them loud" note is now discharged.
7. **No `mcps.json` loader existed** — the check needed one; landed parse-only (interpolation deferred to G), with per-entry errors so one bad entry degrades alone.

### H. Production compactor — done

> § *Context compaction*. Built on a branch off `cab3a91`, in parallel with and unaware of A and B, then merged. **Status: built, wired, and delivered.** "Only the wiring was missing" understated the gap twice over. First, the injected `Compactor` type could not express failure, so it changed shape (returns `std::optional<CompactionResult>`; prompt/model/provider/cancel are per-call arguments rather than bound at construction, so a future `/agent` switch cannot compact with a stale preset's prompt). Second, the bundled `prompts/compression.md` did not exist anywhere in the tree.

**Acceptance criteria**
- [x] Summarises via the provider using `prompts/compression.md`, or a preset's `compression_prompt` override.
- [x] **Never splits a `tool_use`/`tool_result` pair** — already implemented and tested as boundary snapping; the production path must route through it, not around it. Proven load-bearing: removing the snap from `MaybeCompact` fails the end-to-end test.
- [x] Compaction failure degrades to "no compaction" rather than losing the transcript — `nullopt` result, history untouched, plus a per-turn cooldown so a failing summarizer is not re-invoked on every tool cycle of the same turn.

**Test cases**
- [x] Real compactor against `FakeLlmProvider`; assert the boundary invariant still holds end to end.
- [x] Provider error during compaction leaves history untouched (and cools down for the rest of the turn).
- [x] Preset-level `compression_prompt` override is used when present.
- [x] Unit coverage of `compact_messages`: verbatim pass-through of tool blocks, `context_tokens_before` ground-truth vs bytes/4 fallback (both over the whole context), empty-prompt short-circuit (provider never called), non-text response rejection, cancellation via the stream sink.
- [x] Seeding: a missing prompt is copied into the config dir, an existing one is never overwritten, a missing bundle is a no-op, and a seeded prompt reaches `AgentPreset::compression_prompt`.

#### What the merge with A+B found

1. **`Session` was still injecting the no-op stub.** The draft built the summarizer but nothing consumed it — `session.cpp` returned the range's first message unchanged. One statement, and without it the other ~310 lines are dead code.
2. **The bundled prompt had no delivery mechanism, which made the whole feature inert.** `AgentPresetRegistry` reads `<config>/prompts/compression.md`; the draft authored the file under a new `bundled/` with no install rule and nothing that copied it anywhere. So `compression_prompt` was `""` on every real install, `compact_messages` short-circuited, and compaction never ran on any session — visible only as one log line. Closed with `common/bundled_assets`, a CMake install rule, a build-tree staging step, and first-run seeding that never overwrites.
3. **`MINIMAL_PLUGIN_DIR` was defined by CMake and never read.** `BundledPluginDir()` hardcoded `lib/minimal/plugins`, so overriding `MINIMAL_PLUGIN_INSTALL_DIR` moved the install without moving the search. Found while building the parallel mechanism for prompts; both now resolve from the compiled-in cache value.
4. **`original_token_count` had no consumer** — computed, returned, dropped. Now logged with the message count on each successful compaction. No wire event: nothing in the protocol specifies one and no client could render it, so it belongs with the TUI. (§8 later found its two branches measured different spans, and renamed it `context_tokens_before`.)
5. **A concern that did not survive checking.** Repeated compaction within one turn looked unbounded — the cooldown covers failure but not success. It is not: each pass replaces `cut ≈ size/2` messages with one, so history halves every time and `history_.size() < 2` is the floor. It terminates in log2(n) passes. The reasoning is recorded in `MaybeCompact` so the next reader does not re-derive it and "fix" it with an invented threshold.

### I. `SessionObserver` registry

> § *`SessionObserver` — observe and react*. Schedule alongside A2, since `OnSessionEnded` needs a session to end.

**Acceptance criteria.** `tools-bash` registers an observer whose `OnSessionEnded` calls `BashStore().Erase(session_id)`; the cwd/env map no longer grows for the process lifetime (`bash_session_state.hpp:21`).

**Test cases**
- [x] Ending a session erases exactly that session's bash state and no other's.
- [x] A throwing observer does not take down session teardown.

### J. Permission advisor

> Specified by `docs/EXTENSIBILITY.md` §3. Independent of E and G — the permission substrate it composes with is built and tested (A3's wire checker, the executor's mode resolution, D's ceilings).

A plugin answers `Ask`-gated tool calls automatically and abstains to the wire checker — the human — when it has no answer. The substrate (mode lattice, precedence chains, out-of-workdir fold, subagent ceilings, run modes, grant ownership) is untouched; the **decision source for `Ask`** becomes pluggable. Composition is an advisor *above* the wire checker, never a replacement of it: abstain and throw both fall through to the human. **A post-J review added a budget:** `permissions.json` `advisor_timeout_ms` (default 30 s, 0 = off) bounds one `Advise` — the call runs on its own thread, the wait polls the cancel source, and a wedged advisor (one that ignores its cancellation contract) is abandoned at the budget's edge and the human prompted, instead of wedging the worker join, the reaper and process shutdown.

**What.** Checker injection in `Session` (`SessionParams` carries `std::unique_ptr<tools::PermissionChecker>`; `WirePermissionChecker` is the default and stops being a by-value member — `session.hpp:276` is the only structural change), `PermissionAdvisorRegistry` + `minimal_plugin_register_permission_advisor` in the plugin ABI, an `AdvisedPermissionChecker` decorator (advise → abstain/throw → wire), `permissions.json` `"advisor": "<plugin>"` with `"advisor_inherit"` for subagents (default **off**), and a sanity-check warning for an advisor name that resolves to nothing.

**How.** `SessionParams` carries the full checker, as the spec says. That forces one structural prerequisite: `WirePermissionChecker` is constructed from `Session`'s `queue_`/`pending_` (`session.hpp:286-288`), so no producer can build a *composed* checker until those exist outside `Session`. They are extracted into a per-session `SessionChannels { EventQueue; PendingRequests; }` block, created by the params producer and owned by `Session` through a `shared_ptr`; `Session`'s member-init constructs the default `WirePermissionChecker` from it when `params.permission_checker` is null. Every producer (`CreateSession`/`ResumeSession` at `session_service.cpp:65,195`, `SubagentManager::Spawn` at `subagent_manager.cpp:108-116`, the test helpers) creates the block. Composition then lives with the producers, who have both ingredients: `SessionServiceImpl` resolves `permissions.json`'s `"advisor"` through the `PermissionAdvisorRegistry` and, when set, assigns `params.permission_checker = AdvisedPermissionChecker(advisor, wire)` — the advised checker **owns** its inner wire checker. `SubagentManager` receives the resolved advisor + `advisor_inherit` at construction and repeats the composition for children only when opted in; ceilings already travel separately and are enforced before the advisor runs (`tool_executor.cpp:91`). The payoff: tests inject fake checkers through params directly, and the invariant "making `checker_` concrete again fails any test that injects a fake" holds literally.

**`Advise` signature** (§3.2 of the extensibility spec left the shape to this track): `std::optional<PermissionResponse> Advise(const PermissionRequest&, const AdvisorContext&)` with `AdvisorContext { std::function<bool()> is_cancelled; }` — the request struct stays a plain proto-adjacent value; cancellation reaches the advisor as a predicate supplied by `Session`.

**Implementation order** (each step testable before the next):

1. `src/server/tools/permission_advisor.{hpp,cpp}`: `PermissionAdvisor`, `AdvisorContext`, `AdvisedPermissionChecker` (advise → response; abstain **or throw** → inner checker), `PermissionAdvisorRegistry` with the `SetAttribution`/`ClearAttribution` window (`tool_registry.hpp:56-73`) so the loader stamps plugin names. Unit tests: allow/deny/abstain/throw fall-through; an undisclosed granted key leaves `session_grants_` untouched (executor-level; fakes at `tests/support/fakes.hpp:254-288`).
2. `SessionChannels` extraction + `SessionParams::permission_checker` (`session.hpp:61-98, 286-288`; `session.cpp:26-43`). Update all params producers (`session_service.cpp:65,195`; `subagent_manager.cpp:108`; test helpers `session_registry_test.cpp:90`, `subagent_test.cpp:148`, `session_test.cpp`). Session tests: default path unchanged; a fake checker injected through params is honored end to end.
3. Config: `PermissionsConfig` gains `std::optional<std::string> advisor` + `bool advisor_inherit = false` (`permissions_config.hpp:11-19`; parse tests in `permissions_config_test.cpp`). The first plugin-name field in `permissions.json` — the sanity check gains a new consumer rather than extending an existing pattern.
4. ABI + loader: `minimal_plugin_register_permission_advisor(PermissionAdvisorRegistry&)` (pattern ×4 live, `plugin_abi.h:60-99`); loader dispatch + attribution window; `PluginRecord::registered_permission_advisor`; startup printout (`main.cpp:127-148`). `main.cpp` constructs the registry (~`:109`), passes it to the loader, `SanityCheckInputs`, and `SessionServiceImpl`.
5. Producer composition: `session_service` sets `params.permission_checker` from the resolved advisor; subagent inheritance via the `SubagentManager` ctor flag (child default: no advisor).
6. Sanity check: `check_advisor()` in the `RunAll` list (`sanity_check.cpp:78-98`) — the configured name must resolve to a loaded plugin's registration, else Warning; registry passed via `SanityCheckInputs` like `command_registry` (`sanity_check.hpp:48`). Tests in `sanity_check_test.cpp`.
7. E2E: `tests/fixtures/fixture_advisor.cpp` — the first fixture to exercise a registration seam beyond tools/providers (modeled on `fixture_good.cpp`, built by `minimal_add_fixture_plugin`, `tests/fixtures/CMakeLists.txt:4-16`); allows one tool, abstains on another. The advisor E2E is also the first service/e2e test that runs `PluginLoader` (none do today — `session_service_test.cpp:45-127` loads none): abstained tool → real wire prompt → answer returns → `TurnCompletedEvent`; allowed tool never prompts.

**File map:** `tools/permission_advisor.{hpp,cpp}` (new); `agent/session.{hpp,cpp}`; `rpc/session_service.{hpp,cpp}`; `agent/subagent_manager.{hpp,cpp}`; `config/permissions_config.{hpp,cpp}`; `plugins/plugin_abi.h`, `plugin_loader.{hpp,cpp}`; `main.cpp`; `config/sanity_check.{hpp,cpp}`; tests: `permission_advisor_test.cpp` (new), `permissions_config_test.cpp`, `sanity_check_test.cpp`, `session_test.cpp`, `session_registry_test.cpp`, `subagent_test.cpp`, `fixture_advisor.cpp` + fixtures CMake, advisor E2E.

**Acceptance criteria — all met:**
- [x] An advisor's Allow runs the tool; its Deny produces the same error `ToolResultBlock` a human deny does; its abstain round-trips a real permission prompt over the wire. *(E2E: `session_service_test.cpp`, the abstained tool is the only prompt on the stream.)*
- [x] An advisor never sees — let alone approves — a tool the substrate resolved to `AlwaysDeny`, including via the `AllowOnly` run mode (`Ask` short-circuits before the advisor is consulted). *(executor-level: `permission_advisor_test.cpp` AllowOnly + ceiling cases, both assert `advisor->seen` stays empty.)*
- [x] Subagent ceilings hold with an advisor active in the parent; a child defaults to **no** advisor, and `advisor_inherit: true` still cannot exceed the parent ceilings. *(subagent-level inherit-gate tests + the executor-level ceiling-ordering test.)*
- [x] Grants returned by an advisor are filtered against the disclosure exactly as a human's are — the existing double filter (`tool_executor.cpp` + `proto_convert.cpp`) holds unchanged, proven by the undisclosed-key case.
- [x] A throwing advisor degrades to the wire prompt, never a lost turn. *(std and non-standard exceptions.)*
- [x] A wedged advisor that ignores its cancellation contract costs only its budget (`advisor_timeout_ms`), then the human decides; cancellation abandons the wait before the budget is spent. *(deadline-harnessed, `tests/support/deadline.hpp`.)*
- [x] A throwing advisor degrades to the wire prompt, never a lost turn. *(std and non-standard exceptions.)*

**Test cases**
- [x] Unit: advisor allow / deny / abstain / throw — abstain and throw both fall through to the wire checker.
- [x] Unit: `AllowOnly` + advisor → the tool is denied before the advisor runs (assert it is never consulted).
- [x] Unit: grant filter — an advisor returning an undisclosed key leaves `session_grants_` untouched.
- [x] Unit: cancellation mid-`Advise` → the call returns (detached-thread deadline harness; a joined future would hang the suite).
- [x] Integration: subagent spawned from an advisor-governed parent — child has no advisor by default (the prompt arriving IS the proof the advisor never ran); with `advisor_inherit` the child's advisor sees only requests that passed the parent ceilings.
- [x] E2E over a real socket: `fixture-advisor` — the first fixture to exercise a registration seam beyond tools/providers, loaded through the real `PluginLoader` (the first service test to do so) — allows one tool and abstains on another; the prompt reaches the client, the answer returns, the turn completes with `TurnCompletedEvent`.
- [x] **Invariant tests, all proven load-bearing per §6:** removing the abstain fall-through (abstain → Deny) fails the abstain, throw-degradation and cancellation tests; removing the grant filter fails the undisclosed-key test; making the session's checker concrete again fails both fake-injection tests (session-level and the E2E).

### K. Per-plugin config

> Specified by `docs/EXTENSIBILITY.md` §2.3. Independent of everything; sequenced **before L** so the new `register_services` symbol ships with the registration context and never needs a second signature sweep.

**What.** `ToolRegistrationContext` (`plugin_abi.h:51-56`) generalizes to `PluginRegistrationContext`: it keeps the verbatim `tools` object from `settings.json` and gains `config` — the plugin's own slice from its `plugins.json` entry:

```json
{"plugins": [{"file": "tools-bash.so", "enabled": true, "config": {"timeout_ms": 30000}}]}
```

Core imposes no schema — the same rule as `tools_settings` today. Every `register_*` symbol takes the context: tools, provider factories, session observers, commands, and auth (unwired, but swept for uniformity). Today only tool-registering plugins receive anything, and only the whole `tools` blob (`main.cpp:115-125`); a provider, observer, or command plugin has no sanctioned way to read its own settings.

**How.**

- `PluginConfig::Entry` (`plugin_discovery.hpp:22-34`) gains `nlohmann::json config`. Parse degrades to `{}` with a warning on a non-object value — the file's existing degrade-don't-throw rule. **The serializer must round-trip `config`:** `main.cpp:96` rewrites `plugins.json` after discovery, and a config that vanishes on the next save is user config silently dropped.
- `plugin_abi.h`: the signature sweep. Source compatibility makes it a rebuild for plugin authors, not a break.
- `plugin_loader.cpp` copies the entry's config into the context before each register call, inside the attribution window it already opens (pattern: tools at `:114-118`, commands at `:141-146`).
- Bundled plugins update mechanically. **Implemented deviation:** the knob migration was taken immediately and strict — `tools-bash` and `tools-fs` parse their settings from `context.config` with **no** `tools.*` fallback (`tools.bash.*` / `tools.read.*` / `tools.edit.*` in settings.json are no longer honored); `bundled/settings.json`'s `tools` object is now empty and remains only as the verbatim channel for third-party tool plugins.

**Acceptance criteria**
- [x] A plugin's `config` object reaches every register symbol verbatim, and only that plugin's.
- [x] A malformed `config` (non-object) degrades to `{}` with a startup warning; the plugin still loads.
- [x] The `main.cpp:96` rewrite preserves `config` — no user-config loss on save.
- [x] `tools_settings` still carries the whole verbatim `tools` object to tool plugins.

**Test cases** (`tests/plugin_discovery_test.cpp` — new, parse/serialize/discovery — and `tests/plugin_loader_test.cpp`)
- [x] Parse: `config` present / absent / non-object; serialize round-trips it through a load→save→load cycle (plus discovery propagation: entry config → candidate → context).
- [x] Two plugins with different configs — each context sees only its own slice (isolation; observed through the real dlopen via the fixtures' config-echo tools).
- [x] **Invariant test:** removing the per-plugin copy (every context gets one shared entry's config) fails the isolation test.
- [x] One bundled plugin compiles through the new signature and still parses its settings unchanged.

### L. Cross-plugin services (ServiceRegistry)

> Specified by `docs/EXTENSIBILITY.md` §2.2. Sequenced after K. **Done.** The missing half of plugin-owned state: `SessionObserver` covers ordered notification (push), this covers queries (pull). §2.5's server half dispatches `PluginCall` *through* this registry when E lands the client half.

**What.** A sanctioned channel by which one `.so` can reach another's services:

```cpp
// src/server/plugins/include/minimal/plugins/service_registry.hpp
class ServiceRegistry {
public:
    // Register an abstract interface under a name. Non-owning: plugin handles
    // are never dlclosed, so a process-lifetime pointer is safe.
    void Provide(std::string name, void* service);
    // Null when absent: the provider plugin may be disabled -- a runtime
    // condition, not an error.
    void* Find(std::string_view name) const;
};
```

**Typed at the edges, named in the middle.** The provider ships its interface header (e.g. `bash_state.hpp` with `tools-bash`); a consumer `static_cast`s `Find`'s result to it. The name is the only coupling; the header is the contract, under the same source-compatibility model as everything else. Collisions resolve through the existing `NameRegistry` as a new kind — **fatal, not shadowed**. No bus, no subscribe: push already exists, and an event bus would put ordering/concurrency burden back on plugin authors — exactly what serialized observer dispatch removed.

**How.** As specified, plus three implementation details:

- `NameKind::Service` in `name_registry.hpp:11`; `ResolveRegistrationNames` picks up service attributions, and `main.cpp` feeds them through the existing collision/sanity path (`sanity_check.cpp` turns them Fatal, naming both plugins). **Post-L review:** `main.cpp` also `Seal()`s the registry right after `LoadAll` — after that a `Provide` is logged and ignored, so the unguarded map's safety is a property of the registry, not a comment about load order (a runtime registration would race concurrent `Find`s and slip past collision detection besides).
- `minimal_plugin_register_services(ServiceRegistry&, const PluginRegistrationContext&)` — loader dispatch with an attribution window, like tools and commands. `PluginRegistrationContext` gains `ServiceRegistry* services` — the CONSUMER side: a plugin captures the pointer at registration and resolves with `Find` lazily at use time, since plugin load order is file order and a consumer can load before its provider.
- The provider's shipped interface must be **abstract** (header-only, pure virtual): the concrete store's symbols live inside the providing `.so` (`RTLD_LOCAL`), so a consumer calling them by symbol would fail at `dlopen(RTLD_NOW)`; virtual dispatch needs no symbols. `tools-bash` ships `bash_state_service.hpp` (`BashSessionStateService`: `Erase`/`Reset`) and `BashSessionStateStore` implements it.
- **First consumer:** `/restart-shell` (extracted to `restart_shell.{hpp,cpp}` so tests drive it in-process) resolves the store through `Find` and answers a clear `kError` when the service is absent. One deviation: the command STAYED in `tools-bash` rather than moving to `commands-core` — the reasoning is that a command belongs to the plugin that owns the state it touches; the service path and the absent-provider degrade are exercised by tests against the extracted command.

**Acceptance criteria**
- [x] A plugin Provides at registration; another consumer Finds, casts, and calls through the interface.
- [x] `Find` for a name no plugin provided returns null — no error, no warning.
- [x] Two plugins providing the same name are fatal at startup, naming both.
- [x] `/restart-shell` works through the service path and degrades cleanly when the service is absent.

**Test cases**
- [x] Provide/Find round-trip; Find-before-Provide and Find-never-provided both null.
- [x] Collision: fatal, diagnostic names both claimants.
- [x] Loader attribution records the providing plugin; `NameRegistry::Resolve()` surfaces it.
- [x] `/restart-shell` through the registry (in-process); the absent-provider error path.
- [x] **Invariant test:** removing the collision check for `NameKind::Service` (last-wins) fails the collision test.

### M. Tool history access — done

> Specified by `docs/EXTENSIBILITY.md` §2.1. Independent. Two refinements found while wiring it: **ownership** (as planned below) and **the window** (the tool-facing type).

**What.** `ToolContext` (`tool.hpp:29-60`) gains an optional read-only handle on the conversation it runs inside — **as `session::HistoryWindow`**, not the raw `SessionReader` the spec sketched:

```cpp
// tool.hpp -- set by the agent loop per invocation; null when unavailable
const session::HistoryWindow* history = nullptr;
```

Anything context-aware — a "summarize the thread so far" tool, retrieval keyed to recent turns, a wrapper referencing what was last edited — becomes expressible.

**How.**

- **Ownership.** The agent loop has no `SessionReader` today (only `HistoryStore&`, `agent_loop.hpp:191`) and `Session` doesn't hold one; `CreateResolved` builds one ad-hoc for resume (`session_registry.cpp:121`). `SessionParams` gained a reader pointer, owned by `Session`; the agent loop sets `ctx.history` from it per invocation (`agent_loop.cpp:386-397`). Stores that are not SQLite-backed (test fakes) leave it null, so `history == nullptr` is the natural "unavailable" case, not an error path.
- **The window — the refinement.** The spec had the constraint stated as a comment and the pointer typed `const SessionReader*`, whose `LoadAll()` is public (it is the resume path). Implemented instead as `session::HistoryWindow` (`history_window.hpp`): a header-only, non-owning wrapper holding the reader pointer **and the session id it is bound to**, exposing only keyset-paged `GetMessages(before_seq, limit)`. The windowed-only rule is a property of the type, not a comment — no path a tool can reach materializes the full history, and the isolation is structural (a tool cannot page another session's transcript, because it has no way to name one). `SessionReader` itself is unchanged; `LoadAll` stays the resume path's alone.
- **Wiring.** `Session` owns `history_reader_` + the `HistoryWindow` built from it in the constructor body, both declared before `loop_` so destruction order keeps the loop's borrowed pointer safe; `SessionContext` carries the window pointer. The registry is the one funnel every session passes through — top-level and `SubagentManager` spawns alike — so injecting the reader there binds a child's window to the child's own id with no extra code.
- **Frozen surface.** `history_window.hpp` (with `session_reader.hpp`) joined the utilities include set in `plugin_abi.h`: a plugin tool using `ctx.history` needs it declared surface.

**Acceptance criteria**
- [x] A tool reads a windowed range of the transcript through `ctx.history` during a live turn.
- [x] Sessions on non-SQLite stores (tests) see `history == nullptr` and tools handle it.
- [x] A subagent's tool sees the child's transcript only — never the parent's.
- [x] No path a tool can reach materializes the full history — enforced by the wrapper, not just stated.

**Test cases** (`tests/tool_history_test.cpp`)
- [x] End-to-end with a scripted provider over a real SQLite store: a tool reads the last messages via `ctx.history` mid-turn (user turn + its own tool_use, newest first) and the result reaches the model.
- [x] Null-history path (in-memory fake store) — the tool's degrade works, no crash.
- [x] Subagent isolation: parent has N messages, child M; the child's tool observes M, and the parent's text is nowhere in its window.
- [x] `HistoryWindow` direct: newest-first paging, keyset cursor, `has_more`, and binding (another session's row never appears).
- [x] **Invariant test:** the end-to-end read asserts `history != nullptr`, so the agent loop building the context without the reader fails it.

### N. Model auto-discovery — done

> Specified by `docs/EXTENSIBILITY.md` §2.6. Independent of E and G. The payoff is concrete: `context_window_tokens` — the field window-relative compaction resolves against (§10) — stops being the one most likely to be stale or absent, and a provider serving fifty models stops being unconfigurable by hand.

**Status: implemented as specified, with the decisions below.** `src/server/llm/model_catalog.hpp` (`DiscoveredModel`, `ModelCatalog::List` — const, per the same no-mutable-state contract as `LlmProvider` — and `ModelCatalogRegistry` mirroring `ProviderFactoryRegistry`); `minimal_plugin_register_model_catalogs` in the ABI (additive, no signature sweep); `providers.json` `"discover": true` accepted with or without a static `models` object (the legacy array still refused by name); `ProvidersConfig` retains per-provider entries, injects a discovery hook from `main.cpp` after plugin load (config layer never links plugins), caches per provider (generation-keyed, always 0), negative-caches failures, and returns discovered/enriched models from an overlay map. The one OpenAI-shape catalog ships beside `provider-openai` (`openai_model_catalog.{hpp,cpp}`: pure `ParseModelList` + an `HttpClient::Get` on `<url>/models`, reading `context_length` / `meta.n_ctx_train` / `max_model_len`, missing fields degrading to `nullopt`); its listing request timeout is 10 s, not the chat-length one, because a hung catalog must not wedge a resolver.

**Concurrency — the post-N review's restructure (recorded because the first draft got it wrong).** The first draft called the hook under the single global config mutex: one hung catalog stalled *every* `Resolve` — all session creation — for the full 10 s, and `/providers refresh` held the lock across serial fetches of every discover-provider (N×10 s process-wide). It also erased overlay entries on refresh while `Resolve` handed out raw pointers into them — a use-after-free window between the RPC pool and a concurrent refresh. The shipped shape:

- **`Resolve` returns `std::shared_ptr<const ResolvedModel>`** — a snapshot. Overlay entries are written once and never mutated in place; refresh *replaces* entries, and outstanding holders keep their snapshot alive. A vanished model leaves the listing immediately without dangling anyone.
- **Per-provider fetch gates.** The network call (`FetchOutcome`, the hook) runs holding ONLY the provider's gate — never the global mutex — with a double-check under the gate after waiting (another resolver may have fetched first). The gate preserves the "N threads, one fetch" property per provider; unrelated providers, and all plain config reads, proceed regardless of how slow one catalog is. Lock order is gate → mutex, never the reverse.
- **`/providers refresh` fetches per provider outside the global mutex** too — providers refresh in parallel, a hung catalog costs its own 10 s, not the process's.
- **The transport is loopback-tested.** `Mode::Get` originally fell into the streaming branch of the curl write callback and silently dropped every GET body — every real catalog fetch was misread as a malformed response and negative-cached, invisible to the canned-body tests. `tests/http_client_test.cpp` now runs a loopback HTTP server (Get round-trip, Post round-trip, body-cap abort), and `tests/model_catalog_live_test.cpp` exercises the real catalog against a running backend when `MINIMAL_LLM_BASE_URL` is set (skip otherwise). Buffered responses are capped at 8 MiB in bytes, not just seconds.

**Decisions taken during implementation (recorded here because the spec text left them open):**

1. **The sanity check is network-free.** "Never at startup" and "warns when a catalog is unreachable" conflict unless *unreachable* is structural. The check warns when a discover-entry's backend kind registers no `ModelCatalog` and warns once per provider when its backend is unregistered; a discover-entry with a registered backend counts as usable with zero static models (the "zero discovered" fatal materializes at first resolve instead, with the miss reporting naming the failure); preset/`default_model` refs under a discover-entry that resolve to no static model are not flagged — unverifiable without a network call.
2. **No persisted cache, no TTL.** Evaluated and rejected: the server is short-lived (auto-spawned, exits with the last client), so a TTL on a runtime cache is meaningless and a persisted cache would add a state file plus invalidation keys while reintroducing month-stale windows — the exact problem the track fixes. The hook + generation-keyed cache shape leaves the door open if a deployment ever shows the pain.
3. **`/providers refresh` shipped in v1.** Mid-process backend changes (llama.cpp restarted serving a different model set) are real; the refresh command evicts the cache and refetches immediately, reporting per provider, so the operator gets the fresh list rather than a promise. It is `ReadOnly()` — meaning it never touches the conversation path, which is what qualifies it for the RPC pool beside live turns; the per-provider fetch gates (above) are what make the network I/O and cache mutation safe there, not the label. The `/providers` listing also resolves each ref's context window, which warms cold discover-entries' caches — a listing fetches, by operator decision, because the listing is where the operator looks for the fresh list; the gated fetch means it costs one catalog call per cold provider, never a stall.
4. **Resume after the backend's list changed** needs no mechanism: it degrades to today's fail-loud shape (session creation throws "unknown model"), now with the honest reason — "discovery succeeded, model not served" vs "discovery failed: <reason>". No silent substitution of another discovered model behind an existing transcript.

**Acceptance criteria**
- [x] A provider with `"discover": true` and no `models` parses and serves discovered refs at first resolve.
- [x] Discovery failure degrades: static models still resolve, and the miss error distinguishes typo from dead catalog.
- [x] A discovered model's `context_window_tokens` reaches the compaction trigger (via `ApplyModel` into the preset; proven through the command fixture and the preset registry tests).
- [x] Nothing ever writes discovery results back to `providers.json`.
- [x] Sanity check: unreachable catalog warns (structurally); zero static ∪ zero discovered still fails usable-provider.

**Test cases** (`tests/providers_config_test.cpp`, `tests/model_catalog_test.cpp`, `tests/commands_test.cpp`, `tests/sanity_check_test.cpp`, `tests/agent_preset_registry_test.cpp`, `tests/plugin_loader_test.cpp` + `fixture-good`)
- [x] Parse: `discover` + no-`models` accepted; the legacy array still refused by name; non-boolean `discover` refused.
- [x] Lazy fetch: `List` called once on first miss; a second resolve of the same ref hits the cache (assert the call count).
- [x] Negative cache: a failed discovery is not re-attempted per resolve.
- [x] Union: `ModelRefs()` includes cached discovered entries, sorted; a cold cache contributes none.
- [x] Catalog impl: canned JSON bodies — llama.cpp shape, OpenRouter shape, vLLM shape (`max_model_len`), missing metadata fields degrade to `nullopt`.
- [x] Enrichment: a static model with an omitted window gains the discovered window (overlay copy; the parsed entry is never mutated); a static model with its own window is untouched.
- [x] Refresh: evict-then-refetch visible to a subsequent `Resolve` and to the listing; per-provider outcomes; a dead catalog reports instead of throwing; no discover-entries is an honest error.
- [x] Concurrency: parallel `Resolve` from eight threads fetches once and resolves for all.
- [x] **Invariant tests:** removing the miss→discovery fallback fails the lazy-fetch test; resolving `context_window_tokens` from static-only fails the compaction-window test.

### O. Project context files (AGENTS.md / CLAUDE.md) — done

> Independent of the other tracks. The de-facto project-instruction convention: a repo documents its build/test conventions in `AGENTS.md` (the emerging cross-tool standard) or `CLAUDE.md` (Claude Code's name), and the harness should put that in front of the model without anyone pasting it into a prompt.

**What.** Each session's system prompt gains the instructions from the first context file found in the session's `project_root`. Discovery and content are config (`settings.json`):

```json
"project_context": { "enabled": true, "files": ["AGENTS.md", "CLAUDE.md"],
                     "max_bytes": 0, "search_ancestors": false }
```

`files` is tried in order within each directory (first existing wins; `CLAUDE.md` is the fallback, not an additional file); `max_bytes: 0` means unlimited, and a set cap truncates with a visible marker line; `search_ancestors` walks upward from `project_root` to the filesystem root — nearest directory first, THEN the configured file order within it, so a `CLAUDE.md` in the project beats an `AGENTS.md` three levels up (closer instructions are more specific), and a `~/AGENTS.md` becomes a global fallback, which is documented as a feature rather than fenced off.

**How, and the decisions the spec left open:**

- **The loader never throws and treats absence as the normal state** (`src/server/config/project_context.{hpp,cpp}`, the `SkillRegistry` pattern). A found-but-unreadable file is distinguished from "not found" through an error out-param, so the registry can log one honest warning instead of silently ignoring instructions the operator wrote. An empty or whitespace-only file is skipped and the search CONTINUES — it carries no instructions and must not stop `CLAUDE.md` from being found.
- **Injection is a SESSION property, not a preset one.** The file lives in the session's `project_root`, which no preset knows; the rendered section rides `SessionParams::project_context` into the `AgentLoop`, which appends it after the preset body in `BuildChatRequest`. Baking it into `AgentPreset::system_prompt` would have been simpler but wrong twice: the preset registry's cache is keyed by preset name (project-specific prompts would pollute it), and a mid-session `/agent` re-resolves the preset from disk — the project's instructions would silently vanish at exactly the moment the operator switched agents mid-task.
- **Loaded once in `SessionRegistry::CreateResolved`** — the one funnel top-level creation, resume, and `SubagentManager` spawns share — so a child inherits its parent's context through the parent's `project_root` with no extra code, and a resumed session re-reads it deterministically. The section is constant for the session's life; the prompt-cache prefix is never disturbed mid-session. If the file changed between sessions, a resumed session picks up the new text — accepted, the same spirit as config having no hot reload.
- **The registry defaults to DISABLED** until `main.cpp` wires the parsed settings in (`set_project_context`), the same shape as `set_commands`: an unwired producer is a feature that is off, not one running on guessed defaults — and no registry-constructing test scans the filesystem by accident.
- **A symlinked `project_root` is canonicalized first** (`weakly_canonical`), so the ancestor walk starts where the root actually sits — the same canonicalize-first stance as `path_confinement`.
- **File names are validated as file NAMES**: `files` entries containing `/` or `\` are rejected at parse time — the setting means "the convention file", not "an arbitrary path I resolved once".
- **Occupancy note, deliberately not "fixed"**: the compaction trigger's fallback estimate (bytes/4 over history) still does not count the system prompt, so a very large context file under-states occupancy until the backend's usage reports arrive (which capture it). This is the same estimate-only stance the compactor already records; capping is the operator's lever (`max_bytes`), not a new accounting mechanism.

**Acceptance criteria**
- [x] `AGENTS.md` wins over `CLAUDE.md` in one directory; `CLAUDE.md` is the fallback when `AGENTS.md` is absent; absence is silent.
- [x] The section is appended after the preset body, names the winning file, and survives `/agent`.
- [x] A subagent inherits the context from the parent's `project_root` through the `CreateResolved` funnel, with no spawner changes.
- [x] `search_ancestors` defaults OFF; when ON, proximity beats file order across directories.
- [x] `max_bytes` truncates with a visible marker recording shown/total bytes; a file exactly at the cap is not truncated.
- [x] The feature is inert until wired: a registry without `set_project_context` loads nothing.

**Test cases** (`tests/project_context_test.cpp` — new; `tests/settings_config_test.cpp`; `tests/session_registry_test.cpp`; `tests/agent_loop_test.cpp`)
- [x] Loader: found/verbatim/fallback/order; disabled; empty and whitespace-only skipped with the search continuing; no-candidate yields nullopt with no error; `max_bytes` truncation and exact-boundary; a directory named `AGENTS.md` is not a candidate; ancestor walk on/off and the proximity invariant; canonicalized `source_path` (macOS `/var` → `/private/var`).
- [x] Renderer: header + body; the truncation marker; empty body renders nothing.
- [x] Settings: defaults (`enabled`, both standard names, no cap, no ancestors); overrides; rejection of non-object, non-array `files`, empty/non-string entries, path-shaped names, negative `max_bytes`.
- [x] Registry: context reaches `BuildChatRequest().system` with the preset body still at the front; `CLAUDE.md` fallback through a real session; absent when disabled or no file.
- [x] Loop: exact concatenation; survives `SetPreset`; stable across rebuilds (the cached-prefix invariant).

---

### O. Search tools (grep/glob) — done

> Independent of E and G. The payoff is context efficiency: a structured, `.gitignore`-aware search result (line numbers, counts, bounded) instead of the model round-tripping a raw `rg` dump through bash, and a file-finding primitive that does not require the model to remember `find`'s argument order.

**Status: implemented.** New bundled plugin `plugins/server/tools-search/` — categorically identical to `tools-fs` (separate `.so`, links `minimal::server_core`, bundled ⇒ enabled, fatal sanity-check on load failure), registering `grep` and `glob` through the ordinary `minimal_plugin_register_tools` seam. Shared internals: `glob_matcher` (glob→RE2 translation, one flag set away between the two callers' brace semantics), `ignore_rules` (gitignore sets + the per-depth stack), `tree_walker` (the bounded traversal both tools run on). Settings come from the plugin's own `config` object per the track-K mechanism; both tools are `ReadOnly` with `path_confinement_supported()`/`ExtractPath()`, so permission classification stays where it already was. `re2` joined the vcpkg manifest (registry-verified) behind a `minimal_re2` interface target, same normalization pattern as tree-sitter.

**Decisions taken during implementation (recorded because the spec text left them open):**

1. **RE2, not std::regex/PCRE2 — the linear-time guarantee is the requirement.** Patterns are model-authored; a backtracking engine lets `(a+)+b` wedge the session worker thread for unbounded time, the exact hang bash's `timeout_ms` exists to bound. Linear time makes it unreachable rather than bounded. Cost: no backreferences/lookarounds, stated in the tool description so the model stops reaching for them.
2. **Real `.gitignore` support, git-index deliberately not consulted.** Same call `SuggestPaths` made: no git dependency. Untracked-but-unignored files are searched — ripgrep's default behaviour, not a gap. Precedence is git-faithful (deeper file wins; last matching rule in a file wins, which is what makes `!` re-inclusion work), and ignored-directory pruning is git-faithful too (nothing under an ignored directory is considered — re-inclusion under one is impossible here as in git). One untranslatable line (unterminated `[`) becomes a rule that never matches rather than voiding the file: a malformed line must never *widen* what a search returns.
3. **Symlinks are never followed — files as well as directories.** A symlinked directory is the classic traversal wedge; a symlinked *file* silently aims the search outside the root the model pointed at. Both cost more than they buy in a local single-user harness.
4. **Truncation totals are honest lower bounds.** A budget-stopped walk reports "showing X of **at least** Y" — the unscanned remainder is by definition unknown, and an exact-looking number over a partial walk trains the model to trust truncated results. The re-run hint (`offset=`) rides the same footer; `read`'s footer is the precedent for the whole affordance, the model having no structured channel.
5. **Disclosures survive a zero-match result.** Skipped-oversize, timeout, and cancellation notes are emitted even when nothing matched — a bare "No matches found" over a partial walk is a *false negative*, the worst shape a search tool can produce.
6. **The walker is plugin code, not a core utility.** Only `path_confinement`/`file_access`/`atomic_write` are core-exposed; a second consumer (a third-party search-adjacent plugin) promotes it later, per the philosophy. Both tools' traversal needs are identical, so it lives inside the one plugin rather than being duplicated.
7. **Content-mode blocks are atomic against the budget.** A match and its context are emitted or withheld together; emitting a fraction would present context with no match. `offset`/`head_limit` budget MATCHES, and context rides free — the pagination affordance stays predictable.
8. **Brace alternation is a flag, not a behaviour.** `{a,b}` in the glob tool and glob filter; literal braces in gitignore patterns, as git treats them. One translator, two callers, the difference explicit — the alternative was two translators drifting apart.

**Acceptance criteria**
- [x] Both tools load as one bundled plugin and register through the ordinary seam; a `plugins.json` `config` slice tunes every limit (track-K mechanism, no new config surface).
- [x] Both are `ReadOnly` and declare their paths; the out-of-workdir `Ask` fold applies to an escaping search root without tool-side permission code.
- [x] Every invocation is bounded: per-entry cancellation poll, wall-clock timeout, per-file size cap, result cap with footer.
- [x] gitignore semantics match git for anchor/dir-only/negation/`**`/classes, including nested-file precedence; `.git`/`.hg` always skipped; symlinks never followed.
- [x] Deterministic output: per-directory name sort; glob mtime-sorted with a rel-path tiebreak.

**Test cases** (`tests/tools_search_test.cpp` — tier 1 + tier 3 against `temp_dir.hpp`, 31 cases)
- [x] Translation: `*`/`?`/`[...]`/`{a,b}`/`**` in its three placements; escapes; regex metachars taken literally; untranslatable patterns fail per-caller policy.
- [x] gitignore: basename-at-depth, anchoring, dir-only, comments/escapes, last-rule-wins negation, nested-stack precedence, `gen/**` not matching `gen` itself.
- [x] Walker: deterministic order, VCS dirs skipped, per-level gitignore (including nested re-inclusion), ignored-directory pruning, symlink refusal, oversize skip+count, cancellation, callback limit, timeout reported.
- [x] grep: all three output modes, context blocks with `--` separators and zero-gap merging, case insensitivity, binary skip, glob filter, head_limit/offset pagination with honest footers, invalid regex as error result, wrong-typed input as error result, file-path search root, `emit_output` streaming, cancellation partial + "at least".
- [x] glob: `**` patterns, mtime ordering, literal fast path, head_limit footer, invalid pattern as error.
- [x] **Invariant tests:** removing the counter propagation in the walker's finish step fails every count assertion (demonstrated); removing the offset/budget merge guard fails the content-mode pagination test.

---

## 5. Dependent tracks (after A)

### C0. Provider and model addressing — done

> § *Config*. Split out of C because `/model` needs a way to name a model and the schema had none.

`providers.json` made an entry *be* a model, so two models on one endpoint duplicated the connection
and `parameters`/`max_tokens`/`context_window_tokens` came from the entry regardless of which model a
preset had selected — one llama.cpp serving a 128k model and a 32k model was **inexpressible**.

Providers are now an object keyed by name carrying a connection plus a nested `models` map, with
provider-level defaults a model may override. A model is named `<provider>/<model>` in all four
positions that name one, through a single parser in `common/` (the client validates `--model` too).
Preset `provider:` + `model:` collapsed into one `model:`; `settings.default_provider` became
`default_model`. The old top-level array is refused by name rather than misparsed — a hard break, no
dual-schema path.

**Precedence, which is the point:** a preset supplies a default model; `--model` (and later `/model`)
outranks it. `/agent` and `/model` are two knobs.

- [x] Two models on one provider share a connection and carry different context windows.
- [x] A model inherits provider-level `parameters`/`max_tokens` and may override either.
- [x] `<provider>/<model>` parsing rejects every ambiguous form.
- [x] **Invariant:** the legacy array is refused, naming the file and the new shape.
- [x] **Invariant:** a user-chosen model outranks the preset's.
- [x] Verified end to end: the same endpoint reached as `local/…` (full answer, exit 0) and
      `capped/…` (`max_tokens: 48` → truncated, exit 2), proving the per-model cap reaches the wire.

### C. `/`-command system — done

> § *`/`-command system — `src/server/commands/`*, § *Name collisions and aliases*.

`src/server/commands/`: `Command` -> (`HarnessCommand`, `LLMCommand`), registry, dispatcher,
`ConfirmationBroker` + `rpc/wire_confirmation_broker` (built like `WirePermissionChecker`, register
before publishing), and `CommandContext` over a `SessionControl` interface — an interface rather than
`Session&` so `commands/` does not depend on the thing that runs it, and so a command is testable
without a worker thread, a provider and a transcript store.

**Two things this track fixed that were not on the list.** `NameRegistry` had existed and been
unit-tested since the plugin work landed while nothing outside its own tests ever *constructed* one,
so duplicate registrations shadowed silently via `insert_or_assign` and which plugin won depended on
directory iteration order; the aliases map was likewise parsed from `plugins.json` and
re-serialized without ever being applied. C is the first track with a second registry, which is what
made connecting them worth more than reimplementing per registry. Collisions are now fatal at
startup, naming both claimants.

**A bug the manual run caught after the unit tests were green.** `--model` was applied to the preset
by `SessionRegistry` but never *recorded* on the session, so `/agent` — which re-resolves the preset
from disk — silently reverted to the preset's own model. It was invisible because the success message
printed a bare model id, identical for both references in the test config. Making the message print
the full `<provider>/<model>` reference is what exposed it, and it now has its own test.

Core registry plus `Command` → (`LlmCommand`, `HarnessCommand`) hierarchy; `CommandContext` borrowing session-owned references. Read-only `HarnessCommand`s dispatch to a short-lived task — *not* inline on the reader thread, which must never block; everything else queues for the worker at a turn boundary.

Fills the empty `commands-core` plugin and unblocks `/restart-shell` in `tools-bash` (`plugin.cpp:62`).

- [x] Read-only commands execute without blocking the reader.  *(proven over a real channel: the turn is parked on a permission prompt and `/peek` still answers; queueing it instead fails the test)*
- [x] Non-read-only commands land at a turn boundary, never between a `tool_use` and its `tool_result`.  *(structural — turns and commands share one queue and the worker pops only between `RunTurn` calls)*
- [x] Alias resolution and collision rules match § *Name collisions and aliases*; permission keys are never aliases.  *(and the collision check now RUNS — `NameRegistry` had never been constructed outside its own tests)*
- [x] `/restart-shell` resets the same map `restart: true` does — proving plugin-owned state has one owner.

### D. Subagents — done — done

> § *Subagents*. Built on the merged A+B+H tree. **Status: built, wired, and exercised end to end over a real socket.** `src/server/agent/subagent_spawn.{hpp,cpp}` (request/outcome types, `intersect_allowed`), `subagent_manager.{hpp,cpp}` (the spawn itself), `spawn_subagent_tool.{hpp,cpp}` (the core `Execute`-gated tool). A subagent is a `Session` with a parent pointer — the same entity, not a second implementation. Blocking and non-blocking paths both route through one `AgentLoop::RunTurn`.

- [x] Blocking subagent returns its final text as the parent's `ToolResultBlock`.
- [x] Non-blocking completion is injected at a turn boundary (`QueueInjectedMessage` already existed for exactly this — and gained a mutex, because its producer is now another session's worker).
- [x] Terminating a blocking subagent unblocks the parent with an error result rather than hanging (§ *Session lifetime*). Cancelling the parent turn does the same, terminating the child.
- [x] A subagent cannot spawn subagents — enforced in `agent_loop.cpp` and asserted end to end: a child whose preset allows `"*"` still gets a request with no `spawn_subagent` definition.
- [x] `clamp_run_mode` holds: a child is never more permissive than its parent. Run mode is clamped at spawn; the parent's policies travel as ceilings the child's `ToolExecutor` folds in via `most_restrictive`; `allowed_tools`/`allowed_skills`/`allowed_mcp_servers` are intersected (`"*"`-aware); held grants are copied.
- [x] Consent-grant inheritance decided and tested — copy-at-spawn, the lean this plan recorded, now in `ToolExecutor`'s seed-grant constructor argument.

#### What the implementation found

1. **The wire had no way to attach without side effects.** `OpenSession`'s stream is named by its first `ClientEvent`, and every existing payload does something (turn, cancel, answer). Attaching to a subagent child — a session the client never sends work to — was therefore inexpressible, which made the attach obligation (§4 A4) unimplementable, not just untested. Added an `Attach` payload: a no-op whose only job is to name the stream.
2. **Blocking spawn does not drive the child's loop from the parent's worker, contra the architecture's wording.** The child's own worker runs the single turn in both modes; a blocking `execute()` synchronously waits on a completion signal (`SessionParams::on_turn_finished`, fired on every exit path of `RunOneTurn`) while observing the parent's cancellation. One drive path instead of two, and terminate-unblocks-parent falls out of the existing teardown join rather than needing its own mechanism. `ARCHITECTURE.md` was updated in the same change.
3. **Non-blocking injection is cross-thread by construction** — the child's worker queues into the parent's loop — so `injected_` gained the mutex it never needed while the feature was unbuilt.
4. **A completion is an error, not an empty success, when the child stopped with nothing to say** (cancelled, failed, or `max_turns`-capped mid-loop). The parent model adapting to a named reason is the design intent of every spawn error; an empty block reading like success is the failure the bundled `subagent.md` itself warns against.
5. **Testing infrastructure, again.** Parallel `ctest` runs discovered cases against the same binary sharing fixed temp arenas — one case's constructor `remove_all()` wiping another's socket mid-test. Every multi-case fixture arena is now unique per process and per fixture (`pid` + counter). Also: ASan over the in-process tiers found a pre-existing one-byte out-of-bounds read in `file_access_test.cpp` (a 14-byte `std::string` built from a 13-byte literal); fixed.

### E. TUI client

> § *TUI rendering*, § *TUI layout*, § *Tab states and attention*, § *Message-kind rendering*, § *@-mention file references*.

Largest remaining surface: FTXUI layout, md4c markdown rendering, tree-sitter highlighting, themes, session picker, running-tools panel, permission modal, @-mentions (`SuggestPaths`). Depends on A3 for every RPC it uses and on B for scrollback.

The permission modal is where consent scoping becomes real UI — and where the **wording obligations** in § *Consent scoping* must be honoured: a grant is blanket permission for a binary, and disclosure is a lower bound.

- [ ] Permission modal renders `PermissionDisclosure` including per-binary session grants.
- [ ] Streaming tokens render without full-buffer reflow at 512k–1M context.
- [ ] Tab bar: colour + single `!` attention glyph only (no metadata clutter).
- [ ] Scrollback pages via `GetMessages` without competing with live turn traffic.

#### E additions — extension seams (added post-hoc; sequenced after the original E definition-of-done)

> Specified by `docs/EXTENSIBILITY.md` §4 (TUI core vs. pluggable) and §4.4 (command results never enter the chat). Appended without rewriting E's original scope, which is in flight: the frame lands first, then these.

- [ ] Client plugin host — loader, ABI tag, and the registries, mirroring the server-side pattern (`src/client/plugins/`; nothing exists there today).
- [ ] `ListCommands` RPC — unary; name, summary, usage, kind, display, read-only. Fetched once at connect (plugins load at startup and config is read once, so the list cannot change mid-connection — no invalidation machinery). Prompt-editor `/`-autocomplete consumes it and annotates prompt shorthands.
- [ ] `CommandResult` display modes — one additive proto field + dispatcher pass-through (the server half is small); results surface as status line, view, or transient error, never as chat rows. Headless `-p` prints regardless.
- [ ] **Invariant tests:** no command path appends a message to the store except a prompt shorthand's interpolated turn; no `CommandResult` is rendered or persisted as a chat message; subagent completion injections stay messages.
- [ ] Prompt-shorthand commands land as a plugin example (`review`-style) — the only command kind whose content reaches the model, and only as the submitted turn.
- [ ] Tool-result renderer slot — `ToolResultRendererRegistry`; a renderer for a disabled tool's name falls back to default rendering.
- [ ] Status-bar segment registry — the format string orders and selects by id; an unknown id renders nothing with a startup warning.
- [ ] Message-kind presentation slots — agent/thinking/tool-activity/system keyed renderers; core supplies the defaults.
- [ ] Theme sections — `plugin.role` namespacing, merged after core roles (no shadowing), sanity-checked.
- [ ] Attention sink — plugins may raise/clear tab or global attention; the `!` glyph stays the only color-independent signal.
- [ ] Modal-stack invariant — permission/confirmation modals are always topmost; plugin modals cannot preempt or mask them.

**E2 follow-on** (nothing else blocks on these; the big-interface items):

- [ ] `ChatSurface` exclusive slot — one plugin may replace the whole chat view; second registration warns and is ignored.
- [ ] Markdown engine exclusive slot + language-keyed grammar registration.

### G. MCP client — done

> § *MCP client — `src/server/mcp/`*. **Built, wired, and exercised end to end over a real socket plus a live real-server smoke test.**

`src/server/mcp/`: `naming` (mangling + validation), `env_interpolation` (connection-time `${ENV_VAR}`, fail-closed), `mcp_tool` (the `Tool` adapter + result conversion), `mcp_connection` (per-connection worker thread, deadlines, cancellation; stdio via a cpp-mcp **shim island**, http via a hand-rolled Streamable-HTTP client over `HttpClient`+`SseParser`), and `mcp_client_manager` (shared processes, discovery cache, liveness, respawn). Wired into `SessionRegistry`/`Session` (liveness at session start and `/agent`), `BuildChatRequest` (`DefinitionsForMcpServers`), `SpawnSubagentTool` (dead-server gating), and `main.cpp` (serve-stage construction).

The plan-vs-architecture "lazy connection" discrepancy was resolved in the architecture's favour: liveness and discovery fire at session start (and `/agent` switches) for allowed servers, never at boot; the `mcps.json` env/header `${ENV_VAR}` interpolation promised by `mcps_config.hpp` is resolved at connect time.

- [x] Tools from a stdio MCP server appear as `mcp__<server>__<tool>` and execute. *(E2E over a real socket: discovery at session start, `ToolExecutionEvent` round-trip, `TurnCompletedEvent` last.)*
- [x] Per-server permission policy governs them. *(E2E: `mcp_default_mode: ask` prompts over the wire; allow runs the tool, deny produces an error `ToolResultBlock`.)*
- [x] A dead/hanging MCP server degrades to a tool error, never a hung worker. *(Deadlines at every layer — open, discovery, call — against scripted hang/garbage/die-after-init fakes, all under the deadline harness; `SIGPIPE` from a dead child is disabled, exit 141 verified before the fix.)*
- [x] No MCP process spawns at boot. *(Manager construction spawns nothing; liveness checks at session start spawn per allowed server — the original "lazy until first tool call" wording was stale, see § *MCP client*.)*

#### What first contact found

1. **cpp-mcp and the harness cannot share a translation unit.** cpp-mcp vendors its own older nlohmann/json (ABI `v3_11_310`) while the tree links vcpkg's (`v3_12_0`); the static library's symbols simply do not bind. The `mcp_stdio_shim` island (one TU, cpp-mcp headers only, string/bool facade) is the resolution — recorded in ARCHITECTURE.md, since any future consumer of cpp-mcp must go through it.
2. **cpp-mcp's `sse_client` cannot speak to a modern http MCP server.** The gateway (official python SDK) answers Streamable HTTP; the probe caught this before any wrapper was built on the wrong transport. Hand-rolled client over `HttpClient`/`SseParser` instead — both already exposed as utilities.
3. **cpp-mcp's `initialize` blocks indefinitely if the child dies during startup**, and a request blocks up to a fixed internal 60 s. All deadlines are therefore ours: one worker thread per connection, callers wait in slices on their own slots, wedged workers are detached rather than joined.
4. **A dead child's stdin write killed the whole test process with SIGPIPE (exit 141).** The shim ignores SIGPIPE process-wide before spawning; children inherit SIG_IGN — the hosted-runtime trade, and a real server-hosting hazard that no unit test of the happy path would have found.
5. **`allowed_tools: ["*"]` swept MCP tools into every request.** `DefinitionsFor` now excludes `mcp__*` names; the model sees `DefinitionsForMcpServers(live_servers)` only. A de-scope test (assert the tools vanish after `/agent`) failed against the original code — the invariant test earning its keep.
6. **ToolRegistry went from read-only-after-startup to concurrently mutated.** Re-discovery replaces a server's tool set while session workers call `DefinitionsFor`/`Find`; a `shared_mutex` plus the never-destroy retired list (the executor holds the raw pointer across `execute()`) make replacement safe. Verified with a concurrent read-during-replace test.
7. **First registration of the invoker was moved-from**, leaving every McpTool after the first with a null invoker and a "not connected" error — caught by the env-marker round-trip test, not by review.
8. **Header names arrive verbatim from the wire.** Comparing `"Content-Type"` case-sensitively silently disabled SSE parsing (the body then failed to parse as JSON); the same class of bug `FindHeaderCI` already existed for.

**Test infrastructure.** A scripted stdio MCP server (built on cpp-mcp's own `mcp::server`) provides deterministic hang/slow/die-after-init/garbage modes; it carries an orphan watchdog because a fake server outliving its parent holds the parent's stdio pipes open and ctests waits for EOF — a run that has passed but will not end. The live smoke test (`mcp_live_test.cpp`, `MINIMAL_ENABLE_LIVE_MCP_TESTS=ON`) is a hard requirement where enabled — a missing `gateway` FAILS — and CI never sets the option.

---

## 6. Cross-cutting standards

Applies to every milestone. These are existing project practice, recorded so they survive contributor turnover.

- **Invariant tests must be proven load-bearing.** Delete the invariant, watch the test fail, restore. Already applied to the sort comparator, `MaxTokens`, the grant conjunction, the vacuous-truth guard, and every skill-registry diagnostic. A test that passes with the fix removed is not a test.
- **Verify by execution, not inspection.** Plugin linkage was confirmed with `nm -u`/`otool -L`; the ABI mismatch was demonstrated against a running server; the bash hangs were measured (8008 ms → 0 ms). Claims about behaviour need a run behind them.
- **The pinned toolchain does not synthesize implicit `operator==` for user-defined classes** (verified against AppleClang 21: even `struct E { bool b; };` has no `==`; an explicit `= default` works). Compare fields explicitly, as the existing tests do — do not add defaulted operators to production types just for test convenience.
- **Zero warnings.** `-Wall -Wextra` on all first-party targets; the tree is clean and must stay so.
- **ASan is a preset, not a memory.** `cmake --preset asan && cmake --build --preset asan && ctest
  --preset asan` runs the whole suite with first-party code AND dependencies instrumented
  (`cmake/triplets/arm64-osx-asan.cmake`; the triplet name keys vcpkg's cache, so the instrumented
  dependency tree installs beside the normal one and the preset's first run is the slow one). Leak
  detection is off by decision: plugin handles are held for process lifetime on purpose
  (ARCHITECTURE.md, "Server plugins"), and uninstrumented third-party allocators made LSan noise
  exceed signal. Sanitizer findings are test bugs until proven otherwise — the preset's first run
  caught a use-after-scope in a test that destroyed its release flag before the worker polling it.
- **TSan is a preset, not an accident.** `cmake --preset tsan && cmake --build --preset tsan && ctest
  --preset tsan` is the same overlay-triplet shape (`cmake/triplets/arm64-osx-tsan.cmake`), for the
  same reason with the opposite sign: an uninstrumented dependency hides races in happens-before
  terms, so the false-negative risk argues for instrumenting everything, not just first-party code.
  Unlike ASan there is no option to turn off — a race report fails ctest by default. The suite is
  otherwise the same 678 tests, no TSAN_OPTIONS tuning. The first run caught exactly one finding: the
  cancel-during-backoff rendezvous spun on `FakeLlmProvider::call_count()` while the session worker
  recorded the request — a data race whose practical harm was zero and whose existence contradicted
  the test's own "deterministic rendezvous" comment. The fix is the mutex `ScriptedProviderScript`
  already wore; `requests()` deliberately stays a bare reference read after join. The races the
  review pass found (#4, #7, #9, the client's command counter) were fixed without TSan available to
  verify them, so this run was their verification, and it is what keeps them caught.
- **The presets pin the generator** (`Ninja`); ad-hoc `-G Ninja` configurations were how every
  build here has actually been made, and a preset that cannot configure from a clean checkout is a
  gap, not a default.
- **`ARCHITECTURE.md` is updated in the same change that diverges from it.** The bash rework's core finding was a restriction that outlived its rationale because the doc was updated without re-deriving the rule.
- **Test tiers** (§ *Testing*): pure logic → agent loop against `FakeLlmProvider` → tools against a temp `project_root` → end-to-end over a real socket.

---

## 7. Known gaps carried forward

Not scheduled, recorded so they are decisions rather than oversights.

- **Server-wide fd hygiene** — no `CLOEXEC` discipline on gRPC/SQLite/log fds; every bash command inherits them. The bash pipes are handled; the rest is not. MCP stdio children are spawned through cpp-mcp and have the same gap.
- **`mcps.json` `args[]` entries containing spaces cannot reach the child** — cpp-mcp splits the stdio command line on whitespace; the shim inherits the limitation. Recorded in ARCHITECTURE.md.
- ~~**`tree-sitter-bash` for command disclosure**~~ — **landed** (§12.40). The original deferral reason recorded here was wrong and had to be corrected first: it said the hand-rolled scanner's "failure mode is safe (fewer grants offered)". It was not safe: shell reserved words were recorded as programs, which consumed the command slot and demoted the real program to an argument, so `ls; if true; then rm -rf /; fi` disclosed only `{ls}` and reported itself *complete* — more authorised than the operator saw, not less (§12.1). That hole was closed first (§12.1 again), and the grammar now replaces the walker: `command_name` nodes at every command position, ERROR/MISSING nodes mapped onto `complete = false` — the error signal being the point, since it is the one thing a hand-rolled walker cannot produce. The `FetchContent` grammar-vendoring path this built (tree-sitter-bash has no vcpkg port) is the same plumbing track E needs for highlighting grammars. Every §12.1 scanner test passes unchanged; the grammar version adds fail-closed-on-error, heredoc-bodies-are-data, and no-junk-words guarantees the walker could not make.
- ~~**Unknown `allowed_skills` entries are silently dropped**~~ — still true of resolution (correct for lookup), but now *reported* at startup by F (`naming_model` check).
- **`themes/*.json` and `keybindings.json` are specified but unread** — nothing outside a comment in `config_paths.hpp` refers to them, so they are deliberately not seeded either. They arrive with the TUI (track E), which is the first thing that would read one.
- **The compaction trigger now follows the model's window** (`{"percent": 80}` by default), so the
  "0 means never, and nothing says so" gap is closed: a window-relative strategy against a model with
  no declared `context_window_tokens` logs once and disables itself rather than staying silent.
- **`compaction_keep_recent_tokens` is parsed but unused** — the compaction cut is half the working history by message count (snapped to a tool-call cycle); selecting it by tokens needs per-message counts that are not tracked in memory (§ *Context compaction*).
- **Backend `usage: 0`** from some OpenAI-compatible proxies — no fallback estimate designed (§ *Deferred*). Note the production compactor's `context_tokens_before` falls back to a bytes/4 estimate in exactly this case; it is an admission of roughness, not a precision claim.
- **No HTTP connection reuse** — a fresh CURL handle per provider call is the deliberate shape of the statelessness contract ("no mutable state across calls"), but a remote backend pays a TCP/TLS handshake per tool cycle. A per-call handle *pool* would recover most of it without shared state; deferred until a remote-backend deployment actually feels it.
- **Terminate latency can hit `stream_idle_timeout`** — a backend that stalls silently is observed as cancelled only at event boundaries and the 25 ms backoff poll, so `Terminate`'s join can block for up to the 60 s idle timeout before the sink abort propagates. Bounded, and local llama.cpp does not stall this way; recorded rather than tuned.
- **The shared-token auth plugin is deferred** — the `AuthProvider` registry, the pass-through default, and the wire guard are built (§11); the example plugin that registers one is not.
- ~~**`tree-sitter` grammars**~~ — the dead vcpkg entry is real now: `tree-sitter` core moved to base dependencies and `tree-sitter-bash` is vendored via `FetchContent` (the command scanner, §12.40). The rest of the curated set arrives with track E, which now only needs to declare targets on plumbing that already exists.
- Everything under § *Deferred / open items*: Anthropic provider, Windows, `llamacpp-kv`, parallel tool dispatch, session detach, tab-bar overflow.

---

## 8. Post-merge review of A + B + H

A + B + H were built on three branches off one base and merged in sequence. Each was internally
green; §1's "risk this creates" predicted that the mismatches would live *between* them, and the
merges bore that out. This is what a deliberate read of all three against `ARCHITECTURE.md` found
afterwards, and what was done about it.

**The largest finding was not a bug in any of them.** `bundled/` held exactly one file. The
`agents/{plan,build,subagent}.md` the architecture calls bundled did not exist anywhere in the tree,
and neither did `settings.json` or `permissions.json` — so `LoadSettingsConfig` threw on a cold
config dir and `main.cpp` turned that into exit 1. Fixing that filename revealed the next. Milestone
H closed the delivery gap for one prompt; it was still open for everything the harness needs to
start.

### Correctness

| What | Why it mattered |
|---|---|
| `PendingRequests` had one permanent cancel latch serving both `Session::Cancel` (ends a turn) and `Session::Terminate` (ends a session) | One `CancelTurn` deny-locked the session: every later permission prompt returned "no answer" instantly, which `WirePermissionChecker` maps to `Deny`. The session kept reporting itself alive and accepting turns while refusing every gated tool, silently. Split into `CancelAll` (generation bump) and `Shutdown` (the latch). |
| The retry loop never backed off | Both `ARCHITECTURE.md`'s pseudocode and `provider.hpp:82` put backoff in the loop; a 429 was answered with four more requests inside a few milliseconds. Exponential, **equal** jitter (full jitter can draw zero, which both leaves a real 429 unbackoffed and makes the wait unassertable), polling cancellation while it sleeps. |
| `original_token_count` measured two different spans | Ground truth was the whole request's `input_tokens`; the fallback estimated only the compacted range. The log printed either as "what was replaced". Renamed `context_tokens_before`; both branches now measure the whole context. |
| A `MaxTokens`-truncated summary was accepted | Cut off mid-sentence, and it permanently replaces half the conversation with a fragment whose only other copy is in SQLite. Refused, same rule the loop already applies to `MaxTokens`. |
| The compaction call dropped the preset's sampling settings | Chasing it found a wider gap: `ChatRequest::max_tokens` had **no producer anywhere**, and the OpenAI serializer erases any `max_tokens` set through `parameters` because it treats the field as structural. An output cap was unreachable on every request in the system. Added as a typed `providers.json` field. |
| `OnTurnCompleted` never fired for a failed turn | Both catch blocks returned before it, so an observer holding per-turn state leaked it once per failure — the same defect the tool lifecycle event avoids by emitting finish even on error. |
| `before_seq <= 0` meant "no cursor", but `seq` starts at 0 | A client paging back to the first message and passing its seq got the newest page again. Now proto3 `optional`. Changing it silently rewrites every call site (`0` converts to an engaged optional), so `GetMessages(id, 0, n)` is a deleted overload. |

### Cold start

`bundled/` gained `agents/{plan,build,subagent}.md`, `settings.json`, `permissions.json`,
`providers.json` (`[]`) and `providers.example.json`. Seeding moved from first server run to
`cmake --install` (`cmake/SeedConfig.cmake`), which **deletes** machinery rather than adding it:
`BundledPromptDir`, `SeedBundledPrompts`, `MINIMAL_PROMPT_INSTALL_DIR`, the `install(DIRECTORY)`
rule, the POST_BUILD staging step and the `main.cpp` stage-1 call are all gone. Config files have no
runtime lookup path at all now. Plugins keep the executable-relative probe, because they are code
the user does not edit.

`providers.json` seeds empty on purpose — a `base_url` that is right on one machine is wrong on the
next, and a plausible default pointing at nothing is worse than an obviously empty one.
The compaction trigger seeds as `{"percent": 80}` (§10): it is live the moment the user's provider
entry declares `context_window_tokens` (both `providers.example.json` entries do), and until then
the loop warns once per session that auto-compaction is off rather than going silently inert.

### Efficiency

`BuildChatRequest` deep-copied the whole working history per provider call **and per retry**, and
`openai_provider.cpp` copied the request again to normalize it — two full copies of every message and
string per call, against a stated 512k–1M token goal. `ChatRequest::messages` is now a `std::span`
with an explicit lifetime contract. Measured on a 2 MB history: **113.5 µs/call → 0.06 µs/call**
(`tests/chat_request_bench.cpp`, tagged `[.bench]`).

Also: `GetMessages` used `GetSession` as an existence check and discarded the `COUNT(*)` it computed,
putting an O(n) count in front of the keyset seek whose whole purpose is that a page costs the same
on a long transcript; `ListSessions` had no `LIMIT` at all; `EventQueue` was unbounded, and the
initial-attach guard bounds *time*, not volume.

### The client's command line

Three defects in twenty lines of `argv` loop, none of them caught by anything, because the parsing had no tests at all — it lived inside `main()` where nothing could reach it. They compounded into one confusing symptom:

```
minimal -p --server unix:///tmp/x.sock "read the file"
```

sent the model the prompt `"--server"`, dropped the target, dropped the real prompt, and reported that it could not reach the **default** socket. `-p` consumed the next token unconditionally; unrecognised arguments were silently discarded, which is what kept the first two quiet; and `--server` demanded a full gRPC target while `minimal-server --listen` takes a bare path.

Parsing moved to `client/cli_args.{hpp,cpp}` so it can be tested, with two rules: a value-taking flag never accepts a flag-shaped value (`--flag=value` for values that start with `-`), and anything unrecognised is an error. `--server` now accepts what `--listen` accepts. 11 cases in `tests/client_args_test.cpp`; reverting each defect fails them.

One aside worth keeping: a Catch2 `TEST_CASE` name that *starts* with `-` breaks `catch_discover_tests`, which passes the name to the binary as argv where Catch2 reads it as a flag. The suite passed run directly and failed under `ctest` — the same class of mistake the tests were written about.

### Practice this confirmed

- **The from-scratch build and the sanitizer both earned their keep**, and both required reading the
  output rather than the exit code. ASan's one report back then was inside gRPC's own `send_goaway`
  frame and reproduced with every change stashed — an uninstrumented dependency, not a defect.
  Closed since: §11's `asan` preset instruments the dependencies too (overlay triplet), and the
  suite runs clean under it. Instrumenting only first-party code is not an option — that same
  mismatch is what produced the report.
- **A ceiling is not an invariant test.** The first "the retry waited" assertion was `elapsed < 2s`,
  which passes with the backoff deleted. Under full jitter no single run *can* assert a floor; the
  fix was to change the algorithm to equal jitter so the floor exists, not to loosen the test.
- **The compiler cannot catch a sentinel change.** Every `before_seq` call site kept compiling and
  quietly changed meaning; the tests caught all eleven. A deleted overload is what stops the twelfth
  from being written.

---

## 9. Checkbox audit

Every box in this document checked against the tree rather than against memory, since a plan that
records intent as achievement is worse than one that records nothing.

**Milestones A, B, D, H and I are green.** 46 boxes ticked, 1 partial with the reason on the line, and
17 open — all of which belong to C, E, F and G, which are scheduled and unbuilt rather than
regressed.

The audit found three real gaps, all in Milestone A's test list, all in shipped behaviour:

1. **`CancelTurn` had no over-the-wire test.** Cancellation was covered in-process only, so the
   reader thread's `kCancel` dispatch branch — the one a real client actually uses — had never
   executed under test. §8 then changed exactly that path. Closed: a permission prompt is the
   rendezvous, so "mid-turn" is deterministic rather than a sleep, and the test also drives a
   *second* turn to prove the session still prompts and still runs a tool. Restoring the old
   permanent latch fails it, independently of the in-process test.
2. **Payload ordering was never asserted.** The round-trip test checked that events arrive and carry
   the right `session_id`, not the order — which is the part a client's state machine depends on.
   Closed: `TurnCompletedEvent` last and exactly once, tool start before finish, both between the
   two `message_stop`s.
3. **The child-attach obligation was unenforceable** — nothing emitted `SubagentStartedEvent`, so
   there was no child to attach to. Closed with D: the event is emitted, an `Attach` client event
   makes the attach expressible (§5 D, finding 1), and `subagent_e2e_test.cpp` proves the
   obligation over a real socket for both blocking and non-blocking spawns.

One partial stays partial by choice: the single-writer invariant is proven at the queue
(`event_queue_test.cpp`) but not at the stream. The stream-level version would need two concurrent
`Write()`s on one `ServerReaderWriter`, which is now refused outright — a second client gets
`ALREADY_EXISTS` (§8) — so the reachable half is guarded and the unreachable half would be a test of
gRPC rather than of this code.

**Writing the `CancelTurn` test found a bug in the test, not the code**, which is worth recording:
the first draft scripted a response for turn 1 that turn 1 never consumed, because the cancel breaks
the loop before it asks again. Turn 2 silently ate it and reported "no prompt". A scripted fake
provider makes the number of provider calls per turn an assertion whether or not you meant it to be.

---

## 10. Compaction trigger: from token count to strategy choice

Post-milestone change, landed against the A+B+H tree. The trigger was a single server-wide number
(`memory.context_compaction_trigger_tokens`) that had to anticipate every model's window at config
time — 120000 is right for one backend and wrong for the next three. It is now a one-of strategy
under `memory.context_compaction_trigger`: `{"percent": 1..100}` of the provider's
`context_window_tokens`, `{"tokens": N}` (the old behaviour), or `{"buffer_tokens": N}` (fire when
the context comes within N of the window edge). Absent means never; the loader rejects anything but
exactly one known key, because a silently-ignored typo here is compaction going inert — the exact
failure the bundled-assets tests exist to catch.

**What it needed.** The window was parsed from `providers.json` and consumed by nothing. It now
rides on the resolved `AgentPreset`, not on `LoopConfig`, because `/agent` can switch provider
entries and the trigger must track the window in use. `resolve_compaction_trigger` is a pure
function of (strategy, window), returning 0 for "never" — and the agent loop warns **once per
session** when a window-relative strategy meets an entry that declared no window, since that is a
live state (the seeded `providers.json` is empty), not a corner case. This rewrites §8's cold-start
rationale: the seed is now `{"percent": 80}`, live the moment an entry declares its window.

**Compatibility.** The legacy flat key still parses as the fixed strategy, so an existing
`settings.json` does not silently stop compacting; the new object wins when both are present.

**Deliberately not touched.** The cut is still half the working history by message count —
`compaction_keep_recent_tokens` remains the §7 gap. This changed *when* compaction fires, not what
it keeps.

---

## 11. Post-review remediation

A deliberate read of the whole tree against `ARCHITECTURE.md` after the E-track began — the same
exercise as §8, but aimed at correctness and efficiency rather than at merge seams. Each item below
landed with the invariant test that proves it load-bearing (delete the fix, watch the test fail),
except where the note says otherwise. Status lines are records, not claims: everything here is in
the tree and green.

### The one that would have shipped broken

1. **`Session::Terminate` deadlocked against its own queue.** It joined the worker before closing
   the outbound `EventQueue`, but `Push` blocks at the 65,536 cap and is woken only by `Close` or
   `Pop` — reachable whenever a client stalls or dies mid-stream and the queue fills: the writer
   stops draining, the worker parks in `Push`, the join waits for a close scheduled after the join,
   and `grpc::Server::Shutdown` (which waits for in-flight RPCs) bricks the server. The headless
   client never lived long enough to reach this; the TUI is exactly the client that produces slow
   writers. Close-before-join; the drain-before-close rule in `Pop` keeps the backlog semantics.
   Deadline-bounded test deadlocks against the old order.

### Correctness

2. **A blocking subagent could wait forever.** The completion hook fired only from `RunOneTurn`, so
   a terminate landing before the seeded turn ran (initial-attach reaper, terminate racing
   `SubmitTurn`) left the parent's `future` permanently unset. `Terminate` now fires the hook with a
   synthesized `Cancelled` result — the same every-exit-path rule the tool-finish event follows.
3. **One odd exception killed the server.** `RunOneTurn` caught `LlmError` and `std::exception`; the
   seq read, the observer notify, and the completion hook ran OUTSIDE the try, and anything
   non-standard escaped to `std::terminate` through `WorkerMain`. Each tail stage is guarded
   separately, so a failing stage cannot skip the later ones.
4. **CancelTurn lost races in both clearing directions.** A flag cleared before the pop swallowed a
   cancel that landed between turns; cleared after the pop, it swallowed one landing in the
   pop-to-clear window. Cancel is a generation now: a turn is cancelled iff the generation moved
   since its pop. A cancel landing inside retry backoff also reported `Unknown` instead of
   `Cancelled` — the headless exit code derives from this value, and exit 3 ("model error") for a
   user cancel was simply the wrong answer.
5. **The subagent ceiling looked up the wrong key.** The base policy resolves per-tool overrides
   through the permission key (the rule §F installed); the ceiling — the parent's policy — matched
   on the surface name, so an ALIASED tool missed its parent's per-tool `deny` and the child could
   run exactly what the operator had denied, under its alias. Second enforcement point of the same
   rule; the invariant test fails against the surface-name lookup.
6. **`spawn_subagent` was invisible to the collision check.** Core registers it at serve stage,
   after `ResolveRegistrationNames` ran, so a plugin claiming the name silently shadowed (or was
   shadowed by) it via `insert_or_assign`. Reserved core names feed the check; both registries now
   keep the FIRST registration of a duplicate, before and after alias application, identically.
7. **`atomic_write` clobbered file modes.** The rename installs a new inode, and the unconditional
   0644 stripped executable bits and widened 0600 private files on their next edit. Existing targets
   keep their mode; new files get 0644; the rename is followed by a best-effort parent-directory
   fsync; the temp fd is `CLOEXEC` (a concurrently forked bash command must not inherit the file's
   new content).
8. **Keyset paging did not seek.** `?2 IS NULL OR seq < ?2` made SQLite's planner drop the range
   constraint: equality on `session_id` plus a plain index walk from `MAX(seq)` — O(n) per page,
   O(n²) to page a transcript, against the doc's "costs the same for a long transcript" claim.
   `COALESCE(?2, INT64_MAX)` folds the no-cursor case into one comparison; `EXPLAIN QUERY PLAN` is
   asserted against the real schema.
9. **The command pool replaced detached threads.** One detached, never-joined thread per read-only
   command meant unbounded threads under a flood, a shutdown window where a task held shared_ptrs
   into registries being destroyed, and a blocking `queue_.Push` from a non-worker thread. A
   bounded, joined pool answers saturated posts with an error — a `request_id` a client waits on is
   never answered with silence. `/providers` reads a preset snapshot taken under the session lock;
   the reference read raced `/agent`'s rewrite of the same strings (a TSan-class data race, UB).

### Efficiency (against the 512k–1M token goal)

10. **`ProviderInstanceCache` now exists.** The architecture specified it; the tree constructed a
    fresh provider per session. Instances are shared per `providers.json` entry (the API key is
    deliberately not in the key; the generation counter is always zero until a reload ever exists),
    sessions hold a `shared_ptr`, and `/agent` away-and-back reuses the instance. The fixtures that
    handed each session a fresh stateful fake were fighting the statelessness contract sharing rests
    on, and were moved to the shared-script pattern that was already built for multi-session tests.
11. **Compaction stopped copying the history.** The `Compactor` takes a `std::span` (the compactor
    reads; nothing mutates history during the call); the kept tail is moved, not copied; the
    boundary snap maintains use/result counts incrementally instead of rescanning per shrink
    (O(cut²) worst case gone); and the retry loop builds the `ChatRequest` once per tool round
    instead of once per attempt.
12. **The request body is streamed into a string.** `BuildRequestBody` used to materialize a
    nlohmann DOM of the whole history and dump it — two copies of every message per provider call.
    Message text is escaped straight into the buffer; `parameters` keys naming structural fields are
    DROPPED rather than emitted after them (emit-then-overwrite would have left precedence to the
    receiving parser's duplicate-key handling — the guarantee now rests on nothing but the output);
    tool arguments remain a JSON-encoded string per the OpenAI contract, enforced by test.
13. **The last unbounded buffers got bounds.** The session work queue (32 items, refusals answered
    on the wire — an unbounded deque of transcript-sized turns behind a minutes-long worker) and the
    bash fd-9 state payload (1 MiB, overflow discarded WHOLESALE — adopting a truncated prefix would
    silently apply partial environment). `ChildCount` counts only non-terminated children, so
    `max_concurrent_subagents` means concurrent.

### Wired as specified, verified on the wire

14. **Auth.** `AuthProvider` + exclusive registry + `EnsureAuthenticated` at the head of every
    service method. The pass-through default is the local-only topology, stated as policy rather
    than omission. Placement note recorded in the code: the sync gRPC API does not invoke
    experimental interceptors for synchronous handlers, so the guard is explicit at each method head
    — and the E2E case exists precisely because an in-process-only guard proves nothing. The
    shared-token example plugin is a §7 gap.
15. **Client hardening.** `RunTurn` auto-denies a mid-turn permission request instead of parking the
    server worker on an answer nobody can send (headless forces `AllowOnly`, so reaching there means
    a non-conforming run mode — degrade loudly, never hang); command request ids count instead of
    colliding on `"cmd-1"`; a non-blocking subagent finishing during parent teardown logs the drop
    instead of claiming the result was queued; plugin discovery warns when one basename exists
    bundled AND user-installed (shared enablement entry, guaranteed collision, previously silent).

### Not done, deliberately

- **HTTP connection reuse, `stream_idle_timeout`-bounded terminate latency, the shared-token
  plugin, tree-sitter grammars** — all recorded in §7 with the reason each is a decision.
- **A `catch(...)`-shaped seam test for the compactor** —    the compactor already returns failure as
  a value; only the loop's bookkeeping needed the guard, and that is tested.

## 12. Third-pass review

A third deliberate read of the whole tree, after §8 (merge seams) and §11 (correctness and
efficiency). This one was aimed at what those two did not look at: the permission scanner's actual
parse, extension points that are declared but never fire, config the server promises to honour and
doesn't, and races no sanitizer in the tree can see.

The through-line is worth stating, because it predicts where the next one will find things: **almost
every remaining defect is a comment describing a mechanism that isn't there.** A guard that never
receives its trigger, a hook nothing calls, a settings block nothing reads, a containment claim a
bounded queue contradicts, a totality guarantee `if constexpr` doesn't give — and the one below, a
conjunction the scanner does not actually compute. That gap is worse than an honest absence, because
every one of them reads as done.

### Authorisation

1. **Shell keywords ate the command slot, and a held grant then ran an ungranted program.** The
   scanner recorded the first word after a separator as the program; reserved words are not
   separators, so `then` was recorded *as* the program of its list and the real one was demoted to
   an argument. `ls; if true; then rm -rf /; fi` disclosed `{ls, if, then, fi}` — `if`/`then`/`fi`
   resolve to no binary and are dropped — leaving `{bash:/bin/ls}` with `complete = true`. A session
   already holding a grant on `ls` satisfied the conjunction and ran it with no prompt.

   **This was not the accepted lower bound.** `complete = false` is scoped to *syntactic* blindness
   — `$X args`, `eval "$CMD"` — and `find | xargs rm` is the recorded semantic limit. Neither
   applies: `rm` is a literal in a command position, the operator granted `ls` and nothing else, and
   the harness told them the disclosure was complete. The difference from the `xargs` case is what
   the operator was *shown*: granting xargs is a decision about xargs, taken knowingly. Nobody
   decides anything about a program that was never disclosed.

   The whole reserved set is skipped, not just the subset followed by a command list: skipping `for`
   or `[[` records one extra word (`f`, `-f`) that resolves to no binary and is dropped at
   disclosure, which is the same over-approximation `ResolveOnPath` already accepts and errs toward
   more keys in the conjunction, never fewer. `ARCHITECTURE.md` gains the floor this owes rule 1 —
   *a literal program in a command position is either named or `complete` is false* — since the
   lower-bound wording alone did not distinguish the two cases and that is what let this look
   acceptable.

   Fixed alongside, same file: `$'...'` was consumed as a bare `$x` token, so the closing quote
   opened a fresh quote region and swallowed the rest of the line — dropping every later pipeline
   stage from the disclosure. It is a quoted literal, so `$'rm'` now names `rm`.

   Invariant tests at both levels: the disclosure must name `rm`, and at the executor seam a grant
   on `ls` must not silence a second command hiding another program behind `then`. Both fail against
   the old scanner (12 assertions), including the one that asserts the prompt count — i.e. the
   unprompted execution itself.

   §7's deferral of `tree-sitter-bash` is corrected in place: it was recorded as safe-by-failure-mode,
   which is what this disproves. Still deferred now that the hole is closed, but the argument for it
   is no longer only hit rate — a hand-rolled walker cannot tell "I parsed this" from "I mis-parsed
   this", which is exactly how this produced `complete = true`, and a grammar can.

### Correctness

2. **A throwing tool poisoned the session for good.** `ToolExecutor::Run` called plugin code --
   `category_for`, `Disclose`, `execute`, all arbitrary native code by design -- with no guard, and
   the agent loop did not wrap it either. The loop persists the assistant message carrying the
   `tool_use` BEFORE running any tool and appends results AFTER all of them, so a throw left an
   orphaned `tool_use` in history: the exact state `snap_compaction_boundary` exists to prevent, and
   a hard 400 on both wire formats. `Session::RunOneTurn` caught the exception, the session
   survived, and then failed every subsequent turn forever.

   Three live routes, none exotic, all reading input the MODEL wrote: `fs_tools` used `value()` and
   `get<int>()` on `offset`/`limit`/`view_range`/`command` without checking type, so
   `{"offset": "1"}` was a `type_error` out of the tool; and `SessionDatabase::AppendMessage` called
   `dump()` with the default strict UTF-8 handler, which throws on invalid UTF-8 -- nothing between
   a child process's raw stdout and that line validates encoding, so one `cat` of a binary file did
   it. All three fixed: the executor answers every call on every path (`Run` is now a `try` around
   `RunGuarded`, so a later edit cannot slip a return past the catch), `fs_tools` type-checks the way
   its own `Insert` already did, and persisted content substitutes U+FFFD. `Run` never throwing is
   stated on the declaration, since it is a contract the loop depends on rather than an
   implementation detail.

3. **The server never exited after an `OpenSession` exception.** The catch path restored the attach
   count and stopped; the success path fires `on_last_client_detached_` on the zero transition, which
   `main()` wires to `RequestShutdown()`. So a client whose stream unwound decremented to zero
   without ever firing it, and the no-daemon design -- "refcounted by attached clients, gone when the
   last one leaves" -- silently did not hold: the process kept the socket forever and the next
   auto-spawning client would find a zombie it could not replace. The catch block existed precisely
   so the refcount could not leak, and its comment says so; it just did not carry the consequence of
   reaching zero. Both exits call one `ReleaseAttachment` now, because splitting that decision across
   two sites is what let them drift.

4. **`AgentPresetRegistry::Resolve` raced on its own cache.** A `mutable unordered_map` populated
   lazily, with no mutex in the class, called from `SessionRegistry::Create` on a gRPC handler thread
   (before it takes its own lock) and from `SubagentManager::Spawn` on a session worker. Two
   concurrent `CreateSession`s, or a `CreateSession` racing a `spawn_subagent`, is a concurrent
   `emplace` into one map -- and it returned a `const AgentPreset&` into a container a rehash could
   move. The class comment's "config is read once at startup, there is no hot-reload" is true of the
   FILES; the cache is built at runtime from whichever thread asks first. Now mutex-guarded and
   returning by value, with `ResolveUncached` run OUTSIDE the lock so disk I/O does not serialize
   every concurrent session creation -- two threads racing an unseen preset may both parse it, which
   is harmless because the result is a pure function of files that do not change at runtime.

5. **A client-supplied page limit was trusted.** `limit` is a wire `int32` with a floor and no
   ceiling; `SessionDatabase` binds `limit + 1` for the has-more probe, so `INT32_MAX` is signed
   overflow -- and in practice binds a NEGATIVE limit, which SQLite reads as no limit at all. One
   `GetMessages{limit: 2147483647}` then loaded an entire transcript, every row decoded and copied
   into one proto response: the O(n) blowup the keyset paging (§11.8) exists to prevent, reachable
   from any client since authentication is pass-through by default. `ListSessions` had the same gap,
   and its proto comment already documented a bound it did not have. Both clamp to 1000 now --
   clamped rather than refused, because asking for more than we serve is not an error and `has_more`
   already tells the client to page.

### Mechanisms that were described but not wired

6. **A hung observer stalled the gRPC reader thread.** `Dispatch` posted with `EventQueue::Push`,
   which BLOCKS at the cap -- while `NotifySessionCreated` runs inside `Session`'s constructor,
   reached from `SessionRegistry::CreateResolved` on the gRPC HANDLER thread, and
   `NotifySessionEnded` runs from `Terminate` on the reaper and in `~SessionRegistry`. So the
   containment property both the header and `ARCHITECTURE.md` state outright -- "a hung observer
   stalls further notifications rather than leaking one pool thread; contained (sessions keep
   running; only notifications stop)" -- was the opposite of what happened: a wedged plugin observer
   filled the queue and then blocked session creation and teardown, including the one thread the
   three-thread topology forbids from blocking. `TryPush` now, with dropped notifications counted and
   logged, since a plugin that misses an `OnSessionEnded` leaks its per-session state and the leak
   would otherwise be the only evidence. The queue bound also became explicit (1024): inheriting
   `EventQueue`'s 65536 default silently reserved a stream-event-sized backlog for notification
   tasks that each carry a `SessionInfo` copy.

7. **`SessionObserver` was half a contract.** `ARCHITECTURE.md` documents `OnToolExecuted` and
   `TurnSummary::tool_names_invoked`; track I is checked off. `NotifyToolExecuted`'s only caller in
   the tree was its own unit test, and `RunOneTurn` built `TurnSummary` without ever setting
   `tool_names_invoked`. A plugin author implementing the documented interface got silence and an
   empty vector -- worse than the hook not existing, because it reads as working. The loop now
   records each tool name (before the call, so a failed tool still appears -- what ran is the
   question, not what succeeded) and times it, and the session's lifecycle sink fires
   `NotifyToolExecuted` on the FINISH event: a started/finished pair is the wire's concern, an
   observer wants one record of a completed call.

8. **`/compact` was queued behind the running turn instead of refused.** `Dispatch` takes a `busy`
   flag for exactly this, and both production callers passed `false` unconditionally -- so only the
   unit tests ever exercised it, and the behaviour was verbatim what `command_dispatcher.hpp`
   forbids: "queueing would make /compact silently run minutes later against a context that had
   moved on." The check now runs on the reader, where the answer can be returned without queueing
   the thing being refused.

   The rule is an **LLMCommand**, not any non-read-only command, and the old `!ReadOnly()` predicate
   conflated two real behaviours: a mutating `HarnessCommand` (`/agent`, `/model`) is MEANT to queue
   and land at the next turn boundary -- that is what the shared work queue is for -- while an
   LLMCommand needs the provider and history to itself. The type exists to carry that distinction
   ("a type cannot be forgotten the way a flag can"), so the type is what is asked, in both places.
   An unknown command is also answered immediately now, rather than queued behind a long turn purely
   to be told it was unknown.

9. **`AgentLoop::ClearHistory` touched `injected_` without its mutex.** `/clear` runs on this
   session's worker; `QueueInjectedMessage` runs on a NON-BLOCKING SUBAGENT's worker from its
   completion hook. A background child finishing while the user types `/clear` is a data race on a
   `vector<Message>` -- the kind that corrupts rather than crashes. Now locked, and the occupancy
   estimate is reset with the history it described.

### Errors that were not errors

10. **`sqlite3_step` failures were indistinguishable from end-of-rows.** All three row loops were
    `while (step(stmt) == SQLITE_ROW)` with the terminating code discarded, so `SQLITE_BUSY`,
    `SQLITE_IOERR` and `SQLITE_CORRUPT` all exited the loop exactly as `SQLITE_DONE` did and every
    caller read the short result as a complete one. `LoadMessages` is the resume path and the worst
    of them: a partial read becomes the loop's working history while `SqliteHistoryStore` keeps
    appending at `MAX(seq) + 1`, so the session runs on a transcript with a hole in it and nothing
    ever reports it. `GetMessagesPage` returned `has_more = false` on failure, rendering a truncated
    page as the top of the conversation. The file's own comment says an unparseable row is "surfaced
    at read time rather than smuggled to the client as garbage" -- the JSON decode was strict, the
    read beneath it was not, so the strict half was guarding the lax half's output.

    Two neighbours in the same file, same shape: `SessionExists` returned false for a LOCKED
    database exactly as for a missing row, so the RPC layer told the client its session had been
    deleted; and the append path reported any `UPDATE` failure as "cannot append to unknown session",
    sending the next reader after a wiring bug when the disk was full.

    The proof is a deliberately corrupted database: without the check `LoadMessages` returns a
    plausible short transcript, with it the failure surfaces.

11. **Two registration namespaces had no guard.**

    *Provider kinds had no collision rule.* `ProviderFactoryRegistry::Register` returned false on a
    duplicate and its own header said the caller "wants to report which two plugins collided" -- the
    loader ignored the return value, so a second plugin registering `openai` was dropped in silence
    and which backend a session actually got depended on plugin load order. Tools and commands both
    feed `ResolveRegistrationNames` and collide fatally; this was the one namespace exempt from the
    rule that exists to prevent exactly this. The registry now carries the same attribution window
    the other two use, records the loser, and the sanity check reports it as fatal -- fatal rather
    than aliasable, because there is no alias mechanism for a kind.

    *A throwing registration callback killed the server.* `LoadOne` invoked five third-party
    callbacks bare, so a throw propagated out of `LoadAll` into `main()`, which has no handler: the
    process died without naming the plugin, and the attribution window stayed open, filing every
    later plugin's registrations under the one that had already failed. Every other plugin entry
    point in the tree was already wrapped -- observers, commands, and (after §12.2) tool execution.
    A throw is now the `RegistrationThrew` load outcome, reported like any other. Tested against a
    real `.so` that really throws, because a mock cannot demonstrate a process dying.

12. **The streaming provider had three ways to turn an error into a silent empty turn.**

    *A non-2xx with an empty body read as success.* `MaybeFireHeadersReady` is reachable only from
    `WriteCallback` -- the first body byte -- so a 4xx/5xx with no body never fired it at all:
    `is_error_status` stayed false, `response_status` stayed 0, and `StreamComplete` fell through to
    `Finalize()` and returned an EMPTY ASSISTANT MESSAGE for an error the server had already
    reported. Fired after `curl_easy_perform` too, idempotently.

    *Non-`OpenAiParseError` exceptions were caught and dropped.* `FeedFrame` reaches raw nlohmann
    accessors in several places -- the content delta, the tool-call fields, `ParseUsage` -- and only
    `json::parse` was wrapped. A proxy sending `usage: null` threw a `type_error` that the
    stash-and-rethrow lambda caught and then discarded, because its only handler was for
    `OpenAiParseError`. The NON-streaming path has caught `json::exception` all along; the streaming
    path had drifted. Now classified, with `LlmError` rethrown unchanged so it keeps its category.

    *Blocks that opened never closed on a truncated stream.* `ContentBlockStop` was emitted only
    inside the `finish_reason` branch, so an idle timeout, a dropped connection or a cancelled sink
    left the client holding a block it had been told about and would never be told ended -- the same
    rule the tool lifecycle already states for the running-tools panel. `CloseOpenBlocks` runs on
    both exits, idempotently. And a tool call whose id/name never arrived is DROPPED rather than
    finalized with empty fields, which used to reach the executor as `unknown tool: ` -- an error
    attributed to a call the model believes it made. Nothing is orphaned by dropping it: there is no
    tool_use in history to answer, and the loop already handles a ToolUse stop reason with no blocks.

### Config the server promised and did not keep

13. **`logging.*` was parsed, seeded, documented, and never applied.** `LoggingSettings` is fully
    parsed, seeded into `bundled/settings.json`, and `ARCHITECTURE.md` describes it as backing a
    spdlog rotating file sink under the data dir. Nothing read it: `DefaultLogger()` returned
    spdlog's console default, so there was no file sink, `level` was ignored, and every
    `logger_.debug()` in the tree was dead output.

    That is worse than a missing feature, because a large share of this codebase's error handling IS
    the log -- dropped `CommandResult`s, a dropped subagent result, the plugin-exists-twice warning,
    compaction disabled for a session. They are visible today only because the server happens to run
    in its own terminal; the moment a client auto-spawns it (which `main` already anticipates: "NO
    DAEMON: refcounted by attached clients") they go nowhere. `common::ConfigureLogging` runs at the
    top of `main`, before anything logs, because `DefaultLogger()` caches spdlog's default on first
    use for the process lifetime. The console sink stays; a log file that cannot be opened degrades
    to console-only rather than refusing to start.

14. **`plugins.json` was rewritten every boot, non-atomically and lossily.** A plain truncating
    `ofstream`, on every start, whether or not discovery changed anything -- while the tree already
    had `tools::write_file` (same-directory temp, fsync, rename, mode preserved, §11.7) doing this
    correctly for model-written files. The operator's own config deserved at least what a tool call
    gets. And the serializer reconstructed the document from the two fields the parser models, so
    any other top-level key a user added was silently deleted on the next boot. Unknown keys now
    round-trip, and the write happens only when the content actually changed.

15. **The headless client.** Four, all small, all in the same file:
    - `PrintJsonLine` was `fputs(dump)` then `fputc('\n')` -- two stdio calls, each taking stdout's
      lock separately -- called from the main thread AND every subagent-attach thread, so another
      thread's line could land between a JSON body and its newline. That breaks the "one JSON value
      per line" contract the file states outright. One locked `fwrite` now.
    - The exit code derived from the last `message_delta`, not from `TurnCompletedEvent`.
      `ARCHITECTURE.md` says the opposite, and §11.4 changed the server specifically so a cancel
      inside retry backoff reports `Cancelled` rather than `Unknown` -- but that path emits no
      `message_delta`, so the exit code stayed 0 and the §11.4 fix was unobservable from the client.
      `hit_max_turns` was dropped entirely; it now maps to the truncation code, since stopping at the
      turn ceiling is not success.
    - `prompt.substr(0, 60)` could split a UTF-8 sequence, and `CreateSessionRequest.name` is a
      proto3 `string`: the SERVER's parser rejects invalid UTF-8 while the client's serializer only
      logs. Any non-ASCII prompt whose 60th byte fell mid-character failed session creation with an
      opaque parse error.
    - `command_counter_` was a plain `uint64_t` incremented from concurrent streams -- its own
      comment describes that exact scenario as the reason it exists, and `++` from two threads is a
      race that can produce the duplicate id it is meant to prevent.

### Efficiency (against the 512k-1M token goal)

16. **`CappedBuffer::Append` was O(cap) per 8 KiB chunk.** It erased down to exactly `tail_cap_` on
    every append, and reads arrive in 8 KiB chunks -- so once the tail was full, every chunk memmoved
    ~768 KB to discard 8 KB. A 200 MB build log (an ordinary `make`) meant ~25,000 erases of ~768 KB,
    about 19 GB of memmove on the session worker, spent entirely on throwing bytes away. Compacting
    only past 2x makes the work proportional to the data. **Measured: 260 ms -> 5.1 ms for 200 MB,
    51x, with an identical dropped-byte count.** Adjacent: the fd-9 state payload restarted
    accumulating after an overflow, so `yes >&9` refilled and cleared 1 MiB for the whole timeout;
    the overflow is latched now.

17. **The session picker's keyset cursor had no index to seek.** §11.8 made the MESSAGE cursor a real
    seek; `ListSessions` was still a full table scan plus `USE TEMP B-TREE FOR ORDER BY` on every
    page, because nothing indexed `(updated_at, id)` -- so the cursor predicate filtered instead of
    sought and paging stayed O(total sessions) per page, the exact growth it exists to remove.
    Migration v3 adds it with DESC/ASC matching the ORDER BY so the sort disappears too, plus
    `sessions(parent_session_id)` -- the canonical unindexed-foreign-key case, where every
    `DELETE FROM sessions` full-scanned looking for cascade children.

18. **Three copies of every persisted message became two.** `AppendMessage` built a JSON DOM, dumped
    it to a string, then bound it `SQLITE_TRANSIENT`, which tells SQLite to malloc and memcpy the
    whole thing again -- for a tool result that can be hundreds of KB. `SQLITE_STATIC` with an
    explicit length (no `strlen` either) removes the third. `~CachedStmt` now clears bindings as well
    as resetting, since a borrowed pointer parked in a cached statement is not something to leave to
    "nothing steps it".

19. **Smaller, same path.** The assistant message copied `response.content` where it could move it --
    `response` is loop-local and dead immediately after, so this duplicated every block and string of
    the turn. `estimate_tokens` called `input.dump()` to read `.size()` and threw the string away,
    over the whole transcript, on the resume path; it estimates structurally now.

20. **`read_window` did not keep its documented memory bound.** `ARCHITECTURE.md` promises "memory
    proportional to `limit`, not file size", and `std::getline` materializes the WHOLE line before
    anything truncates it -- so a 40 MB minified file with no newline (and no NUL, so the binary
    probe passes) was a 40 MB allocation to display 2000 characters. It reads into a fixed buffer and
    discards the remainder now, with a 1 MiB hard cap when no `max_line_length` is configured,
    because "no configured truncation" is not the same as "willing to allocate a whole file for one
    line". `offset + limit - 1` also overflowed for model-written values near INT_MAX; computed in
    int64 and clamped.

    The test covers the behaviour (an enormous line still yields a truncated one), not the bound
    itself: a peak-RSS assertion inside a shared test binary is not dependable, so the memory
    property rests on the structure -- one fixed-size buffer, nothing else retaining the line.

### Tier 4 — correctness nits and stale contracts

The third pass's bottom tier, folded in two commits. Each is small; each is a case of the code
disagreeing with what a nearby comment or doc claims, which is the theme of the whole pass.

21. **`proto_convert`'s int64 guard admitted the one value it existed to stop.**
    `static_cast<double>(INT64_MAX)` rounds *up* to 2^63, so a `<=` against it accepted
    `9223372036854775808.0` — whose cast to `int64_t` is the UB the guard was written to prevent,
    wire-reachable through any tool_use input. Strict `<` at the top; INT64_MIN is exactly -2^63 and
    needs no such care, so the bounds are deliberately asymmetric. Same file: the header's totality
    guarantee ("a missing mapping is a compile error") held for the enum switches via `-Wswitch` and
    for neither `std::visit` + `if constexpr` chain — a future `ContentBlock` alternative would have
    compiled silently, serialized as `BLOCK_NOT_SET`, and been *dropped* on the way back. Both chains
    now end in `static_assert` on a dependent false.

22. **`Session::RunOneTurn` used `store` where its own exactly-once claim names `exchange`.** The
    claim was true, but only because `Terminate` joins the worker before reading the flag — correct
    by a mechanism other than the one documented, which is how the next edit breaks it. Firing twice
    would set a `std::promise` twice. Both sides exchange now; the property holds whatever the
    ordering.

23. **`sanity_check`'s error checks were half inert.** `!ec && is_regular_file(path, ec)` tested a
    default-constructed `error_code` before the call wrote it, so the code `is_regular_file` reported
    was never read — three copies of the pattern in the tree, one inert. And the presets scan's
    range-for over `directory_iterator(dir, ec)` made only *construction* non-throwing: `operator++`
    is the throwing overload, so one unreadable entry escaped `RunAll` and skipped every later check
    — breaking the linter property the class documents. `it.increment(ec)` throughout.

24. **`ProviderInstanceCache::GetOrCreate` read `generation_` outside `mu_`.** Harmless while the
    field is always zero; the field exists *solely* for the day a config reload bumps it, which is
    precisely the day the unsynchronised read becomes a race between the key and the bump. Lock
    first, then build the key.

25. **`SessionServiceImpl` stored a `PermissionsConfig` it never read**, and its run-mode comment
    pointed a future reader at this service for a security-relevant default that is actually applied
    in `AgentPresetRegistry`. The member, the constructor parameter, and the wrong pointer are all
    gone.

26. **`skill_registry`'s `LoadCandidate` used the throwing `is_directory`/`is_regular_file`** in a
    function documented never to throw — an unreadable parent directory escaped the loader as a
    `filesystem_error` instead of becoming a load-error entry. The `error_code` overloads, which the
    same file already used elsewhere.

27. **`InheritNumber` accepted floats.** `is_number()` is true for `4096.7` and `get<int>` truncates,
    so a typo'd `max_tokens` silently became a different, valid-looking limit. Present-but-fractional
    is now a named parse error carrying the model reference — same integer-only rule
    `settings_config` already applied.

28. **A typo'd top-level key in providers.json parsed as a valid empty config.** `"provider"`
    instead of `"providers"` surfaced much later as "providers.json is empty", pointing at the
    preset. A non-empty object with no `"providers"` key is refused at parse time, naming the first
    key actually found; `{}` stays a valid empty config.

29. **`mcps.json` leaked nlohmann's exceptions to users.** `entry.at("name")` threw
    `json.exception.out_of_range` into a blanket catch, so a missing key read like a library bug
    while every neighbouring field had a hand-written message. `name` and `transport` get the
    hand-written treatment.

30. **`insert` rewrote line endings `str_replace` left alone.** Rejoining lines unconditionally
    appended `\n` to the last line, so a file not ending in a newline gained one — editing bytes
    the caller never named. The original terminator state is preserved; the two mutating paths of
    one tool agree.

31. **`ResolveOnPath`'s two branches had drifted.** The slash branch omitted `is_regular_file`, so
    `access(dir, X_OK)` — which succeeds for directories on POSIX — offered a *grant* for a path
    that cannot be a program. Over-disclosure only, but disclosure is what consent is given against.

32. **`WrapCommand` spliced the state trailer onto a trailing `\`.** Backslash-newline is a line
    continuation, so the plain separator made `ls \` run `ls __minimal_rc=$?` — the marker became
    an argument and the state the trailer exists to report was lost. An extra newline first is
    correct for both backslash parities.

33. **The unbounded-response gaps §11.13 missed.** The blocking HTTP path accumulated the response
    body with no cap (a misbehaving proxy grows a string until the process dies); now a 64 MiB cap
    enforced in the write callback — deterministic, unlike `CURLOPT_MAXFILESIZE_LARGE`, which is
    advisory without a Content-Length — with the failure named after the cap. The SSE parser grew
    `line_buffer_`/`pending_data_` without bound on a stream with no newline; past 1 MiB it throws,
    and the streaming path's existing catch-and-classify turns that into an `LlmError` — the
    fail-closed posture of §12.11. The provider's `error_body` accumulator (classification only,
    never parsed) is capped at 64 KiB.

34. **settings.json was parsed three times at startup, from two sources of truth.** main ran the
    loop on the sanity check's parse while `AgentPresetRegistry` resolved `default_model` from its
    own — the sanity check could validate content the server was not running. The registry takes
    the caller's `SettingsConfig` now; one parse, in the sanity check, handed to both. (The early
    logging-only read in main stays: it must run before the sanity check so its findings have
    somewhere to go.)

35. **`compact_messages` logged to `DefaultLogger()`**, so its diagnostics carried no session id
    while the agent loop's adjacent compaction log did. `CompactionRequest` carries a `Logger*`
    and a session label; the compactor's lines read as one stream with the loop's.

36. **`SpawnSubagentTool` held a bare reference to the `SubagentManager`**, which declaration order
    in `main()` destroys *before* the registry holding the tool — safe only because
    `sessions.TerminateAll()` runs first on the normal path. The capture owns a `shared_ptr`; the
    order no longer matters.

37. **`command_aliases` was renamed to `aliases`, with no fallback.** The old name claimed a scope
    it did not have: the one flat `plugin.name`-keyed namespace aliases TOOLS as well as commands.
    A leftover `command_aliases` key is ignored and warned about on every boot (it round-trips
    verbatim rather than being deleted), because with no fallback the alternative to the warning is
    aliases silently not applying after an upgrade. The serializer also no longer writes an empty
    `aliases: {}` into everyone's plugins.json.

38. **The permission path's blocking `Push` was safe only by a two-file dependency nothing
    documented.** `WirePermissionChecker::check` parks the worker on a full queue; it is woken by
    `EventQueue::Close()`, which `Session::Terminate` calls only *after* `PendingRequests::Shutdown`
    has unblocked the `AwaitResponse`. Swapping those two lines deadlocks the worker on an unasked
    question. Both ends now say so, each pointing at the other.

39. **The wedged-observer test itself was a stack-use-after-scope at teardown.** Found by the
    ASan preset, which had not been run since the test landed in item 7's commit: the test's
    `release`/`entered` atomics were declared AFTER the registry, so destruction (reverse order)
    destroyed them BEFORE the registry's destructor joined the wedged dispatcher — which drains its
    queued tasks, each touching `entered`, in exactly that window. The atomics are declared first
    now, with the ordering rule written down where the next test of this shape will read it. This is
    also why the verification bar is "the ASan preset runs", not "it ran once".

40. **The bash command scanner is `tree-sitter-bash` now.** The hand-rolled walker that closed #1
    was replaced by the grammar, as the review's sequencing planned. What it bought, concretely:
    `command_name` nodes at every command position (function bodies, subshells, case arms,
    substitutions) instead of the walker's separator approximation; heredoc bodies come back as
    data, so `cat <<EOF` blocks no longer offer grants for programs that only ever existed as
    echoed text; junk words the walker recorded as programs (the loop variable `f`, `[`) are gone;
    and a word like `foo$(nproc)` as the program is `complete = false` rather than confidently
    recorded as `foo`. The structural win is the error signal: the walker could not tell "I parsed
    this" from "I mis-parsed this", which is exactly how #1 produced `complete = true` on a string
    it had misread; a parse whose tree contains ERROR or MISSING nodes now fails closed, root check
    included so missing *unnamed* tokens (`)`, `fi`) are caught too. The caveats the review named
    were honoured: `time` and `coproc` are the two keywords the grammar leaves unstructured — they
    parse as a plain command with the real program demoted to an argument, the exact shape of #1 —
    so the reserved-word rule survived as a targeted check that names the program from among the
    arguments; and every §12.1 scanner test passes unchanged, as the replacement's contract.
    Plumbers' notes: tree-sitter core moved from the `client` vcpkg feature to base dependencies
    (it was a dead entry — nothing linked it), the grammar is `FetchContent`-vendored at `v0.25.1`
    with the `SOURCE_SUBDIR` trick so its standalone-oriented CMakeLists is never invoked, and the
    two C files are compiled without our warning flags — third-party C, same policy as cpp-mcp.

### Deliberately not done

- **Memoizing `ToolRegistry::DefinitionsFor`.** It deep-copies every tool's `ToolDefinition`,
  `input_schema` included, once per tool round. Real, and the same family as §11.11-11.12 -- but
  order ~20 us against a provider call measured in hundreds of milliseconds, and a cache would need
  an invalidation rule plus a span-lifetime rule (MCP tools register after startup, and a session
  worker could hold a span across an invalidation). That is a genuine complexity increase for a cost
   that does not show up. Recorded rather than done, so the next reader does not rediscover it as an
   oversight.

---

## 13. MCP client remediation

The §11 exercise, aimed at the track-G MCP client after its first review. Each item landed as its
own commit with the invariant test that proves it load-bearing (delete the fix, watch the test
fail). One item retires a §11 deliberate-deferral: **HTTP connection reuse** is now built
(item 7 below) — the deferral was written before the MCP http transport existed to want it.

### Correctness

1. **The http transport could wedge its worker forever.** `StreamPost` set no total timeout in
   streaming mode, so a server that accepted the connection and went silent held the connection's
   worker thread indefinitely (stdio was capped inside cpp-mcp at 60 s; http had nothing).
   `request_timeout` now caps the WHOLE transfer in both modes — providers leave it 0 for streams
   and are unchanged — which also aligns the transport's failure with the caller's deadline.
2. **`tools/list` pagination had no cap.** Each page was individually within the request deadline,
   so a server that never stops returning `nextCursor` occupied the worker forever. Capped by the
   new `tools.mcp.max_list_pages` (default 100, loader rejects < 1); a cap hit is a named discovery
   failure, and a truncated listing can no longer silently hide tools by wedging instead.
3. **A wedged stdio call leaked the child process.** Teardown detached the worker and relied on
   cpp-mcp's internal 60 s cap — or, if the server process exited inside that window, orphaned the
   child forever (the detached worker's destructors never run at exit). Teardown now aborts the
   transport first: the stdio child is SIGTERMed (waitpid(WNOHANG) proves liveness AND ownership at
   signal time, so a reused pid is never signalled; a detached watcher escalates to SIGKILL after
   5 s), the blocking read ends, and the worker unwinds. The vendored cpp-mcp — pinned, no releases
   to track — carries a tiny header patch (atomic `process_id_` + `child_pid()`), applied
   idempotently by `cmake/ApplyCppMcpPatches.cmake`, which fails loudly on a half-applied or
   diverged tree rather than building silently.
4. **One odd exception in a worker job killed the server.** `WorkerState::Loop` ran jobs bare; an
   exception escaping the thread function reaches `std::terminate`. Contained: log, continue, the
   caller's deadline is the backstop.
5. **Wire hardening on the http path.** A stringified integer id echo (some SDKs stringify) now
   matches; an SSE stream that ends without the matching frame is a NAMED error ("closed the stream
   without a response to request N") instead of the misleading "unparseable response" the empty
   buffer produced; the slot's `done`/`value` reads happen under the slot mutex instead of leaning
   on same-thread callback timing documented three files away.

### Efficiency

6. **Cancellation stopped polling.** Callers waited in 25 ms condition-variable slices — 40
   wakeups per second per in-flight MCP call — solely to notice a cancellation flag.
   `tools::CancelToken` makes it event-driven end to end: one mutex+cv+flag per turn; BOTH events a
   waiter cares about (its slot completing, cancellation firing) notify the same cv; `Session`
   mints a token per turn pop, `Cancel`/`Terminate` fire every live one, the agent loop hands it to
   every `ToolContext`, and `McpConnection::WaitSlot` is a zero-poll `wait_until` loop. The
   polling `is_cancelled` stays for tools that merely check it occasionally (bash). Lock order is
   one-directional (worker: slot.mu → token.mu; waiter releases token.mu before slot.mu) — holding
   both would close a deadlock cycle, so the invariant is in the code shape, not just the comment.
7. **HTTP connection reuse (retires the §11 deferral).** `llm::HttpSession` keeps one CURL handle
   across transfers — reset after EVERY transfer by scope guard, so per-transfer options cannot
   leak while the connection cache survives — and the MCP http transport holds one for its life.
   Providers keep the stateless per-call-handle `HttpClient` untouched. Proven by a hand-framed
   HTTP server that counts ACCEPTED CONNECTIONS: three requests, one connection.
8. **`EnsureAlive` stopped serializing the world.** The manager-wide mutex was held across ping +
   connect + discovery — up to ~2 minutes of I/O per dead server, across ALL sessions, with
   `StatusOf`/`ServerNames` blocked behind it. The manager mutex now covers only the map lookup;
   per-server start mutexes serialize each server's own I/O. A 2 s verified-TTL dedups
   near-simultaneous checks (subagent spawn after the parent's check, concurrent session starts) —
   deliberately short: the first draft's 10 s masked a scripted death in the existing respawn test,
   which now guards exactly that trade.
9. **`ToolRegistry` lost its retired list.** Superseded MCP tools were parked in an unbounded
   `retired_` vector for the process lifetime (a flapping server retired its whole tool set per
   re-discovery), and the entire safety argument for it lived in comments — `Find()` handed out a
   raw pointer used across a permission Ask that can block minutes on a human. Registry-wide
   `shared_ptr` makes the executor's reference the lifetime guarantee, structurally; the old
   invariant test now proves the superseded object stays callable AND that dropping the executor's
   reference destroys it.
10. **`McpClientManager::config_` was dead weight** — a full copy of the parsed mcps.json kept for
     the process lifetime after the constructor consumed it. Now a local.

## 14. Background bash

The full spec is ARCHITECTURE.md § Background execution. Summary of what landed, the decisions
behind it, and the invariant tests that make them load-bearing. Every piece landed as its own
commit-sized change against the 841-test suite, all green.

### What landed

1. **Generalized the deferred-completion channel.** The non-blocking subagent's queue-and-flush
   became a first-class core mechanism: `InjectionQueue` lives on `SessionChannels` (the
   `shared_ptr` block a cross-thread handle must outlive to be safe), `AgentLoop` holds it from
   `SessionContext`, and `ToolContext` gains `schedule_injection` / `session_lifetime` /
   `emit_task_event` — three lifetime-safe handles a plugin can capture past its own invocation
   without ever seeing an agent-layer type.
2. **The turn-start flush.** Flushes used to run only at tool-cycle boundaries and turn end, so a
   completion landing on an *idle* session missed the next turn entirely — the loop built its
   request, called the provider, then appended the message. Injections now precede the seed
   (chronological, and visible to the turn that should carry them). One existing session test
   asserted the old (wrong) ordering; it was updated, deliberately.
3. **The seedless wake turn.** `settings.json` `"background_wake"` (default **off** — a wake is a
   provider call nobody asked for), resolved once per session at creation. The worker's wait
   predicate (`ShouldWakeForInjections`) notices a pending injection on an idle session and runs
   `AgentLoop::RunTurn()` with no seed — the injected messages ARE the user content. The
   lost-wakeup window is closed the same way `CancelToken::Notify` closes it: an empty critical
   section before `notify_all`, since a predicate change wakes nobody by itself.
4. **The plugin side, entirely in `tools-bash`**: `bash` gains `run_in_background`; a
   `BackgroundTaskRegistry` forks through the same `LaunchProcess` (extracted to
   `bash_process.{hpp,cpp}` with `CappedBuffer`, the wrapper, and the pgid machinery) and a
   DETACHED watcher drains output, discards the fd-9 state payload, reaps deadline-bounded,
   classifies, emits `BackgroundTaskEvent`, and injects a tool-result user message keyed to the
   original `tool_use_id` — the exact shape the subagent path uses and the session-naming rule
   already skips. `/tasks` (ReadOnly `HarnessCommand`) lists the operator's view; the
   `SessionObserver` erases the per-session records alongside the shell-state map.
5. **Wire surface, additive only**: `BackgroundTaskEvent` (proto oneof case 12; nested state enum),
   mapped from a core-neutral struct in `tool.hpp` — plugins never touch protobuf. Live output
   deltas keep the original `tool_use_id`, so a client's panel row reopens after its ack's finish
   event; that is the mode, and the running-tools section says so.

### Decisions worth their reasoning

- **No output tool, no kill tool.** Mid-run output is the model's redirect-to-file pattern (pipes
  are why bash has no schema knobs); killing is `kill -- -PID` — the child setsids, so pid == pgid
  and one signal reaches the tree, via an ordinary bash call. The ack text says both. The
  alternative was two more tool names, two more permission keys, and two more surfaces to replace
  when a better executor plugin arrives.
- **Cancellation is asymmetric by design**: `CancelTurn` never kills a task (outliving the turn is
  the point — the watcher holds no turn `CancelToken`); session terminate kills within one ~100 ms
  poll slice (`session_lifetime` expiry IS the kill mechanism, since `OnSessionEnded` is
  fire-and-forget and cannot order against teardown); process exit sweeps the process-global pgid
  set via `atexit` (pids forgotten on reap, so only live groups are held — no pid-reuse exposure).
- **Background state is never adopted.** A task's final cwd/env is stale by the time it reports;
  adopting would stomp what foreground calls set up in between. The watcher still DRAINS fd 9 —
  a child blocked writing a full pipe never exits.
- **Two ceilings, two rationales.** The sync deadline bounds a BLOCKED WORKER; a background task
  never holds one, so `background_max_timeout_ms` (default 600 s, the default budget when the
  model supplies none) exists separately rather than silently inheriting the sync rationale.
- **`-p` and background don't mix, honestly.** The stream closes when the headless turn ends,
  which terminates the session, which kills the tasks. `run_in_background` under `-p` means
  "don't wait within this turn," nothing more.

### Invariant tests

- [x] Ack returns in milliseconds, not the command's runtime; result injected at completion keyed
      to the original `tool_use_id` (`bash_background_test.cpp`).
- [x] Failure injects an ERROR result with output leading and `[exit status: N]` attached.
- [x] Live channel uncapped, injected record capped with the omission marker — two artifacts, two
      audiences, verified against a 100 KB `yes`.
- [x] Session lifetime expiry kills the task within the poll deadline and injects nothing.
- [x] The wall-clock ceiling kills the group; `killpg(pid, 0)` then fails `ESRCH` — the group is
      actually gone, not just marked.
- [x] Over-limit is a refusal naming the limit, never a wait; `CountRunning` drains to 0 after expiry.
- [x] A `cd`/`export` in a background command leaves the session's shell state untouched.
- [x] Turn-start flush: the injected message precedes the seed in the working history AND in the
      first request the provider saw (`agent_loop_test.cpp`).
- [x] The seedless wake turn runs to a natural stop carrying only the injections; `wake_on_injection`
      off means no provider call and flush-before-seed on the next user turn (`session_test.cpp`).

## 15. Plugin storage + the `checklist` plugin + the `PluginCall` server half

Three pieces landing together, because each is the other's first consumer:

1. **`PluginStorage`** (`src/server/plugins/plugin_storage.{hpp,cpp}`) — the sanctioned way a plugin
   persists state across restarts. One shared SQLite file (`DataDir()/plugins.db`), the session
   store's discipline (one connection, one mutex every method locks, WAL, `BEGIN IMMEDIATE`
   + `user_version` migrations in one transaction, newer-schema refusal) extracted and namespaced.
   `plugin` is part of the primary key and the first argument of every method, so one plugin cannot
   read another's rows — structural, not conventional; cross-plugin goes through the service
   channel. Values are opaque strings. The escape hatch for state that does not fit a KV shape is
   `PluginRegistrationContext::data_dir` — the loader provisions
   `<DataDir()>/plugins/<plugin_name>/` before registration and a plugin may keep its own database
   there. Open failure is fatal at startup: the seam is handed to plugins at load, bundled plugins
   may depend on it, and a store a newer binary wrote must not be silently rebuilt over.
   Exposed via `PluginRegistrationContext::storage` (additive members, not new symbols). No
   session-scoped mechanism: scoping is a keying decision, cleanup a plugin decision — recorded in
   ARCHITECTURE § *Plugin storage* because "purge on OnSessionEnded breaks resume" is the
   non-obvious half.
2. **`PluginCall` server half** — §EXTENSIBILITY 2.5 landed early, deliberately. The proto was
   already declared; the stub answered `UNIMPLEMENTED`. The calling convention lives in
   `plugin_rpc.hpp`: a plugin serves UI consumers through a `PluginRpcHandler` provided under
   `<plugin_name>.rpc` (the suffix keeps the dispatch target clear of plain §2.2 service names —
   a collision there is fatal, so it is made unexpressible). `DispatchPluginCall` is grpc-free so
   the dispatch is unit-testable without a channel; `session_service.cpp` only maps outcomes to
   statuses (`NOT_FOUND` = plugin disabled/absent, `INVALID_ARGUMENT` = bad request or handler
   refusal, `INTERNAL` = handler threw) behind the usual `EnsureAuthenticated` guard.
   `PluginSubscribe` stays `UNIMPLEMENTED` — no consumer needs live push yet, and "poll first"
   is the honest shape for model-driven, low-frequency data. Why land before track E: the
   mechanism is the plugin-first philosophy's data path, and any frontend (web/mobile over plain
   gRPC included) can now consume plugin state with no client-plugin machinery.
3. **`checklist`** (`plugins/server/checklist/`) — bundled plugin, the model's per-session task
   scratchpad. One tool (`checklist`, category `ReadOnly` — reaches no filesystem or process, so
   the default chain allows it; gating a todo list trains click-through), ops `set` (full replace,
   TodoWrite ergonomics) / `update` (mutate by id) / `view`; the result text is a GFM tasklist —
   that text is the MODEL's medium, NOT the UI's. UI presentation is deliberately not
   server-rendered: the plugin provides `checklist.rpc` (`get`/`list`, plugin-defined JSON) and any
   consumer — TUI client-plugin view, web, mobile, script — fetches the same structured items over
   the generic channel. State is one list per session keyed by session id in the plugin's storage
   namespace; it survives resume and is not purged on session end (tiny rows, no session-deleted
   hook). `context.storage == nullptr` degrades to clear error results, never a crash.

`bundled/agents/plan.md` gained `checklist` in `allowed_tools`; `build.md` already allows all
tools. `plugins.json` self-appends the new bundled entry on first start (provenance bundled,
enabled), needing no migration.

# minimal — Development Plan

Companion to `ARCHITECTURE.md`, which remains the single source of truth for *design*. This document covers only *sequencing*: what is built, what blocks what, and what "done" means for each remaining piece. Every milestone cites the `ARCHITECTURE.md` section that specifies it; where this plan and that document disagree, that document wins and this one is stale.

**Status: Milestones A, B and H are complete, and a post-merge review of all three has been actioned (§8). 444 tests passing, clean build, zero warnings.**

`minimal -p` holds real conversations against a local model using `read`, `bash`, and `skill`; a permission prompt round-trips over the wire in both directions; a session grant silences the second prompt; the server exits when the last client detaches. Transcripts persist to SQLite and survive the server process — `GetMessages` and `ListSessions` answer from the database, so a session the server has no live object for still reads back. `cmake --install` now produces a config dir the server actually starts against (§8). **Sessions cannot yet be resumed**, only read (§4 B).

---

## 1. Where the project actually is

**Milestone A closed the gap this section described.** The server now serves; the headless client is a real client; every layer below has been executed rather than only unit-tested. What follows is updated for that.

### Built and tested

| Subsystem | Location | Tests |
|---|---|---|
| Config: settings/providers/permissions/plugins, frontmatter, presets, skills, XDG paths | `src/server/config/` | 63 |
| Permission model: lattice, run modes, precedence chains, out-of-workdir fold, MCP policy, consent scoping | `src/server/tools/` | 40 |
| Tools: `Tool`/registry/executor, path confinement, `file_access`, `atomic_write`, `SkillTool` | `src/server/tools/` | 47 |
| Plugins: discovery, `dlopen` loader + ABI guard, collision/alias registry | `src/server/plugins/` | 16 |
| LLM: provider interface, chat/stream types, SSE parser, HTTP client, token accounting | `src/server/llm/` | 30 |
| Agent loop: tool cycle, retries, cancellation, compaction trigger + boundary snapping, production compactor | `src/server/agent/` | 29 |
| `tools-fs`, `tools-bash`, `provider-openai` | `plugins/server/` | 74 |

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

### Still not built

`src/server/commands/`, `src/server/mcp/`, the TUI, subagents, the sanity check, **session resume** (see §4 B). `commands-core` is an empty plugin shell.

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
- **F. Startup sanity check** — pure config-layer work over already-built loaders. `main.cpp` is already staged for it (`main.cpp:114`).
- ~~**H. Production compactor**~~ — **done.** `compact_messages` replaces `Session`'s no-op stub, and the bundled prompt is delivered (§4 H).
- ~~**I. `SessionObserver` registry**~~ — **done**, landed with A2. `minimal_plugin_register_session_observer` is live in the plugin ABI and `tools-bash` implements it.

**Strictly dependent on A:** C, D, E, G, and every RPC beyond `CreateSession`/`OpenSession`. **A is now done, so all of them are unblocked.**

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
- [ ] Resolve-before-await and await-before-resolve both work (the ordering race).
- [ ] Two concurrent waiters on different ids are resolved independently.
- [ ] Resolve with an unknown id: no-op, no crash.
- [ ] Termination wakes a blocked waiter; assert it returns rather than joining a hung thread.
- [ ] Queue: backlog buffered before attach, flushed in order on attach.
- [ ] Queue: single-writer invariant — concurrent producers never interleave a partial event.
- [ ] **Invariant test:** a deliberately-reintroduced "resolve only if a waiter is already registered" bug must fail the ordering-race case.

### A2 — `Session`

**What.** `src/server/agent/session.{hpp,cpp}`: owns `AgentLoop`, `ToolExecutor`, provider instance, resolved `AgentPreset`, `HistoryStore`, the outbound queue, the atomic cancel flag, and the worker thread. Plus `SessionRegistry` keyed by session id.

**How.** Three threads per session (§ *Concurrency model* table). The worker blocks freely; the reader never blocks on anything but `Read()`; one writer per attached stream is the sole `Write()` caller. Termination reuses the cancellation path — atomic flag → `bool` stream sink aborts the provider → tools observe `is_cancelled` — then joins and reaps. **No new termination mechanism.**

**Acceptance criteria**
- A session runs a full turn against `FakeLlmProvider` with a real `ToolExecutor` and real `tools-fs`/`tools-bash` tools — no fakes below the provider.
- Cancelling mid-turn aborts the provider stream and stops the worker within one tool boundary.
- Stream close after attach terminates the session; a never-attached session terminates on the ~10 s initial-attach guard (§ *Session lifetime*, "one carve-out").
- `project_root` comes from `CreateSessionRequest` and reaches `ToolContext::project_root` and bash's initial cwd.

**Test cases** (`tests/session_test.cpp`)
- [ ] Full turn: user text → tool call → tool result → assistant text, through the real executor.
- [ ] Cancel mid-turn: worker exits, no zombie thread, partial transcript retained.
- [ ] Terminate with a turn in flight: joins cleanly, no leaked thread, no use-after-free of the queue.
- [ ] Never-attached session terminates on the initial-attach guard.
- [ ] Two sessions with different `project_root`s do not see each other's bash cwd/env (integration with `BashSessionStateStore`).
- [ ] **Invariant test:** running the agent loop on the reader thread instead of the worker must deadlock the permission path — assert the worker/reader split is load-bearing rather than incidental.

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
- [ ] Round-trip a turn end to end; assert `ServerEvent` payload ordering.
- [ ] Permission allow and deny, both round-tripped over the wire.
- [ ] A disclosure with grants: answering with `granted_keys` silences the second prompt (proves consent scoping survives serialisation — currently only proven in-process).
- [ ] `CancelTurn` mid-stream.
- [ ] Client disconnects mid-turn: session terminates, no leaked thread.
- [ ] Two concurrent sessions on one channel do not cross-talk (`session_id` routing).
- [ ] **Invariant test:** removing the single-writer discipline must produce interleaved/corrupt writes under concurrent producers.

### A4 — Headless client (`minimal -p`)

**What.** `src/client/app.cpp` `RunHeadless` (`app.cpp:18`) and a real `SessionClient` (currently a constructor and destructor).

**How.** Connect, `CreateSession(project_root = cwd)`, send one `UserChatTurn`, stream `ServerEvent`s to stdout as text or JSONL, exit on stop reason. Defaults to `PermissionRunMode::AllowOnly` so an `Ask`-gated tool auto-denies instead of blocking on a human who was never attached (§ *Non-interactive client*).

**Acceptance criteria**
- `minimal -p "list the files here"` against a local llama server produces a real answer using real tools.
- Exit code reflects the terminal stop reason; JSONL mode is machine-parseable.
- An `Ask`-gated tool auto-denies and the run completes rather than hanging.

**Test cases**
- [ ] Scripted end-to-end against a fake provider over a real socket (CI-safe, no network).
- [ ] `AllowOnly` auto-deny path completes rather than blocking.
- [ ] Attaching to a child session is a client obligation (§ *Session lifetime*) — assert the headless client does it, since the initial-attach guard is a safety net and not a designed flow.

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

> § *Session storage — SQLite*. Built on a branch off `cab3a91`, in parallel with and unaware of Milestone A, then merged. **Done** — 31 cases in `tests/session_store_test.cpp` plus 5 integration cases in `tests/session_service_test.cpp`.

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

### F. Startup sanity check

> § *Startup sanity check — `src/server/config/sanity_check.{hpp,cpp}`*. Independent; `main.cpp:114` is already staged for it.

**How.** `StartupSanityCheck::RunAll()` returns `vector<CheckResult>` and **runs every check regardless of earlier failures** — it is a linter, not a fail-fast validator, so a misconfigured setup yields one complete diagnostic instead of a fix-one-rerun-find-the-next loop. A cold start with nothing configured is one more failing check with a clear reason, not a bespoke onboarding wizard.

**Acceptance criteria.** The checks enumerated in that section: every config file parses; a usable `providers.json` entry exists with a resolvable key *and* a registered provider kind; each `mcps.json` entry's `command` resolves on `PATH` (stdio) or `url` is well-formed (http); `project_root` exists and is writable; the SQLite database is accessible and not newer than the binary; each discovered plugin's ABI tag matches; `max_concurrent_subagents` is sane; **no registration-name collisions (fatal)**; every `command_aliases` key resolves to something registered.

Bundled-plugin failures are Fatal, user-plugin failures Warning.

**Test cases** (`tests/sanity_check_test.cpp`)
- [ ] Each check fires on its own broken fixture and passes on a good one.
- [ ] **Two independent failures both appear in one `RunAll()` result** — the linter property is the whole point and is the easy one to regress into fail-fast.
- [ ] Bundled-vs-user severity split is respected.
- [ ] A name collision is fatal — this is the rule that makes tool replacement deliberate (§ *Name collisions and aliases*).
- [ ] Unknown `allowed_skills` entries are reported (currently silently dropped by design — `agent_preset_registry.cpp` notes "a future sanity_check makes them loud").

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
- [ ] Ending a session erases exactly that session's bash state and no other's.
- [ ] A throwing observer does not take down session teardown.

---

## 5. Dependent tracks (after A)

### C. `/`-command system

> § *`/`-command system — `src/server/commands/`*, § *Name collisions and aliases*.

Core registry plus `Command` → (`LlmCommand`, `HarnessCommand`) hierarchy; `CommandContext` borrowing session-owned references. Read-only `HarnessCommand`s dispatch to a short-lived task — *not* inline on the reader thread, which must never block; everything else queues for the worker at a turn boundary.

Fills the empty `commands-core` plugin and unblocks `/restart-shell` in `tools-bash` (`plugin.cpp:62`).

- [ ] Read-only commands execute without blocking the reader.
- [ ] Non-read-only commands land at a turn boundary, never between a `tool_use` and its `tool_result`.
- [ ] Alias resolution and collision rules match § *Name collisions and aliases*; permission keys are never aliases.
- [ ] `/restart-shell` resets the same map `restart: true` does — proving plugin-owned state has one owner.

### D. Subagents

> § *Subagents*. Only `kSpawnSubagentToolName` and the no-nested-subagents exclusion exist today.

A subagent is a `Session` with a parent pointer — the same entity, not a second implementation. Blocking and non-blocking paths both route through one `AgentLoop::RunTurn`.

- [ ] Blocking subagent returns its final text as the parent's `ToolResultBlock`.
- [ ] Non-blocking completion is injected at a turn boundary (`QueueInjectedMessage` already exists for exactly this).
- [ ] Terminating a blocking subagent unblocks the parent with an error result rather than hanging (§ *Session lifetime*).
- [ ] A subagent cannot spawn subagents — already enforced in `agent_loop.cpp:85`; assert it end to end.
- [ ] `clamp_run_mode` holds: a child is never more permissive than its parent.
- [ ] Consent-grant inheritance decided and tested — plan leans copy-at-spawn, since re-prompting for every binary makes subagents unusable (see § *Consent scoping*, non-interactive note).

### E. TUI client

> § *TUI rendering*, § *TUI layout*, § *Tab states and attention*, § *Message-kind rendering*, § *@-mention file references*.

Largest remaining surface: FTXUI layout, md4c markdown rendering, tree-sitter highlighting, themes, session picker, running-tools panel, permission modal, @-mentions (`SuggestPaths`). Depends on A3 for every RPC it uses and on B for scrollback.

The permission modal is where consent scoping becomes real UI — and where the **wording obligations** in § *Consent scoping* must be honoured: a grant is blanket permission for a binary, and disclosure is a lower bound.

- [ ] Permission modal renders `PermissionDisclosure` including per-binary session grants.
- [ ] Streaming tokens render without full-buffer reflow at 512k–1M context.
- [ ] Tab bar: colour + single `!` attention glyph only (no metadata clutter).
- [ ] Scrollback pages via `GetMessages` without competing with live turn traffic.

### G. MCP client

> § *MCP client — `src/server/mcp/`*. cpp-mcp is pinned and fetched but never referenced; only permission plumbing exists.

Lazy connection on first MCP tool call. Tools register as `mcp__<server>__<tool>` — `mcp_server_of` and the per-server permission chain are already built and tested.

- [ ] Tools from a stdio MCP server appear as `mcp__<server>__<tool>` and execute.
- [ ] Per-server permission policy governs them (already tested in isolation — assert it end to end).
- [ ] A dead/hanging MCP server degrades to a tool error, never a hung worker.
- [ ] Connection is lazy: no MCP process spawns until a tool is actually called.

---

## 6. Cross-cutting standards

Applies to every milestone. These are existing project practice, recorded so they survive contributor turnover.

- **Invariant tests must be proven load-bearing.** Delete the invariant, watch the test fail, restore. Already applied to the sort comparator, `MaxTokens`, the grant conjunction, the vacuous-truth guard, and every skill-registry diagnostic. A test that passes with the fix removed is not a test.
- **Verify by execution, not inspection.** Plugin linkage was confirmed with `nm -u`/`otool -L`; the ABI mismatch was demonstrated against a running server; the bash hangs were measured (8008 ms → 0 ms). Claims about behaviour need a run behind them.
- **The pinned toolchain does not synthesize implicit `operator==` for user-defined classes** (verified against AppleClang 21: even `struct E { bool b; };` has no `==`; an explicit `= default` works). Compare fields explicitly, as the existing tests do — do not add defaulted operators to production types just for test convenience.
- **Zero warnings.** `-Wall -Wextra` on all first-party targets; the tree is clean and must stay so.
- **`ARCHITECTURE.md` is updated in the same change that diverges from it.** The bash rework's core finding was a restriction that outlived its rationale because the doc was updated without re-deriving the rule.
- **Test tiers** (§ *Testing*): pure logic → agent loop against `FakeLlmProvider` → tools against a temp `project_root` → end-to-end over a real socket.

---

## 7. Known gaps carried forward

Not scheduled, recorded so they are decisions rather than oversights.

- **Server-wide fd hygiene** — no `CLOEXEC` discipline on gRPC/SQLite/log fds; every bash command inherits them. The bash pipes are handled; the rest is not.
- **`tree-sitter-bash` for command disclosure** — the hand-rolled scanner is conservative and its failure mode is safe (fewer grants offered). Upgrading raises the hit rate, not the safety floor. The grammar is already a planned client dependency.
- **Unknown `allowed_skills` entries are silently dropped** — correct behaviour for resolution, but should be *reported* by F.
- **`themes/*.json` and `keybindings.json` are specified but unread** — nothing outside a comment in `config_paths.hpp` refers to them, so they are deliberately not seeded either. They arrive with the TUI (track E), which is the first thing that would read one.
- **`compaction_keep_recent_tokens` is parsed but unused** — the compaction cut is half the working history by message count (snapped to a tool-call cycle); selecting it by tokens needs per-message counts that are not tracked in memory (§ *Context compaction*).
- **Backend `usage: 0`** from some OpenAI-compatible proxies — no fallback estimate designed (§ *Deferred*). Note the production compactor's `context_tokens_before` falls back to a bytes/4 estimate in exactly this case; it is an admission of roughness, not a precision claim.
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
`context_compaction_trigger_tokens` defaults to `0`, which means *never*, so the seeded
`settings.json` sets a real value: without it the prompt H delivered still never gets used.

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
  output rather than the exit code. ASan's one report is inside gRPC's own `send_goaway` frame and
  reproduces with every change stashed — an uninstrumented dependency, not a defect.
- **A ceiling is not an invariant test.** The first "the retry waited" assertion was `elapsed < 2s`,
  which passes with the backoff deleted. Under full jitter no single run *can* assert a floor; the
  fix was to change the algorithm to equal jitter so the floor exists, not to loosen the test.
- **The compiler cannot catch a sentinel change.** Every `before_seq` call site kept compiling and
  quietly changed meaning; the tests caught all eleven. A deleted overload is what stops the twelfth
  from being written.
